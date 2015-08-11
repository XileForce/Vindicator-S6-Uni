#!/system/bin/sh

# Tweak VM and Swap
echo "120"	> /proc/sys/vm/swappiness
echo "100"	> /sys/fs/cgroup/memory/sw/memory.swappiness
echo "50"	> /proc/sys/vm/vfs_cache_pressure
echo "500"	> /proc/sys/vm/dirty_writeback_centisecs
echo "500"	> /proc/sys/vm/dirty_expire_centisecs
echo "25"	> /sys/module/zswap/parameters/max_pool_percent

#  Start SuperSU daemon
#  Wait for 5 seconds from boot before starting the SuperSU daemon
sleep 5
/system/xbin/daemonsu --auto-daemon &

#  Set interactive governor tuning
#  Wait for 10 seconds from boot so we can ovverride TouchWiz's overrides
sleep 5

#set apollo interactive governor
echo "15000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/above_hispeed_delay
echo "30000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/boostpulse_duration
echo "95" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/go_hispeed_load
echo "1" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/io_is_busy
echo "25000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/min_sample_time
echo "20000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/multi_enter_time
echo "30000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/multi_exit_time
echo "20000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/single_enter_time
echo "30000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/single_exit_time
echo "85" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/target_loads
echo "20000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/timer_rate
echo "15000" 	> /sys/devices/system/cpu/cpu0/cpufreq/interactive/timer_slack

#set atlas interactive governor
echo "10000 1600000:3000 1704000:3000 1800000:3000 1896000:3000 2000000:3000 2100000:3000" > /sys/devices/system/cpu/cpu4/cpufreq/interactive/above_hispeed_delay
echo "30000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/boostpulse_duration
echo "95" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/go_hispeed_load
echo "1" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/io_is_busy
echo "25000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/min_sample_time
echo "20000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/multi_enter_time
echo "30000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/multi_exit_time
echo "20000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/single_enter_time
echo "30000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/single_exit_time
echo "90" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/target_loads
echo "20000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/timer_rate
echo "15000" 	> /sys/devices/system/cpu/cpu4/cpufreq/interactive/timer_slack

#Set Power Aware Scheduling Off By Default
echo "0" > /sys/kernel/hmp/power_migration

