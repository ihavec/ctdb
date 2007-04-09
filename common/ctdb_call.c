/* 
   ctdb_call protocol code

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
/*
  see http://wiki.samba.org/index.php/Samba_%26_Clustering for
  protocol design and packet details
*/
#include "includes.h"
#include "lib/events/events.h"
#include "lib/tdb/include/tdb.h"
#include "system/network.h"
#include "system/filesys.h"
#include "../include/ctdb_private.h"

/*
  local version of ctdb_call
*/
static int ctdb_call_local(struct ctdb_db_context *ctdb_db, struct ctdb_call *call,
			   struct ctdb_ltdb_header *header, TDB_DATA *data,
			   uint32_t caller)
{
	struct ctdb_call_info *c;
	struct ctdb_registered_call *fn;
	struct ctdb_context *ctdb = ctdb_db->ctdb;

	c = talloc(ctdb, struct ctdb_call_info);
	CTDB_NO_MEMORY(ctdb, c);

	c->key = call->key;
	c->call_data = &call->call_data;
	c->record_data.dptr = talloc_memdup(c, data->dptr, data->dsize);
	c->record_data.dsize = data->dsize;
	CTDB_NO_MEMORY(ctdb, c->record_data.dptr);
	c->new_data = NULL;
	c->reply_data = NULL;
	c->status = 0;

	for (fn=ctdb_db->calls;fn;fn=fn->next) {
		if (fn->id == call->call_id) break;
	}
	if (fn == NULL) {
		ctdb_set_error(ctdb, "Unknown call id %u\n", call->call_id);
		talloc_free(c);
		return -1;
	}

	if (fn->fn(c) != 0) {
		ctdb_set_error(ctdb, "ctdb_call %u failed\n", call->call_id);
		talloc_free(c);
		return -1;
	}

	if (header->laccessor != caller) {
		header->lacount = 0;
	}
	header->laccessor = caller;
	header->lacount++;

	/* we need to force the record to be written out if this was a remote access,
	   so that the lacount is updated */
	if (c->new_data == NULL && header->laccessor != ctdb->vnn) {
		c->new_data = &c->record_data;
	}

	if (c->new_data) {
		if (ctdb_ltdb_store(ctdb_db, call->key, header, *c->new_data) != 0) {
			ctdb_set_error(ctdb, "ctdb_call tdb_store failed\n");
			talloc_free(c);
			return -1;
		}
	}

	if (c->reply_data) {
		call->reply_data = *c->reply_data;
		talloc_steal(ctdb, call->reply_data.dptr);
	} else {
		call->reply_data.dptr = NULL;
		call->reply_data.dsize = 0;
	}
	call->status = c->status;

	talloc_free(c);

	return 0;
}

/*
  send an error reply
*/
static void ctdb_send_error(struct ctdb_context *ctdb, 
			    struct ctdb_req_header *hdr, uint32_t status,
			    const char *fmt, ...) PRINTF_ATTRIBUTE(4,5);
static void ctdb_send_error(struct ctdb_context *ctdb, 
			    struct ctdb_req_header *hdr, uint32_t status,
			    const char *fmt, ...)
{
	va_list ap;
	struct ctdb_reply_error *r;
	char *msg;
	int msglen, len;

	va_start(ap, fmt);
	msg = talloc_vasprintf(ctdb, fmt, ap);
	if (msg == NULL) {
		ctdb_fatal(ctdb, "Unable to allocate error in ctdb_send_error\n");
	}
	va_end(ap);

	msglen = strlen(msg)+1;
	len = offsetof(struct ctdb_reply_error, msg);
	r = ctdb->methods->allocate_pkt(ctdb, len + msglen);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	talloc_set_name_const(r, "send_error packet");

	r->hdr.length    = len + msglen;
	r->hdr.ctdb_magic = CTDB_MAGIC;
	r->hdr.ctdb_version = CTDB_VERSION;
	r->hdr.operation = CTDB_REPLY_ERROR;
	r->hdr.destnode  = hdr->srcnode;
	r->hdr.srcnode   = ctdb->vnn;
	r->hdr.reqid     = hdr->reqid;
	r->status        = status;
	r->msglen        = msglen;
	memcpy(&r->msg[0], msg, msglen);

	talloc_free(msg);

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
}


