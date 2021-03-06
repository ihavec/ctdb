#!/bin/bash

test_info()
{
    cat <<EOF
Verify that NFS connections are monitored and that NFS tickles are sent.

We create a connection to the NFS server on a node and confirm that
this connection is registered in the nfs-tickles/ subdirectory in
shared storage.  Then disable the relevant NFS server node and ensure
that it send an appropriate reset packet.

Prerequisites:

* An active CTDB cluster with at least 2 nodes with public addresses.

* Test must be run on a real or virtual cluster rather than against
  local daemons.

* Test must not be run from a cluster node.

* Cluster nodes must be listening on the NFS TCP port (2049).

Steps:

1. Verify that the cluster is healthy.
2. Connect from the current host (test client) to TCP port 2049 using
   the public address of a cluster node.
3. Determine the source socket used for the connection.
4. Ensure that CTDB records the source socket details in the nfs-tickles
   directory on shared storage.
5. Disable the node that the connection has been made to.
6. Verify that a TCP tickle (a reset packet) is sent to the test client.

Expected results:

* CTDB should correctly record the socket in the nfs-tickles directory
  and should send a reset packet when the node is disabled.
EOF
}

. "${TEST_SCRIPTS_DIR}/integration.bash"

set -e

ctdb_test_init "$@"

ctdb_test_check_real_cluster

cluster_is_healthy

# Reset configuration
ctdb_restart_when_done

ctdb_test_exit_hook_add ctdb_test_eventscript_uninstall

ctdb_test_eventscript_install

# We need this for later, so we know how long to sleep.
try_command_on_node any $CTDB getvar MonitorInterval
monitor_interval="${out#*= }"
#echo "Monitor interval on node $test_node is $monitor_interval seconds."

select_test_node_and_ips

test_port=2049

echo "Connecting to node ${test_node} on IP ${test_ip}:${test_port} with netcat..."

nc -d -w $(($monitor_interval * 4)) $test_ip $test_port &
nc_pid=$!
ctdb_test_exit_hook_add "kill $nc_pid >/dev/null 2>&1"

wait_until_get_src_socket "tcp" "${test_ip}:${test_port}" $nc_pid "nc"
src_socket="$out"
echo "Source socket is $src_socket"

wait_for_monitor_event $test_node

echo "Sleeping until tickles are synchronised across nodes..."
try_command_on_node $test_node $CTDB getvar TickleUpdateInterval
sleep_for "${out#*= }"

if try_command_on_node any "test -r /etc/ctdb/events.d/61.nfstickle" ; then
    echo "Trying to determine NFS_TICKLE_SHARED_DIRECTORY..."
    if [ -f /etc/sysconfig/nfs ]; then
	f="/etc/sysconfig/nfs"
    elif [ -f /etc/default/nfs ]; then
	f="/etc/default/nfs"
    elif [ -f /etc/ctdb/sysconfig/nfs ]; then
	f="/etc/ctdb/sysconfig/nfs"
    fi
    try_command_on_node -v any "[ -r $f ] &&  sed -n -e s@^NFS_TICKLE_SHARED_DIRECTORY=@@p $f" || true

    nfs_tickle_shared_directory="${out:-/gpfs/.ctdb/nfs-tickles}"

    try_command_on_node $test_node hostname
    test_hostname=$out

    try_command_on_node -v any cat "${nfs_tickle_shared_directory}/$test_hostname/$test_ip"
else
    echo "That's OK, we'll use \"ctdb gettickles\", which is newer..."
    try_command_on_node -v any "ctdb -Y gettickles $test_ip $test_port"
fi

if [ "${out/${src_socket}/}" != "$out" ] ; then
    echo "GOOD: NFS connection tracked OK."
else
    echo "BAD: Socket not tracked in NFS tickles."
    testfailures=1
fi

tcptickle_sniff_start $src_socket "${test_ip}:${test_port}"

# We need to be nasty to make that the node being failed out doesn't
# get a chance to send any tickles and confuse our sniff.
echo "Killing ctdbd on ${test_node}..."
try_command_on_node $test_node killall -9 ctdbd

wait_until_node_has_status $test_node disconnected

tcptickle_sniff_wait_show
