/* 
   ctdb database library

   Copyright (C) Andrew Tridgell  2006

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
#define CTDB_FLAG_TORTURE      (1<<1)

/* 
   a message handler ID meaning "give me all messages"
 */
#define CTDB_SRVID_ALL (~(uint64_t)0)

/*
  srvid type : RECOVERY
*/
#define CTDB_SRVID_RECOVERY	0xF100000000000000LL

/* 
   a message handler ID meaning that the cluster has been reconfigured
 */
#define CTDB_SRVID_RECONFIGURE 0xF200000000000000LL

/* 
   a message handler ID meaning that an IP address has been released
 */
#define CTDB_SRVID_RELEASE_IP 0xF300000000000000LL

/* 
   a message ID meaning that a nodes flags have changed
 */
#define CTDB_SRVID_NODE_FLAGS_CHANGED 0xF400000000000000LL

/* 
   a message ID meaning that a node should be banned
 */
#define CTDB_SRVID_BAN_NODE 0xF500000000000000LL

/* 
   a message ID meaning that a node should be unbanned
 */
#define CTDB_SRVID_UNBAN_NODE 0xF600000000000000LL

/*
  a message to tell the recovery daemon to fetch a set of records
 */
#define CTDB_SRVID_VACUUM_FETCH 0xF700000000000000LL


/* used on the domain socket, send a pdu to the local daemon */
#define CTDB_CURRENT_NODE     0xF0000001
/* send a broadcast to all nodes in the cluster, active or not */
#define CTDB_BROADCAST_ALL    0xF0000002
/* send a broadcast to all nodes in the current vnn map */
#define CTDB_BROADCAST_VNNMAP 0xF0000003
/* send a broadcast to all connected nodes */
#define CTDB_BROADCAST_CONNECTED 0xF0000004


enum control_state {CTDB_CONTROL_WAIT, CTDB_CONTROL_DONE, CTDB_CONTROL_ERROR, CTDB_CONTROL_TIMEOUT};

struct ctdb_client_control_state {
	struct ctdb_context *ctdb;
	uint32_t reqid;
	int32_t status;
	TDB_DATA outdata;
	enum control_state state;
	char *errormsg;
	struct ctdb_req_control *c;

	/* if we have a callback registered for the completion (or failure) of
	   this control
	   if a callback is used, it MUST talloc_free the cb_data passed to it
	*/
	struct {
		void (*fn)(struct ctdb_client_control_state *);
		void *private_data;
	} async;	
};


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
int ctdb_set_tdb_dir_persistent(struct ctdb_context *ctdb, const char *dir);

/*
  set some flags
*/
void ctdb_set_flags(struct ctdb_context *ctdb, unsigned flags);

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
  Check that a specific ip address exists in the node list and returns
  the id for the node or -1
*/
int ctdb_ip_to_nodeid(struct ctdb_context *ctdb, const char *nodeip);

/*
  start the ctdb protocol
*/
int ctdb_start(struct ctdb_context *ctdb);
int ctdb_start_daemon(struct ctdb_context *ctdb, bool do_fork);

/*
  attach to a ctdb database
*/
struct ctdb_db_context *ctdb_attach(struct ctdb_context *ctdb, const char *name, bool persistent);

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
  initiate an ordered ctdb cluster shutdown
  this function will never return
*/
void ctdb_shutdown(struct ctdb_context *ctdb);

/* return pnn of this node */
uint32_t ctdb_get_pnn(struct ctdb_context *ctdb);

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
int ctdb_send_message(struct ctdb_context *ctdb, uint32_t pnn,
		      uint64_t srvid, TDB_DATA data);


/* 
   Fetch a ctdb record from a remote node
 . Underneath this will force the
   dmaster for the record to be moved to the local node. 
*/
struct ctdb_record_handle *ctdb_fetch_lock(struct ctdb_db_context *ctdb_db, TALLOC_CTX *mem_ctx, 
					   TDB_DATA key, TDB_DATA *data);

int ctdb_record_store(struct ctdb_record_handle *h, TDB_DATA data);

int ctdb_fetch(struct ctdb_db_context *ctdb_db, TALLOC_CTX *mem_ctx, 
	       TDB_DATA key, TDB_DATA *data);

int ctdb_register_message_handler(struct ctdb_context *ctdb, 
				  TALLOC_CTX *mem_ctx,
				  uint64_t srvid,
				  ctdb_message_fn_t handler,
				  void *private_data);

struct ctdb_db_context *find_ctdb_db(struct ctdb_context *ctdb, uint32_t id);


struct ctdb_context *ctdb_cmdline_client(struct event_context *ev);

struct ctdb_statistics;
int ctdb_ctrl_statistics(struct ctdb_context *ctdb, uint32_t destnode, struct ctdb_statistics *status);

int ctdb_ctrl_shutdown(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode);

struct ctdb_vnn_map;
int ctdb_ctrl_getvnnmap(struct ctdb_context *ctdb, 
		struct timeval timeout, uint32_t destnode, 
		TALLOC_CTX *mem_ctx, struct ctdb_vnn_map **vnnmap);
