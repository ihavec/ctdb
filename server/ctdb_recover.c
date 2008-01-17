/* 
   ctdb recovery code

   Copyright (C) Andrew Tridgell  2007
   Copyright (C) Ronnie Sahlberg  2007

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
#include "lib/tdb/include/tdb.h"
#include "system/time.h"
#include "system/network.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "../include/ctdb_private.h"
#include "lib/util/dlinklist.h"
#include "db_wrap.h"

/*
  lock all databases - mark only
 */
static int ctdb_lock_all_databases_mark(struct ctdb_context *ctdb)
{
	struct ctdb_db_context *ctdb_db;
	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("Attempt to mark all databases locked when not frozen\n"));
		return -1;
	}
	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		if (tdb_lockall_mark(ctdb_db->ltdb->tdb) != 0) {
			return -1;
		}
	}
	return 0;
}

/*
  lock all databases - unmark only
 */
static int ctdb_lock_all_databases_unmark(struct ctdb_context *ctdb)
{
	struct ctdb_db_context *ctdb_db;
	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("Attempt to unmark all databases locked when not frozen\n"));
		return -1;
	}
	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		if (tdb_lockall_unmark(ctdb_db->ltdb->tdb) != 0) {
			return -1;
		}
	}
	return 0;
}


int 
ctdb_control_getvnnmap(struct ctdb_context *ctdb, uint32_t opcode, TDB_DATA indata, TDB_DATA *outdata)
{
	CHECK_CONTROL_DATA_SIZE(0);
	struct ctdb_vnn_map_wire *map;
	size_t len;

	len = offsetof(struct ctdb_vnn_map_wire, map) + sizeof(uint32_t)*ctdb->vnn_map->size;
	map = talloc_size(outdata, len);
	CTDB_NO_MEMORY_VOID(ctdb, map);

	map->generation = ctdb->vnn_map->generation;
	map->size = ctdb->vnn_map->size;
	memcpy(map->map, ctdb->vnn_map->map, sizeof(uint32_t)*map->size);

	outdata->dsize = len;
	outdata->dptr  = (uint8_t *)map;

	return 0;
}

int 
ctdb_control_setvnnmap(struct ctdb_context *ctdb, uint32_t opcode, TDB_DATA indata, TDB_DATA *outdata)
{
	struct ctdb_vnn_map_wire *map = (struct ctdb_vnn_map_wire *)indata.dptr;

	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("Attempt to set vnnmap when not frozen\n"));
		return -1;
	}

	talloc_free(ctdb->vnn_map);

	ctdb->vnn_map = talloc(ctdb, struct ctdb_vnn_map);
	CTDB_NO_MEMORY(ctdb, ctdb->vnn_map);

	ctdb->vnn_map->generation = map->generation;
	ctdb->vnn_map->size       = map->size;
	ctdb->vnn_map->map = talloc_array(ctdb->vnn_map, uint32_t, map->size);
	CTDB_NO_MEMORY(ctdb, ctdb->vnn_map->map);

	memcpy(ctdb->vnn_map->map, map->map, sizeof(uint32_t)*map->size);

	return 0;
}

int 
ctdb_control_getdbmap(struct ctdb_context *ctdb, uint32_t opcode, TDB_DATA indata, TDB_DATA *outdata)
{
	uint32_t i, len;
	struct ctdb_db_context *ctdb_db;
	struct ctdb_dbid_map *dbid_map;

	CHECK_CONTROL_DATA_SIZE(0);

	len = 0;
	for(ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next){
		len++;
	}


	outdata->dsize = offsetof(struct ctdb_dbid_map, dbs) + sizeof(dbid_map->dbs[0])*len;
	outdata->dptr  = (unsigned char *)talloc_zero_size(outdata, outdata->dsize);
	if (!outdata->dptr) {
		DEBUG(0, (__location__ " Failed to allocate dbmap array\n"));
		exit(1);
	}

	dbid_map = (struct ctdb_dbid_map *)outdata->dptr;
	dbid_map->num = len;
	for (i=0,ctdb_db=ctdb->db_list;ctdb_db;i++,ctdb_db=ctdb_db->next){
		dbid_map->dbs[i].dbid       = ctdb_db->db_id;
		dbid_map->dbs[i].persistent = ctdb_db->persistent;
	}

	return 0;
}

