#!/bin/sh

. "${EVENTSCRIPTS_TESTS_DIR}/common.sh"

define_test "port 139 down, ctdb checktcpport not implemented, debug"

ctdb_not_implemented "checktcpport"

# This has to go before the setup, otherwise it will write a dud file.
export CTDB_DEBUGLEVEL=4

setup_nmap_output_filter

setup_samba
tcp_port_down 139

required_result 1 <<EOF
DEBUG: ctdb_check_ports - checker ctdb not implemented
DEBUG: output from checker was:
DEBUG: ctdb checktcpport 445 (exited with 1) with output:
$ctdb_not_implemented
ERROR: samba tcp port 139 is not responding
DEBUG: nmap -n -oG - -PS 127.0.0.1 -p 445,139 shows this output:
DEBUG: # Nmap 5.21 scan initiated DATE as: nmap -n -oG - -PS 127.0.0.1 -p 445,139
DEBUG: Host: 127.0.0.1 ()	Status: Up
DEBUG: Host: 127.0.0.1 ()	Ports: 445/open/tcp//microsoft-ds///, 139/closed/tcp//netbios-ssn///
DEBUG: # Nmap done at DATE -- 1 IP address (1 host up) scanned in 0.04 seconds
EOF

simple_test
