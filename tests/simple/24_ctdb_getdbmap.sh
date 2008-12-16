#!/bin/bash

test_info()
{
    cat <<EOF
Verify that  'ctdb getdbmap' operates as expected.

This test creates some test databases using 'ctdb attach'.

Prerequisites:

* An active CTDB cluster with at least 2 active nodes.

Steps:

1. Verify that the status on all of the ctdb nodes is 'OK'.
2. Get the database on using 'ctdb getdbmap'.
3. Verify that the output is valid.

Expected results:

* 'ctdb getdbmap' shows a valid listing of databases.
EOF
}

. ctdb_test_functions.bash

ctdb_test_init "$@"

set -e

onnode 0 $CTDB_TEST_WRAPPER cluster_is_healthy

# Restart when done since things are likely to be broken.
ctdb_test_exit_hook="restart_ctdb"

make_temp_db_filename ()
{
    dd if=/dev/urandom count=1 bs=512 2>/dev/null |
    md5sum |
    awk '{printf "%s.tdb\n", $1}'
}

try_command_on_node -v 0 "ctdb getdbmap"

db_map_pattern='^(Number of databases:[[:digit:]]+|dbid:0x[[:xdigit:]]+ name:[^[:space:]]+ path:[^[:space:]]+)$'

sanity_check_output $(($num_db_init + 1)) "$dbmap_pattern" "$out"

num_db_init=$(echo "$out" | sed -n -e '1s/.*://p')

for i in $(seq 1 5) ; do
    f=$(make_temp_db_filename)
    echo "Creating test database: $f"
    try_command_on_node 0 ctdb attach "$f"
    try_command_on_node 0 ctdb getdbmap
    sanity_check_output $(($num_db_init + 1)) "$dbmap_pattern" "$out"
    num=$(echo "$out" | sed -n -e '1s/^.*://p')
    if [ $num = $(($num_db_init + $i)) ] ; then
	echo "OK: correct number of additional databases"
    else
	echo "BAD: no additional database"
	exit 1
    fi
    if [ "${out/name:${f} /}" != "$out" ] ; then
	echo "OK: getdbmap knows about \"$f\""
    else
	echo "BAD: getdbmap does not know about \"$f\""
	exit 1
    fi
done

echo "OK, that worked... expect a restart..."

ctdb_test_exit
