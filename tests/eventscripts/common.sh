# Hey Emacs, this is a -*- shell-script -*- !!!  :-)

# Print a message and exit.
die () { echo "$@" >&2 ; exit 1 ; }

# Augment PATH with relevant stubs/ directories.  We do this by actually
# setting PATH, and also by setting $EVENTSCRIPTS_PATH and then
# prepending that to $PATH in rc.local to avoid the PATH reset in
# functions.

EVENTSCRIPTS_PATH=""

if [ -d "${EVENTSCRIPTS_TESTS_DIR}/stubs" ] ; then
    EVENTSCRIPTS_PATH="${EVENTSCRIPTS_TESTS_DIR}/stubs"
fi

export EVENTSCRIPTS_TESTCASE_DIR=$(dirname "$0")
if [ $(basename "$EVENTSCRIPTS_TESTCASE_DIR") = "eventscripts" ] ; then
    # Just a test script, no testcase subdirectory.
    EVENTSCRIPTS_TESTCASE_DIR="$EVENTSCRIPTS_TESTS_DIR"
else
    if [ -d "${EVENTSCRIPTS_TESTCASE_DIR}/stubs" ] ; then
	EVENTSCRIPTS_PATH="${EVENTSCRIPTS_TESTCASE_DIR}/stubs:${EVENTSCRIPTS_PATH}"
    fi
fi

export EVENTSCRIPTS_PATH

PATH="${EVENTSCRIPTS_PATH}:${PATH}"

if [ -d "${EVENTSCRIPTS_TESTCASE_DIR}/etc" ] ; then
    CTDB_ETCDIR="${EVENTSCRIPTS_TESTCASE_DIR}/etc"
elif [ -d "${EVENTSCRIPTS_TESTS_DIR}/etc" ] ; then
    CTDB_ETCDIR="${EVENTSCRIPTS_TESTS_DIR}/etc"
else
    die "Unable to set \$CTDB_ETCDIR"
fi
export CTDB_ETCDIR

if [ -d "${EVENTSCRIPTS_TESTCASE_DIR}/etc-ctdb" ] ; then
    CTDB_BASE="${EVENTSCRIPTS_TESTCASE_DIR}/etc-ctdb"
elif [ -d "${EVENTSCRIPTS_TESTCASE_DIR}/etc/ctdb" ] ; then
    CTDB_BASE="${EVENTSCRIPTS_TESTCASE_DIR}/etc/ctdb"
elif [ -d "${EVENTSCRIPTS_TESTS_DIR}/etc-ctdb" ] ; then
    CTDB_BASE="${EVENTSCRIPTS_TESTS_DIR}/etc-ctdb"
else
    die "Unable to set \$CTDB_BASE"
fi
export CTDB_BASE

export EVENTSCRIPTS_TESTS_VAR_DIR="${EVENTSCRIPTS_TESTS_DIR}/var"
if [ "$EVENTSCRIPTS_TESTS_VAR_DIR" != "/var" ] ; then
    rm -r "$EVENTSCRIPTS_TESTS_VAR_DIR"
fi
mkdir -p "$EVENTSCRIPTS_TESTS_VAR_DIR"
export CTDB_VARDIR="$EVENTSCRIPTS_TESTS_VAR_DIR/ctdb"

######################################################################

if [ "$EVENTSCRIPT_TESTS_VERBOSE" = "yes" ] ; then
    debug () { echo "$@" ; }
else
    debug () { : ; }
fi

eventscripts_tests_cleanup_hooks=""

# This loses quoting!
eventscripts_test_add_cleanup ()
{
    eventscripts_tests_cleanup_hooks="${eventscripts_tests_cleanup_hooks}${eventscripts_tests_cleanup_hooks:+ ; }$*"
}

trap 'eval $eventscripts_tests_cleanup_hooks' 0


######################################################################

# General setup fakery

