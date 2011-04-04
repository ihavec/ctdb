/* 
   ctdb daemon code

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

#include "includes.h"
#include "db_wrap.h"
#include "lib/tdb/include/tdb.h"
#include "lib/tevent/tevent.h"
#include "lib/util/dlinklist.h"
#include "system/network.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "../include/ctdb_client.h"
#include "../include/ctdb_private.h"
#include <sys/socket.h>

struct ctdb_client_pid_list {
	struct ctdb_client_pid_list *next, *prev;
	struct ctdb_context *ctdb;
	pid_t pid;
	struct ctdb_client *client;
};

static void daemon_incoming_packet(void *, struct ctdb_req_header *);

static void print_exit_message(void)
{
	DEBUG(DEBUG_NOTICE,("CTDB daemon shutting down\n"));
}



static void ctdb_time_tick(struct event_context *ev, struct timed_event *te, 
				  struct timeval t, void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);

	if (getpid() != ctdb->ctdbd_pid) {
		return;
	}

	event_add_timed(ctdb->ev, ctdb, 
			timeval_current_ofs(1, 0), 
			ctdb_time_tick, ctdb);
}

/* Used to trigger a dummy event once per second, to make
 * detection of hangs more reliable.
 */
static void ctdb_start_time_tickd(struct ctdb_context *ctdb)
{
	event_add_timed(ctdb->ev, ctdb, 
			timeval_current_ofs(1, 0), 
			ctdb_time_tick, ctdb);
}


/* called when the "startup" event script has finished */
static void ctdb_start_transport(struct ctdb_context *ctdb)
{
	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ALERT,(__location__ " startup event finished but transport is DOWN.\n"));
		ctdb_fatal(ctdb, "transport is not initialized but startup completed");
	}

	/* start the transport running */
	if (ctdb->methods->start(ctdb) != 0) {
		DEBUG(DEBUG_ALERT,("transport failed to start!\n"));
		ctdb_fatal(ctdb, "transport failed to start");
	}

	/* start the recovery daemon process */
	if (ctdb_start_recoverd(ctdb) != 0) {
		DEBUG(DEBUG_ALERT,("Failed to start recovery daemon\n"));
		exit(11);
	}

	/* Make sure we log something when the daemon terminates */
	atexit(print_exit_message);

	/* start monitoring for connected/disconnected nodes */
	ctdb_start_keepalive(ctdb);

	/* start monitoring for node health */
	ctdb_start_monitoring(ctdb);

	/* start periodic update of tcp tickle lists */
       	ctdb_start_tcp_tickle_update(ctdb);

	/* start listening for recovery daemon pings */
	ctdb_control_recd_ping(ctdb);

	/* start listening to timer ticks */
	ctdb_start_time_tickd(ctdb);
}

static void block_signal(int signum)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, signum);
	sigaction(signum, &act, NULL);
}


/*
  send a packet to a client
 */
static int daemon_queue_send(struct ctdb_client *client, struct ctdb_req_header *hdr)
{
	CTDB_INCREMENT_STAT(client->ctdb, client_packets_sent);
	if (hdr->operation == CTDB_REQ_MESSAGE) {
		if (ctdb_queue_length(client->queue) > client->ctdb->tunable.max_queue_depth_drop_msg) {
			DEBUG(DEBUG_ERR,("CTDB_REQ_MESSAGE queue full - killing client connection.\n"));
			talloc_free(client);
			return -1;
		}
	}
	return ctdb_queue_send(client->queue, (uint8_t *)hdr, hdr->length);
}

/*
  message handler for when we are in daemon mode. This redirects the message
  to the right client
 */
static void daemon_message_handler(struct ctdb_context *ctdb, uint64_t srvid, 
				    TDB_DATA data, void *private_data)
{
	struct ctdb_client *client = talloc_get_type(private_data, struct ctdb_client);
	struct ctdb_req_message *r;
	int len;

	/* construct a message to send to the client containing the data */
	len = offsetof(struct ctdb_req_message, data) + data.dsize;
	r = ctdbd_allocate_pkt(ctdb, ctdb, CTDB_REQ_MESSAGE, 
			       len, struct ctdb_req_message);
	CTDB_NO_MEMORY_VOID(ctdb, r);

	talloc_set_name_const(r, "req_message packet");

	r->srvid         = srvid;
	r->datalen       = data.dsize;
	memcpy(&r->data[0], data.dptr, data.dsize);

	daemon_queue_send(client, &r->hdr);

	talloc_free(r);
}

/*
  this is called when the ctdb daemon received a ctdb request to 
  set the srvid from the client
 */
int daemon_register_message_handler(struct ctdb_context *ctdb, uint32_t client_id, uint64_t srvid)
{
	struct ctdb_client *client = ctdb_reqid_find(ctdb, client_id, struct ctdb_client);
	int res;
	if (client == NULL) {
		DEBUG(DEBUG_ERR,("Bad client_id in daemon_request_register_message_handler\n"));
		return -1;
	}
	res = ctdb_register_message_handler(ctdb, client, srvid, daemon_message_handler, client);
	if (res != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to register handler %llu in daemon\n", 
			 (unsigned long long)srvid));
	} else {
		DEBUG(DEBUG_INFO,(__location__ " Registered message handler for srvid=%llu\n", 
			 (unsigned long long)srvid));
	}

	return res;
}