/*
  send a redirect reply
*/
static void ctdb_call_send_redirect(struct ctdb_context *ctdb, 
				    struct ctdb_req_call *c, 
				    struct ctdb_ltdb_header *header)
{
	struct ctdb_reply_redirect *r;

	r = ctdb->methods->allocate_pkt(ctdb, sizeof(*r));
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	talloc_set_name_const(r, "send_redirect packet");
	r->hdr.length = sizeof(*r);
	r->hdr.ctdb_magic = CTDB_MAGIC;
	r->hdr.ctdb_version = CTDB_VERSION;
	r->hdr.operation = CTDB_REPLY_REDIRECT;
	r->hdr.destnode  = c->hdr.srcnode;
	r->hdr.srcnode   = ctdb->vnn;
	r->hdr.reqid     = c->hdr.reqid;
	r->dmaster       = header->dmaster;

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
}

/*
  send a dmaster request (give another node the dmaster for a record)

  This is always sent to the lmaster, which ensures that the lmaster
  always knows who the dmaster is. The lmaster will then send a
  CTDB_REPLY_DMASTER to the new dmaster
*/
static void ctdb_call_send_dmaster(struct ctdb_db_context *ctdb_db, 
				   struct ctdb_req_call *c, 
				   struct ctdb_ltdb_header *header,
				   TDB_DATA *key, TDB_DATA *data)
{
	struct ctdb_req_dmaster *r;
	struct ctdb_context *ctdb = ctdb_db->ctdb;
	int len;
	
	len = offsetof(struct ctdb_req_dmaster, data) + key->dsize + data->dsize;
	r = ctdb->methods->allocate_pkt(ctdb, len);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	talloc_set_name_const(r, "send_dmaster packet");
	r->hdr.length    = len;
	r->hdr.ctdb_magic = CTDB_MAGIC;
	r->hdr.ctdb_version = CTDB_VERSION;
	r->hdr.operation = CTDB_REQ_DMASTER;
	r->hdr.destnode  = ctdb_lmaster(ctdb, key);
	r->hdr.srcnode   = ctdb->vnn;
	r->hdr.reqid     = c->hdr.reqid;
	r->db_id         = c->db_id;
	r->dmaster       = c->hdr.srcnode;
	r->keylen        = key->dsize;
	r->datalen       = data->dsize;
	memcpy(&r->data[0], key->dptr, key->dsize);
	memcpy(&r->data[key->dsize], data->dptr, data->dsize);

	if (r->hdr.destnode == ctdb->vnn) {
		/* we are the lmaster - don't send to ourselves */
		ctdb_request_dmaster(ctdb, &r->hdr);
	} else {
		ctdb_queue_packet(ctdb, &r->hdr);

		/* update the ltdb to record the new dmaster */
		header->dmaster = r->hdr.destnode;
		ctdb_ltdb_store(ctdb_db, *key, header, *data);
	}

	talloc_free(r);
}


