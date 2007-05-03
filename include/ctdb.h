/* 
   ctdb database library

   Copyright (C) Andrew Tridgell  2006

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef _CTDB_H
#define _CTDB_H

#define CTDB_IMMEDIATE_MIGRATION	0x00000001
struct ctdb_call {
	int call_id;
	TDB_DATA key;
	TDB_DATA call_data;
	TDB_DATA reply_data;
	uint32_t status;
	uint32_t flags;
};

/*
  structure passed to a ctdb call backend function
*/
struct ctdb_call_info {
	TDB_DATA key;          /* record key */
	TDB_DATA record_data;  /* current data in the record */
	TDB_DATA *new_data;    /* optionally updated record data */
	TDB_DATA *call_data;   /* optionally passed from caller */
	TDB_DATA *reply_data;  /* optionally returned by function */
	uint32_t status;       /* optional reply status - defaults to zero */
};

#define CTDB_ERR_INVALID 1
#define CTDB_ERR_NOMEM 2

/*
  ctdb flags
*/
#define CTDB_FLAG_SELF_CONNECT (1<<0)
#define CTDB_FLAG_TORTURE      (1<<1)


/* 
   a message handler ID meaning "give me all messages"
 */
#define CTDB_SRVID_ALL (~(uint64_t)0)

struct event_context;

/*
  initialise ctdb subsystem
*/
struct ctdb_context *ctdb_init(struct event_context *ev);

/*
  choose the transport
*/
int ctdb_set_transport(struct ctdb_context *ctdb, const char *transport);

/*
  set the directory for the local databases
*/
int ctdb_set_tdb_dir(struct ctdb_context *ctdb, const char *dir);

/*
  set some flags
*/
void ctdb_set_flags(struct ctdb_context *ctdb, unsigned flags);

/*
  clear some flags
*/
void ctdb_clear_flags(struct ctdb_context *ctdb, unsigned flags);

/*
  set max acess count before a dmaster migration
*/
void ctdb_set_max_lacount(struct ctdb_context *ctdb, unsigned count);

/*
  tell ctdb what address to listen on, in transport specific format
*/
int ctdb_set_address(struct ctdb_context *ctdb, const char *address);

int ctdb_set_socketname(struct ctdb_context *ctdb, const char *socketname);

/*
  tell ctdb what nodes are available. This takes a filename, which will contain
  1 node address per line, in a transport specific format
*/
int ctdb_set_nlist(struct ctdb_context *ctdb, const char *nlist);

/*
  start the ctdb protocol
*/
int ctdb_start(struct ctdb_context *ctdb);
int ctdb_start_daemon(struct ctdb_context *ctdb);

/*
  attach to a ctdb database
*/
struct ctdb_db_context *ctdb_attach(struct ctdb_context *ctdb, const char *name);

/*
  find an attached ctdb_db handle given a name
 */
struct ctdb_db_context *ctdb_db_handle(struct ctdb_context *ctdb, const char *name);

/*
  error string for last ctdb error
*/
const char *ctdb_errstr(struct ctdb_context *);

/* a ctdb call function */
typedef int (*ctdb_fn_t)(struct ctdb_call_info *);

/*
  setup a ctdb call function
*/
int ctdb_set_call(struct ctdb_db_context *ctdb_db, ctdb_fn_t fn, uint32_t id);



/*
  make a ctdb call. The associated ctdb call function will be called on the DMASTER
  for the given record
*/
int ctdb_call(struct ctdb_db_context *ctdb_db, struct ctdb_call *call);

/*
  wait for all nodes to be connected - useful for test code
*/
void ctdb_connect_wait(struct ctdb_context *ctdb);

/*
  initiate an ordered ctdb cluster shutdown
  this function will never return
*/
void ctdb_shutdown(struct ctdb_context *ctdb);

/* return vnn of this node */
uint32_t ctdb_get_vnn(struct ctdb_context *ctdb);

/*
  return the number of nodes
*/
uint32_t ctdb_get_num_nodes(struct ctdb_context *ctdb);

/* setup a handler for ctdb messages */
typedef void (*ctdb_message_fn_t)(struct ctdb_context *, uint64_t srvid, 
				  TDB_DATA data, void *);
int ctdb_set_message_handler(struct ctdb_context *ctdb, uint64_t srvid, 
			     ctdb_message_fn_t handler,
			     void *private_data);


int ctdb_call(struct ctdb_db_context *ctdb_db, struct ctdb_call *call);
struct ctdb_client_call_state *ctdb_call_send(struct ctdb_db_context *ctdb_db, struct ctdb_call *call);
int ctdb_call_recv(struct ctdb_client_call_state *state, struct ctdb_call *call);

