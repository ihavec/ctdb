#!/bin/bash

. ctdb_test_functions.bash

set -e

onnode 0 $TEST_WRAP cluster_is_healthy

cmd='ctdb isnotrecmaster || true'
try_command_on_node all "$cmd"
echo "Output of \"$cmd\":"
echo "$out"

num_all_lines=$(echo "$out" |  wc -l)
num_rm_lines=$(echo "$out" | fgrep -c 'this node is the recmaster') || true
num_not_rm_lines=$(echo "$out" | fgrep -c 'this node is not the recmaster') || true

if [ $num_rm_lines -eq 1 ] ; then
    echo "OK, there is only 1 recmaster"
else
    echo "BAD, there are ${num_rm_lines} nodes claiming to be the recmaster"
    testfailures=1
fi

if [ $(($num_all_lines - $num_not_rm_lines)) -eq 1 ] ; then
    echo "OK, all the other nodes claim not to be the recmaster"
else
    echo "BAD, there are only ${num_not_rm_lines} nodes claiming not to be the recmaster"
    testfailures=1
fi

ctdb_test_exit