/*
  this is called when the ctdb daemon received a ctdb request to 
  remove a srvid from the client
 */
int daemon_deregister_message_handler(struct ctdb_context *ctdb, uint32_t client_id, uint64_t srvid)
{
	struct ctdb_client *client = ctdb_reqid_find(ctdb, client_id, struct ctdb_client);
	if (client == NULL) {
		DEBUG(DEBUG_ERR,("Bad client_id in daemon_request_deregister_message_handler\n"));
		return -1;
	}
	return ctdb_deregister_message_handler(ctdb, srvid, client);
}


/*
  destroy a ctdb_client
*/
static int ctdb_client_destructor(struct ctdb_client *client)
{
	struct ctdb_db_context *ctdb_db;

	ctdb_takeover_client_destructor_hook(client);
	ctdb_reqid_remove(client->ctdb, client->client_id);
	CTDB_DECREMENT_STAT(client->ctdb, num_clients);

	if (client->num_persistent_updates != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Client disconnecting with %u persistent updates in flight. Starting recovery\n", client->num_persistent_updates));
		client->ctdb->recovery_mode = CTDB_RECOVERY_ACTIVE;
	}
	ctdb_db = find_ctdb_db(client->ctdb, client->db_id);
	if (ctdb_db) {
		DEBUG(DEBUG_ERR, (__location__ " client exit while transaction "
				  "commit active. Forcing recovery.\n"));
		client->ctdb->recovery_mode = CTDB_RECOVERY_ACTIVE;

		/* legacy trans2 transaction state: */
		ctdb_db->transaction_active = false;

		/*
		 * trans3 transaction state:
		 *
		 * The destructor sets the pointer to NULL.
		 */
		talloc_free(ctdb_db->persistent_state);
	}

	return 0;
}


/*
  this is called when the ctdb daemon received a ctdb request message
  from a local client over the unix domain socket
 */
static void daemon_request_message_from_client(struct ctdb_client *client, 
					       struct ctdb_req_message *c)
{
	TDB_DATA data;
	int res;

	/* maybe the message is for another client on this node */
	if (ctdb_get_pnn(client->ctdb)==c->hdr.destnode) {
		ctdb_request_message(client->ctdb, (struct ctdb_req_header *)c);
		return;
	}

	/* its for a remote node */
	data.dptr = &c->data[0];
	data.dsize = c->datalen;
	res = ctdb_daemon_send_message(client->ctdb, c->hdr.destnode,
				       c->srvid, data);
	if (res != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send message to remote node %u\n",
			 c->hdr.destnode));
	}
}


struct daemon_call_state {
	struct ctdb_client *client;
	uint32_t reqid;
	struct ctdb_call *call;
	struct timeval start_time;
};

/* 
   complete a call from a client 
*/
static void daemon_call_from_client_callback(struct ctdb_call_state *state)
{
	struct daemon_call_state *dstate = talloc_get_type(state->async.private_data, 
							   struct daemon_call_state);
	struct ctdb_reply_call *r;
	int res;
	uint32_t length;
	struct ctdb_client *client = dstate->client;
	struct ctdb_db_context *ctdb_db = state->ctdb_db;

	talloc_steal(client, dstate);
	talloc_steal(dstate, dstate->call);

	res = ctdb_daemon_call_recv(state, dstate->call);
	if (res != 0) {
		DEBUG(DEBUG_ERR, (__location__ " ctdbd_call_recv() returned error\n"));
		CTDB_DECREMENT_STAT(client->ctdb, pending_calls);

		CTDB_UPDATE_LATENCY(client->ctdb, ctdb_db, "call_from_client_cb 1", call_latency, dstate->start_time);
		return;
	}

	length = offsetof(struct ctdb_reply_call, data) + dstate->call->reply_data.dsize;
	r = ctdbd_allocate_pkt(client->ctdb, dstate, CTDB_REPLY_CALL, 
			       length, struct ctdb_reply_call);
	if (r == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to allocate reply_call in ctdb daemon\n"));
		CTDB_DECREMENT_STAT(client->ctdb, pending_calls);
		CTDB_UPDATE_LATENCY(client->ctdb, ctdb_db, "call_from_client_cb 2", call_latency, dstate->start_time);
		return;
	}
	r->hdr.reqid        = dstate->reqid;
	r->datalen          = dstate->call->reply_data.dsize;
	memcpy(&r->data[0], dstate->call->reply_data.dptr, r->datalen);

	res = daemon_queue_send(client, &r->hdr);
	if (res == -1) {
		/* client is dead - return immediately */
		return;
	}
	if (res != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to queue packet from daemon to client\n"));
	}
	CTDB_UPDATE_LATENCY(client->ctdb, ctdb_db, "call_from_client_cb 3", call_latency, dstate->start_time);
	CTDB_DECREMENT_STAT(client->ctdb, pending_calls);
	talloc_free(dstate);
}

struct ctdb_daemon_packet_wrap {
	struct ctdb_context *ctdb;
	uint32_t client_id;
};

/*
  a wrapper to catch disconnected clients
 */