setup_generic ()
{
    debug "Setting up shares (3 existing shares)"
    # Create 3 fake shares/exports.
    export FAKE_SHARES=""
    for i in $(seq 1 3) ; do
	_s="${EVENTSCRIPTS_TESTS_VAR_DIR}/shares/${i}_existing"
	mkdir -p "$_s"
	FAKE_SHARES="${FAKE_SHARES}${FAKE_SHARES:+ }${_s}"
    done

    export FAKE_PROC_NET_BONDING="$EVENTSCRIPTS_TESTS_VAR_DIR/proc-net-bonding"
    mkdir -p "$FAKE_PROC_NET_BONDING"
    rm -f "$FAKE_PROC_NET_BONDING"/*

    export FAKE_ETHTOOL_LINK_DOWN="$EVENTSCRIPTS_TESTS_VAR_DIR/ethtool-link-down"
    mkdir -p "$FAKE_ETHTOOL_LINK_DOWN"
    rm -f "$FAKE_ETHTOOL_LINK_DOWN"/*

    # This can only have 2 levels.  We don't want to resort to usings
    # something dangerous like "rm -r" setup time.
    export FAKE_IP_STATE="$EVENTSCRIPTS_TESTS_VAR_DIR/fake-ip-state"
    mkdir -p "$FAKE_IP_STATE"
    rm -f "$FAKE_IP_STATE"/*/*
    rm -f "$FAKE_IP_STATE"/* 2>/dev/null || true
    rmdir "$FAKE_IP_STATE"/* 2>/dev/null || true
}

tcp_port_down ()
{
    for _i ; do
	debug "Marking TCP port \"${_i}\" as not listening"
	FAKE_TCP_LISTEN=$(echo "$FAKE_TCP_LISTEN" | sed -r -e "s@[[:space:]]*[\.0-9]+:${_i}@@g")
    done
}

shares_missing ()
{
    _fmt="$1" ; shift

    # Replace some shares with non-existent ones.
    _t=""
    _n=1
    _nl="
"
    export MISSING_SHARES_TEXT=""
    for _i in $FAKE_SHARES ; do
	if [ $_n = "$1" ] ; then
	    shift
	    _i="${_i%_existing}_missing"
	    debug "Replacing share $_n with missing share \"$_i\""
	    rmdir "$_i" 2>/dev/null || true
	    MISSING_SHARES_TEXT="${MISSING_SHARES_TEXT}${MISSING_SHARES_TEXT:+${_nl}}"$(printf "$_fmt" "${_i}")
	fi
	_t="${_t}${_t:+ }${_i}"
	_n=$(($_n + 1))
    done
    FAKE_SHARES="$_t"
}

# Setup some fake /proc/net/bonding files with just enough info for
# the eventscripts.

# arg1 is interface name, arg2 is currently active slave (use "None"
# if none), arg3 is MII status ("up" or "down").
setup_bond ()
{
    _iface="$1"
    _slave="${2:-${_iface}_sl_0}"
    _mii_s="${3:-up}"
    _mii_subs="${4:-${_mii_s:-up}}"
    echo "Setting $_iface to be a bond with active slave $_slave and MII status $_mii_s"
    cat >"${FAKE_PROC_NET_BONDING}/$_iface" <<EOF
Bonding Mode: IEEE 802.3ad Dynamic link aggregation
Currently Active Slave: $_slave
# Status of the bond
MII Status: $_mii_s
# Status of 1st pretend adapter
MII Status: $_mii_subs
# Status of 2nd pretend adapter
MII Status: $_mii_subs
EOF
}

ethtool_interfaces_down ()
{
    for _i ; do
	echo "Marking interface $_i DOWN for ethtool"
	touch "${FAKE_ETHTOOL_LINK_DOWN}/${_i}"
    done
}

ethtool_interfaces_up ()
{
    for _i ; do
	echo "Marking interface $_i UP for ethtool"
	rm -f "${FAKE_ETHTOOL_LINK_DOWN}/${_i}"
    done
}

setup_nmap_output_filter ()
{
    OUT_FILTER="-e 's@^(DEBUG: # Nmap 5.21 scan initiated) .+ (as:)@\1 DATE \2@' -e 's@^(DEBUG: # Nmap done at) .+ (--)@\1 DATE \2@'"
}

dump_routes ()
{
    echo "# ip rule show"
    ip rule show

    ip rule show |
    while read _p _x _i _x _t ; do
	# Remove trailing colon after priority/preference.
	_p="${_p%:}"
	# Only remove rules that match our priority/preference.
	[ "$CTDB_PER_IP_ROUTING_RULE_PREF" = "$_p" ] || continue

	echo "# ip route show table $_t"
	ip route show table "$_t"
    done
}

# Copied from 13.per_ip_routing for now... so this is lazy testing  :-(
ipv4_host_addr_to_net ()
{
    _host="$1"
    _maskbits="$2"

    # Convert the host address to an unsigned long by splitting out
    # the octets and doing the math.
    _host_ul=0
    for _o in $(export IFS="." ; echo $_host) ; do
	_host_ul=$(( ($_host_ul << 8) + $_o)) # work around Emacs color bug
    done

    # Calculate the mask and apply it.
    _mask_ul=$(( 0xffffffff << (32 - $_maskbits) ))
    _net_ul=$(( $_host_ul & $_mask_ul ))

    # Now convert to a network address one byte at a time.
    _net=""
    for _o in $(seq 1 4) ; do
	_net="$(($_net_ul & 255))${_net:+.}${_net}"
	_net_ul=$(($_net_ul >> 8))
    done

    echo "${_net}/${_maskbits}"
}

######################################################################

# CTDB fakery

# Evaluate an expression that probably calls functions or uses
# variables from the CTDB functions file.  This is used for test
# initialisation.
eventscript_call ()
{
    (
	. "$CTDB_BASE/functions"
	"$@"
    )
}

# Set output for ctdb command.  Option 1st argument is return code.
ctdb_set_output ()
{
    _out="$EVENTSCRIPTS_TESTS_VAR_DIR/ctdb.out"
    cat >"$_out"

    _rc="$EVENTSCRIPTS_TESTS_VAR_DIR/ctdb.rc"
    echo "${1:-0}" >"$_rc"

    eventscripts_test_add_cleanup "rm -f $_out $_rc"
}

setup_ctdb ()
{
    setup_generic

    export FAKE_CTDB_NUMNODES="${1:-3}"
    echo "Setting up CTDB with ${FAKE_CTDB_NUMNODES} fake nodes"

    export FAKE_CTDB_PNN="${2:-0}"
    echo "Setting up CTDB with PNN ${FAKE_CTDB_PNN}"

    if [ -n "$3" ] ; then
	echo "Setting up CTDB_PUBLIC_ADDRESSES: $3"
	export CTDB_PUBLIC_ADDRESSES=$(mktemp)
	for _i in $3 ; do
	    _ip="${_i%@*}"
	    _ifaces="${_i#*@}"
	    echo "${_ip} ${_ifaces}" >>"$CTDB_PUBLIC_ADDRESSES"
	done
	eventscripts_test_add_cleanup "rm -f $CTDB_PUBLIC_ADDRESSES"
    fi

    export FAKE_CTDB_IFACES_DOWN="$EVENTSCRIPTS_TESTS_VAR_DIR/fake-ctdb/ifaces-down"
    mkdir -p "$FAKE_CTDB_IFACES_DOWN"
    rm -f "$FAKE_CTDB_IFACES_DOWN"/*

    export FAKE_CTDB_NODES_DOWN="$EVENTSCRIPTS_TESTS_VAR_DIR/nodes-down"
    mkdir -p "$FAKE_CTDB_NODES_DOWN"
    rm -f "$FAKE_CTDB_NODES_DOWN"/*

    export FAKE_CTDB_SCRIPTSTATUS="$EVENTSCRIPTS_TESTS_VAR_DIR/scriptstatus"
    mkdir -p "$FAKE_CTDB_SCRIPTSTATUS"
    rm -f "$FAKE_CTDB_SCRIPTSTATUS"/*
}



ctdb_nodes_up ()
{
    for _i ; do
	rm -f "${FAKE_CTDB_NODES_DOWN}/${_i}"
    done
}

ctdb_nodes_down ()
{
    for _i ; do
	touch "${FAKE_CTDB_NODES_DOWN}/${_i}"
    done
}

ctdb_get_interfaces ()
{
    # The echo/subshell forces all the output onto 1 line.
    echo $(ctdb ifaces -Y | awk -F: 'FNR > 1 {print $2}')
}

ctdb_get_1_interface ()
{
    _t=$(ctdb_get_interfaces)
    echo ${_t%% *}
}

ctdb_get_public_addresses ()
{
    _f="${CTDB_PUBLIC_ADDRESSES:-${CTDB_BASE}/public_addresses}"
    echo $(if [ -r "$_f" ] ; then 
	while read _ip _iface ; do
	    echo "${_ip}@${_iface}"
	done <"$_f"
	fi)
}

# Prints the 1st public address as: interface IP maskbits
# This is suitable for passing to 10.interfaces takeip/releaseip
ctdb_get_1_public_address ()
{
    _addrs=$(ctdb_get_public_addresses)
    echo "${_addrs%% *}" | sed -r -e 's#(.*)/(.*)@(.*)#\3 \1 \2#g'
}

ctdb_not_implemented ()
{
    export CTDB_NOT_IMPLEMENTED="$1"
    ctdb_not_implemented="\
DEBUG: ctdb: command \"$1\" not implemented in stub"
}

ctdb_fake_scriptstatus ()
{
    _code="$1"
    _status="$2"
    _err_out="$3"

    _d1=$(date '+%s.%N')
    _d2=$(date '+%s.%N')

    echo "$_code $_status $_err_out" >"$FAKE_CTDB_SCRIPTSTATUS/$script"
}

setup_ctdb_policy_routing ()
{
    export CTDB_PER_IP_ROUTING_CONF="$CTDB_BASE/policy_routing"
    export CTDB_PER_IP_ROUTING_RULE_PREF=100
    export CTDB_PER_IP_ROUTING_TABLE_ID_LOW=1000
    export CTDB_PER_IP_ROUTING_TABLE_ID_HIGH=2000

    # Tests need to create and populate this file
    rm -f "$CTDB_PER_IP_ROUTING_CONF"
}

######################################################################

# Samba fakery

setup_samba ()
{
    setup_ctdb

    if [ "$1" != "down" ] ; then

	debug "Marking Samba services as up, listening and managed by CTDB"
        # Get into known state.
	for i in "samba" "winbind" ; do
	    eventscript_call ctdb_service_managed "$i"
	done
        # All possible service names for all known distros.
	for i in "smb" "nmb" "winbind" "samba" ; do
	    service "$i" force-started
	done

	export CTDB_SAMBA_SKIP_SHARE_CHECK="no"
	export CTDB_MANAGED_SERVICES="foo samba winbind bar"

	export FAKE_TCP_LISTEN="0.0.0.0:445 0.0.0.0:139"
	export FAKE_WBINFO_FAIL="no"

	# Some things in 50.samba are backgrounded and waited for.  If
	# we don't sleep at all then timeouts can happen.  This avoids
	# that...  :-)
	export FAKE_SLEEP_FORCE=0.1
    else
	debug "Marking Samba services as down, not listening and not managed by CTDB"
        # Get into known state.
	for i in "samba" "winbind" ; do
	    eventscript_call ctdb_service_unmanaged "$i"
	done
        # All possible service names for all known distros.
	for i in "smb" "nmb" "winbind" "samba" ; do
	    service "$i" force-stopped
	done

	export CTDB_SAMBA_SKIP_SHARE_CHECK="no"
	export CTDB_MANAGED_SERVICES="foo bar"
	unset CTDB_MANAGES_SAMBA
	unset CTDB_MANAGES_WINBIND

	export FAKE_TCP_LISTEN=""
	export FAKE_WBINFO_FAIL="yes"
    fi

    # This is ugly but if this file isn't removed before each test
    # then configuration changes between tests don't stick.
    rm -f "$CTDB_VARDIR/state/samba/smb.conf.cache"
}

wbinfo_down ()
{
    debug "Making wbinfo commands fail"
    FAKE_WBINFO_FAIL="yes"
}

######################################################################

# NFS fakery

setup_nfs ()
{
    setup_ctdb

    export FAKE_RPCINFO_SERVICES=""

    export CTDB_NFS_SKIP_SHARE_CHECK="no"
    export CTDB_NFS_SKIP_KNFSD_ALIVE_CHECK="no"

    # Reset the failcounts for nfs services.
    eventscript_call eval rm -f '$ctdb_fail_dir/nfs_*'

    rpc_fail_limits_file="${EVENTSCRIPTS_TESTS_VAR_DIR}/rpc_fail_limits"

    # Force this file to exist so tests can be individually run.
    if [ ! -f "$rpc_fail_limits_file" ] ; then
	# This is gross... but is needed to fake through the nfs monitor event.
	eventscript_call ctdb_service_managed "nfs"
	service "nfs" force-started  # might not be enough
	CTDB_RC_LOCAL="$CTDB_BASE/rc.local.nfs.monitor.get-limits" \
	    CTDB_MANAGES_NFS="yes" \
	    "${CTDB_BASE}/events.d/60.nfs" "monitor" >"$rpc_fail_limits_file"
    fi
    
    if [ "$1" != "down" ] ; then
	debug "Setting up NFS environment: all RPC services up, NFS managed by CTDB"

	eventscript_call ctdb_service_managed "nfs"
	service "nfs" force-started  # might not be enough

	export CTDB_MANAGED_SERVICES="foo nfs bar"

	rpc_services_up "nfs" "mountd" "rquotad" "nlockmgr" "status"
    else
	debug "Setting up NFS environment: all RPC services down, NFS not managed by CTDB"

	eventscript_call ctdb_service_unmanaged "nfs"
	service "nfs" force-stopped  # might not be enough
	eventscript_call startstop_nfs stop

	export CTDB_MANAGED_SERVICES="foo bar"
	unset CTDB_MANAGES_NFS
    fi
}

rpc_services_down ()
{
    for _i ; do
	debug "Marking RPC service \"${_i}\" as unavailable"
	FAKE_RPCINFO_SERVICES=$(echo "$FAKE_RPCINFO_SERVICES" | sed -r -e "s@[[:space:]]*${_i}:[0-9]+:[0-9]+@@g")
    done
}

rpc_services_up ()
{
    for _i ; do
	debug "Marking RPC service \"${_i}\" as available"
	case "$_i" in
	    nfs)      _t="2:3" ;;
	    mountd)   _t="1:3" ;;
	    rquotad)  _t="1:2" ;;
	    nlockmgr) _t="3:4" ;;
	    status)   _t="1:1" ;;
	    *) die "Internal error - unsupported RPC service \"${_i}\"" ;;
	esac

	FAKE_RPCINFO_SERVICES="${FAKE_RPCINFO_SERVICES}${FAKE_RPCINFO_SERVICES:+ }${_i}:${_t}"
    done
}

# Set the required result for a particular RPC program having failed
# for a certain number of iterations.  This is probably still a work
# in progress.  Note that we could hook aggressively
# nfs_check_rpc_service() to try to implement this but we're better
# off testing nfs_check_rpc_service() using independent code...  even
# if it is incomplete and hacky.  So, if the 60.nfs eventscript
# changes and the tests start to fail then it may be due to this
# function being incomplete.
rpc_set_service_failure_response ()
{
    _progname="$1"
    # The number of failures defaults to the iteration number.  This
    # will be true when we fail from the 1st iteration... but we need
    # the flexibility to set the number of failures.
    _numfails="${2:-${iteration}}"

    _etc="$CTDB_ETCDIR" # shortcut for readability
    for _c in "$_etc/sysconfig/nfs" "$_etc/default/nfs" "$_etc/ctdb/sysconfig/nfs" ; do
	if [ -r "$_c" ] ; then
	    . "$_c"
	    break
	fi
    done

    # A handy newline.  :-)
    _nl="
"

    # Default
    ok_null

    _ts=$(sed -n -e "s@^${_progname} @@p" "$rpc_fail_limits_file")

    while [ -n "$_ts" ] ; do
	# Get the triple: operator, fail limit and actions.
	_op="${_ts%% *}" ; _ts="${_ts#* }"
	_li="${_ts%% *}" ; _ts="${_ts#* }"
	# We've lost some of the quoting but we can simulate
	# because we know an operator is always the first in a
	# triple.
  	_actions=""
	while [ -n "$_ts" ] ; do
	    # If this is an operator then we've got all of the
	    # actions.
	    case "$_ts" in
		-*) break ;;
	    esac

	    _actions="${_actions}${_actions:+ }${_ts%% *}"
	    # Special case for end of list.
	    if [ "$_ts" != "${_ts#* }" ] ; then
		_ts="${_ts#* }"
	    else
		_ts=""
	    fi
	done

	if [ "$_numfails" "$_op" "$_li" ] ; then
	    _out=""
	    _rc=0
	    for _action in $_actions ; do
		case "$_action" in
		    verbose)
			_ver=1
			_pn="$_progname"
			case "$_progname" in
			    knfsd) _ver=3 ; _pn="nfs" ;;
			    lockd) _ver=4 ; _pn="nlockmgr" ;;
			    statd) _pn="status" ;;
			esac
			_out="\
ERROR: $_pn failed RPC check:
rpcinfo: RPC: Program not registered
program $_pn version $_ver is not available"
			;;
		    restart|restart:*)
			_opts=""
			_p="rpc.${_progname}"
			case "$_progname" in
			    statd)
				_opts="${STATD_HOSTNAME:+ -n }${STATD_HOSTNAME}"
				;;
			esac
			case "${_progname}${_action#restart}" in
			    knfsd)
				_t="\
Trying to restart NFS service
Starting nfslock: OK
Starting nfs: OK"
				;;
			    knfsd:bs)
				_t="Trying to restart NFS service"
				;;
			    lockd)
				_t="\
Trying to restart lock manager service
Stopping nfslock: OK
Starting nfslock: OK"
				;;
			    lockd:*)
				_t="Trying to restart lock manager service"
				;;
			    *)
				_t="Trying to restart $_progname [${_p}${_opts}]"
			esac
			_out="${_out}${_out:+${_nl}}${_t}"
			;;
		    unhealthy)
			_rc=1
		esac
	    done
	    required_result $_rc "$_out"
	    return
	fi
    done
}

######################################################################

# VSFTPD fakery

setup_vsftpd ()
{
    if [ "$1" != "down" ] ; then
	die "setup_vsftpd up not implemented!!!"
    else
	debug "Setting up VSFTPD environment: service down, not managed by CTDB"

	eventscript_call ctdb_service_unmanaged vsftpd
	service vsftpd force-stopped

	export CTDB_MANAGED_SERVICES="foo"
	unset CTDB_MANAGES_VSFTPD
    fi
}

######################################################################

# HTTPD fakery

setup_httpd ()
{
    if [ "$1" != "down" ] ; then
	die "setup_httpd up not implemented!!!"
    else
	debug "Setting up HTTPD environment: service down, not managed by CTDB"

	for i in "apache2" "httpd" ; do
	    eventscript_call ctdb_service_unmanaged "$i"
	    service "$i" force-stopped
	done

	export CTDB_MANAGED_SERVICES="foo"
	unset CTDB_MANAGES_HTTPD
    fi
}

######################################################################

# Result and test functions

# Set some globals and print the summary.
define_test ()
{
    desc="$1"

    _f="$0"
    _f="${_f#./}"          # strip leading ./
    _f="${_f#simple/}"     # strip leading simple/
    _f="${_f#multievent/}" # strip leading multievent/
    _f="${_f%%/*}"         # if subdir, strip off file
    _f="${_f%.sh}"         # strip off .sh suffix if any

    # Remaining format should be NN.service.event.NNN or NN.service.NNN:
    _num="${_f##*.}"
    _f="${_f%.*}"
    case "$_f" in
	*.*.*)
	    script="${_f%.*}"
	    event="${_f##*.}"
	    ;;
	*.*)
	    script="$_f"
	    unset event
	    ;;
	*)
	    die "Internal error - unknown testcase filename format"
    esac

    printf "%-14s %-10s %-4s - %s\n\n" "$script" "$event" "$_num" "$desc"
}

# Set the required result for a test.
# - Argument 1 is exit code.
# - Argument 2, if present is the required test output but "--"
#   indicates empty output.
# If argument 2 is not present or null then read required test output
# from stdin.
required_result ()
{
    required_rc="${1:-0}"
    if [ -n "$2" ] ; then
	if [ "$2" = "--" ] ; then
	    required_output=""
	else
	    required_output="$2"
	fi
    else
	if ! tty -s ; then
	    required_output=$(cat)
	else
	    required_output=""
	fi
    fi
}

ok ()
{
    required_result 0 "$@"
}

ok_null ()
{
    ok --
}

result_print ()
{
    _passed="$1"
    _out="$2"
    _rc="$3"
    _iteration="$4"

    if [ "$EVENTSCRIPT_TESTS_VERBOSE" = "yes" ] || ! $_passed ; then
	if [ -n "$_iteration" ] ; then
	    cat <<EOF

==================================================
Iteration $_iteration
EOF
	fi

cat <<EOF
--------------------------------------------------
Output (Exit status: ${_rc}):
--------------------------------------------------
EOF
	echo "$_out" | cat $EVENTSCRIPT_TESTS_CAT_RESULTS_OPTS
    fi

    if ! $_passed ; then
	cat <<EOF
--------------------------------------------------
Required output (Exit status: ${required_rc}):
--------------------------------------------------
EOF
	echo "$required_output" | cat $EVENTSCRIPT_TESTS_CAT_RESULTS_OPTS

	if $EVENTSCRIPT_TESTS_DIFF_RESULTS ; then
	    _outr=$(mktemp)
	    echo "$required_output" >"$_outr"

	    _outf=$(mktemp)
	    echo "$_out" >"$_outf"

	    cat <<EOF
--------------------------------------------------
Diff:
--------------------------------------------------
EOF
	    diff -u "$_outr" "$_outf" | cat -A
	    rm "$_outr" "$_outf"
	fi
    fi
}

result_footer ()
{
    _passed="$1"

    if [ "$EVENTSCRIPT_TESTS_VERBOSE" = "yes" ] || ! $_passed ; then

	cat <<EOF
--------------------------------------------------
CTDB_BASE="$CTDB_BASE"
CTDB_ETCDIR="$CTDB_ETCDIR"
ctdb client is "$(which ctdb)"
--------------------------------------------------
EOF
    fi

    if $_passed ; then
	echo "PASSED"
	return 0
    else
	echo
	echo "FAILED"
	return 1
    fi
}

# Run an eventscript once.  The test passes if the return code and
# output match those required.

# Any args are passed to the eventscript.

# Eventscript tracing can be done by setting:
#   EVENTSCRIPTS_TESTS_TRACE="sh -x"

# or similar.  This will almost certainly make a test fail but is
# useful for debugging.
simple_test ()
{
    [ -n "$event" ] || die 'simple_test: $event not set'

    echo "Running \"$script $event${1:+ }$*\""
    _out=$($EVENTSCRIPTS_TESTS_TRACE "${CTDB_BASE}/events.d/$script" "$event" "$@" 2>&1)
    _rc=$?

    if [ -n "$OUT_FILTER" ] ; then
	_fout=$(echo "$_out" | eval sed -r $OUT_FILTER)
    else
	_fout="$_out"
    fi

    if [ "$_fout" = "$required_output" -a $_rc = $required_rc ] ; then
	_passed=true
    else
	_passed=false
    fi

    result_print "$_passed" "$_out" "$_rc"
    result_footer "$_passed"
}

simple_test_event ()
{
    # If something has previously failed then don't continue.
    : ${_passed:=true}
    $_passed || return 1

    event="$1" ; shift
    echo "##################################################"
    simple_test "$@"
}

# Run an eventscript iteratively.
# - 1st argument is the number of iterations.
# - 2nd argument is something to eval to do setup for every iteration.
#   The easiest thing to do here is to define a function and pass it
#   here.
# - Subsequent arguments come in pairs: an iteration number and
#   something to eval for that iteration.  Each time an iteration
#   number is matched the associated argument is given to eval after
#   the default setup is done.  The iteration numbers need to be given
#   in ascending order.
#
# Some optional args can be given *before* these, surrounded by extra
# "--" args.  These args are passed to the eventscript.  Quoting is
# lost.
#
# One use of the 2nd and further arguments is to call
# required_result() to change what is expected of a particular
# iteration.
iterate_test ()
{
    [ -n "$event" ] || die 'simple_test: $event not set'

    args=""
    if [ "$1" = "--" ] ; then
	shift
	while [ "$1" != "--" ] ; do
	    args="${args}${args:+ }$1"
	    shift
	done
	shift
    fi

    _repeats="$1"
    _setup_default="$2"
    shift 2

    echo "Running $_repeats iterations of \"$script $event\" $args"

    _result=true

    for iteration in $(seq 1 $_repeats) ; do
	# This is inefficient because the iteration-specific setup
	# might completely replace the default one.  However, running
	# the default is good because it allows you to revert to a
	# particular result without needing to specify it explicitly.
	eval $_setup_default
	if [ $iteration = "$1" ] ; then
	    eval $2
	    shift 2
	fi

	_out=$($EVENTSCRIPTS_TESTS_TRACE "${CTDB_BASE}/events.d/$script" "$event" $args 2>&1)
	_rc=$?

    if [ -n "$OUT_FILTER" ] ; then
	_fout=$(echo "$_out" | eval sed -r $OUT_FILTER)
    else
	_fout="$_out"
    fi

	if [ "$_fout" = "$required_output" -a $_rc = $required_rc ] ; then
	    _passed=true
	else
	    _passed=false
	    _result=false
	fi

	result_print "$_passed" "$_out" "$_rc" "$iteration"
    done

    result_footer "$_result"
}
