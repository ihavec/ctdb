/* 
   ctdb_call protocol code

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
/*
  see http://wiki.samba.org/index.php/Samba_%26_Clustering for
  protocol design and packet details
*/
#include "includes.h"
#include "lib/events/events.h"
#include "lib/tdb/include/tdb.h"
#include "lib/util/dlinklist.h"
#include "system/network.h"
#include "system/filesys.h"
#include "../include/ctdb_private.h"

/*
  find the ctdb_db from a db index
 */
 struct ctdb_db_context *find_ctdb_db(struct ctdb_context *ctdb, uint32_t id)
{
	struct ctdb_db_context *ctdb_db;

	for (ctdb_db=ctdb->db_list; ctdb_db; ctdb_db=ctdb_db->next) {
		if (ctdb_db->db_id == id) {
			break;
		}
	}
	return ctdb_db;
}


/*
  a varient of input packet that can be used in lock requeue
*/
static void ctdb_call_input_pkt(void *p, struct ctdb_req_header *hdr)
{
	struct ctdb_context *ctdb = talloc_get_type(p, struct ctdb_context);
	ctdb_input_pkt(ctdb, hdr);
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

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send error. Transport is DOWN\n"));
		return;
	}

	va_start(ap, fmt);
	msg = talloc_vasprintf(ctdb, fmt, ap);
	if (msg == NULL) {
		ctdb_fatal(ctdb, "Unable to allocate error in ctdb_send_error\n");
	}
	va_end(ap);

	msglen = strlen(msg)+1;
	len = offsetof(struct ctdb_reply_error, msg);
	r = ctdb_transport_allocate(ctdb, msg, CTDB_REPLY_ERROR, len + msglen, 
				    struct ctdb_reply_error);
	CTDB_NO_MEMORY_FATAL(ctdb, r);

	r->hdr.destnode  = hdr->srcnode;
	r->hdr.reqid     = hdr->reqid;
	r->status        = status;
	r->msglen        = msglen;
	memcpy(&r->msg[0], msg, msglen);

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(msg);
}


/**
 * send a redirect reply
 *
 * The logic behind this function is this:
 *
 * A client wants to grab a record and sends a CTDB_REQ_CALL packet
 * to its local ctdb (ctdb_request_call). If the node is not itself
 * the record's DMASTER, it first redirects the packet to  the
 * record's LMASTER. The LMASTER then redirects the call packet to
 * the current DMASTER. But there is a race: The record may have
 * been migrated off the DMASTER while the redirected packet is
 * on the wire (or in the local queue). So in case the record has
 * migrated off the new destinaton of the call packet, instead of
 * going back to the LMASTER to get the new DMASTER, we try to
 * reduce rountrips by fist chasing the record a couple of times
 * before giving up the direct chase and finally going back to the
 * LMASTER (again). Note that this works because of this: When
 * a record is migrated off a node, then the new DMASTER is stored
 * in the record's copy on the former DMASTER.
 *
 * The maxiumum number of attempts for direct chase to make before
 * going back to the LMASTER is configurable by the tunable
 * "MaxRedirectCount".
 */
static void ctdb_call_send_redirect(struct ctdb_context *ctdb, 
				    TDB_DATA key,
				    struct ctdb_req_call *c, 
				    struct ctdb_ltdb_header *header)
{
	
	uint32_t lmaster = ctdb_lmaster(ctdb, &key);
	if (ctdb->pnn == lmaster) {
		c->hdr.destnode = header->dmaster;
	} else if ((c->hopcount % ctdb->tunable.max_redirect_count) == 0) {
		c->hdr.destnode = lmaster;
	} else {
		c->hdr.destnode = header->dmaster;
	}
	c->hopcount++;
	ctdb_queue_packet(ctdb, &c->hdr);
}


