#!/bin/sh

. "${TEST_SCRIPTS_DIR}/unit.sh"

define_test "knfsd down, 6 iterations, dump 5 threads, none hung"

# knfsd fails and attempts to restart it fail.
setup_nfs
rpc_services_down "nfs"

# Additionally, any hung threads should have stack traces dumped.
CTDB_NFS_DUMP_STUCK_THREADS=5
FAKE_NFSD_THREAD_PIDS=""

iterate_test 6 'ok_null' \
    2 'rpc_set_service_failure_response "nfsd"' \
    4 'rpc_set_service_failure_response "nfsd"' \
    6 'rpc_set_service_failure_response "nfsd"'
