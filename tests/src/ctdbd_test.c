/*
   ctdbd test include file

   Copyright (C) Martin Schwenke  2011

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _CTDBD_TEST_C
#define _CTDBD_TEST_C

#include "includes.h"
#include "lib/tevent/tevent.h"
#include "lib/tdb/include/tdb.h"
#include "ctdb_private.h"

/*
 * Need these, since they're defined in ctdbd.c but we can't include
 * that.
 */
int script_log_level;
bool fast_start;
void ctdb_load_nodes_file(struct ctdb_context *ctdb) {}


/* UTIL_OBJ */
#include "lib/util/idtree.c"
#include "lib/util/db_wrap.c"
#include "lib/util/strlist.c"
#include "lib/util/util.c"
#include "lib/util/util_time.c"
#include "lib/util/util_file.c"
#include "lib/util/fault.c"
#include "lib/util/substitute.c"
#include "lib/util/signal.c"

/* CTDB_COMMON_OBJ */
#include "common/ctdb_io.c"
#include "common/ctdb_util.c"
#include "common/ctdb_ltdb.c"
#include "common/ctdb_message.c"
#include "common/cmdline.c"
#include "lib/util/debug.c"
#include "common/rb_tree.c"
#include "common/system_common.c"
#include "common/ctdb_logging.c"

/* CTDB_SERVER_OBJ */
#include "server/ctdb_daemon.c"
#include "server/ctdb_lockwait.c"
#include "server/ctdb_recoverd.c"
#include "server/ctdb_recover.c"
#include "server/ctdb_freeze.c"
#include "server/ctdb_tunables.c"
#include "server/ctdb_monitor.c"
#include "server/ctdb_server.c"
#include "server/ctdb_control.c"
#include "server/ctdb_call.c"
#include "server/ctdb_ltdb_server.c"
#include "server/ctdb_traverse.c"
#include "server/eventscript.c"
#include "server/ctdb_takeover.c"
#include "server/ctdb_serverids.c"
#include "server/ctdb_persistent.c"
#include "server/ctdb_keepalive.c"
#include "server/ctdb_logging.c"
#include "server/ctdb_uptime.c"
#include "server/ctdb_vacuum.c"
#include "server/ctdb_banning.c"
#include "server/ctdb_statistics.c"

/* CTDB_CLIENT_OBJ */
#include "client/ctdb_client.c"

/* CTDB_TCP_OBJ */
#include "tcp/tcp_connect.c"
#include "tcp/tcp_io.c"
#include "tcp/tcp_init.c"

#endif /* _CTDBD_TEST_C */