/*
  send a dmaster reply

  caller must have the chainlock before calling this routine. Caller must be
  the lmaster
*/
static void ctdb_send_dmaster_reply(struct ctdb_db_context *ctdb_db,
				    struct ctdb_ltdb_header *header,
				    TDB_DATA key, TDB_DATA data,
				    uint32_t new_dmaster,
				    uint32_t reqid)
{
	struct ctdb_context *ctdb = ctdb_db->ctdb;
	struct ctdb_reply_dmaster *r;
	int ret, len;
	TALLOC_CTX *tmp_ctx;

	if (ctdb->pnn != ctdb_lmaster(ctdb, &key)) {
		DEBUG(DEBUG_ALERT,(__location__ " Caller is not lmaster!\n"));
		return;
	}

	header->dmaster = new_dmaster;
	ret = ctdb_ltdb_store(ctdb_db, key, header, data);
	if (ret != 0) {
		ctdb_fatal(ctdb, "ctdb_send_dmaster_reply unable to update dmaster");
		return;
	}

	if (ctdb->methods == NULL) {
		ctdb_fatal(ctdb, "ctdb_send_dmaster_reply cant update dmaster since transport is down");
		return;
	}

	/* put the packet on a temporary context, allowing us to safely free
	   it below even if ctdb_reply_dmaster() has freed it already */
	tmp_ctx = talloc_new(ctdb);

	/* send the CTDB_REPLY_DMASTER */
	len = offsetof(struct ctdb_reply_dmaster, data) + key.dsize + data.dsize + sizeof(uint32_t);
	r = ctdb_transport_allocate(ctdb, tmp_ctx, CTDB_REPLY_DMASTER, len,
				    struct ctdb_reply_dmaster);
	CTDB_NO_MEMORY_FATAL(ctdb, r);

	r->hdr.destnode  = new_dmaster;
	r->hdr.reqid     = reqid;
	r->rsn           = header->rsn;
	r->keylen        = key.dsize;
	r->datalen       = data.dsize;
	r->db_id         = ctdb_db->db_id;
	memcpy(&r->data[0], key.dptr, key.dsize);
	memcpy(&r->data[key.dsize], data.dptr, data.dsize);
	memcpy(&r->data[key.dsize+data.dsize], &header->flags, sizeof(uint32_t));

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(tmp_ctx);
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
	uint32_t lmaster = ctdb_lmaster(ctdb, key);

	if (ctdb->methods == NULL) {
		ctdb_fatal(ctdb, "Failed ctdb_call_send_dmaster since transport is down");
		return;
	}

	if (data->dsize != 0) {
		header->flags |= CTDB_REC_FLAG_MIGRATED_WITH_DATA;
	}

	if (lmaster == ctdb->pnn) {
		ctdb_send_dmaster_reply(ctdb_db, header, *key, *data, 
					c->hdr.srcnode, c->hdr.reqid);
		return;
	}
	
	len = offsetof(struct ctdb_req_dmaster, data) + key->dsize + data->dsize
			+ sizeof(uint32_t);
	r = ctdb_transport_allocate(ctdb, ctdb, CTDB_REQ_DMASTER, len, 
				    struct ctdb_req_dmaster);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	r->hdr.destnode  = lmaster;
	r->hdr.reqid     = c->hdr.reqid;
	r->db_id         = c->db_id;
	r->rsn           = header->rsn;
	r->dmaster       = c->hdr.srcnode;
	r->keylen        = key->dsize;
	r->datalen       = data->dsize;
	memcpy(&r->data[0], key->dptr, key->dsize);
	memcpy(&r->data[key->dsize], data->dptr, data->dsize);
	memcpy(&r->data[key->dsize + data->dsize], &header->flags, sizeof(uint32_t));

	header->dmaster = c->hdr.srcnode;
	if (ctdb_ltdb_store(ctdb_db, *key, header, *data) != 0) {
		ctdb_fatal(ctdb, "Failed to store record in ctdb_call_send_dmaster");
	}
	
	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
}