static void daemon_incoming_packet_wrap(void *p, struct ctdb_req_header *hdr)
{
	struct ctdb_client *client;
	struct ctdb_daemon_packet_wrap *w = talloc_get_type(p, 
							    struct ctdb_daemon_packet_wrap);
	if (w == NULL) {
		DEBUG(DEBUG_CRIT,(__location__ " Bad packet type '%s'\n", talloc_get_name(p)));
		return;
	}

	client = ctdb_reqid_find(w->ctdb, w->client_id, struct ctdb_client);
	if (client == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Packet for disconnected client %u\n",
			 w->client_id));
		talloc_free(w);
		return;
	}
	talloc_free(w);

	/* process it */
	daemon_incoming_packet(client, hdr);	
}


/*
  this is called when the ctdb daemon received a ctdb request call
  from a local client over the unix domain socket
 */
static void daemon_request_call_from_client(struct ctdb_client *client, 
					    struct ctdb_req_call *c)
{
	struct ctdb_call_state *state;
	struct ctdb_db_context *ctdb_db;
	struct daemon_call_state *dstate;
	struct ctdb_call *call;
	struct ctdb_ltdb_header header;
	TDB_DATA key, data;
	int ret;
	struct ctdb_context *ctdb = client->ctdb;
	struct ctdb_daemon_packet_wrap *w;

	CTDB_INCREMENT_STAT(ctdb, total_calls);
	CTDB_DECREMENT_STAT(ctdb, pending_calls);

