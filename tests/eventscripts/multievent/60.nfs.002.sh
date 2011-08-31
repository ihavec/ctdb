#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "takeip, monitor -> reconfigure"

setup_nfs

public_address=$(ctdb_get_1_public_address)

ok_null

simple_test_event "takeip" $public_address

# This currently assumes that ctdb scriptstatus will always return a
# good status (when replaying).  That should change and we will need
# to split this into 2 tests.
ok <<EOF
Reconfiguring service "nfs"...
Starting nfslock: OK
Starting nfs: OK
Replaying previous status for this script due to reconfigure...
EOF

simple_test_event "monitor"
