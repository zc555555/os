#!/bin/bash
#
# Test script for Task 1: Entangled CPU mutual exclusion
# Must be run as root on a system with the modified kernel
#

CPU1=1
CPU2=3
DURATION=15

echo "============================================"
echo "  Entangled CPU Mutual Exclusion Test"
echo "============================================"
echo ""

# Check root
if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root"
    exit 1
fi

# Check if procfs files exist
if [ ! -f /proc/sys/kernel/entangled_cpus_1 ]; then
    echo "Error: /proc/sys/kernel/entangled_cpus_1 not found"
    echo "Make sure you're running the modified kernel!"
    exit 1
fi

# Function to get CPU time for a process
get_cpu_time() {
    local pid=$1
    if [ -f /proc/$pid/stat ]; then
        cat /proc/$pid/stat | awk '{print $14+$15}'
    else
        echo 0
    fi
}

# Function to monitor which CPU a process is running on
monitor_cpu() {
    local pid=$1
    local name=$2
    if [ -f /proc/$pid/stat ]; then
        local cpu=$(cat /proc/$pid/stat | awk '{print $39}')
        echo "$name (PID $pid): CPU $cpu"
    fi
}

echo "Step 1: Check procfs interface"
echo "--------------------------------"
echo "entangled_cpus_1: $(cat /proc/sys/kernel/entangled_cpus_1)"
echo "entangled_cpus_2: $(cat /proc/sys/kernel/entangled_cpus_2)"
echo ""

echo "Step 2: Set entangled CPUs ($CPU1 <-> $CPU2)"
echo "----------------------------------------------"
echo $CPU1 > /proc/sys/kernel/entangled_cpus_1
echo $CPU2 > /proc/sys/kernel/entangled_cpus_2
echo "entangled_cpus_1: $(cat /proc/sys/kernel/entangled_cpus_1)"
echo "entangled_cpus_2: $(cat /proc/sys/kernel/entangled_cpus_2)"
echo ""

echo "Step 3: Test with SAME user (should work normally)"
echo "----------------------------------------------------"

# Start two CPU-intensive tasks as root on both CPUs
taskset -c $CPU1 yes > /dev/null &
PID1=$!
taskset -c $CPU2 yes > /dev/null &
PID2=$!

echo "Started processes: PID1=$PID1 (CPU $CPU1), PID2=$PID2 (CPU $CPU2)"
echo "Both are running as UID $(id -u)"
sleep 3

echo ""
echo "Monitoring for 5 seconds..."
for i in {1..5}; do
    monitor_cpu $PID1 "Process1"
    monitor_cpu $PID2 "Process2"
    echo "---"
    sleep 1
done

kill $PID1 $PID2 2>/dev/null
wait 2>/dev/null
echo ""
echo "Same user test complete. Both should have run on their assigned CPUs."
echo ""

echo "Step 4: Test with DIFFERENT users"
echo "-----------------------------------"
echo "This test requires two different user accounts."
echo ""

# Check if testuser1 exists, if not try to use nobody
USER1="root"
USER2="nobody"

# Start process as root on CPU1
taskset -c $CPU1 yes > /dev/null &
PID1=$!
echo "Process 1 (User: $USER1, UID $(id -u $USER1)) started on CPU $CPU1, PID=$PID1"

sleep 1

# Start process as nobody on CPU2
sudo -u $USER2 taskset -c $CPU2 yes > /dev/null &
PID2=$!
echo "Process 2 (User: $USER2, UID $(id -u $USER2)) started on CPU $CPU2, PID=$PID2"

echo ""
echo "Monitoring for $DURATION seconds..."
echo "If mutual exclusion works, Process 2 should NOT run on CPU $CPU2"
echo "when Process 1 is running on CPU $CPU1 (different UIDs)"
echo ""

for i in $(seq 1 $DURATION); do
    echo "=== Second $i ==="
    monitor_cpu $PID1 "Process1 ($USER1)"
    monitor_cpu $PID2 "Process2 ($USER2)"

    # Also check CPU idle time
    echo ""
    sleep 1
done

kill $PID1 $PID2 2>/dev/null
wait 2>/dev/null

echo ""
echo "Step 5: Reset entangled CPUs"
echo "-----------------------------"
echo 0 > /proc/sys/kernel/entangled_cpus_1
echo 0 > /proc/sys/kernel/entangled_cpus_2
echo "entangled_cpus_1: $(cat /proc/sys/kernel/entangled_cpus_1)"
echo "entangled_cpus_2: $(cat /proc/sys/kernel/entangled_cpus_2)"
echo ""

echo "============================================"
echo "  Test Complete"
echo "============================================"
echo ""
echo "Interpretation:"
echo "- If mutual exclusion works correctly:"
echo "  * Same user: Both processes run on their assigned CPUs"
echo "  * Different users: One of the entangled CPUs should be idle"
echo "    (the task is prevented from running)"