	ctdb_db = find_ctdb_db(client->ctdb, c->db_id);
	if (!ctdb_db) {
		DEBUG(DEBUG_ERR, (__location__ " Unknown database in request. db_id==0x%08x",
			  c->db_id));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	if (ctdb_db->unhealthy_reason) {
		/*
		 * this is just a warning, as the tdb should be empty anyway,
		 * and only persistent databases can be unhealthy, which doesn't
		 * use this code patch
		 */
		DEBUG(DEBUG_WARNING,("warn: db(%s) unhealty in daemon_request_call_from_client(): %s\n",
				     ctdb_db->db_name, ctdb_db->unhealthy_reason));
	}

	key.dptr = c->data;
	key.dsize = c->keylen;

	w = talloc(ctdb, struct ctdb_daemon_packet_wrap);
	CTDB_NO_MEMORY_VOID(ctdb, w);	

	w->ctdb = ctdb;
	w->client_id = client->client_id;

	ret = ctdb_ltdb_lock_fetch_requeue(ctdb_db, key, &header, 
					   (struct ctdb_req_header *)c, &data,
					   daemon_incoming_packet_wrap, w, True);
	if (ret == -2) {
		/* will retry later */
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	talloc_free(w);

	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Unable to fetch record\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	dstate = talloc(client, struct daemon_call_state);
	if (dstate == NULL) {
		ret = ctdb_ltdb_unlock(ctdb_db, key);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
		}

		DEBUG(DEBUG_ERR,(__location__ " Unable to allocate dstate\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}
	dstate->start_time = timeval_current();
	dstate->client = client;
	dstate->reqid  = c->hdr.reqid;
	talloc_steal(dstate, data.dptr);

	call = dstate->call = talloc_zero(dstate, struct ctdb_call);
	if (call == NULL) {
		ret = ctdb_ltdb_unlock(ctdb_db, key);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
		}

		DEBUG(DEBUG_ERR,(__location__ " Unable to allocate call\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		CTDB_UPDATE_LATENCY(ctdb, ctdb_db, "call_from_client 1", call_latency, dstate->start_time);
		return;
	}

	call->call_id = c->callid;
	call->key = key;
	call->call_data.dptr = c->data + c->keylen;
	call->call_data.dsize = c->calldatalen;
	call->flags = c->flags;

	if (header.dmaster == ctdb->pnn) {
		state = ctdb_call_local_send(ctdb_db, call, &header, &data);
	} else {
		state = ctdb_daemon_call_send_remote(ctdb_db, call, &header);
	}

	ret = ctdb_ltdb_unlock(ctdb_db, key);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
	}

	if (state == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Unable to setup call send\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		CTDB_UPDATE_LATENCY(ctdb, ctdb_db, "call_from_client 2", call_latency, dstate->start_time);
		return;
	}
	talloc_steal(state, dstate);
	talloc_steal(client, state);

	state->async.fn = daemon_call_from_client_callback;
	state->async.private_data = dstate;
}


static void daemon_request_control_from_client(struct ctdb_client *client, 
					       struct ctdb_req_control *c);

/* data contains a packet from the client */
static void daemon_incoming_packet(void *p, struct ctdb_req_header *hdr)
{
	struct ctdb_client *client = talloc_get_type(p, struct ctdb_client);
	TALLOC_CTX *tmp_ctx;
	struct ctdb_context *ctdb = client->ctdb;

	/* place the packet as a child of a tmp_ctx. We then use
	   talloc_free() below to free it. If any of the calls want
	   to keep it, then they will steal it somewhere else, and the
	   talloc_free() will be a no-op */
	tmp_ctx = talloc_new(client);
	talloc_steal(tmp_ctx, hdr);

	if (hdr->ctdb_magic != CTDB_MAGIC) {
		ctdb_set_error(client->ctdb, "Non CTDB packet rejected in daemon\n");
		goto done;
	}

	if (hdr->ctdb_version != CTDB_VERSION) {
		ctdb_set_error(client->ctdb, "Bad CTDB version 0x%x rejected in daemon\n", hdr->ctdb_version);
		goto done;
	}

	switch (hdr->operation) {
	case CTDB_REQ_CALL:
		CTDB_INCREMENT_STAT(ctdb, client.req_call);
		daemon_request_call_from_client(client, (struct ctdb_req_call *)hdr);
		break;

	case CTDB_REQ_MESSAGE:
		CTDB_INCREMENT_STAT(ctdb, client.req_message);
		daemon_request_message_from_client(client, (struct ctdb_req_message *)hdr);
		break;

	case CTDB_REQ_CONTROL:
		CTDB_INCREMENT_STAT(ctdb, client.req_control);
		daemon_request_control_from_client(client, (struct ctdb_req_control *)hdr);
		break;

	default:
		DEBUG(DEBUG_CRIT,(__location__ " daemon: unrecognized operation %u\n",
			 hdr->operation));
	}

done:
	talloc_free(tmp_ctx);
}

/*
  called when the daemon gets a incoming packet
 */
static void ctdb_daemon_read_cb(uint8_t *data, size_t cnt, void *args)
{
	struct ctdb_client *client = talloc_get_type(args, struct ctdb_client);
	struct ctdb_req_header *hdr;

	if (cnt == 0) {
		talloc_free(client);
		return;
	}

	CTDB_INCREMENT_STAT(client->ctdb, client_packets_recv);

	if (cnt < sizeof(*hdr)) {
		ctdb_set_error(client->ctdb, "Bad packet length %u in daemon\n", 
			       (unsigned)cnt);
		return;
	}
	hdr = (struct ctdb_req_header *)data;
	if (cnt != hdr->length) {
		ctdb_set_error(client->ctdb, "Bad header length %u expected %u\n in daemon", 
			       (unsigned)hdr->length, (unsigned)cnt);
		return;
	}

	if (hdr->ctdb_magic != CTDB_MAGIC) {
		ctdb_set_error(client->ctdb, "Non CTDB packet rejected\n");
		return;
	}

	if (hdr->ctdb_version != CTDB_VERSION) {
		ctdb_set_error(client->ctdb, "Bad CTDB version 0x%x rejected in daemon\n", hdr->ctdb_version);
		return;
	}

	DEBUG(DEBUG_DEBUG,(__location__ " client request %u of type %u length %u from "
		 "node %u to %u\n", hdr->reqid, hdr->operation, hdr->length,
		 hdr->srcnode, hdr->destnode));

	/* it is the responsibility of the incoming packet function to free 'data' */
	daemon_incoming_packet(client, hdr);
}


static int ctdb_clientpid_destructor(struct ctdb_client_pid_list *client_pid)
{
	if (client_pid->ctdb->client_pids != NULL) {
		DLIST_REMOVE(client_pid->ctdb->client_pids, client_pid);
	}

	return 0;
}


static void ctdb_accept_client(struct event_context *ev, struct fd_event *fde, 
			 uint16_t flags, void *private_data)
{
	struct sockaddr_un addr;
	socklen_t len;
	int fd;
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);
	struct ctdb_client *client;
	struct ctdb_client_pid_list *client_pid;
#ifdef _AIX
	struct peercred_struct cr;
	socklen_t crl = sizeof(struct peercred_struct);
#else
	struct ucred cr;
	socklen_t crl = sizeof(struct ucred);
#endif

	memset(&addr, 0, sizeof(addr));
	len = sizeof(addr);
	fd = accept(ctdb->daemon.sd, (struct sockaddr *)&addr, &len);
	if (fd == -1) {
		return;
	}

	set_nonblocking(fd);
	set_close_on_exec(fd);

	DEBUG(DEBUG_DEBUG,(__location__ " Created SOCKET FD:%d to connected child\n", fd));

	client = talloc_zero(ctdb, struct ctdb_client);
#ifdef _AIX
	if (getsockopt(fd, SOL_SOCKET, SO_PEERID, &cr, &crl) == 0) {
#else
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &crl) == 0) {
#endif
		DEBUG(DEBUG_INFO,("Connected client with pid:%u\n", (unsigned)cr.pid));
	}

	client->ctdb = ctdb;
	client->fd = fd;
	client->client_id = ctdb_reqid_new(ctdb, client);
	client->pid = cr.pid;

	client_pid = talloc(client, struct ctdb_client_pid_list);
	if (client_pid == NULL) {
		DEBUG(DEBUG_ERR,("Failed to allocate client pid structure\n"));
		close(fd);
		talloc_free(client);
		return;
	}		
	client_pid->ctdb   = ctdb;
	client_pid->pid    = cr.pid;
	client_pid->client = client;

	DLIST_ADD(ctdb->client_pids, client_pid);

	client->queue = ctdb_queue_setup(ctdb, client, fd, CTDB_DS_ALIGNMENT, 
					 ctdb_daemon_read_cb, client,
					 "client-%u", client->pid);

	talloc_set_destructor(client, ctdb_client_destructor);
	talloc_set_destructor(client_pid, ctdb_clientpid_destructor);
	CTDB_INCREMENT_STAT(ctdb, num_clients);
}



/*
  create a unix domain socket and bind it
  return a file descriptor open on the socket 
*/
static int ux_socket_bind(struct ctdb_context *ctdb)
{
	struct sockaddr_un addr;

	ctdb->daemon.sd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ctdb->daemon.sd == -1) {
		return -1;
	}

	set_close_on_exec(ctdb->daemon.sd);
	set_nonblocking(ctdb->daemon.sd);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, ctdb->daemon.name, sizeof(addr.sun_path));

