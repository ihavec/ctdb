# Hey Emacs, this is a -*- shell-script -*- !!!  :-)

numnodes=3

export CTDB_NODES_SOCKETS=""
for i in $(seq 1 $numnodes) ; do
    CTDB_NODES_SOCKETS="${CTDB_NODES_SOCKETS}${CTDB_NODES_SOCKETS:+ }${PWD}/sock.${i}"
done


######################################################################

fail ()
{
    echo "$*"
    exit 1
}

######################################################################

#. /root/SOFS/autosofs/scripts/tester.bash

test_begin ()
{
    local name="$1"

    teststarttime=$(date '+%s')
    testduration=0

    echo "--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--"
    echo "Running test $name ($(date '+%T'))"
    echo "--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--==--"
}

test_end ()
{
    local name="$1" ; shift
    local status="$1" ; shift
    # "$@" is command-line

    local interp="SKIPPED"
    local statstr=" (reason $*)"
    if [ -n "$status" ] ; then
	if [ $status -eq 0 ] ; then
	    interp="PASSED"
	    statstr=""
	    echo "ALL OK: $*"
	else
	    interp="FAILED"
	    statstr=" (status $status)"
	    testfailures=$(($testfailures+1))
	fi
    fi

    testduration=$(($(date +%s)-$teststarttime))

    echo "=========================================================================="
    echo "TEST ${interp}: ${name}${statstr}, duration: $testduration sec."
    echo "=========================================================================="

}


test_exit() {
    exit $(($testfailures+0))
}

test_run ()
{
    local name="$1" ; shift
    
    [ -n "$1" ] || set -- "$name"

    test_begin "$name"

    local status=0
    "$@" || status=$?

    test_end "$name" "$status" "$*"
    
    return $status
}

########################################

# Sets: $out
try_command_on_node ()
{
    local nodespec="$1" ; shift
    local cmd="$*"

    out=$(onnode -q "$nodespec" "$cmd" 2>&1) || {

	echo "Failed to execute \"$cmd\" on node(s) \"$nodespec\""
	echo "$out"
	exit 1
    }
}

sanity_check_output ()
{
    local min_lines="$1"
    local regexp="$2" # Should be anchored to match whole lines.
    local output="$3"

    local ret=0

    local num_lines=$(echo "$output" | wc -l)
    echo "There are $num_lines lines of output"
    if [ $num_lines -lt $min_lines ] ; then
	echo "BAD: that's less than the required number (${min_lines})"
	ret=1
    fi

    local status=0
    local unexpected # local doesn't pass through status of command on RHS.
    unexpected=$(echo "$output" | egrep -v "$regexp") || status=$?

    # Note that this is reversed.
    if [ $status -eq 0 ] ; then
	echo "BAD: unexpected lines in output:"
	echo "$unexpected"
	ret=1
    else
	echo "Output lines look OK"
    fi

    return $ret
}

#######################################

# Wait until either timeout expires or command succeeds.  The command
# will be tried once per second.
wait_until ()
{
    local timeout="$1" ; shift # "$@" is the command...

    echo -n "|${timeout}|"
    while [ $timeout -gt 0 ] ; do
	if "$@" ; then
	    echo '|'
	    echo "OK"
	    return 0
	fi
	echo -n .
	timeout=$(($timeout - 1))
	sleep 1
    done
    
    echo "*TIMEOUT*"
    
    return 1
}

sleep_for ()
{
    echo -n "|${1}|"
    for i in $(seq 1 $1) ; do
	echo -n '.'
	sleep 1
    done
    echo '|'
}

_cluster_is_healthy ()
{
    local out x count line

    out=$(ctdb -Y status 2>&1) || return 1

    {
        read x
	count=0
        while read line ; do
	    count=$(($count + 1))
	    [ "${line#:*:*:}" != "0:0:0:0:" ] && return 1
        done
	[ $count -gt 0 ] && return $?
    } <<<"$out" # Yay bash!
}

cluster_is_healthy ()
{
    if _cluster_is_healthy ; then
	echo "Cluster is HEALTHY"
	exit 0
    else
	echo "Cluster is UNHEALTHY"
	exit 1
    fi
}

wait_until_healthy ()
{
    local timeout="${1:-120}"

    echo "Waiting for cluster to become healthy..."

    wait_until 120 _cluster_is_healthy
}

# Incomplete! Do not use!
node_has_status ()
{
    local pnn="$1"
    local status="$2"

    local bits
    case "$status" in
	banned)
	    bits="?:1:?:?"
	    ;;
	unbanned)
	    bits="?:0:?:?"
	    ;;
	disabled)
	    bits="?:?:1:?"
	    ;;
	enabled)
	    bits="?:?:0:?"
	    ;;
	*)
	    echo "node_has_status: unknown status \"$status\""
	    return 1
    esac

    local out x line

    out=$(ctdb -Y status 2>&1) || return 1

    {
        read x
        while read line ; do
	    [ "${line#:${pnn}:*:${bits}:}" = "" ] && return 0
        done
	return 1
    } <<<"$out" # Yay bash!
}

wait_until_node_has_status ()
{
    local pnn="$1"
    local status="$2"
    local timeout="${3:-30}"

    echo "Waiting until node $pnn has status \"$status\"..."

    wait_until $timeout node_has_status "$pnn" "$status"
}

# Useful for superficially testing IP failover.
# IPs must be on nodes matching nodeglob.
ips_are_on_nodeglob ()
{
    local nodeglob="$1" ; shift
    local ips="$*"

    local out

    try_command_on_node 1 ctdb ip

    while read ip pnn ; do
	for check in $ips ; do
	    if [ "$check" = "$ip" ] ; then
		case "$pnn" in
		    ($nodeglob) : ;;
		    (*) return 1  ;;
		esac
		ips="${ips/${ip}}" # Remove from list
	    fi
	done
    done <<<"$out" # bashism to avoid problem setting variable in pipeline.

    ips="${ips// }" # Remove any spaces.
    [ -z "$ips" ]
}

wait_until_ips_are_on_nodeglob ()
{
    echo "Waiting for IPs to fail over..."

    wait_until 60 ips_are_on_nodeglob "$@"
}


start_daemons ()
{
    $CTDB_DIR/tests/start_daemons.sh $numnodes >$CTDB_DIR/var/daemons.log
}

_restart_ctdb ()
{
    if [ -e /etc/redhat-release ] ; then
	service ctdb restart
    else
	/etc/init.d/ctdb restart
    fi
}

restart_ctdb ()
{
    if [ -n "$CTDB_NODES_SOCKETS" ] ; then
	onnode all ctdb shutdown
	start_daemons
    else
	onnode -pq all $TEST_WRAP _restart_ctdb 
    fi || return 1
	
    onnode -q 1  $TEST_WRAP wait_until_healthy || return 1

    echo "Setting RerecoveryTimeout to 1"
    onnode -pq all "ctdb setvar RerecoveryTimeout 1"

    #echo "Sleeping to allow ctdb to settle..."
    #sleep_for 10

    echo "ctdb is ready"
}

ctdb_test_exit ()
{
    if ! onnode 0 $TEST_WRAP cluster_is_healthy ; then
	echo "Restarting ctdb on all nodes to get back into known state..."
	restart_ctdb
    fi

    test_exit
}

########################################

export PATH=/usr/local/autocluster:$PATH