int 
ctdb_control_getnodemap(struct ctdb_context *ctdb, uint32_t opcode, TDB_DATA indata, TDB_DATA *outdata)
{
	uint32_t i, num_nodes;
	struct ctdb_node_map *node_map;

	CHECK_CONTROL_DATA_SIZE(0);

	num_nodes = ctdb->num_nodes;

	outdata->dsize = offsetof(struct ctdb_node_map, nodes) + num_nodes*sizeof(struct ctdb_node_and_flags);
	outdata->dptr  = (unsigned char *)talloc_zero_size(outdata, outdata->dsize);
	if (!outdata->dptr) {
		DEBUG(0, (__location__ " Failed to allocate nodemap array\n"));
		exit(1);
	}

	node_map = (struct ctdb_node_map *)outdata->dptr;
	node_map->num = num_nodes;
	for (i=0; i<num_nodes; i++) {
		inet_aton(ctdb->nodes[i]->address.address, &node_map->nodes[i].sin.sin_addr);
		node_map->nodes[i].pnn   = ctdb->nodes[i]->pnn;
		node_map->nodes[i].flags = ctdb->nodes[i]->flags;
	}

	return 0;
}

/* 
   a traverse function for pulling all relevent records from pulldb
 */
struct pulldb_data {
	struct ctdb_context *ctdb;
	struct ctdb_control_pulldb_reply *pulldata;
	uint32_t len;
	bool failed;
};

static int traverse_pulldb(struct tdb_context *tdb, TDB_DATA key, TDB_DATA data, void *p)
{
	struct pulldb_data *params = (struct pulldb_data *)p;
	struct ctdb_rec_data *rec;

	/* add the record to the blob */
	rec = ctdb_marshall_record(params->pulldata, 0, key, NULL, data);
	if (rec == NULL) {
		params->failed = true;
		return -1;
	}
	params->pulldata = talloc_realloc_size(NULL, params->pulldata, rec->length + params->len);
	if (params->pulldata == NULL) {
		DEBUG(0,(__location__ " Failed to expand pulldb_data to %u (%u records)\n", 
			 rec->length + params->len, params->pulldata->count));
		params->failed = true;
		return -1;
	}
	params->pulldata->count++;
	memcpy(params->len+(uint8_t *)params->pulldata, rec, rec->length);
	params->len += rec->length;
	talloc_free(rec);

	return 0;
}

/*
  pul a bunch of records from a ltdb, filtering by lmaster
 */
int32_t ctdb_control_pull_db(struct ctdb_context *ctdb, TDB_DATA indata, TDB_DATA *outdata)
{
	struct ctdb_control_pulldb *pull;
	struct ctdb_db_context *ctdb_db;
	struct pulldb_data params;
	struct ctdb_control_pulldb_reply *reply;

	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("rejecting ctdb_control_pull_db when not frozen\n"));
		return -1;
	}

	pull = (struct ctdb_control_pulldb *)indata.dptr;
	
	ctdb_db = find_ctdb_db(ctdb, pull->db_id);
	if (!ctdb_db) {
		DEBUG(0,(__location__ " Unknown db 0x%08x\n", pull->db_id));
		return -1;
	}

	reply = talloc_zero(outdata, struct ctdb_control_pulldb_reply);
	CTDB_NO_MEMORY(ctdb, reply);

	reply->db_id = pull->db_id;

	params.ctdb = ctdb;
	params.pulldata = reply;
	params.len = offsetof(struct ctdb_control_pulldb_reply, data);
	params.failed = false;

	if (ctdb_lock_all_databases_mark(ctdb) != 0) {
		DEBUG(0,(__location__ " Failed to get lock on entired db - failing\n"));
		return -1;
	}

	if (tdb_traverse_read(ctdb_db->ltdb->tdb, traverse_pulldb, &params) == -1) {
		DEBUG(0,(__location__ " Failed to get traverse db '%s'\n", ctdb_db->db_name));
		ctdb_lock_all_databases_unmark(ctdb);
		talloc_free(params.pulldata);
		return -1;
	}

	ctdb_lock_all_databases_unmark(ctdb);

	outdata->dptr = (uint8_t *)params.pulldata;
	outdata->dsize = params.len;

	return 0;
}