	if (bind(ctdb->daemon.sd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		DEBUG(DEBUG_CRIT,("Unable to bind on ctdb socket '%s'\n", ctdb->daemon.name));
		goto failed;
	}	

	if (chown(ctdb->daemon.name, geteuid(), getegid()) != 0 ||
	    chmod(ctdb->daemon.name, 0700) != 0) {
		DEBUG(DEBUG_CRIT,("Unable to secure ctdb socket '%s', ctdb->daemon.name\n", ctdb->daemon.name));
		goto failed;
	} 


	if (listen(ctdb->daemon.sd, 100) != 0) {
		DEBUG(DEBUG_CRIT,("Unable to listen on ctdb socket '%s'\n", ctdb->daemon.name));
		goto failed;
	}

	return 0;

failed:
	close(ctdb->daemon.sd);
	ctdb->daemon.sd = -1;
	return -1;	
}

static void sig_child_handler(struct event_context *ev,
	struct signal_event *se, int signum, int count,
	void *dont_care, 
	void *private_data)
{
//	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);
	int status;
	pid_t pid = -1;

	while (pid != 0) {
		pid = waitpid(-1, &status, WNOHANG);
		if (pid == -1) {
			DEBUG(DEBUG_ERR, (__location__ " waitpid() returned error. errno:%d\n", errno));
			return;
		}
		if (pid > 0) {
			DEBUG(DEBUG_DEBUG, ("SIGCHLD from %d\n", (int)pid));
		}
	}
}

static void ctdb_setup_event_callback(struct ctdb_context *ctdb, int status,
				      void *private_data)
{
	if (status != 0) {
		ctdb_fatal(ctdb, "Failed to run setup event\n");
		return;
	}
	ctdb_run_notification_script(ctdb, "setup");

	/* tell all other nodes we've just started up */
	ctdb_daemon_send_control(ctdb, CTDB_BROADCAST_ALL,
				 0, CTDB_CONTROL_STARTUP, 0,
				 CTDB_CTRL_FLAG_NOREPLY,
				 tdb_null, NULL, NULL);
}

/*
  start the protocol going as a daemon
*/
int ctdb_start_daemon(struct ctdb_context *ctdb, bool do_fork, bool use_syslog, const char *public_address_list)
{
	int res, ret = -1;
	struct fd_event *fde;
	const char *domain_socket_name;
	struct signal_event *se;

	/* get rid of any old sockets */
	unlink(ctdb->daemon.name);

	/* create a unix domain stream socket to listen to */
	res = ux_socket_bind(ctdb);
	if (res!=0) {
		DEBUG(DEBUG_ALERT,(__location__ " Failed to open CTDB unix domain socket\n"));
		exit(10);
	}

	if (do_fork && fork()) {
		return 0;
	}

	tdb_reopen_all(False);

	if (do_fork) {
		setsid();
		close(0);
		if (open("/dev/null", O_RDONLY) != 0) {
			DEBUG(DEBUG_ALERT,(__location__ " Failed to setup stdin on /dev/null\n"));
			exit(11);
		}
	}
	block_signal(SIGPIPE);

	ctdb->ctdbd_pid = getpid();


	DEBUG(DEBUG_ERR, ("Starting CTDBD as pid : %u\n", ctdb->ctdbd_pid));

	if (ctdb->do_setsched) {
		/* try to set us up as realtime */
		ctdb_set_scheduler(ctdb);
	}

	/* ensure the socket is deleted on exit of the daemon */
	domain_socket_name = talloc_strdup(talloc_autofree_context(), ctdb->daemon.name);
	if (domain_socket_name == NULL) {
		DEBUG(DEBUG_ALERT,(__location__ " talloc_strdup failed.\n"));
		exit(12);
	}

	ctdb->ev = event_context_init(NULL);
	tevent_loop_allow_nesting(ctdb->ev);
	ret = ctdb_init_tevent_logging(ctdb);
	if (ret != 0) {
		DEBUG(DEBUG_ALERT,("Failed to initialize TEVENT logging\n"));
		exit(1);
	}

	ctdb_set_child_logging(ctdb);

	/* initialize statistics collection */
	ctdb_statistics_init(ctdb);

	/* force initial recovery for election */
	ctdb->recovery_mode = CTDB_RECOVERY_ACTIVE;

	if (strcmp(ctdb->transport, "tcp") == 0) {
		int ctdb_tcp_init(struct ctdb_context *);
		ret = ctdb_tcp_init(ctdb);
	}
#ifdef USE_INFINIBAND
	if (strcmp(ctdb->transport, "ib") == 0) {
		int ctdb_ibw_init(struct ctdb_context *);
		ret = ctdb_ibw_init(ctdb);
	}
#endif
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to initialise transport '%s'\n", ctdb->transport));
		return -1;
	}

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ALERT,(__location__ " Can not initialize transport. ctdb->methods is NULL\n"));
		ctdb_fatal(ctdb, "transport is unavailable. can not initialize.");
	}

	/* initialise the transport  */
	if (ctdb->methods->initialise(ctdb) != 0) {
		ctdb_fatal(ctdb, "transport failed to initialise");
	}
	if (public_address_list) {
		ret = ctdb_set_public_addresses(ctdb, public_address_list);
		if (ret == -1) {
			DEBUG(DEBUG_ALERT,("Unable to setup public address list\n"));
			exit(1);
		}
	}


	/* attach to existing databases */
	if (ctdb_attach_databases(ctdb) != 0) {
		ctdb_fatal(ctdb, "Failed to attach to databases\n");
	}

	ret = ctdb_event_script(ctdb, CTDB_EVENT_INIT);
	if (ret != 0) {
		ctdb_fatal(ctdb, "Failed to run init event\n");
	}
	ctdb_run_notification_script(ctdb, "init");

	/* start frozen, then let the first election sort things out */
	if (ctdb_blocking_freeze(ctdb)) {
		ctdb_fatal(ctdb, "Failed to get initial freeze\n");
	}

	/* now start accepting clients, only can do this once frozen */
	fde = event_add_fd(ctdb->ev, ctdb, ctdb->daemon.sd, 
			   EVENT_FD_READ,
			   ctdb_accept_client, ctdb);
	tevent_fd_set_auto_close(fde);

	/* release any IPs we hold from previous runs of the daemon */
	if (ctdb->tunable.disable_ip_failover == 0) {
		ctdb_release_all_ips(ctdb);
	}

	/* start the transport going */
	ctdb_start_transport(ctdb);

	/* set up a handler to pick up sigchld */
	se = event_add_signal(ctdb->ev, ctdb,
				     SIGCHLD, 0,
				     sig_child_handler,
				     ctdb);
	if (se == NULL) {
		DEBUG(DEBUG_CRIT,("Failed to set up signal handler for SIGCHLD\n"));
		exit(1);
	}

	ret = ctdb_event_script_callback(ctdb,
					 ctdb,
					 ctdb_setup_event_callback,
					 ctdb,
					 false,
					 CTDB_EVENT_SETUP,
					 "");
	if (ret != 0) {
		DEBUG(DEBUG_CRIT,("Failed to set up 'setup' event\n"));
		exit(1);
	}

	if (use_syslog) {
		if (start_syslog_daemon(ctdb)) {
			DEBUG(DEBUG_CRIT, ("Failed to start syslog daemon\n"));
			exit(10);
		}
	}

	ctdb_lockdown_memory(ctdb);
	  
	/* go into a wait loop to allow other nodes to complete */
	event_loop_wait(ctdb->ev);

	DEBUG(DEBUG_CRIT,("event_loop_wait() returned. this should not happen\n"));
	exit(1);
}

