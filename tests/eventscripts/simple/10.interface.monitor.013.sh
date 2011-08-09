#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "1 bond, active slaves, link down"

setup_ctdb

iface=$(ctdb_get_1_interface)

setup_bond $iface "" "up" "down"

required_result 1 "No active slaves for 802.ad bond device $iface"

simple_test