/*
  called when a CTDB_REPLY_DMASTER packet comes in, or when the lmaster
  gets a CTDB_REQUEST_DMASTER for itself. We become the dmaster.

  must be called with the chainlock held. This function releases the chainlock
*/
static void ctdb_become_dmaster(struct ctdb_db_context *ctdb_db,
				struct ctdb_req_header *hdr,
				TDB_DATA key, TDB_DATA data,
				uint64_t rsn, uint32_t record_flags)
{
	struct ctdb_call_state *state;
	struct ctdb_context *ctdb = ctdb_db->ctdb;
	struct ctdb_ltdb_header header;

	DEBUG(DEBUG_DEBUG,("pnn %u dmaster response %08x\n", ctdb->pnn, ctdb_hash(&key)));

	ZERO_STRUCT(header);
	header.rsn = rsn;
	header.dmaster = ctdb->pnn;
	header.flags = record_flags;

	state = ctdb_reqid_find(ctdb, hdr->reqid, struct ctdb_call_state);

	if (state) {
		if (state->call->flags & CTDB_CALL_FLAG_VACUUM_MIGRATION) {
			/*
			 * We temporarily add the VACUUM_MIGRATED flag to
			 * the record flags, so that ctdb_ltdb_store can
			 * decide whether the record should be stored or
			 * deleted.
			 */
			header.flags |= CTDB_REC_FLAG_VACUUM_MIGRATED;
		}
	}

	if (ctdb_ltdb_store(ctdb_db, key, &header, data) != 0) {
		ctdb_fatal(ctdb, "ctdb_reply_dmaster store failed\n");
		ctdb_ltdb_unlock(ctdb_db, key);
		return;
	}


	if (state == NULL) {
		DEBUG(DEBUG_ERR,("pnn %u Invalid reqid %u in ctdb_become_dmaster from node %u\n",
			 ctdb->pnn, hdr->reqid, hdr->srcnode));
		ctdb_ltdb_unlock(ctdb_db, key);
		return;
	}

	if (hdr->reqid != state->reqid) {
		/* we found a record  but it was the wrong one */
		DEBUG(DEBUG_ERR, ("Dropped orphan in ctdb_become_dmaster with reqid:%u\n from node %u", hdr->reqid, hdr->srcnode));
		ctdb_ltdb_unlock(ctdb_db, key);
		return;
	}

	ctdb_call_local(ctdb_db, state->call, &header, state, &data);

	ctdb_ltdb_unlock(ctdb_db, state->call->key);

	state->state = CTDB_CALL_DONE;
	if (state->async.fn) {
		state->async.fn(state);
	}
}