/*
  allocate a packet for use in daemon<->daemon communication
 */
struct ctdb_req_header *_ctdb_transport_allocate(struct ctdb_context *ctdb,
						 TALLOC_CTX *mem_ctx, 
						 enum ctdb_operation operation, 
						 size_t length, size_t slength,
						 const char *type)
{
	int size;
	struct ctdb_req_header *hdr;

	length = MAX(length, slength);
	size = (length+(CTDB_DS_ALIGNMENT-1)) & ~(CTDB_DS_ALIGNMENT-1);

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_INFO,(__location__ " Unable to allocate transport packet for operation %u of length %u. Transport is DOWN.\n",
			 operation, (unsigned)length));
		return NULL;
	}

	hdr = (struct ctdb_req_header *)ctdb->methods->allocate_pkt(mem_ctx, size);
	if (hdr == NULL) {
		DEBUG(DEBUG_ERR,("Unable to allocate transport packet for operation %u of length %u\n",
			 operation, (unsigned)length));
		return NULL;
	}
	talloc_set_name_const(hdr, type);
	memset(hdr, 0, slength);
	hdr->length       = length;
	hdr->operation    = operation;
	hdr->ctdb_magic   = CTDB_MAGIC;
	hdr->ctdb_version = CTDB_VERSION;
	hdr->generation   = ctdb->vnn_map->generation;
	hdr->srcnode      = ctdb->pnn;

	return hdr;	
}

struct daemon_control_state {
	struct daemon_control_state *next, *prev;
	struct ctdb_client *client;
	struct ctdb_req_control *c;
	uint32_t reqid;
	struct ctdb_node *node;
};

/*
  callback when a control reply comes in
 */
static void daemon_control_callback(struct ctdb_context *ctdb,
				    int32_t status, TDB_DATA data, 
				    const char *errormsg,
				    void *private_data)
{
	struct daemon_control_state *state = talloc_get_type(private_data, 
							     struct daemon_control_state);
	struct ctdb_client *client = state->client;
	struct ctdb_reply_control *r;
	size_t len;
	int ret;

	/* construct a message to send to the client containing the data */
	len = offsetof(struct ctdb_reply_control, data) + data.dsize;
	if (errormsg) {
		len += strlen(errormsg);
	}
	r = ctdbd_allocate_pkt(ctdb, state, CTDB_REPLY_CONTROL, len, 
			       struct ctdb_reply_control);
	CTDB_NO_MEMORY_VOID(ctdb, r);

	r->hdr.reqid     = state->reqid;
	r->status        = status;
	r->datalen       = data.dsize;
	r->errorlen = 0;
	memcpy(&r->data[0], data.dptr, data.dsize);
	if (errormsg) {
		r->errorlen = strlen(errormsg);
		memcpy(&r->data[r->datalen], errormsg, r->errorlen);
	}

	ret = daemon_queue_send(client, &r->hdr);
	if (ret != -1) {
		talloc_free(state);
	}
}

/*
  fail all pending controls to a disconnected node
 */