/*
  called when a CTDB_REQ_DMASTER packet comes in

  this comes into the lmaster for a record when the current dmaster
  wants to give up the dmaster role and give it to someone else
*/
void ctdb_request_dmaster(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_req_dmaster *c = (struct ctdb_req_dmaster *)hdr;
	struct ctdb_reply_dmaster *r;
	TDB_DATA key, data, data2;
	struct ctdb_ltdb_header header;
	struct ctdb_db_context *ctdb_db;
	int ret, len;

	key.dptr = c->data;
	key.dsize = c->keylen;
	data.dptr = c->data + c->keylen;
	data.dsize = c->datalen;

	for (ctdb_db=ctdb->db_list; ctdb_db; ctdb_db=ctdb_db->next) {
		if (ctdb_db->db_id == c->db_id) {
			break;
		}
	}
	if (!ctdb_db) {
		ctdb_send_error(ctdb, hdr, ret, "Unknown database in request. db_id==0x%08x",c->db_id);
		return;
	}
	
	/* fetch the current record */
	ret = ctdb_ltdb_fetch(ctdb_db, key, &header, hdr, &data2);
	if (ret != 0) {
		ctdb_fatal(ctdb, "ctdb_req_dmaster failed to fetch record");
		return;
	}

	/* its a protocol error if the sending node is not the current dmaster */
	if (header.dmaster != hdr->srcnode) {
		ctdb_fatal(ctdb, "dmaster request from non-master");
		return;
	}

	header.dmaster = c->dmaster;
	if (ctdb_ltdb_store(ctdb_db, key, &header, data) != 0) {
		ctdb_fatal(ctdb, "ctdb_req_dmaster unable to update dmaster");
		return;
	}

	/* send the CTDB_REPLY_DMASTER */
	len = offsetof(struct ctdb_reply_dmaster, data) + data.dsize;
	r = ctdb->methods->allocate_pkt(ctdb, len);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	talloc_set_name_const(r, "reply_dmaster packet");
	r->hdr.length    = len;
	r->hdr.ctdb_magic = CTDB_MAGIC;
	r->hdr.ctdb_version = CTDB_VERSION;
	r->hdr.operation = CTDB_REPLY_DMASTER;
	r->hdr.destnode  = c->dmaster;
	r->hdr.srcnode   = ctdb->vnn;
	r->hdr.reqid     = hdr->reqid;
	r->datalen       = data.dsize;
	memcpy(&r->data[0], data.dptr, data.dsize);

	if (r->hdr.destnode == r->hdr.srcnode) {
		ctdb_reply_dmaster(ctdb, &r->hdr);
	} else {
		ctdb_queue_packet(ctdb, &r->hdr);
	}

	talloc_free(r);
}


/*
  called when a CTDB_REQ_CALL packet comes in
*/
void ctdb_request_call(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_req_call *c = (struct ctdb_req_call *)hdr;
	TDB_DATA data;
	struct ctdb_reply_call *r;
	int ret, len;
	struct ctdb_ltdb_header header;
	struct ctdb_call call;
	struct ctdb_db_context *ctdb_db;

	for (ctdb_db=ctdb->db_list; ctdb_db; ctdb_db=ctdb_db->next) {
		if (ctdb_db->db_id == c->db_id) {
			break;
		}
	}
	if (!ctdb_db) {
		ctdb_send_error(ctdb, hdr, ret, "Unknown database in request. db_id==0x%08x",c->db_id);
		return;
	}

	call.call_id  = c->callid;
	call.key.dptr = c->data;
	call.key.dsize = c->keylen;
	call.call_data.dptr = c->data + c->keylen;
	call.call_data.dsize = c->calldatalen;

	/* determine if we are the dmaster for this key. This also
	   fetches the record data (if any), thus avoiding a 2nd fetch of the data 
	   if the call will be answered locally */

	ret = ctdb_ltdb_fetch(ctdb_db, call.key, &header, hdr, &data);
	if (ret != 0) {
		ctdb_send_error(ctdb, hdr, ret, "ltdb fetch failed in ctdb_request_call");
		return;
	}

	/* if we are not the dmaster, then send a redirect to the
	   requesting node */
	if (header.dmaster != ctdb->vnn) {
		ctdb_call_send_redirect(ctdb, c, &header);
		talloc_free(data.dptr);
		return;
	}

	/* if this nodes has done enough consecutive calls on the same record
	   then give them the record
	   or if the node requested an immediate migration
	*/
	if ( (header.laccessor == c->hdr.srcnode
	      && header.lacount >= ctdb->max_lacount)
	   || c->flags&CTDB_IMMEDIATE_MIGRATION ) {
		ctdb_call_send_dmaster(ctdb_db, c, &header, &call.key, &data);
		talloc_free(data.dptr);
		return;
	}

	ctdb_call_local(ctdb_db, &call, &header, &data, c->hdr.srcnode);

	len = offsetof(struct ctdb_reply_call, data) + call.reply_data.dsize;
	r = ctdb->methods->allocate_pkt(ctdb, len);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	talloc_set_name_const(r, "reply_call packet");
	r->hdr.length    = len;
	r->hdr.ctdb_magic = CTDB_MAGIC;
	r->hdr.ctdb_version = CTDB_VERSION;
	r->hdr.operation = CTDB_REPLY_CALL;
	r->hdr.destnode  = hdr->srcnode;
	r->hdr.srcnode   = hdr->destnode;
	r->hdr.reqid     = hdr->reqid;
	r->status        = call.status;
	r->datalen       = call.reply_data.dsize;
	if (call.reply_data.dsize) {
		memcpy(&r->data[0], call.reply_data.dptr, call.reply_data.dsize);
		talloc_free(call.reply_data.dptr);
	}

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
}

