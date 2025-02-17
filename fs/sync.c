/*
 * High-level sync()-related operations
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/backing-dev.h>
#include "internal.h"
#ifdef CONFIG_ASYNC_FSYNC
#include <linux/statfs.h>
#endif

#define VALID_FLAGS (SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE| \
			SYNC_FILE_RANGE_WAIT_AFTER)

#ifdef CONFIG_ASYNC_FSYNC
#define FLAG_ASYNC_FSYNC        0x1
static struct workqueue_struct *fsync_workqueue = NULL;
struct fsync_work {
	struct work_struct work;
	char pathname[256];
};
#endif

/*
 * Do the filesystem syncing work. For simple filesystems
 * writeback_inodes_sb(sb) just dirties buffers with inodes so we have to
 * submit IO for these buffers via __sync_blockdev(). This also speeds up the
 * wait == 1 case since in that case write_inode() functions do
 * sync_dirty_buffer() and thus effectively write one block at a time.
 */
static int __sync_filesystem(struct super_block *sb, int wait)
{
	if (wait)
		sync_inodes_sb(sb);
	else
		writeback_inodes_sb(sb, WB_REASON_SYNC);

	if (sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, wait);
	return __sync_blockdev(sb->s_bdev, wait);
}

/*
 * Write out and wait upon all dirty data associated with this
 * superblock.  Filesystem data as well as the underlying block
 * device.  Takes the superblock lock.
 */
int sync_filesystem(struct super_block *sb)
{
	int ret;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/*
	 * No point in syncing out anything if the filesystem is read-only.
	 */
	if (sb->s_flags & MS_RDONLY)
		return 0;

	ret = __sync_filesystem(sb, 0);
	if (ret < 0)
		return ret;
	return __sync_filesystem(sb, 1);
}
EXPORT_SYMBOL_GPL(sync_filesystem);

static void sync_inodes_one_sb(struct super_block *sb, void *arg)
{
	if (!(sb->s_flags & MS_RDONLY))
		sync_inodes_sb(sb);
}

static void sync_fs_one_sb(struct super_block *sb, void *arg)
{
	if (!(sb->s_flags & MS_RDONLY) && sb->s_op->sync_fs)
		sb->s_op->sync_fs(sb, *(int *)arg);
}

static void fdatawrite_one_bdev(struct block_device *bdev, void *arg)
{
	filemap_fdatawrite(bdev->bd_inode->i_mapping);
}

static void fdatawait_one_bdev(struct block_device *bdev, void *arg)
{
	filemap_fdatawait(bdev->bd_inode->i_mapping);
}

/*
 * Sync everything. We start by waking flusher threads so that most of
 * writeback runs on all devices in parallel. Then we sync all inodes reliably
 * which effectively also waits for all flusher threads to finish doing
 * writeback. At this point all data is on disk so metadata should be stable
 * and we tell filesystems to sync their metadata via ->sync_fs() calls.
 * Finally, we writeout all block devices because some filesystems (e.g. ext2)
 * just write metadata (such as inodes or bitmaps) to block device page cache
 * and do not sync it on their own in ->sync_fs().
 */
static void do_sync(void)
{
	int nowait = 0, wait = 1;

	wakeup_flusher_threads(0, WB_REASON_SYNC);
	iterate_supers(sync_inodes_one_sb, NULL);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &wait);
	iterate_bdevs(fdatawrite_one_bdev, NULL);
	iterate_bdevs(fdatawait_one_bdev, NULL);
	if (unlikely(laptop_mode))
		laptop_sync_completion();
	return;
}

static DEFINE_MUTEX(sync_mutex);	/* One do_sync() at a time. */
static unsigned long sync_seq;		/* Many sync()s from one do_sync(). */
					/*  Overflow harmless, extra wait. */

/*
 * Only allow one task to do sync() at a time, and further allow
 * concurrent sync() calls to be satisfied by a single do_sync()
 * invocation.
 */
