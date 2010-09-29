/* 
   ctdb statistics code

   Copyright (C) Ronnie Sahlberg 2010

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

#include "includes.h"
#include <string.h>
#include "lib/tevent/tevent.h"
#include "../include/ctdb_private.h"

static void ctdb_statistics_update(struct event_context *ev, struct timed_event *te, 
				   struct timeval t, void *p)
{
	struct ctdb_context *ctdb = talloc_get_type(p, struct ctdb_context);

	memmove(&ctdb->statistics_history[1], &ctdb->statistics_history[0], (MAX_STAT_HISTORY-1)*sizeof(struct ctdb_statistics));
	memcpy(&ctdb->statistics_history[0], &ctdb->statistics_current, sizeof(struct ctdb_statistics));
	ctdb->statistics_history[0].statistics_current_time = timeval_current();


	bzero(&ctdb->statistics_current, sizeof(struct ctdb_statistics));
	ctdb->statistics_current.statistics_start_time = timeval_current();

	event_add_timed(ctdb->ev, ctdb, timeval_current_ofs(10, 0), ctdb_statistics_update, ctdb);
}

int ctdb_statistics_init(struct ctdb_context *ctdb)
{
	bzero(&ctdb->statistics, sizeof(struct ctdb_statistics));

	bzero(&ctdb->statistics_current, sizeof(struct ctdb_statistics));
	ctdb->statistics_current.statistics_start_time = timeval_current();

	bzero(ctdb->statistics_history, sizeof(ctdb->statistics_history));

	event_add_timed(ctdb->ev, ctdb, timeval_current_ofs(10, 0), ctdb_statistics_update, ctdb);
	return 0;
}


int32_t ctdb_control_get_stat_history(struct ctdb_context *ctdb, 
				      struct ctdb_req_control *c,
				      TDB_DATA *outdata)
{
	int len;
	struct ctdb_statistics_wire *stat;

	len = offsetof(struct ctdb_statistics_wire, stats) + MAX_STAT_HISTORY*sizeof(struct ctdb_statistics);

	stat = talloc_size(outdata, len);
	if (stat == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to allocate statistics history structure\n"));
		return -1;
	}

	stat->num = MAX_STAT_HISTORY;
	memcpy(&stat->stats[0], &ctdb->statistics_history[0], sizeof(ctdb->statistics_history));

	outdata->dsize = len;
	outdata->dptr  = (uint8_t *)stat;

	return 0;
}