/*
  called when a CTDB_REPLY_CALL packet comes in

  This packet comes in response to a CTDB_REQ_CALL request packet. It
  contains any reply data freom the call
*/
void ctdb_reply_call(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_call *c = (struct ctdb_reply_call *)hdr;
	struct ctdb_call_state *state;

	state = idr_find(ctdb->idr, hdr->reqid);
	if (state == NULL) return;

	state->call.reply_data.dptr = c->data;
	state->call.reply_data.dsize = c->datalen;
	state->call.status = c->status;

	talloc_steal(state, c);

	state->state = CTDB_CALL_DONE;
}

/*
  called when a CTDB_REPLY_DMASTER packet comes in

  This packet comes in from the lmaster response to a CTDB_REQ_CALL
  request packet. It means that the current dmaster wants to give us
  the dmaster role
*/
void ctdb_reply_dmaster(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_dmaster *c = (struct ctdb_reply_dmaster *)hdr;
	struct ctdb_call_state *state;
	struct ctdb_db_context *ctdb_db;
	TDB_DATA data;

	state = idr_find(ctdb->idr, hdr->reqid);
	if (state == NULL) {
		return;
	}
	ctdb_db = state->ctdb_db;

	data.dptr = c->data;
	data.dsize = c->datalen;

	talloc_steal(state, c);

	/* we're now the dmaster - update our local ltdb with new header
	   and data */
	state->header.dmaster = ctdb->vnn;

	if (ctdb_ltdb_store(ctdb_db, state->call.key, &state->header, data) != 0) {
		ctdb_fatal(ctdb, "ctdb_reply_dmaster store failed\n");
		return;
	}

	ctdb_call_local(ctdb_db, &state->call, &state->header, &data, ctdb->vnn);

	state->state = CTDB_CALL_DONE;
}


/*
  called when a CTDB_REPLY_ERROR packet comes in
*/
void ctdb_reply_error(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_error *c = (struct ctdb_reply_error *)hdr;
	struct ctdb_call_state *state;

	state = idr_find(ctdb->idr, hdr->reqid);
	if (state == NULL) return;

	talloc_steal(state, c);

	state->state  = CTDB_CALL_ERROR;
	state->errmsg = (char *)c->msg;
}


/*
  called when a CTDB_REPLY_REDIRECT packet comes in

  This packet arrives when we have sent a CTDB_REQ_CALL request and
  the node that received it is not the dmaster for the given key. We
  are given a hint as to what node to try next.
*/
void ctdb_reply_redirect(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_redirect *c = (struct ctdb_reply_redirect *)hdr;
	struct ctdb_call_state *state;

	state = idr_find(ctdb->idr, hdr->reqid);
	if (state == NULL) return;

	talloc_steal(state, c);
	
	/* don't allow for too many redirects */
	if (state->redirect_count++ == CTDB_MAX_REDIRECT) {
		c->dmaster = ctdb_lmaster(ctdb, &state->call.key);
	}

	/* send it off again */
	state->node = ctdb->nodes[c->dmaster];

	ctdb_queue_packet(ctdb, &state->c->hdr);
}

