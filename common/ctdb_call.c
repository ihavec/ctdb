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

#include "includes.h"
#include "lib/events/events.h"
#include "system/network.h"
#include "system/filesys.h"
#include "ctdb_private.h"


/*
  local version of ctdb_call
*/
static int ctdb_call_local(struct ctdb_context *ctdb, TDB_DATA key, 
			   struct ctdb_ltdb_header *header, TDB_DATA *data,
			   int call_id, TDB_DATA *call_data, TDB_DATA *reply_data)
{
	struct ctdb_call *c;
	struct ctdb_registered_call *fn;

	c = talloc(ctdb, struct ctdb_call);
	CTDB_NO_MEMORY(ctdb, c);

	c->key = key;
	c->call_data = call_data;
	c->record_data.dptr = talloc_memdup(c, data->dptr, data->dsize);
	c->record_data.dsize = data->dsize;
	CTDB_NO_MEMORY(ctdb, c->record_data.dptr);
	c->new_data = NULL;
	c->reply_data = NULL;

	for (fn=ctdb->calls;fn;fn=fn->next) {
		if (fn->id == call_id) break;
	}
	if (fn == NULL) {
		ctdb_set_error(ctdb, "Unknown call id %u\n", call_id);
		return -1;
	}

	if (fn->fn(c) != 0) {
		ctdb_set_error(ctdb, "ctdb_call %u failed\n", call_id);
		return -1;
	}

	if (c->new_data) {
		if (ctdb_ltdb_store(ctdb, key, header, *c->new_data) != 0) {
			ctdb_set_error(ctdb, "ctdb_call tdb_store failed\n");
			return -1;
		}
	}

	if (reply_data) {
		if (c->reply_data) {
			*reply_data = *c->reply_data;
			talloc_steal(ctdb, reply_data->dptr);
		} else {
			reply_data->dptr = NULL;
			reply_data->dsize = 0;
		}
	}

	talloc_free(c);

	return 0;
}

/*
  send an error reply
*/
static void ctdb_send_error(struct ctdb_context *ctdb, 
			    struct ctdb_req_header *hdr, uint32_t status,
			    const char *fmt, ...)
{
	va_list ap;
	struct ctdb_reply_error *r;
	char *msg;
	int len;
	struct ctdb_node *node;

	va_start(ap, fmt);
	msg = talloc_vasprintf(ctdb, fmt, ap);
	if (msg == NULL) {
		/* can't send an error message, need to rely on call
		   timeouts instead */
		return;
	}
	va_end(ap);

	len = strlen(msg)+1;
	r = talloc_size(ctdb, sizeof(*r) + len);
	r->hdr.length = sizeof(*r) + len;
	r->hdr.operation = CTDB_REPLY_ERROR;
	r->hdr.destnode  = hdr->srcnode;
	r->hdr.srcnode   = ctdb->vnn;
	r->hdr.reqid     = hdr->reqid;
	r->status        = status;
	r->msglen        = len;
	memcpy(&r->msg[0], msg, len);

	talloc_free(msg);

	node = ctdb->nodes[hdr->srcnode];

	ctdb->methods->queue_pkt(node, (uint8_t *)r, r->hdr.length);

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
	struct ctdb_node *node;

	r = talloc_size(ctdb, sizeof(*r));
	r->hdr.length = sizeof(*r);
	r->hdr.operation = CTDB_REPLY_REDIRECT;
	r->hdr.destnode  = c->hdr.srcnode;
	r->hdr.srcnode   = ctdb->vnn;
	r->hdr.reqid     = c->hdr.reqid;
	r->dmaster       = header->dmaster;

	node = ctdb->nodes[r->hdr.destnode];

	ctdb->methods->queue_pkt(node, (uint8_t *)r, r->hdr.length);

	talloc_free(r);
}

