#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "not managed, check no-op"

setup_vsftpd "down"

ok_null

simple_test $cmd