SYSCALL_DEFINE0(sync)
{
	unsigned long snap;
	unsigned long snap_done;

	snap = ACCESS_ONCE(sync_seq);
	smp_mb();  /* Prevent above from bleeding into critical section. */
	mutex_lock(&sync_mutex);
	snap_done = sync_seq;

	/*
	 * If the value in snap is odd, we need to wait for the current
	 * do_sync() to complete, then wait for the next one, in other
	 * words, we need the value of snap_done to be three larger than
	 * the value of snap.  On the other hand, if the value in snap is
	 * even, we only have to wait for the next request to complete,
	 * in other words, we need the value of snap_done to be only two
	 * greater than the value of snap.  The "(snap + 3) & 0x1" computes
	 * this for us (thank you, Linus!).
	 */
	if (ULONG_CMP_GE(snap_done, (snap + 3) & ~0x1)) {
		/*
		 * A full do_sync() executed between our two fetches from
		 * sync_seq, so our work is done!
		 */
		smp_mb(); /* Order test with caller's subsequent code. */
		mutex_unlock(&sync_mutex);
		return 0;
	}

	/* Record the start of do_sync(). */
	ACCESS_ONCE(sync_seq)++;
	WARN_ON_ONCE((sync_seq & 0x1) != 1);
	smp_mb(); /* Keep prior increment out of do_sync(). */

	do_sync();

	/* Record the end of do_sync(). */
	smp_mb(); /* Keep subsequent increment out of do_sync(). */
	ACCESS_ONCE(sync_seq)++;
	WARN_ON_ONCE((sync_seq & 0x1) != 0);
	mutex_unlock(&sync_mutex);
	return 0;
}