/*
  called when a CTDB_REQ_CALL packet comes in
*/
void ctdb_request_call(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_req_call *c = (struct ctdb_req_call *)hdr;
	TDB_DATA key, data, call_data, reply_data;
	struct ctdb_reply_call *r;
	struct ctdb_node *node;
	int ret;
	struct ctdb_ltdb_header header;

	key.dptr = c->data;
	key.dsize = c->keylen;
	call_data.dptr = c->data + c->keylen;
	call_data.dsize = c->calldatalen;

	/* determine if we are the dmaster for this key. This also
	   fetches the record data (if any), thus avoiding a 2nd fetch of the data 
	   if the call will be answered locally */
	ret = ctdb_ltdb_fetch(ctdb, key, &header, &data);
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

	ctdb_call_local(ctdb, key, &header, &data, c->callid, 
			call_data.dsize?&call_data:NULL,
			&reply_data);

	r = talloc_size(ctdb, sizeof(*r) + reply_data.dsize);
	r->hdr.length = sizeof(*r) + reply_data.dsize;
	r->hdr.operation = CTDB_REPLY_CALL;
	r->hdr.destnode  = hdr->srcnode;
	r->hdr.srcnode   = hdr->destnode;
	r->hdr.reqid     = hdr->reqid;
	r->datalen       = reply_data.dsize;
	memcpy(&r->data[0], reply_data.dptr, reply_data.dsize);

	node = ctdb->nodes[hdr->srcnode];

	ctdb->methods->queue_pkt(node, (uint8_t *)r, r->hdr.length);

	talloc_free(reply_data.dptr);
	talloc_free(r);
}

enum call_state {CTDB_CALL_WAIT, CTDB_CALL_DONE, CTDB_CALL_ERROR};

/*
  state of a in-progress ctdb call
*/
struct ctdb_call_state {
	enum call_state state;
	struct ctdb_req_call *c;
	struct ctdb_node *node;
	const char *errmsg;
	TDB_DATA reply_data;
	TDB_DATA key;
	int redirect_count;
};


/*
  called when a CTDB_REPLY_CALL packet comes in
*/
void ctdb_reply_call(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_call *c = (struct ctdb_reply_call *)hdr;
	struct ctdb_call_state *state;
	TDB_DATA reply_data;

	state = idr_find(ctdb->idr, hdr->reqid);

	reply_data.dptr = c->data;
	reply_data.dsize = c->datalen;

	state->reply_data = reply_data;

	talloc_steal(state, c);

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

	talloc_steal(state, c);

	state->state  = CTDB_CALL_ERROR;
	state->errmsg = (char *)c->msg;
}


