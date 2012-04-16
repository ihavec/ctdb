#!/bin/sh

. "${TEST_SCRIPTS_DIR}/unit.sh"

define_test "winbind down"

setup_samba
wbinfo_down

required_result 1 "ERROR: winbind - wbinfo -p returned error"

simple_test