/*
  push a bunch of records into a ltdb, filtering by rsn
 */
int32_t ctdb_control_push_db(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_control_pulldb_reply *reply = (struct ctdb_control_pulldb_reply *)indata.dptr;
	struct ctdb_db_context *ctdb_db;
	int i, ret;
	struct ctdb_rec_data *rec;

	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("rejecting ctdb_control_push_db when not frozen\n"));
		return -1;
	}

	if (indata.dsize < offsetof(struct ctdb_control_pulldb_reply, data)) {
		DEBUG(0,(__location__ " invalid data in pulldb reply\n"));
		return -1;
	}

	ctdb_db = find_ctdb_db(ctdb, reply->db_id);
	if (!ctdb_db) {
		DEBUG(0,(__location__ " Unknown db 0x%08x\n", reply->db_id));
		return -1;
	}

	if (ctdb_lock_all_databases_mark(ctdb) != 0) {
		DEBUG(0,(__location__ " Failed to get lock on entired db - failing\n"));
		return -1;
	}

	rec = (struct ctdb_rec_data *)&reply->data[0];

	DEBUG(1,("starting push of %u records for dbid 0x%x\n",
		 reply->count, reply->db_id));

	for (i=0;i<reply->count;i++) {
		TDB_DATA key, data;
		struct ctdb_ltdb_header *hdr;

		key.dptr = &rec->data[0];
		key.dsize = rec->keylen;
		data.dptr = &rec->data[key.dsize];
		data.dsize = rec->datalen;

		if (data.dsize < sizeof(struct ctdb_ltdb_header)) {
			DEBUG(0,(__location__ " bad ltdb record\n"));
			goto failed;
		}
		hdr = (struct ctdb_ltdb_header *)data.dptr;
		data.dptr += sizeof(*hdr);
		data.dsize -= sizeof(*hdr);

		ret = ctdb_ltdb_store(ctdb_db, key, hdr, data);
		if (ret != 0) {
			DEBUG(0, (__location__ " Unable to store record\n"));
			goto failed;
		}

		rec = (struct ctdb_rec_data *)(rec->length + (uint8_t *)rec);
	}	    

	DEBUG(3,("finished push of %u records for dbid 0x%x\n",
		 reply->count, reply->db_id));

	ctdb_lock_all_databases_unmark(ctdb);
	return 0;

failed:
	ctdb_lock_all_databases_unmark(ctdb);
	return -1;
}


static int traverse_setdmaster(struct tdb_context *tdb, TDB_DATA key, TDB_DATA data, void *p)
{
	uint32_t *dmaster = (uint32_t *)p;
	struct ctdb_ltdb_header *header = (struct ctdb_ltdb_header *)data.dptr;
	int ret;

	/* skip if already correct */
	if (header->dmaster == *dmaster) {
		return 0;
	}

	header->dmaster = *dmaster;

	ret = tdb_store(tdb, key, data, TDB_REPLACE);
	if (ret) {
		DEBUG(0,(__location__ " failed to write tdb data back  ret:%d\n",ret));
		return ret;
	}

	/* TODO: add error checking here */

	return 0;
}

int32_t ctdb_control_set_dmaster(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_control_set_dmaster *p = (struct ctdb_control_set_dmaster *)indata.dptr;
	struct ctdb_db_context *ctdb_db;

	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("rejecting ctdb_control_set_dmaster when not frozen\n"));
		return -1;
	}

	ctdb_db = find_ctdb_db(ctdb, p->db_id);
	if (!ctdb_db) {
		DEBUG(0,(__location__ " Unknown db 0x%08x\n", p->db_id));
		return -1;
	}

	if (ctdb_lock_all_databases_mark(ctdb) != 0) {
		DEBUG(0,(__location__ " Failed to get lock on entired db - failing\n"));
		return -1;
	}

	tdb_traverse(ctdb_db->ltdb->tdb, traverse_setdmaster, &p->dmaster);

	ctdb_lock_all_databases_unmark(ctdb);
	
	return 0;
}