static void do_sync_work(struct work_struct *work)
{
	int nowait = 0;

	/*
	 * Sync twice to reduce the possibility we skipped some inodes / pages
	 * because they were temporarily locked
	 */
	iterate_supers(sync_inodes_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_bdevs(fdatawrite_one_bdev, NULL);
	iterate_supers(sync_inodes_one_sb, &nowait);
	iterate_supers(sync_fs_one_sb, &nowait);
	iterate_bdevs(fdatawrite_one_bdev, NULL);
	printk("Emergency Sync complete\n");
	kfree(work);
}

void emergency_sync(void)
{
	struct work_struct *work;

	work = kmalloc(sizeof(*work), GFP_ATOMIC);
	if (work) {
		INIT_WORK(work, do_sync_work);
		schedule_work(work);
	}
}

/*
 * sync a single super
 */
SYSCALL_DEFINE1(syncfs, int, fd)
{
	struct fd f = fdget(fd);
	struct super_block *sb;
	int ret;

	if (!f.file)
		return -EBADF;
	sb = f.file->f_dentry->d_sb;

	down_read(&sb->s_umount);
	ret = sync_filesystem(sb);
	up_read(&sb->s_umount);

	fdput(f);
	return ret;
}

/**
 * vfs_fsync_range - helper to sync a range of data & metadata to disk
 * @file:		file to sync
 * @start:		offset in bytes of the beginning of data range to sync
 * @end:		offset in bytes of the end of data range (inclusive)
 * @datasync:		perform only datasync
 *
 * Write back data in range @start..@end and metadata for @file to disk.  If
 * @datasync is set only metadata needed to access modified file data is
 * written.
 */
int vfs_fsync_range(struct file *file, loff_t start, loff_t end, int datasync)
{
	if (!file->f_op || !file->f_op->fsync)
		return -EINVAL;
	return file->f_op->fsync(file, start, end, datasync);
}
EXPORT_SYMBOL(vfs_fsync_range);

/**
 * vfs_fsync - perform a fsync or fdatasync on a file
 * @file:		file to sync
 * @datasync:		only perform a fdatasync operation
 *
 * Write back data and metadata for @file to disk.  If @datasync is
 * set only metadata needed to access modified file data is written.
 */
int vfs_fsync(struct file *file, int datasync)
{
	return vfs_fsync_range(file, 0, LLONG_MAX, datasync);
}
EXPORT_SYMBOL(vfs_fsync);

#ifdef CONFIG_ASYNC_FSYNC
#define LOW_STORAGE_THRESHOLD   786432
int async_fsync(struct file *file, int fd)
{
        struct inode *inode = file->f_mapping->host;
        struct super_block *sb = inode->i_sb;
        struct kstatfs st;

        if ((sb->fsync_flags & FLAG_ASYNC_FSYNC) == 0)
                return 0;

        if (fd_statfs(fd, &st))
                return 0;

        if (st.f_bfree > LOW_STORAGE_THRESHOLD)
                return 0;

        return 1;
}

static int do_async_fsync(char *pathname)
{
        struct file *file;
        int ret;
        file = filp_open(pathname, O_RDWR, 0);
        if (IS_ERR(file)) {
                pr_debug("%s: can't open %s\n", __func__, pathname);
                return -EBADF;
        }
        ret = vfs_fsync(file, 0);

        filp_close(file, NULL);
        return ret;
}

static void do_afsync_work(struct work_struct *work)
{
        struct fsync_work *fwork =
                container_of(work, struct fsync_work, work);
        int ret = -EBADF;

        pr_debug("afsync: %s\n", fwork->pathname);
        ret = do_async_fsync(fwork->pathname);
        if (ret != 0 && ret != -EBADF)
                pr_info("afsync return %d\n", ret);
        else
                pr_debug("afsync: %s done\n", fwork->pathname);
        kfree(fwork);
}
#endif

static int do_fsync(unsigned int fd, int datasync)
{
	struct fd f = fdget(fd);
	int ret = -EBADF;
#ifdef CONFIG_ASYNC_FSYNC
        struct fsync_work *fwork;
#endif

	if (f.file) {
#ifdef CONFIG_ASYNC_FSYNC
                ktime_t fsync_t, fsync_diff;
                char pathname[256], *path;
                path = d_path(&(f.file->f_path), pathname, sizeof(pathname));
                if (IS_ERR(path))
                        path = "(unknown)";
                else if (async_fsync(f.file, fd)) {
                        if (!fsync_workqueue)
                                fsync_workqueue =
                                        create_singlethread_workqueue("fsync");
                        if (!fsync_workqueue)
                                goto no_async;

                        if (IS_ERR(path))
                                goto no_async;

                        fwork = kmalloc(sizeof(*fwork), GFP_KERNEL);
                        if (fwork) {
                                strncpy(fwork->pathname, path,
                                        sizeof(fwork->pathname) - 1);
                                INIT_WORK(&fwork->work, do_afsync_work);
                                queue_work(fsync_workqueue, &fwork->work);
                                fdput(f);
                                return 0;
                        }
                }
no_async:
                fsync_t = ktime_get();
#endif
		ret = vfs_fsync(f.file, datasync);
		fdput(f);
#ifdef CONFIG_ASYNC_FSYNC
                fsync_diff = ktime_sub(ktime_get(), fsync_t);
                if (ktime_to_ms(fsync_diff) >= 5000) {
                        pr_info("VFS: %s pid:%d(%s)(parent:%d/%s)\
                                takes %lld ms to fsync %s.\n", __func__,
                                current->pid, current->comm,
                                current->parent->pid, current->parent->comm,
                                ktime_to_ms(fsync_diff), path);
                }
#endif
	}
	return ret;
}

SYSCALL_DEFINE1(fsync, unsigned int, fd)
{
	return do_fsync(fd, 0);
}

SYSCALL_DEFINE1(fdatasync, unsigned int, fd)
{
	return do_fsync(fd, 1);
}

/**
 * generic_write_sync - perform syncing after a write if file / inode is sync
 * @file:	file to which the write happened
 * @pos:	offset where the write started
 * @count:	length of the write
 *
 * This is just a simple wrapper about our general syncing function.
 */
int generic_write_sync(struct file *file, loff_t pos, loff_t count)
{
	if (!(file->f_flags & O_DSYNC) && !IS_SYNC(file->f_mapping->host))
		return 0;
	return vfs_fsync_range(file, pos, pos + count - 1,
			       (file->f_flags & __O_SYNC) ? 0 : 1);
}
EXPORT_SYMBOL(generic_write_sync);

/*
 * sys_sync_file_range() permits finely controlled syncing over a segment of
 * a file in the range offset .. (offset+nbytes-1) inclusive.  If nbytes is
 * zero then sys_sync_file_range() will operate from offset out to EOF.
 *
 * The flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE: wait upon writeout of all pages in the range
 * before performing the write.
 *
 * SYNC_FILE_RANGE_WRITE: initiate writeout of all those dirty pages in the
 * range which are not presently under writeback. Note that this may block for
 * significant periods due to exhaustion of disk request structures.
 *
 * SYNC_FILE_RANGE_WAIT_AFTER: wait upon writeout of all pages in the range
 * after performing the write.
 *
 * Useful combinations of the flag bits are:
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE: ensures that all pages
 * in the range which were dirty on entry to sys_sync_file_range() are placed
 * under writeout.  This is a start-write-for-data-integrity operation.
 *
 * SYNC_FILE_RANGE_WRITE: start writeout of all dirty pages in the range which
 * are not presently under writeout.  This is an asynchronous flush-to-disk
 * operation.  Not suitable for data integrity operations.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE (or SYNC_FILE_RANGE_WAIT_AFTER): wait for
 * completion of writeout of all pages in the range.  This will be used after an
 * earlier SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE operation to wait
 * for that operation to complete and to return the result.
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE|SYNC_FILE_RANGE_WAIT_AFTER:
 * a traditional sync() operation.  This is a write-for-data-integrity operation
 * which will ensure that all pages in the range which were dirty on entry to
 * sys_sync_file_range() are committed to disk.
 *
 *
 * SYNC_FILE_RANGE_WAIT_BEFORE and SYNC_FILE_RANGE_WAIT_AFTER will detect any
 * I/O errors or ENOSPC conditions and will return those to the caller, after
 * clearing the EIO and ENOSPC flags in the address_space.
 *
 * It should be noted that none of these operations write out the file's
 * metadata.  So unless the application is strictly performing overwrites of
 * already-instantiated disk blocks, there are no guarantees here that the data
 * will be available after a crash.
 */
SYSCALL_DEFINE4(sync_file_range, int, fd, loff_t, offset, loff_t, nbytes,
				unsigned int, flags)
{
	int ret;
	struct fd f;
	struct address_space *mapping;
	loff_t endbyte;			/* inclusive */
	umode_t i_mode;

	ret = -EINVAL;
	if (flags & ~VALID_FLAGS)
		goto out;

	endbyte = offset + nbytes;

	if ((s64)offset < 0)
		goto out;
	if ((s64)endbyte < 0)
		goto out;
	if (endbyte < offset)
		goto out;

	if (sizeof(pgoff_t) == 4) {
		if (offset >= (0x100000000ULL << PAGE_CACHE_SHIFT)) {
			/*
			 * The range starts outside a 32 bit machine's
			 * pagecache addressing capabilities.  Let it "succeed"
			 */
			ret = 0;
			goto out;
		}
		if (endbyte >= (0x100000000ULL << PAGE_CACHE_SHIFT)) {
			/*
			 * Out to EOF
			 */
			nbytes = 0;
		}
	}

	if (nbytes == 0)
		endbyte = LLONG_MAX;
	else
		endbyte--;		/* inclusive */

	ret = -EBADF;
	f = fdget(fd);
	if (!f.file)
		goto out;

	i_mode = file_inode(f.file)->i_mode;
	ret = -ESPIPE;
	if (!S_ISREG(i_mode) && !S_ISBLK(i_mode) && !S_ISDIR(i_mode) &&
			!S_ISLNK(i_mode))
		goto out_put;

	mapping = f.file->f_mapping;
	if (!mapping) {
		ret = -EINVAL;
		goto out_put;
	}

	ret = 0;
	if (flags & SYNC_FILE_RANGE_WAIT_BEFORE) {
		ret = filemap_fdatawait_range(mapping, offset, endbyte);
		if (ret < 0)
			goto out_put;
	}

	if (flags & SYNC_FILE_RANGE_WRITE) {
		ret = filemap_fdatawrite_range(mapping, offset, endbyte);
		if (ret < 0)
			goto out_put;
	}

	if (flags & SYNC_FILE_RANGE_WAIT_AFTER)
		ret = filemap_fdatawait_range(mapping, offset, endbyte);

out_put:
	fdput(f);
out:
	return ret;
}

/* It would be nice if people remember that not all the world's an i386
   when they introduce new system calls */
SYSCALL_DEFINE4(sync_file_range2, int, fd, unsigned int, flags,
				 loff_t, offset, loff_t, nbytes)
{
	return sys_sync_file_range(fd, offset, nbytes, flags);
}