/*
  destroy a ctdb_call
*/
static int ctdb_call_destructor(struct ctdb_call_state *state)
{
	idr_remove(state->node->ctdb->idr, state->c->hdr.reqid);
	return 0;
}


/*
  called when a ctdb_call times out
*/
void ctdb_call_timeout(struct event_context *ev, struct timed_event *te, 
		       struct timeval t, void *private)
{
	struct ctdb_call_state *state = talloc_get_type(private, struct ctdb_call_state);
	state->state = CTDB_CALL_ERROR;
	ctdb_set_error(state->node->ctdb, "ctdb_call %u timed out",
		       state->c->hdr.reqid);
}

/*
  construct an event driven local ctdb_call

  this is used so that locally processed ctdb_call requests are processed
  in an event driven manner
*/
struct ctdb_call_state *ctdb_call_local_send(struct ctdb_db_context *ctdb_db, 
					     struct ctdb_call *call,
					     struct ctdb_ltdb_header *header,
					     TDB_DATA *data)
{
	struct ctdb_call_state *state;
	struct ctdb_context *ctdb = ctdb_db->ctdb;
	int ret;

	state = talloc_zero(ctdb_db, struct ctdb_call_state);
	CTDB_NO_MEMORY_NULL(ctdb, state);

	talloc_steal(state, data->dptr);

	state->state = CTDB_CALL_DONE;
	state->node = ctdb->nodes[ctdb->vnn];
	state->call = *call;
	state->ctdb_db = ctdb_db;

	ret = ctdb_call_local(ctdb_db, &state->call, header, data, ctdb->vnn);

	return state;
}


/*
  make a remote ctdb call - async send

  This constructs a ctdb_call request and queues it for processing. 
  This call never blocks.
*/
struct ctdb_call_state *ctdb_call_send(struct ctdb_db_context *ctdb_db, struct ctdb_call *call)
{
	uint32_t len;
	struct ctdb_call_state *state;
	int ret;
	struct ctdb_ltdb_header header;
	TDB_DATA data;
	struct ctdb_context *ctdb = ctdb_db->ctdb;


	/*
	  if we are the dmaster for this key then we don't need to
	  send it off at all, we can bypass the network and handle it
	  locally. To find out if we are the dmaster we need to look
	  in our ltdb
	*/
	ret = ctdb_ltdb_fetch(ctdb_db, call->key, &header, ctdb_db, &data);
	if (ret != 0) return NULL;

	if (header.dmaster == ctdb->vnn && !(ctdb->flags & CTDB_FLAG_SELF_CONNECT)) {
		return ctdb_call_local_send(ctdb_db, call, &header, &data);
	}

	state = talloc_zero(ctdb_db, struct ctdb_call_state);
	CTDB_NO_MEMORY_NULL(ctdb, state);

	talloc_steal(state, data.dptr);

	len = offsetof(struct ctdb_req_call, data) + call->key.dsize + call->call_data.dsize;
	state->c = ctdb->methods->allocate_pkt(ctdb, len);
	CTDB_NO_MEMORY_NULL(ctdb, state->c);
	talloc_set_name_const(state->c, "req_call packet");
	talloc_steal(state, state->c);

	state->c->hdr.length    = len;
	state->c->hdr.ctdb_magic = CTDB_MAGIC;
	state->c->hdr.ctdb_version = CTDB_VERSION;
	state->c->hdr.operation = CTDB_REQ_CALL;
	state->c->hdr.destnode  = header.dmaster;
	state->c->hdr.srcnode   = ctdb->vnn;
	/* this limits us to 16k outstanding messages - not unreasonable */
	state->c->hdr.reqid     = idr_get_new(ctdb->idr, state, 0xFFFF);
	state->c->flags         = call->flags;
	state->c->db_id         = ctdb_db->db_id;
	state->c->callid        = call->call_id;
	state->c->keylen        = call->key.dsize;
	state->c->calldatalen   = call->call_data.dsize;
	memcpy(&state->c->data[0], call->key.dptr, call->key.dsize);
	memcpy(&state->c->data[call->key.dsize], 
	       call->call_data.dptr, call->call_data.dsize);
	state->call                = *call;
	state->call.call_data.dptr = &state->c->data[call->key.dsize];
	state->call.key.dptr       = &state->c->data[0];

