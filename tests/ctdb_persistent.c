/* 
   simple tool to test persistent databases

   Copyright (C) Andrew Tridgell  2006-2007
   Copyright (c) Ronnie sahlberg  2007

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
#include "lib/events/events.h"
#include "system/filesys.h"
#include "popt.h"
#include "cmdline.h"

#include <sys/time.h>
#include <time.h>

static struct timeval tp1,tp2;

static void start_timer(void)
{
	gettimeofday(&tp1,NULL);
}

static double end_timer(void)
{
	gettimeofday(&tp2,NULL);
	return (tp2.tv_sec + (tp2.tv_usec*1.0e-6)) - 
		(tp1.tv_sec + (tp1.tv_usec*1.0e-6));
}

static int timelimit = 10;

static unsigned int pnn;

static TDB_DATA old_data;

static int success = true;

static void each_second(struct event_context *ev, struct timed_event *te, 
					 struct timeval t, void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);
	int i;
	uint32_t *old_counters;


	printf("Counters: ");
	old_counters = (uint32_t *)old_data.dptr;
	for (i=0;i<old_data.dsize/sizeof(uint32_t); i++) {
		printf("%6u ", old_counters[i]);
	}
	printf("\n"); 

	event_add_timed(ev, ctdb, timeval_current_ofs(1, 0), each_second, ctdb);
}

static void check_counters(struct ctdb_context *ctdb, TDB_DATA data)
{
	int i;
	uint32_t *counters, *old_counters;

	counters     = (uint32_t *)data.dptr;
	old_counters = (uint32_t *)old_data.dptr;

	/* check that all the counters are monotonic increasing */
	for (i=0; i<old_data.dsize/sizeof(uint32_t); i++) {
		if (counters[i]<old_counters[i]) {
			printf("ERROR: counters has decreased for node %u  From %u to %u\n", i, old_counters[i], counters[i]);
			success = false;
		}
	}

	if (old_data.dsize != data.dsize) {
		old_data.dsize = data.dsize;
		old_data.dptr = talloc_realloc_size(ctdb, old_data.dptr, old_data.dsize);
	}

	memcpy(old_data.dptr, data.dptr, data.dsize);
}



static void test_store_records(struct ctdb_context *ctdb, struct event_context *ev)
{
	TDB_DATA key, data;
	struct ctdb_db_context *ctdb_db;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	int ret;
	struct ctdb_record_handle *h;
	uint32_t *counters;
	int first_time = true;	
	ctdb_db = ctdb_db_handle(ctdb, "persistent.tdb");

	key.dptr = discard_const("testkey");
	key.dsize = strlen((const char *)key.dptr)+1;

	start_timer();
	while (end_timer() < timelimit) {
		h = ctdb_fetch_lock(ctdb_db, tmp_ctx, key, &data);
		if (h == NULL) {
			printf("Failed to fetch record '%s' on node %d\n", 
			       (const char *)key.dptr, ctdb_get_pnn(ctdb));
			talloc_free(tmp_ctx);
			return;
		}

		if (data.dsize < sizeof(uint32_t) * (pnn+1)) {
			unsigned char *ptr = data.dptr;

			data.dptr = talloc_zero_size(tmp_ctx, sizeof(uint32_t) * (pnn+1));
			memcpy(data.dptr, ptr, data.dsize);
			talloc_free(ptr);

			data.dsize = sizeof(uint32_t) * (pnn+1);
		}

		if (data.dptr == NULL) {
			printf("Failed to realloc array\n");
			talloc_free(tmp_ctx);
			return;
		}

		counters = (uint32_t *)data.dptr;

		if (first_time) {
			counters[pnn] = 0;
			first_time = false;
		}

		/* bump our counter */
		counters[pnn]++;

		ret = ctdb_record_store(h, data);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,("Failed to store record\n"));
			exit(1);
		}

		/* store the counters and verify that they are sane */
		if (pnn == 0) {
			check_counters(ctdb, data);
		}

		talloc_free(h);
	}

	talloc_free(tmp_ctx);
}

/*
  main program
*/
int main(int argc, const char *argv[])
{
	struct ctdb_context *ctdb;
	struct ctdb_db_context *ctdb_db;

	struct poptOption popt_options[] = {
		POPT_AUTOHELP
		POPT_CTDB_CMDLINE
		{ "timelimit", 't', POPT_ARG_INT, &timelimit, 0, "timelimit", "integer" },
		POPT_TABLEEND
	};
	int opt;
	const char **extra_argv;
	int extra_argc = 0;
	poptContext pc;
	struct event_context *ev;

	pc = poptGetContext(argv[0], argc, argv, popt_options, POPT_CONTEXT_KEEP_FIRST);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		default:
			fprintf(stderr, "Invalid option %s: %s\n", 
				poptBadOption(pc, 0), poptStrerror(opt));
			exit(1);
		}
	}

	/* setup the remaining options for the main program to use */
	extra_argv = poptGetArgs(pc);
	if (extra_argv) {
		extra_argv++;
		while (extra_argv[extra_argc]) extra_argc++;
	}

	ev = event_context_init(NULL);

	ctdb = ctdb_cmdline_client(ev);

	/* attach to a specific database */
	ctdb_db = ctdb_attach(ctdb, "persistent.tdb", true);
	if (!ctdb_db) {
		printf("ctdb_attach failed - %s\n", ctdb_errstr(ctdb));
		exit(1);
	}

	printf("Waiting for cluster\n");
	while (1) {
		uint32_t recmode=1;
		ctdb_ctrl_getrecmode(ctdb, ctdb, timeval_zero(), CTDB_CURRENT_NODE, &recmode);
		if (recmode == 0) break;
		event_loop_once(ev);
	}

	pnn = ctdb_get_pnn(ctdb);
	printf("Starting test on node %u. running for %u seconds\n", pnn, timelimit);

	if (pnn == 0) {
		event_add_timed(ev, ctdb, timeval_current_ofs(1, 0), each_second, ctdb);
	}

	test_store_records(ctdb, ev);

	if (pnn == 0) {
		if (success != true) {
			printf("The test FAILED\n");
			return 1;
		} else {
			printf("SUCCESS!\n");
		}
	}
	return 0;
}