/*
  called when a CTDB_REQ_DMASTER packet comes in

  this comes into the lmaster for a record when the current dmaster
  wants to give up the dmaster role and give it to someone else
*/
void ctdb_request_dmaster(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_req_dmaster *c = (struct ctdb_req_dmaster *)hdr;
	TDB_DATA key, data, data2;
	struct ctdb_ltdb_header header;
	struct ctdb_db_context *ctdb_db;
	uint32_t record_flags = 0;
	size_t len;
	int ret;

	key.dptr = c->data;
	key.dsize = c->keylen;
	data.dptr = c->data + c->keylen;
	data.dsize = c->datalen;
	len = offsetof(struct ctdb_req_dmaster, data) + key.dsize + data.dsize
			+ sizeof(uint32_t);
	if (len <= c->hdr.length) {
		record_flags = *(uint32_t *)&c->data[c->keylen + c->datalen];
	}

	ctdb_db = find_ctdb_db(ctdb, c->db_id);
	if (!ctdb_db) {
		ctdb_send_error(ctdb, hdr, -1,
				"Unknown database in request. db_id==0x%08x",
				c->db_id);
		return;
	}
	
	/* fetch the current record */
	ret = ctdb_ltdb_lock_fetch_requeue(ctdb_db, key, &header, hdr, &data2,
					   ctdb_call_input_pkt, ctdb, False);
	if (ret == -1) {
		ctdb_fatal(ctdb, "ctdb_req_dmaster failed to fetch record");
		return;
	}
	if (ret == -2) {
		DEBUG(DEBUG_INFO,(__location__ " deferring ctdb_request_dmaster\n"));
		return;
	}

	if (ctdb_lmaster(ctdb, &key) != ctdb->pnn) {
		DEBUG(DEBUG_ALERT,("pnn %u dmaster request to non-lmaster lmaster=%u gen=%u curgen=%u\n",
			 ctdb->pnn, ctdb_lmaster(ctdb, &key), 
			 hdr->generation, ctdb->vnn_map->generation));
		ctdb_fatal(ctdb, "ctdb_req_dmaster to non-lmaster");
	}

	DEBUG(DEBUG_DEBUG,("pnn %u dmaster request on %08x for %u from %u\n", 
		 ctdb->pnn, ctdb_hash(&key), c->dmaster, c->hdr.srcnode));

	/* its a protocol error if the sending node is not the current dmaster */
	if (header.dmaster != hdr->srcnode) {
		DEBUG(DEBUG_ALERT,("pnn %u dmaster request for new-dmaster %u from non-master %u real-dmaster=%u key %08x dbid 0x%08x gen=%u curgen=%u c->rsn=%llu header.rsn=%llu reqid=%u keyval=0x%08x\n",
			 ctdb->pnn, c->dmaster, hdr->srcnode, header.dmaster, ctdb_hash(&key),
			 ctdb_db->db_id, hdr->generation, ctdb->vnn_map->generation,
			 (unsigned long long)c->rsn, (unsigned long long)header.rsn, c->hdr.reqid,
			 (key.dsize >= 4)?(*(uint32_t *)key.dptr):0));
		if (header.rsn != 0 || header.dmaster != ctdb->pnn) {
			ctdb_fatal(ctdb, "ctdb_req_dmaster from non-master");
			return;
		}
	}

	if (header.rsn > c->rsn) {
		DEBUG(DEBUG_ALERT,("pnn %u dmaster request with older RSN new-dmaster %u from %u real-dmaster=%u key %08x dbid 0x%08x gen=%u curgen=%u c->rsn=%llu header.rsn=%llu reqid=%u\n",
			 ctdb->pnn, c->dmaster, hdr->srcnode, header.dmaster, ctdb_hash(&key),
			 ctdb_db->db_id, hdr->generation, ctdb->vnn_map->generation,
			 (unsigned long long)c->rsn, (unsigned long long)header.rsn, c->hdr.reqid));
	}

	/* use the rsn from the sending node */
	header.rsn = c->rsn;

	/* store the record flags from the sending node */
	header.flags = record_flags;

	/* check if the new dmaster is the lmaster, in which case we
	   skip the dmaster reply */
	if (c->dmaster == ctdb->pnn) {
		ctdb_become_dmaster(ctdb_db, hdr, key, data, c->rsn, record_flags);
	} else {
		ctdb_send_dmaster_reply(ctdb_db, &header, key, data, c->dmaster, hdr->reqid);

		ret = ctdb_ltdb_unlock(ctdb_db, key);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
		}
	}
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
	struct ctdb_call *call;
	struct ctdb_db_context *ctdb_db;

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed ctdb_request_call. Transport is DOWN\n"));
		return;
	}


	ctdb_db = find_ctdb_db(ctdb, c->db_id);
	if (!ctdb_db) {
		ctdb_send_error(ctdb, hdr, -1,
				"Unknown database in request. db_id==0x%08x",
				c->db_id);
		return;
	}

	call = talloc(hdr, struct ctdb_call);
	CTDB_NO_MEMORY_FATAL(ctdb, call);

	call->call_id  = c->callid;
	call->key.dptr = c->data;
	call->key.dsize = c->keylen;
	call->call_data.dptr = c->data + c->keylen;
	call->call_data.dsize = c->calldatalen;

	/* determine if we are the dmaster for this key. This also
	   fetches the record data (if any), thus avoiding a 2nd fetch of the data 
	   if the call will be answered locally */

	ret = ctdb_ltdb_lock_fetch_requeue(ctdb_db, call->key, &header, hdr, &data,
					   ctdb_call_input_pkt, ctdb, False);
	if (ret == -1) {
		ctdb_send_error(ctdb, hdr, ret, "ltdb fetch failed in ctdb_request_call");
		return;
	}
	if (ret == -2) {
		DEBUG(DEBUG_INFO,(__location__ " deferred ctdb_request_call\n"));
		return;
	}

	/* if we are not the dmaster, then send a redirect to the
	   requesting node */
	if (header.dmaster != ctdb->pnn) {
		talloc_free(data.dptr);
		ctdb_call_send_redirect(ctdb, call->key, c, &header);

		ret = ctdb_ltdb_unlock(ctdb_db, call->key);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
		}
		return;
	}

	if (c->hopcount > ctdb->statistics.max_hop_count) {
		ctdb->statistics.max_hop_count = c->hopcount;
	}

	/* Try if possible to migrate the record off to the caller node.
	 * From the clients perspective a fetch of the data is just as 
	 * expensive as a migration.
	 */
	if (c->hdr.srcnode != ctdb->pnn) {
		if (ctdb_db->transaction_active) {
			DEBUG(DEBUG_INFO, (__location__ " refusing migration"
			      " of key %s while transaction is active\n",
			      (char *)call->key.dptr));
		} else {
			DEBUG(DEBUG_DEBUG,("pnn %u starting migration of %08x to %u\n",
				 ctdb->pnn, ctdb_hash(&(call->key)), c->hdr.srcnode));
			ctdb_call_send_dmaster(ctdb_db, c, &header, &(call->key), &data);
			talloc_free(data.dptr);

			ret = ctdb_ltdb_unlock(ctdb_db, call->key);
			if (ret != 0) {
				DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
			}
			return;
		}
	}

	ctdb_call_local(ctdb_db, call, &header, hdr, &data);

	ret = ctdb_ltdb_unlock(ctdb_db, call->key);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
	}

	len = offsetof(struct ctdb_reply_call, data) + call->reply_data.dsize;
	r = ctdb_transport_allocate(ctdb, ctdb, CTDB_REPLY_CALL, len, 
				    struct ctdb_reply_call);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	r->hdr.destnode  = hdr->srcnode;
	r->hdr.reqid     = hdr->reqid;
	r->status        = call->status;
	r->datalen       = call->reply_data.dsize;
	if (call->reply_data.dsize) {
		memcpy(&r->data[0], call->reply_data.dptr, call->reply_data.dsize);
	}

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
}