/* send a ctdb message */
int ctdb_send_message(struct ctdb_context *ctdb, uint32_t vnn,
		      uint64_t srvid, TDB_DATA data);


/* 
   Fetch a ctdb record from a remote node
 . Underneath this will force the
   dmaster for the record to be moved to the local node. 
*/
struct ctdb_record_handle *ctdb_fetch_lock(struct ctdb_db_context *ctdb_db, TALLOC_CTX *mem_ctx, 
					   TDB_DATA key, TDB_DATA *data);

int ctdb_record_store(struct ctdb_record_handle *h, TDB_DATA data);

int ctdb_register_message_handler(struct ctdb_context *ctdb, 
				  TALLOC_CTX *mem_ctx,
				  uint64_t srvid,
				  ctdb_message_fn_t handler,
				  void *private_data);

struct ctdb_db_context *find_ctdb_db(struct ctdb_context *ctdb, uint32_t id);


struct ctdb_context *ctdb_cmdline_client(struct event_context *ev);

struct ctdb_status;
int ctdb_ctrl_status(struct ctdb_context *ctdb, uint32_t destnode, struct ctdb_status *status);

struct ctdb_vnn_map;
int ctdb_ctrl_getvnnmap(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, struct ctdb_vnn_map **vnnmap);
int ctdb_ctrl_setvnnmap(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, struct ctdb_vnn_map *vnnmap);

/* table that contains a list of all dbids on a node
 */
struct ctdb_dbid_map {
	uint32_t num;
	uint32_t dbids[1];
};
int ctdb_ctrl_getdbmap(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, struct ctdb_dbid_map **dbmap);


/* table that contains a list of all nodes a ctdb knows about and their 
   status
 */
struct ctdb_node_and_flags {
	uint32_t vnn;
	uint32_t flags;
};
struct ctdb_node_map {
	uint32_t num;
	struct ctdb_node_and_flags *nodes;
};
int ctdb_ctrl_getnodemap(struct ctdb_context *ctdb, uint32_t destnode, 
		    TALLOC_CTX *mem_ctx, struct ctdb_node_map *nodemap);

struct ctdb_key_list {
	uint32_t dbid;
	uint32_t num;
	TDB_DATA *keys;
	struct ctdb_ltdb_header *headers;
	uint32_t *lmasters;
	TDB_DATA *data;
};
int ctdb_ctrl_pulldb(struct ctdb_context *ctdb, uint32_t destnode, uint32_t dbid, uint32_t lmaster, TALLOC_CTX *mem_ctx, struct ctdb_key_list *keys);
int ctdb_ctrl_copydb(struct ctdb_context *ctdb, uint32_t sourcenode, uint32_t destnode, uint32_t dbid, uint32_t lmaster, TALLOC_CTX *mem_ctx);

int ctdb_ctrl_getdbpath(struct ctdb_context *ctdb, uint32_t dbid, TALLOC_CTX *mem_ctx, const char **path);

int ctdb_ctrl_process_exists(struct ctdb_context *ctdb, uint32_t destnode, pid_t pid);

int ctdb_ctrl_ping(struct ctdb_context *ctdb, uint32_t destnode);

int ctdb_ctrl_get_config(struct ctdb_context *ctdb);

int ctdb_ctrl_get_debuglevel(struct ctdb_context *ctdb, uint32_t destnode, uint32_t *level);
int ctdb_ctrl_set_debuglevel(struct ctdb_context *ctdb, uint32_t destnode, uint32_t level);

/*
  change dmaster for all keys in the database to the new value
 */
int ctdb_ctrl_setdmaster(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, uint32_t dbid, uint32_t dmaster);

/*
  delete all records from a tdb
 */
int ctdb_ctrl_cleardb(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, uint32_t dbid);

/*
  write a record on a specific db (this implicitely updates dmaster of the record to locally be the vnn of the node where the control is executed on)
 */
int ctdb_ctrl_write_record(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, uint32_t dbid, TDB_DATA key, TDB_DATA data);

#define CTDB_RECOVERY_NORMAL		0
#define CTDB_RECOVERY_ACTIVE		1
/*
  get the recovery mode of a remote node
 */
int ctdb_ctrl_getrecmode(struct ctdb_context *ctdb, uint32_t destnode, uint32_t *recmode);
/*
  set the recovery mode of a remote node
 */
int ctdb_ctrl_setrecmode(struct ctdb_context *ctdb, uint32_t destnode, uint32_t recmode);

uint32_t *ctdb_get_connected_nodes(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx,
				   uint32_t *num_nodes);

int ctdb_status_reset(struct ctdb_context *ctdb, uint32_t destnode);

int ctdb_set_logfile(struct ctdb_context *ctdb, const char *logfile);

#endif