struct ctdb_set_recmode_state {
	struct ctdb_context *ctdb;
	struct ctdb_req_control *c;
	uint32_t recmode;
	int fd[2];
	struct timed_event *te;
	struct fd_event *fde;
	pid_t child;
};

/*
  called when the 'recovered' event script has finished
 */
static void ctdb_recovered_callback(struct ctdb_context *ctdb, int status, void *p)
{
	struct ctdb_set_recmode_state *state = talloc_get_type(p, struct ctdb_set_recmode_state);

	ctdb_enable_monitoring(state->ctdb);

	if (status == 0) {
		ctdb->recovery_mode = state->recmode;
	} else {
		DEBUG(0,(__location__ " recovered event script failed (status %d)\n", status));
	}

	ctdb_request_control_reply(ctdb, state->c, NULL, status, NULL);
	talloc_free(state);

	gettimeofday(&ctdb->last_recovery_time, NULL);
}

/*
  called if our set_recmode child times out. this would happen if
  ctdb_recovery_lock() would block.
 */
static void ctdb_set_recmode_timeout(struct event_context *ev, struct timed_event *te, 
					 struct timeval t, void *private_data)
{
	struct ctdb_set_recmode_state *state = talloc_get_type(private_data, 
					   struct ctdb_set_recmode_state);

	ctdb_request_control_reply(state->ctdb, state->c, NULL, -1, "timeout in ctdb_set_recmode");
	talloc_free(state);
}


/* when we free the recmode state we must kill any child process.
*/
static int set_recmode_destructor(struct ctdb_set_recmode_state *state)
{
	kill(state->child, SIGKILL);
	waitpid(state->child, NULL, 0);
	return 0;
}

/* this is called when the client process has completed ctdb_recovery_lock()
   and has written data back to us through the pipe.
*/
static void set_recmode_handler(struct event_context *ev, struct fd_event *fde, 
			     uint16_t flags, void *private_data)
{
	struct ctdb_set_recmode_state *state= talloc_get_type(private_data, 
					     struct ctdb_set_recmode_state);
	char c = 0;
	int ret;

	/* we got a response from our child process so we can abort the
	   timeout.
	*/
	talloc_free(state->te);
	state->te = NULL;


	/* read the childs status when trying to lock the reclock file.
	   child wrote 0 if everything is fine and 1 if it did manage
	   to lock the file, which would be a problem since that means
	   we got a request to exit from recovery but we could still lock
	   the file   which at this time SHOULD be locked by the recovery
	   daemon on the recmaster
	*/		
	ret = read(state->fd[0], &c, 1);
	if (ret != 1 || c != 0) {
		ctdb_request_control_reply(state->ctdb, state->c, NULL, -1, "managed to lock reclock file from inside daemon");
		talloc_free(state);
		return;
	}


	ctdb_disable_monitoring(state->ctdb);

	/* call the events script to tell all subsystems that we have recovered */
	ret = ctdb_event_script_callback(state->ctdb, 
					 timeval_current_ofs(state->ctdb->tunable.script_timeout, 0),
					 state, 
					 ctdb_recovered_callback, 
					 state, "recovered");

	if (ret != 0) {
		ctdb_enable_monitoring(state->ctdb);

		ctdb_request_control_reply(state->ctdb, state->c, NULL, -1, "failed to run eventscript from set_recmode");
		talloc_free(state);
		return;
	}
}

/*
  set the recovery mode
 */
