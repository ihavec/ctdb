#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "takeip, monitor -> reconfigure, replay timedout"

setup_nfs

public_address=$(ctdb_get_1_public_address)

err="waiting, waiting..."

ok_null

simple_test_event "takeip" $public_address

ctdb_fake_scriptstatus -62 "TIMEDOUT" "$err"

required_result 1 <<EOF
Reconfiguring service "nfs"...
Starting nfslock: OK
Starting nfs: OK
Replaying previous status for this script due to reconfigure...
[Replay of TIMEDOUT scriptstatus - note incorrect return code.] $err
EOF

simple_test_event "monitor"