/*
  called when a CTDB_REPLY_CALL packet comes in

  This packet comes in response to a CTDB_REQ_CALL request packet. It
  contains any reply data from the call
*/
void ctdb_reply_call(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_call *c = (struct ctdb_reply_call *)hdr;
	struct ctdb_call_state *state;

	state = ctdb_reqid_find(ctdb, hdr->reqid, struct ctdb_call_state);
	if (state == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " reqid %u not found\n", hdr->reqid));
		return;
	}

	if (hdr->reqid != state->reqid) {
		/* we found a record  but it was the wrong one */
		DEBUG(DEBUG_ERR, ("Dropped orphaned call reply with reqid:%u\n",hdr->reqid));
		return;
	}

	state->call->reply_data.dptr = c->data;
	state->call->reply_data.dsize = c->datalen;
	state->call->status = c->status;

	talloc_steal(state, c);

	state->state = CTDB_CALL_DONE;
	if (state->async.fn) {
		state->async.fn(state);
	}
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
	struct ctdb_db_context *ctdb_db;
	TDB_DATA key, data;
	uint32_t record_flags = 0;
	size_t len;
	int ret;

	ctdb_db = find_ctdb_db(ctdb, c->db_id);
	if (ctdb_db == NULL) {
		DEBUG(DEBUG_ERR,("Unknown db_id 0x%x in ctdb_reply_dmaster\n", c->db_id));
		return;
	}
	
	key.dptr = c->data;
	key.dsize = c->keylen;
	data.dptr = &c->data[key.dsize];
	data.dsize = c->datalen;
	len = offsetof(struct ctdb_reply_dmaster, data) + key.dsize + data.dsize
		+ sizeof(uint32_t);
	if (len <= c->hdr.length) {
		record_flags = *(uint32_t *)&c->data[c->keylen + c->datalen];
	}

	ret = ctdb_ltdb_lock_requeue(ctdb_db, key, hdr,
				     ctdb_call_input_pkt, ctdb, False);
	if (ret == -2) {
		return;
	}
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to get lock in ctdb_reply_dmaster\n"));
		return;
	}

	ctdb_become_dmaster(ctdb_db, hdr, key, data, c->rsn, record_flags);
}