	state->node   = ctdb->nodes[header.dmaster];
	state->state  = CTDB_CALL_WAIT;
	state->header = header;
	state->ctdb_db = ctdb_db;

	talloc_set_destructor(state, ctdb_call_destructor);

	ctdb_queue_packet(ctdb, &state->c->hdr);

	event_add_timed(ctdb->ev, state, timeval_current_ofs(CTDB_REQ_TIMEOUT, 0), 
			ctdb_call_timeout, state);
	return state;
}


/*
  make a remote ctdb call - async recv. 

  This is called when the program wants to wait for a ctdb_call to complete and get the 
  results. This call will block unless the call has already completed.
*/
int ctdb_call_recv(struct ctdb_call_state *state, struct ctdb_call *call)
{
	struct ctdb_record_handle *rec;

	while (state->state < CTDB_CALL_DONE) {
		event_loop_once(state->node->ctdb->ev);
	}
	if (state->state != CTDB_CALL_DONE) {
		ctdb_set_error(state->node->ctdb, "%s", state->errmsg);
		talloc_free(state);
		return -1;
	}

	rec = state->fetch_private;

	/* ugly hack to manage forced migration */
	if (rec != NULL) {
		rec->data->dptr = talloc_steal(rec, state->call.reply_data.dptr);
		rec->data->dsize = state->call.reply_data.dsize;
		talloc_free(state);
		return 0;
	}

	if (state->call.reply_data.dsize) {
		call->reply_data.dptr = talloc_memdup(state->node->ctdb,
						      state->call.reply_data.dptr,
						      state->call.reply_data.dsize);
		call->reply_data.dsize = state->call.reply_data.dsize;
	} else {
		call->reply_data.dptr = NULL;
		call->reply_data.dsize = 0;
	}
	call->status = state->call.status;
	talloc_free(state);
	return 0;
}

/*
  full ctdb_call. Equivalent to a ctdb_call_send() followed by a ctdb_call_recv()
*/
int ctdb_call(struct ctdb_db_context *ctdb_db, struct ctdb_call *call)
{
	struct ctdb_call_state *state;
	state = ctdb_call_send(ctdb_db, call);
	return ctdb_call_recv(state, call);
}



struct ctdb_record_handle *ctdb_fetch_lock(struct ctdb_db_context *ctdb_db, TALLOC_CTX *mem_ctx, 
					   TDB_DATA key, TDB_DATA *data)
{
	struct ctdb_call call;
	struct ctdb_record_handle *rec;
	struct ctdb_call_state *state;
	int ret;

	ZERO_STRUCT(call);
	call.call_id = CTDB_FETCH_FUNC;
	call.key = key;
	call.flags = CTDB_IMMEDIATE_MIGRATION;

	rec = talloc(mem_ctx, struct ctdb_record_handle);
	CTDB_NO_MEMORY_NULL(ctdb_db->ctdb, rec);

	rec->ctdb_db = ctdb_db;
	rec->key = key;
	rec->key.dptr = talloc_memdup(rec, key.dptr, key.dsize);
	rec->data = data;

	state = ctdb_call_send(ctdb_db, &call);
	state->fetch_private = rec;

	ret = ctdb_call_recv(state, &call);
	if (ret != 0) {
		talloc_free(rec);
		return NULL;
	}

	return rec;
}


int ctdb_record_store(struct ctdb_record_handle *rec, TDB_DATA data)
{
	int ret;
	struct ctdb_ltdb_header header;

	/* should be avoided if possible    hang header off rec ? */
	ret = ctdb_ltdb_fetch(rec->ctdb_db, rec->key, &header, NULL, NULL);
	if (ret) {
		ctdb_set_error(rec->ctdb_db->ctdb, "Fetch of locally held record failed");
		return ret;
	}

	ret = ctdb_ltdb_store(rec->ctdb_db, rec->key, &header, data);
		
	return ret;
}