int ctdb_ctrl_setvnnmap(struct ctdb_context *ctdb,
		struct timeval timeout, uint32_t destnode, 
		TALLOC_CTX *mem_ctx, struct ctdb_vnn_map *vnnmap);

/* table that contains a list of all dbids on a node
 */
struct ctdb_dbid_map {
	uint32_t num;
	struct ctdb_dbid {
		uint32_t dbid;
		bool persistent;
	} dbs[1];
};
int ctdb_ctrl_getdbmap(struct ctdb_context *ctdb, 
	struct timeval timeout, uint32_t destnode, 
	TALLOC_CTX *mem_ctx, struct ctdb_dbid_map **dbmap);


struct ctdb_node_map;

int ctdb_ctrl_getnodemap(struct ctdb_context *ctdb, 
		    struct timeval timeout, uint32_t destnode, 
		    TALLOC_CTX *mem_ctx, struct ctdb_node_map **nodemap);

int ctdb_ctrl_reload_nodes_file(struct ctdb_context *ctdb, 
		    struct timeval timeout, uint32_t destnode);

struct ctdb_key_list {
	uint32_t dbid;
	uint32_t num;
	TDB_DATA *keys;
	struct ctdb_ltdb_header *headers;
	TDB_DATA *data;
};

int ctdb_ctrl_pulldb(
       struct ctdb_context *ctdb, uint32_t destnode, uint32_t dbid,
       uint32_t lmaster, TALLOC_CTX *mem_ctx,
       struct timeval timeout, TDB_DATA *outdata);

struct ctdb_client_control_state *ctdb_ctrl_pulldb_send(
       struct ctdb_context *ctdb, uint32_t destnode, uint32_t dbid,
       uint32_t lmaster, TALLOC_CTX *mem_ctx, struct timeval timeout);

int ctdb_ctrl_pulldb_recv(
       struct ctdb_context *ctdb,
       TALLOC_CTX *mem_ctx, struct ctdb_client_control_state *state,
       TDB_DATA *outdata);

int ctdb_ctrl_pushdb(
       struct ctdb_context *ctdb, uint32_t destnode, uint32_t dbid,
       TALLOC_CTX *mem_ctx,
       struct timeval timeout, TDB_DATA indata);

struct ctdb_client_control_state *ctdb_ctrl_pushdb_send(
       struct ctdb_context *ctdb, uint32_t destnode, uint32_t dbid,
       TALLOC_CTX *mem_ctx, struct timeval timeout,
       TDB_DATA indata);

int ctdb_ctrl_pushdb_recv(
       struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx,
       struct ctdb_client_control_state *state);


int ctdb_ctrl_copydb(struct ctdb_context *ctdb, 
	struct timeval timeout, uint32_t sourcenode, 
	uint32_t destnode, uint32_t dbid, uint32_t lmaster, 
	TALLOC_CTX *mem_ctx);

int ctdb_ctrl_getdbpath(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, uint32_t dbid, TALLOC_CTX *mem_ctx, const char **path);
int ctdb_ctrl_getdbname(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, uint32_t dbid, TALLOC_CTX *mem_ctx, const char **name);
int ctdb_ctrl_createdb(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, TALLOC_CTX *mem_ctx, const char *name, bool persistent);

int ctdb_ctrl_process_exists(struct ctdb_context *ctdb, uint32_t destnode, pid_t pid);

int ctdb_ctrl_ping(struct ctdb_context *ctdb, uint32_t destnode);

int ctdb_ctrl_get_config(struct ctdb_context *ctdb);

int ctdb_ctrl_get_debuglevel(struct ctdb_context *ctdb, uint32_t destnode, int32_t *level);
int ctdb_ctrl_set_debuglevel(struct ctdb_context *ctdb, uint32_t destnode, int32_t level);

/*
  change dmaster for all keys in the database to the new value
 */
int ctdb_ctrl_setdmaster(struct ctdb_context *ctdb, 
	struct timeval timeout, uint32_t destnode, 
	TALLOC_CTX *mem_ctx, uint32_t dbid, uint32_t dmaster);

/*
  write a record on a specific db (this implicitely updates dmaster of the record to locally be the vnn of the node where the control is executed on)
 */
int ctdb_ctrl_write_record(struct ctdb_context *ctdb, uint32_t destnode, TALLOC_CTX *mem_ctx, uint32_t dbid, TDB_DATA key, TDB_DATA data);

#define CTDB_RECOVERY_NORMAL		0
#define CTDB_RECOVERY_ACTIVE		1

/*
  get the recovery mode of a remote node
 */
int ctdb_ctrl_getrecmode(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct timeval timeout, uint32_t destnode, uint32_t *recmode);