/*
  called when a CTDB_REPLY_REDIRECT packet comes in
*/
void ctdb_reply_redirect(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_redirect *c = (struct ctdb_reply_redirect *)hdr;
	struct ctdb_call_state *state;

	state = idr_find(ctdb->idr, hdr->reqid);

	talloc_steal(state, c);
	
	/* don't allow for too many redirects */
	if (state->redirect_count++ == CTDB_MAX_REDIRECT) {
		c->dmaster = ctdb_lmaster(ctdb, state->key);
	}

	/* send it off again */
	state->node = ctdb->nodes[c->dmaster];

	if (ctdb->methods->queue_pkt(state->node, (uint8_t *)state->c, 
				     state->c->hdr.length) != 0) {
		state->state = CTDB_CALL_ERROR;
		state->errmsg = "unable to queue in ctdb_reply_redirect";
	}
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
  called when a call times out
*/
void ctdb_call_timeout(struct event_context *ev, struct timed_event *te, 
		       struct timeval t, void *private)
{
	struct ctdb_call_state *state = talloc_get_type(private, struct ctdb_call_state);
	state->state = CTDB_CALL_ERROR;
	ctdb_set_error(state->node->ctdb, "ctdb_call timed out");
}

/*
  fake an event driven local ctdb_call
*/
struct ctdb_call_state *ctdb_call_local_send(struct ctdb_context *ctdb, 
					     TDB_DATA key, int call_id, 
					     TDB_DATA *call_data, TDB_DATA *reply_data,
					     struct ctdb_ltdb_header *header,
					     TDB_DATA *data)
{
	struct ctdb_call_state *state;
	int ret;

	state = talloc_zero(ctdb, struct ctdb_call_state);
	CTDB_NO_MEMORY_NULL(ctdb, state);

	state->state = CTDB_CALL_DONE;
	state->node = ctdb->nodes[ctdb->vnn];

	ret = ctdb_call_local(ctdb, key, header, data, 
			      call_id, call_data, &state->reply_data);
	return state;
}


/*
  make a remote ctdb call - async send
*/
struct ctdb_call_state *ctdb_call_send(struct ctdb_context *ctdb, 
				       TDB_DATA key, int call_id, 
				       TDB_DATA *call_data, TDB_DATA *reply_data)
{
	uint32_t len;
	struct ctdb_call_state *state;
	int ret;
	struct ctdb_ltdb_header header;
	TDB_DATA data;

	/*
	  if we are the dmaster for this key then we don't need to
	  send it off at all, we can bypass the network and handle it
	  locally. To find out if we are the dmaster we need to look
	  in our ltdb
	*/
	ret = ctdb_ltdb_fetch(ctdb, key, &header, &data);
	if (ret != 0) return NULL;

	if (header.dmaster == ctdb->vnn && !(ctdb->flags & CTDB_FLAG_SELF_CONNECT)) {
		return ctdb_call_local_send(ctdb, key, call_id, call_data, reply_data,
					    &header, &data);
	}

	state = talloc_zero(ctdb, struct ctdb_call_state);
	CTDB_NO_MEMORY_NULL(ctdb, state);

	len = sizeof(*state->c) + key.dsize + (call_data?call_data->dsize:0);
	state->c = talloc_size(ctdb, len);
	CTDB_NO_MEMORY_NULL(ctdb, state->c);

	state->c->hdr.length    = len;
	state->c->hdr.operation = CTDB_REQ_CALL;
	state->c->hdr.destnode  = header.dmaster;
	state->c->hdr.srcnode   = ctdb->vnn;
	/* this limits us to 16k outstanding messages - not unreasonable */
	state->c->hdr.reqid     = idr_get_new(ctdb->idr, state, 0xFFFF);
	state->c->callid        = call_id;
	state->c->keylen        = key.dsize;
	state->c->calldatalen   = call_data?call_data->dsize:0;
	memcpy(&state->c->data[0], key.dptr, key.dsize);
	if (call_data) {
		memcpy(&state->c->data[key.dsize], call_data->dptr, call_data->dsize);
	}
	state->key.dptr         = &state->c->data[0];
	state->key.dsize        = key.dsize;

	state->node = ctdb->nodes[header.dmaster];
	state->state = CTDB_CALL_WAIT;

	talloc_set_destructor(state, ctdb_call_destructor);

	if (ctdb->methods->queue_pkt(state->node, (uint8_t *)state->c, len) != 0) {
		talloc_free(state);
		return NULL;
	}

	event_add_timed(ctdb->ev, state, timeval_current_ofs(CTDB_REQ_TIMEOUT, 0), 
			ctdb_call_timeout, state);
	return state;
}


/*
  make a remote ctdb call - async recv
*/
int ctdb_call_recv(struct ctdb_call_state *state, TDB_DATA *reply_data)
{
	while (state->state < CTDB_CALL_DONE) {
		event_loop_once(state->node->ctdb->ev);
	}
	if (state->state != CTDB_CALL_DONE) {
		ctdb_set_error(state->node->ctdb, "%s", state->errmsg);
		talloc_free(state);
		return -1;
	}
	if (reply_data) {
		reply_data->dptr = talloc_memdup(state->node->ctdb,
						 state->reply_data.dptr,
						 state->reply_data.dsize);
		reply_data->dsize = state->reply_data.dsize;
	}
	talloc_free(state);
	return 0;
}

/*
  full ctdb_call
*/
int ctdb_call(struct ctdb_context *ctdb, 
	      TDB_DATA key, int call_id, 
	      TDB_DATA *call_data, TDB_DATA *reply_data)
{
	struct ctdb_call_state *state;
	state = ctdb_call_send(ctdb, key, call_id, call_data, reply_data);
	return ctdb_call_recv(state, reply_data);
}