void ctdb_daemon_cancel_controls(struct ctdb_context *ctdb, struct ctdb_node *node)
{
	struct daemon_control_state *state;
	while ((state = node->pending_controls)) {
		DLIST_REMOVE(node->pending_controls, state);
		daemon_control_callback(ctdb, (uint32_t)-1, tdb_null, 
					"node is disconnected", state);
	}
}

/*
  destroy a daemon_control_state
 */
static int daemon_control_destructor(struct daemon_control_state *state)
{
	if (state->node) {
		DLIST_REMOVE(state->node->pending_controls, state);
	}
	return 0;
}

/*
  this is called when the ctdb daemon received a ctdb request control
  from a local client over the unix domain socket
 */
static void daemon_request_control_from_client(struct ctdb_client *client, 
					       struct ctdb_req_control *c)
{
	TDB_DATA data;
	int res;
	struct daemon_control_state *state;
	TALLOC_CTX *tmp_ctx = talloc_new(client);

	if (c->hdr.destnode == CTDB_CURRENT_NODE) {
		c->hdr.destnode = client->ctdb->pnn;
	}

	state = talloc(client, struct daemon_control_state);
	CTDB_NO_MEMORY_VOID(client->ctdb, state);

	state->client = client;
	state->c = talloc_steal(state, c);
	state->reqid = c->hdr.reqid;
	if (ctdb_validate_pnn(client->ctdb, c->hdr.destnode)) {
		state->node = client->ctdb->nodes[c->hdr.destnode];
		DLIST_ADD(state->node->pending_controls, state);
	} else {
		state->node = NULL;
	}

	talloc_set_destructor(state, daemon_control_destructor);

	if (c->flags & CTDB_CTRL_FLAG_NOREPLY) {
		talloc_steal(tmp_ctx, state);
	}
	
	data.dptr = &c->data[0];
	data.dsize = c->datalen;
	res = ctdb_daemon_send_control(client->ctdb, c->hdr.destnode,
				       c->srvid, c->opcode, client->client_id,
				       c->flags,
				       data, daemon_control_callback,
				       state);
	if (res != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send control to remote node %u\n",
			 c->hdr.destnode));
	}

	talloc_free(tmp_ctx);
}

/*
  register a call function
*/
int ctdb_daemon_set_call(struct ctdb_context *ctdb, uint32_t db_id,
			 ctdb_fn_t fn, int id)
{
	struct ctdb_registered_call *call;
	struct ctdb_db_context *ctdb_db;

	ctdb_db = find_ctdb_db(ctdb, db_id);
	if (ctdb_db == NULL) {
		return -1;
	}

	call = talloc(ctdb_db, struct ctdb_registered_call);
	call->fn = fn;
	call->id = id;

	DLIST_ADD(ctdb_db->calls, call);	
	return 0;
}



/*
  this local messaging handler is ugly, but is needed to prevent
  recursion in ctdb_send_message() when the destination node is the
  same as the source node
 */
struct ctdb_local_message {
	struct ctdb_context *ctdb;
	uint64_t srvid;
	TDB_DATA data;
};

static void ctdb_local_message_trigger(struct event_context *ev, struct timed_event *te, 
				       struct timeval t, void *private_data)
{
	struct ctdb_local_message *m = talloc_get_type(private_data, 
						       struct ctdb_local_message);
	int res;

	res = ctdb_dispatch_message(m->ctdb, m->srvid, m->data);
	if (res != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to dispatch message for srvid=%llu\n", 
			  (unsigned long long)m->srvid));
	}
	talloc_free(m);
}

static int ctdb_local_message(struct ctdb_context *ctdb, uint64_t srvid, TDB_DATA data)
{
	struct ctdb_local_message *m;
	m = talloc(ctdb, struct ctdb_local_message);
	CTDB_NO_MEMORY(ctdb, m);

	m->ctdb = ctdb;
	m->srvid = srvid;
	m->data  = data;
	m->data.dptr = talloc_memdup(m, m->data.dptr, m->data.dsize);
	if (m->data.dptr == NULL) {
		talloc_free(m);
		return -1;
	}

	/* this needs to be done as an event to prevent recursion */
	event_add_timed(ctdb->ev, m, timeval_zero(), ctdb_local_message_trigger, m);
	return 0;
}

/*
  send a ctdb message
*/
int ctdb_daemon_send_message(struct ctdb_context *ctdb, uint32_t pnn,
			     uint64_t srvid, TDB_DATA data)
{
	struct ctdb_req_message *r;
	int len;

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_INFO,(__location__ " Failed to send message. Transport is DOWN\n"));
		return -1;
	}

	/* see if this is a message to ourselves */
	if (pnn == ctdb->pnn) {
		return ctdb_local_message(ctdb, srvid, data);
	}

	len = offsetof(struct ctdb_req_message, data) + data.dsize;
	r = ctdb_transport_allocate(ctdb, ctdb, CTDB_REQ_MESSAGE, len,
				    struct ctdb_req_message);
	CTDB_NO_MEMORY(ctdb, r);

	r->hdr.destnode  = pnn;
	r->srvid         = srvid;
	r->datalen       = data.dsize;
	memcpy(&r->data[0], data.dptr, data.dsize);

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
	return 0;
}



struct ctdb_client_notify_list {
	struct ctdb_client_notify_list *next, *prev;
	struct ctdb_context *ctdb;
	uint64_t srvid;
	TDB_DATA data;
};