struct ctdb_client_control_state *ctdb_ctrl_getrecmode_send(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_getrecmode_recv(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct ctdb_client_control_state *state, uint32_t *recmode);


/*
  set the recovery mode of a remote node
 */
int ctdb_ctrl_setrecmode(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, uint32_t recmode);
/*
  get the monitoring mode of a remote node
 */
int ctdb_ctrl_getmonmode(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, uint32_t *monmode);

/*
  set the monitoring mode of a remote node to active
 */
int ctdb_ctrl_enable_monmode(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode);

/*
  set the monitoring mode of a remote node to disabled
 */
int ctdb_ctrl_disable_monmode(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode);


/*
  get the recovery master of a remote node
 */
int ctdb_ctrl_getrecmaster(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct timeval timeout, uint32_t destnode, uint32_t *recmaster);

struct ctdb_client_control_state *ctdb_ctrl_getrecmaster_send(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_getrecmaster_recv(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct ctdb_client_control_state *state, uint32_t *recmaster);



/*
  set the recovery master of a remote node
 */
int ctdb_ctrl_setrecmaster(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, uint32_t recmaster);

uint32_t *ctdb_get_connected_nodes(struct ctdb_context *ctdb, 
				   struct timeval timeout, 
				   TALLOC_CTX *mem_ctx,
				   uint32_t *num_nodes);

int ctdb_statistics_reset(struct ctdb_context *ctdb, uint32_t destnode);

int ctdb_set_logfile(struct ctdb_context *ctdb, const char *logfile, bool use_syslog);

typedef int (*ctdb_traverse_func)(struct ctdb_context *, TDB_DATA, TDB_DATA, void *);
int ctdb_traverse(struct ctdb_db_context *ctdb_db, ctdb_traverse_func fn, void *private_data);

int ctdb_dump_db(struct ctdb_db_context *ctdb_db, FILE *f);

/*
  get the pid of a ctdb daemon
 */
int ctdb_ctrl_getpid(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode, uint32_t *pid);

int ctdb_ctrl_freeze(struct ctdb_context *ctdb, struct timeval timeout, 
			uint32_t destnode);

struct ctdb_client_control_state *
ctdb_ctrl_freeze_send(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, 
			struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_freeze_recv(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, 
			struct ctdb_client_control_state *state);

int ctdb_ctrl_thaw(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_getpnn(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_get_tunable(struct ctdb_context *ctdb, 
			  struct timeval timeout, 
			  uint32_t destnode,
			  const char *name, uint32_t *value);

int ctdb_ctrl_set_tunable(struct ctdb_context *ctdb, 
			  struct timeval timeout, 
			  uint32_t destnode,
			  const char *name, uint32_t value);

int ctdb_ctrl_list_tunables(struct ctdb_context *ctdb, 
			    struct timeval timeout, 
			    uint32_t destnode,
			    TALLOC_CTX *mem_ctx,
			    const char ***list, uint32_t *count);

int ctdb_ctrl_modflags(struct ctdb_context *ctdb, 
		       struct timeval timeout, 
		       uint32_t destnode, 
		       uint32_t set, uint32_t clear);

enum ctdb_server_id_type { SERVER_TYPE_SAMBA=1 };

struct ctdb_server_id {
	enum ctdb_server_id_type type;
	uint32_t pnn;
	uint32_t server_id;
};

struct ctdb_server_id_list {
	uint32_t num;
	struct ctdb_server_id server_ids[1];
};


int ctdb_ctrl_register_server_id(struct ctdb_context *ctdb,
		struct timeval timeout,
		struct ctdb_server_id *id);
int ctdb_ctrl_unregister_server_id(struct ctdb_context *ctdb, 
		struct timeval timeout, 
		struct ctdb_server_id *id);
int ctdb_ctrl_check_server_id(struct ctdb_context *ctdb,
		struct timeval timeout, uint32_t destnode, 
		struct ctdb_server_id *id, uint32_t *status);
int ctdb_ctrl_get_server_id_list(struct ctdb_context *ctdb,
		TALLOC_CTX *mem_ctx,
		struct timeval timeout, uint32_t destnode, 
		struct ctdb_server_id_list **svid_list);

struct ctdb_uptime {
	struct timeval current_time;
	struct timeval ctdbd_start_time;
	struct timeval last_recovery_time;
};

int ctdb_socket_connect(struct ctdb_context *ctdb);

/*
  get the uptime of a remote node
 */
int ctdb_ctrl_uptime(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct timeval timeout, uint32_t destnode, struct ctdb_uptime **uptime);

struct ctdb_client_control_state *ctdb_ctrl_uptime_send(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_uptime_recv(struct ctdb_context *ctdb, TALLOC_CTX *mem_ctx, struct ctdb_client_control_state *state, struct ctdb_uptime **uptime);

int ctdb_ctrl_end_recovery(struct ctdb_context *ctdb, struct timeval timeout, uint32_t destnode);

int ctdb_ctrl_getreclock(struct ctdb_context *ctdb, 
	struct timeval timeout, uint32_t destnode, 
	TALLOC_CTX *mem_ctx, const char **reclock);

uint32_t *list_of_active_nodes(struct ctdb_context *ctdb,
				struct ctdb_node_map *node_map,
				TALLOC_CTX *mem_ctx,
				bool include_self);
uint32_t *list_of_vnnmap_nodes(struct ctdb_context *ctdb,
				struct ctdb_vnn_map *vnn_map,
				TALLOC_CTX *mem_ctx,
				bool include_self);

int ctdb_read_pnn_lock(int fd, int32_t pnn);

#endif