int32_t ctdb_control_set_recmode(struct ctdb_context *ctdb, 
				 struct ctdb_req_control *c,
				 TDB_DATA indata, bool *async_reply,
				 const char **errormsg)
{
	uint32_t recmode = *(uint32_t *)indata.dptr;
	int ret;
	struct ctdb_set_recmode_state *state;
	pid_t parent = getpid();

	if (ctdb->freeze_mode != CTDB_FREEZE_FROZEN) {
		DEBUG(0,("Attempt to change recovery mode to %u when not frozen\n", 
			 recmode));
		(*errormsg) = "Cannot change recovery mode while not frozen";
		return -1;
	}

	if (recmode != ctdb->recovery_mode) {
		DEBUG(0,(__location__ " Recovery mode set to %s\n", 
			 recmode==CTDB_RECOVERY_NORMAL?"NORMAL":"ACTIVE"));
	}

	if (recmode != CTDB_RECOVERY_NORMAL ||
	    ctdb->recovery_mode != CTDB_RECOVERY_ACTIVE) {
		ctdb->recovery_mode = recmode;
		return 0;
	}

	/* some special handling when ending recovery mode */
	state = talloc(ctdb, struct ctdb_set_recmode_state);
	CTDB_NO_MEMORY(ctdb, state);

	/* For the rest of what needs to be done, we need to do this in
	   a child process since 
	   1, the call to ctdb_recovery_lock() can block if the cluster
	      filesystem is in the process of recovery.
	   2, running of the script may take a while.
	*/
	ret = pipe(state->fd);
	if (ret != 0) {
		talloc_free(state);
		DEBUG(0,(__location__ " Failed to open pipe for set_recmode child\n"));
		return -1;
	}

	state->child = fork();
	if (state->child == (pid_t)-1) {
		close(state->fd[0]);
		close(state->fd[1]);
		talloc_free(state);
		return -1;
	}

	if (state->child == 0) {
		char cc = 0;
		close(state->fd[0]);

		/* we should not be able to get the lock on the nodes list, 
		  as it should  be held by the recovery master 
		*/
		if (ctdb_recovery_lock(ctdb, false)) {
			DEBUG(0,("ERROR: recovery lock file %s not locked when recovering!\n", ctdb->recovery_lock_file));
			cc = 1;
		}

		write(state->fd[1], &cc, 1);
		/* make sure we die when our parent dies */
		while (kill(parent, 0) == 0 || errno != ESRCH) {
			sleep(5);
		}
		_exit(0);
	}
	close(state->fd[1]);

	talloc_set_destructor(state, set_recmode_destructor);

	state->te = event_add_timed(ctdb->ev, state, timeval_current_ofs(3, 0),
			ctdb_set_recmode_timeout, state);

	state->fde = event_add_fd(ctdb->ev, state, state->fd[0],
				EVENT_FD_READ|EVENT_FD_AUTOCLOSE,
				set_recmode_handler,
				(void *)state);
	if (state->fde == NULL) {
		talloc_free(state);
		return -1;
	}

	state->ctdb    = ctdb;
	state->recmode = recmode;
	state->c       = talloc_steal(state, c);

	*async_reply = true;

	return 0;
}


/*
  try and get the recovery lock in shared storage - should only work
  on the recovery master recovery daemon. Anywhere else is a bug
 */
bool ctdb_recovery_lock(struct ctdb_context *ctdb, bool keep)
{
	struct flock lock;

	if (ctdb->recovery_lock_fd != -1) {
		close(ctdb->recovery_lock_fd);
	}
	ctdb->recovery_lock_fd = open(ctdb->recovery_lock_file, O_RDWR|O_CREAT, 0600);
	if (ctdb->recovery_lock_fd == -1) {
		DEBUG(0,("ctdb_recovery_lock: Unable to open %s - (%s)\n", 
			 ctdb->recovery_lock_file, strerror(errno)));
		return false;
	}

	set_close_on_exec(ctdb->recovery_lock_fd);

	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	lock.l_start = 0;
	lock.l_len = 1;
	lock.l_pid = 0;

	if (fcntl(ctdb->recovery_lock_fd, F_SETLK, &lock) != 0) {
		close(ctdb->recovery_lock_fd);
		ctdb->recovery_lock_fd = -1;
		if (keep) {
			DEBUG(0,("ctdb_recovery_lock: Failed to get recovery lock on '%s'\n", ctdb->recovery_lock_file));
		}
		return false;
	}

	if (!keep) {
		close(ctdb->recovery_lock_fd);
		ctdb->recovery_lock_fd = -1;
	}

	DEBUG(0,("ctdb_recovery_lock: Got recovery lock on '%s'\n", ctdb->recovery_lock_file));

	return true;
}