static int ctdb_client_notify_destructor(struct ctdb_client_notify_list *nl)
{
	int ret;

	DEBUG(DEBUG_ERR,("Sending client notify message for srvid:%llu\n", (unsigned long long)nl->srvid));

	ret = ctdb_daemon_send_message(nl->ctdb, CTDB_BROADCAST_CONNECTED, (unsigned long long)nl->srvid, nl->data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to send client notify message\n"));
	}

	return 0;
}

int32_t ctdb_control_register_notify(struct ctdb_context *ctdb, uint32_t client_id, TDB_DATA indata)
{
	struct ctdb_client_notify_register *notify = (struct ctdb_client_notify_register *)indata.dptr;
        struct ctdb_client *client = ctdb_reqid_find(ctdb, client_id, struct ctdb_client); 
	struct ctdb_client_notify_list *nl;

	DEBUG(DEBUG_INFO,("Register srvid %llu for client %d\n", (unsigned long long)notify->srvid, client_id));

	if (indata.dsize < offsetof(struct ctdb_client_notify_register, notify_data)) {
		DEBUG(DEBUG_ERR,(__location__ " Too little data in control : %d\n", (int)indata.dsize));
		return -1;
	}

	if (indata.dsize != (notify->len + offsetof(struct ctdb_client_notify_register, notify_data))) {
		DEBUG(DEBUG_ERR,(__location__ " Wrong amount of data in control. Got %d, expected %d\n", (int)indata.dsize, (int)(notify->len + offsetof(struct ctdb_client_notify_register, notify_data))));
		return -1;
	}


        if (client == NULL) {
                DEBUG(DEBUG_ERR,(__location__ " Could not find client parent structure. You can not send this control to a remote node\n"));
                return -1;
        }

	for(nl=client->notify; nl; nl=nl->next) {
		if (nl->srvid == notify->srvid) {
			break;
		}
	}
	if (nl != NULL) {
                DEBUG(DEBUG_ERR,(__location__ " Notification for srvid:%llu already exists for this client\n", (unsigned long long)notify->srvid));
                return -1;
        }

	nl = talloc(client, struct ctdb_client_notify_list);
	CTDB_NO_MEMORY(ctdb, nl);
	nl->ctdb       = ctdb;
	nl->srvid      = notify->srvid;
	nl->data.dsize = notify->len;
	nl->data.dptr  = talloc_size(nl, nl->data.dsize);
	CTDB_NO_MEMORY(ctdb, nl->data.dptr);
	memcpy(nl->data.dptr, notify->notify_data, nl->data.dsize);
	
	DLIST_ADD(client->notify, nl);
	talloc_set_destructor(nl, ctdb_client_notify_destructor);

	return 0;
}

int32_t ctdb_control_deregister_notify(struct ctdb_context *ctdb, uint32_t client_id, TDB_DATA indata)
{
	struct ctdb_client_notify_deregister *notify = (struct ctdb_client_notify_deregister *)indata.dptr;
        struct ctdb_client *client = ctdb_reqid_find(ctdb, client_id, struct ctdb_client); 
	struct ctdb_client_notify_list *nl;

	DEBUG(DEBUG_INFO,("Deregister srvid %llu for client %d\n", (unsigned long long)notify->srvid, client_id));

        if (client == NULL) {
                DEBUG(DEBUG_ERR,(__location__ " Could not find client parent structure. You can not send this control to a remote node\n"));
                return -1;
        }

	for(nl=client->notify; nl; nl=nl->next) {
		if (nl->srvid == notify->srvid) {
			break;
		}
	}
	if (nl == NULL) {
                DEBUG(DEBUG_ERR,(__location__ " No notification for srvid:%llu found for this client\n", (unsigned long long)notify->srvid));
                return -1;
        }

	DLIST_REMOVE(client->notify, nl);
	talloc_set_destructor(nl, NULL);
	talloc_free(nl);

	return 0;
}

struct ctdb_client *ctdb_find_client_by_pid(struct ctdb_context *ctdb, pid_t pid)
{
	struct ctdb_client_pid_list *client_pid;

	for (client_pid = ctdb->client_pids; client_pid; client_pid=client_pid->next) {
		if (client_pid->pid == pid) {
			return client_pid->client;
		}
	}
	return NULL;
}


/* This control is used by samba when probing if a process (of a samba daemon)
   exists on the node.
   Samba does this when it needs/wants to check if a subrecord in one of the
   databases is still valied, or if it is stale and can be removed.
   If the node is in unhealthy or stopped state we just kill of the samba
   process holding htis sub-record and return to the calling samba that
   the process does not exist.
   This allows us to forcefully recall subrecords registered by samba processes
   on banned and stopped nodes.
*/
int32_t ctdb_control_process_exists(struct ctdb_context *ctdb, pid_t pid)
{
        struct ctdb_client *client;

	if (ctdb->nodes[ctdb->pnn]->flags & (NODE_FLAGS_BANNED|NODE_FLAGS_STOPPED)) {
		client = ctdb_find_client_by_pid(ctdb, pid);
		if (client != NULL) {
			DEBUG(DEBUG_NOTICE,(__location__ " Killing client with pid:%d on banned/stopped node\n", (int)pid));
			talloc_free(client);
		}
		return -1;
	}

	return kill(pid, 0);
}
