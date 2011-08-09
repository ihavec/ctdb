#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "knfsd down, 6 iterations"

# knfsd fails and attempts to restart it fail.

setup_nfs
rpc_services_down "nfs"

iterate_test 6 'ok_null' \
    2 'rpc_set_service_failure_response "knfsd"' \
    4 'rpc_set_service_failure_response "knfsd"' \
    6 'rpc_set_service_failure_response "knfsd"'
