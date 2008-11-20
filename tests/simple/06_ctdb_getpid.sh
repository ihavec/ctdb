#!/bin/bash

. ctdb_test_functions.bash

set -e

onnode 0 $TEST_WRAP cluster_is_healthy

# This is an attempt at being independent of the number of nodes
# reported by "ctdb getpid -n all".
try_command_on_node 0 "ctdb listnodes | wc -l"

num_nodes="$out"

echo "There are $num_nodes nodes..."

# Call getpid a few different ways and make sure the answer is always the same.

cmd="onnode -q all ctdb getpid"
try_command_on_node 1 "$cmd"
pids_onnode="$out"
echo "Results from \"$cmd\":"
echo "$pids_onnode"

cmd="onnode -q 1 ctdb getpid -n all"
try_command_on_node 1 "$cmd"
pids_getpid_all="$out"
echo "Results from \"$cmd\":"
echo "$pids_getpid_all"

cmd=""
n=0
while [ $n -lt $num_nodes ] ; do
    cmd="${cmd}${cmd:+; }ctdb getpid -n $n"
    n=$(($n + 1))
done
try_command_on_node 1 "$cmd"
pids_getpid_n="$out"
echo "Results from \"$cmd\":"
echo "$pids_getpid_n"

if [ "$pids_onnode" = "$pids_getpid_all" -a \
    "$pids_getpid_all" = "$pids_getpid_n" ] ; then
    echo "They're the same... cool!"
else
    echo "Error: they differ."
    testfailures=1
fi

echo "Checking each PID for validity"

n=0
while [ $n -lt $num_nodes ] ; do
    read line
    pid=${line#Pid:}
    try_command_on_node $n "ls -l /proc/${pid}/exe | sed -e 's@.*/@@'"
    echo -n "Node ${n}, PID ${pid} looks to be running \"$out\" - "
    if [ "$out" = "ctdbd" ] ; then
	echo "GOOD!"
    else
	echo "BAD!"
	testfailures=1
    fi
    n=$(($n + 1))
done <<<"$pids_onnode"

ctdb_test_exit
