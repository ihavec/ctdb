# Hey Emacs, this is a -*- shell-script -*- !!!  :-)

test_prog="$(dirname ${TEST_SUBDIR})/bin/ctdb_takeover_tests ctdb_takeover_run_core"

define_test ()
{
    _f=$(basename "$0" ".sh")

    case "$_f" in
	nondet.*)
	    algorithm="nondet"
	    export CTDB_LCP2="no"
	    ;;
	lcp2.*)
	    algorithm="lcp2"
	    export CTDB_LCP2="yes"
	    ;;
	*)
	    die "Unknown algorithm for testcase \"$_f\""
    esac

    printf "%-12s - %s\n" "$_f" "$1"
}

simple_test ()
{
    # Do some filtering of the output to replace date/time.
    if [ "$algorithm" = "lcp2" -a -n "$CTDB_TEST_LOGLEVEL" ] ; then
	OUT_FILTER='s@^.*:@DATE\ TIME\ \[PID\]:@'
    fi

    _states="$1"
    _out=$($test_prog $_states 2>&1)

    result_check "Algorithm: $algorithm"
}
