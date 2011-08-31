#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "reconfigure (synthetic), twice"
# This checks that the lock is released...

setup_nfs

public_address=$(ctdb_get_1_public_address)

err=""

ok <<EOF
Reconfiguring service "nfs"...
Starting nfslock: OK
Starting nfs: OK
EOF

simple_test_event "reconfigure"
simple_test_event "reconfigure"