/*
  delete a record as part of the vacuum process
  only delete if we are not lmaster or dmaster, and our rsn is <= the provided rsn
  use non-blocking locks
 */
int32_t ctdb_control_delete_record(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_rec_data *rec = (struct ctdb_rec_data *)indata.dptr;
	struct ctdb_db_context *ctdb_db;
	TDB_DATA key, data;
	struct ctdb_ltdb_header *hdr, *hdr2;
	
	/* these are really internal tdb functions - but we need them here for
	   non-blocking lock of the freelist */
	int tdb_lock_nonblock(struct tdb_context *tdb, int list, int ltype);
	int tdb_unlock(struct tdb_context *tdb, int list, int ltype);

	if (indata.dsize < sizeof(uint32_t) || indata.dsize != rec->length) {
		DEBUG(0,(__location__ " Bad record size in ctdb_control_delete_record\n"));
		return -1;
	}

	ctdb_db = find_ctdb_db(ctdb, rec->reqid);
	if (!ctdb_db) {
		DEBUG(0,(__location__ " Unknown db 0x%08x\n", rec->reqid));
		return -1;
	}

	key.dsize = rec->keylen;
	key.dptr  = &rec->data[0];
	data.dsize = rec->datalen;
	data.dptr = &rec->data[rec->keylen];

	if (ctdb_lmaster(ctdb, &key) == ctdb->pnn) {
		DEBUG(2,(__location__ " Called delete on record where we are lmaster\n"));
		return -1;
	}

	if (data.dsize != sizeof(struct ctdb_ltdb_header)) {
		DEBUG(0,(__location__ " Bad record size\n"));
		return -1;
	}

	hdr = (struct ctdb_ltdb_header *)data.dptr;

	/* use a non-blocking lock */
	if (tdb_chainlock_nonblock(ctdb_db->ltdb->tdb, key) != 0) {
		return -1;
	}

	data = tdb_fetch(ctdb_db->ltdb->tdb, key);
	if (data.dptr == NULL) {
		tdb_chainunlock(ctdb_db->ltdb->tdb, key);
		return 0;
	}

	if (data.dsize < sizeof(struct ctdb_ltdb_header)) {
		if (tdb_lock_nonblock(ctdb_db->ltdb->tdb, -1, F_WRLCK) == 0) {
			tdb_delete(ctdb_db->ltdb->tdb, key);
			tdb_unlock(ctdb_db->ltdb->tdb, -1, F_WRLCK);
			DEBUG(0,(__location__ " Deleted corrupt record\n"));
		}
		tdb_chainunlock(ctdb_db->ltdb->tdb, key);
		free(data.dptr);
		return 0;
	}
	
	hdr2 = (struct ctdb_ltdb_header *)data.dptr;

	if (hdr2->rsn > hdr->rsn) {
		tdb_chainunlock(ctdb_db->ltdb->tdb, key);
		DEBUG(2,(__location__ " Skipping record with rsn=%llu - called with rsn=%llu\n",
			 (unsigned long long)hdr2->rsn, (unsigned long long)hdr->rsn));
		free(data.dptr);
		return -1;		
	}

	if (hdr2->dmaster == ctdb->pnn) {
		tdb_chainunlock(ctdb_db->ltdb->tdb, key);
		DEBUG(2,(__location__ " Attempted delete record where we are the dmaster\n"));
		free(data.dptr);
		return -1;				
	}

	if (tdb_lock_nonblock(ctdb_db->ltdb->tdb, -1, F_WRLCK) != 0) {
		tdb_chainunlock(ctdb_db->ltdb->tdb, key);
		free(data.dptr);
		return -1;				
	}

	if (tdb_delete(ctdb_db->ltdb->tdb, key) != 0) {
		tdb_unlock(ctdb_db->ltdb->tdb, -1, F_WRLCK);
		tdb_chainunlock(ctdb_db->ltdb->tdb, key);
		DEBUG(2,(__location__ " Failed to delete record\n"));
		free(data.dptr);
		return -1;						
	}

	tdb_unlock(ctdb_db->ltdb->tdb, -1, F_WRLCK);
	tdb_chainunlock(ctdb_db->ltdb->tdb, key);
	free(data.dptr);
	return 0;	
}
