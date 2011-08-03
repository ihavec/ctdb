#!/bin/sh

. "${ONNODE_TESTS_DIR}/common.sh"

cmd="$ONNODE ok hostname"

echo "$cmd - 2nd node disconnected, extra status columns"

ctdb_set_output <<EOF
:Node:IP:Disconnected:Banned:Disabled:Unhealthy:Stopped:Inactive:X1:X2:X3:X4:
:0:192.168.1.101:0:0:0:0:0:0:0:0:0:0:
:1:192.168.1.102:1:0:0:0:0:0:0:0:0:0:
:2:192.168.1.103:0:0:0:0:0:0:0:0:0:0:
:3:192.168.1.104:0:0:0:0:0:0:0:0:0:0:
EOF

required_result <<EOF

>> NODE: 192.168.1.101 <<
-n 192.168.1.101 hostname

>> NODE: 192.168.1.103 <<
-n 192.168.1.103 hostname

>> NODE: 192.168.1.104 <<
-n 192.168.1.104 hostname
EOF

simple_test $cmd