/*
  called when a CTDB_REPLY_ERROR packet comes in
*/
void ctdb_reply_error(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_reply_error *c = (struct ctdb_reply_error *)hdr;
	struct ctdb_call_state *state;

	state = ctdb_reqid_find(ctdb, hdr->reqid, struct ctdb_call_state);
	if (state == NULL) {
		DEBUG(DEBUG_ERR,("pnn %u Invalid reqid %u in ctdb_reply_error\n",
			 ctdb->pnn, hdr->reqid));
		return;
	}

	if (hdr->reqid != state->reqid) {
		/* we found a record  but it was the wrong one */
		DEBUG(DEBUG_ERR, ("Dropped orphaned error reply with reqid:%u\n",hdr->reqid));
		return;
	}

	talloc_steal(state, c);

	state->state  = CTDB_CALL_ERROR;
	state->errmsg = (char *)c->msg;
	if (state->async.fn) {
		state->async.fn(state);
	}
}


/*
  destroy a ctdb_call
*/
static int ctdb_call_destructor(struct ctdb_call_state *state)
{
	DLIST_REMOVE(state->ctdb_db->ctdb->pending_calls, state);
	ctdb_reqid_remove(state->ctdb_db->ctdb, state->reqid);
	return 0;
}


/*
  called when a ctdb_call needs to be resent after a reconfigure event
*/
static void ctdb_call_resend(struct ctdb_call_state *state)
{
	struct ctdb_context *ctdb = state->ctdb_db->ctdb;

	state->generation = ctdb->vnn_map->generation;

	/* use a new reqid, in case the old reply does eventually come in */
	ctdb_reqid_remove(ctdb, state->reqid);
	state->reqid = ctdb_reqid_new(ctdb, state);
	state->c->hdr.reqid = state->reqid;

	/* update the generation count for this request, so its valid with the new vnn_map */
	state->c->hdr.generation = state->generation;

	/* send the packet to ourselves, it will be redirected appropriately */
	state->c->hdr.destnode = ctdb->pnn;

	ctdb_queue_packet(ctdb, &state->c->hdr);
	DEBUG(DEBUG_NOTICE,("resent ctdb_call\n"));
}

/*
  resend all pending calls on recovery
 */
void ctdb_call_resend_all(struct ctdb_context *ctdb)
{
	struct ctdb_call_state *state, *next;
	for (state=ctdb->pending_calls;state;state=next) {
		next = state->next;
		ctdb_call_resend(state);
	}
}

/*
  this allows the caller to setup a async.fn 
*/
static void call_local_trigger(struct event_context *ev, struct timed_event *te, 
		       struct timeval t, void *private_data)
{
	struct ctdb_call_state *state = talloc_get_type(private_data, struct ctdb_call_state);
	if (state->async.fn) {
		state->async.fn(state);
	}
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
	state->call  = talloc(state, struct ctdb_call);
	CTDB_NO_MEMORY_NULL(ctdb, state->call);
	*(state->call) = *call;
	state->ctdb_db = ctdb_db;

	ret = ctdb_call_local(ctdb_db, state->call, header, state, data);

	event_add_timed(ctdb->ev, state, timeval_zero(), call_local_trigger, state);

	return state;
}


/*
  make a remote ctdb call - async send. Called in daemon context.

  This constructs a ctdb_call request and queues it for processing. 
  This call never blocks.
*/
struct ctdb_call_state *ctdb_daemon_call_send_remote(struct ctdb_db_context *ctdb_db, 
						     struct ctdb_call *call, 
						     struct ctdb_ltdb_header *header)
{
	uint32_t len;
	struct ctdb_call_state *state;
	struct ctdb_context *ctdb = ctdb_db->ctdb;

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed send packet. Transport is down\n"));
		return NULL;
	}

	state = talloc_zero(ctdb_db, struct ctdb_call_state);
	CTDB_NO_MEMORY_NULL(ctdb, state);
	state->call = talloc(state, struct ctdb_call);
	CTDB_NO_MEMORY_NULL(ctdb, state->call);

	state->reqid = ctdb_reqid_new(ctdb, state);
	state->ctdb_db = ctdb_db;
	talloc_set_destructor(state, ctdb_call_destructor);

	len = offsetof(struct ctdb_req_call, data) + call->key.dsize + call->call_data.dsize;
	state->c = ctdb_transport_allocate(ctdb, state, CTDB_REQ_CALL, len, 
					   struct ctdb_req_call);
	CTDB_NO_MEMORY_NULL(ctdb, state->c);
	state->c->hdr.destnode  = header->dmaster;

	/* this limits us to 16k outstanding messages - not unreasonable */
	state->c->hdr.reqid     = state->reqid;
	state->c->flags         = call->flags;
	state->c->db_id         = ctdb_db->db_id;
	state->c->callid        = call->call_id;
	state->c->hopcount      = 0;
	state->c->keylen        = call->key.dsize;
	state->c->calldatalen   = call->call_data.dsize;
	memcpy(&state->c->data[0], call->key.dptr, call->key.dsize);
	memcpy(&state->c->data[call->key.dsize], 
	       call->call_data.dptr, call->call_data.dsize);
	*(state->call)              = *call;
	state->call->call_data.dptr = &state->c->data[call->key.dsize];
	state->call->key.dptr       = &state->c->data[0];

	state->state  = CTDB_CALL_WAIT;
	state->generation = ctdb->vnn_map->generation;

	DLIST_ADD(ctdb->pending_calls, state);

	ctdb_queue_packet(ctdb, &state->c->hdr);

	return state;
}

/*
  make a remote ctdb call - async recv - called in daemon context

  This is called when the program wants to wait for a ctdb_call to complete and get the 
  results. This call will block unless the call has already completed.
*/
int ctdb_daemon_call_recv(struct ctdb_call_state *state, struct ctdb_call *call)
{
	while (state->state < CTDB_CALL_DONE) {
		event_loop_once(state->ctdb_db->ctdb->ev);
	}
	if (state->state != CTDB_CALL_DONE) {
		ctdb_set_error(state->ctdb_db->ctdb, "%s", state->errmsg);
		talloc_free(state);
		return -1;
	}

	if (state->call->reply_data.dsize) {
		call->reply_data.dptr = talloc_memdup(call,
						      state->call->reply_data.dptr,
						      state->call->reply_data.dsize);
		call->reply_data.dsize = state->call->reply_data.dsize;
	} else {
		call->reply_data.dptr = NULL;
		call->reply_data.dsize = 0;
	}
	call->status = state->call->status;
	talloc_free(state);
	return 0;
}


/* 
   send a keepalive packet to the other node
*/
void ctdb_send_keepalive(struct ctdb_context *ctdb, uint32_t destnode)
{
	struct ctdb_req_keepalive *r;
	
	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send keepalive. Transport is DOWN\n"));
		return;
	}

	r = ctdb_transport_allocate(ctdb, ctdb, CTDB_REQ_KEEPALIVE,
				    sizeof(struct ctdb_req_keepalive), 
				    struct ctdb_req_keepalive);
	CTDB_NO_MEMORY_FATAL(ctdb, r);
	r->hdr.destnode  = destnode;
	r->hdr.reqid     = 0;
	
	ctdb->statistics.keepalive_packets_sent++;

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
}
