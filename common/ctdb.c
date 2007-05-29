/* 
   ctdb main protocol code

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
#include "lib/tdb/include/tdb.h"
#include "lib/events/events.h"
#include "lib/util/dlinklist.h"
#include "system/network.h"
#include "system/filesys.h"
#include "../include/ctdb_private.h"

/*
  choose the transport we will use
*/
int ctdb_set_transport(struct ctdb_context *ctdb, const char *transport)
{
	ctdb->transport = talloc_strdup(ctdb, transport);
	return 0;
}

/*
  choose the logfile location
*/
int ctdb_set_logfile(struct ctdb_context *ctdb, const char *logfile)
{
	ctdb->logfile = talloc_strdup(ctdb, logfile);
	if (ctdb->logfile != NULL) {
		int fd;
		close(1);
		close(2);
		fd = open(ctdb->logfile, O_WRONLY|O_APPEND|O_CREAT, 0666);
		if (fd == -1) {
			abort();
		}
		if (fd != 1) {
			dup2(fd, 1);
			close(fd);
		}
		/* also catch stderr of subcommands to the log file */
		dup2(1, 2);
	}
	return 0;
}


/*
  set some ctdb flags
*/
void ctdb_set_flags(struct ctdb_context *ctdb, unsigned flags)
{
	ctdb->flags |= flags;
}

/*
  clear some ctdb flags
*/
void ctdb_clear_flags(struct ctdb_context *ctdb, unsigned flags)
{
	ctdb->flags &= ~flags;
}

/*
  set max acess count before a dmaster migration
*/
void ctdb_set_max_lacount(struct ctdb_context *ctdb, unsigned count)
{
	ctdb->max_lacount = count;
}

/*
  set the directory for the local databases
*/
int ctdb_set_tdb_dir(struct ctdb_context *ctdb, const char *dir)
{
	ctdb->db_directory = talloc_strdup(ctdb, dir);
	if (ctdb->db_directory == NULL) {
		return -1;
	}
	return 0;
}

/*
  add a node to the list of active nodes
*/
static int ctdb_add_node(struct ctdb_context *ctdb, char *nstr)
{
	struct ctdb_node *node, **nodep;

	nodep = talloc_realloc(ctdb, ctdb->nodes, struct ctdb_node *, ctdb->num_nodes+1);
	CTDB_NO_MEMORY(ctdb, nodep);

	ctdb->nodes = nodep;
	nodep = &ctdb->nodes[ctdb->num_nodes];
	(*nodep) = talloc_zero(ctdb->nodes, struct ctdb_node);
	CTDB_NO_MEMORY(ctdb, *nodep);
	node = *nodep;

	if (ctdb_parse_address(ctdb, node, nstr, &node->address) != 0) {
		return -1;
	}
	node->ctdb = ctdb;
	node->name = talloc_asprintf(node, "%s:%u", 
				     node->address.address, 
				     node->address.port);
	/* this assumes that the nodes are kept in sorted order, and no gaps */
	node->vnn = ctdb->num_nodes;

	if (ctdb->address.address &&
	    ctdb_same_address(&ctdb->address, &node->address)) {
		ctdb->vnn = node->vnn;
		node->flags |= NODE_FLAGS_CONNECTED;
	}

	ctdb->num_nodes++;
	node->dead_count = 0;

	return 0;
}

/*
  setup the node list from a file
*/
int ctdb_set_nlist(struct ctdb_context *ctdb, const char *nlist)
{
	char **lines;
	int nlines;
	int i;

	talloc_free(ctdb->node_list_file);
	ctdb->node_list_file = talloc_strdup(ctdb, nlist);

	lines = file_lines_load(nlist, &nlines, ctdb);
	if (lines == NULL) {
		ctdb_set_error(ctdb, "Failed to load nlist '%s'\n", nlist);
		return -1;
	}
	while (nlines > 0 && strcmp(lines[nlines-1], "") == 0) {
		nlines--;
	}

	for (i=0;i<nlines;i++) {
		if (ctdb_add_node(ctdb, lines[i]) != 0) {
			talloc_free(lines);
			return -1;
		}
	}

	/* initialize the vnn mapping table now that we have num_nodes setup */
/*
XXX we currently initialize it to the maximum number of nodes to 
XXX make it behave the same way as previously.  
XXX Once we have recovery working we should initialize this always to 
XXX generation==0 (==invalid) and let the recovery tool populate this 
XXX table for the daemons. 
*/
	ctdb->vnn_map = talloc(ctdb, struct ctdb_vnn_map);
	CTDB_NO_MEMORY(ctdb, ctdb->vnn_map);

	ctdb->vnn_map->generation = 1;
	ctdb->vnn_map->size = ctdb->num_nodes;
	ctdb->vnn_map->map = talloc_array(ctdb->vnn_map, uint32_t, ctdb->vnn_map->size);
	CTDB_NO_MEMORY(ctdb, ctdb->vnn_map->map);

	for(i=0;i<ctdb->vnn_map->size;i++) {
		ctdb->vnn_map->map[i] = i;
	}
	
	talloc_free(lines);
	return 0;
}


/*
  setup the local node address
*/
int ctdb_set_address(struct ctdb_context *ctdb, const char *address)
{
	if (ctdb_parse_address(ctdb, ctdb, address, &ctdb->address) != 0) {
		return -1;
	}
	
	ctdb->name = talloc_asprintf(ctdb, "%s:%u", 
				     ctdb->address.address, 
				     ctdb->address.port);
	return 0;
}


/*
  setup the local socket name
*/
int ctdb_set_socketname(struct ctdb_context *ctdb, const char *socketname)
{
	ctdb->daemon.name = talloc_strdup(ctdb, socketname);
	return 0;
}
/*
  return the vnn of this node
*/
uint32_t ctdb_get_vnn(struct ctdb_context *ctdb)
{
	return ctdb->vnn;
}

/*
  return the number of nodes
*/
uint32_t ctdb_get_num_nodes(struct ctdb_context *ctdb)
{
	return ctdb->num_nodes;
}

/*
  return the number of connected nodes
*/
uint32_t ctdb_get_num_connected_nodes(struct ctdb_context *ctdb)
{
	int i;
	uint32_t count=0;
	for (i=0;i<ctdb->vnn_map->size;i++) {
		if (ctdb->nodes[ctdb->vnn_map->map[i]]->flags & NODE_FLAGS_CONNECTED) {
			count++;
		}
	}
	return count;
}


/*
  called when we need to process a packet. This can be a requeued packet
  after a lockwait, or a real packet from another node
*/
void ctdb_input_pkt(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	TALLOC_CTX *tmp_ctx;

	/* place the packet as a child of the tmp_ctx. We then use
	   talloc_free() below to free it. If any of the calls want
	   to keep it, then they will steal it somewhere else, and the
	   talloc_free() will only free the tmp_ctx */
	tmp_ctx = talloc_new(ctdb);
	talloc_steal(tmp_ctx, hdr);

	DEBUG(3,(__location__ " ctdb request %u of type %u length %u from "
		 "node %u to %u\n", hdr->reqid, hdr->operation, hdr->length,
		 hdr->srcnode, hdr->destnode));

	switch (hdr->operation) {
	case CTDB_REQ_CALL:
	case CTDB_REPLY_CALL:
	case CTDB_REQ_DMASTER:
	case CTDB_REPLY_DMASTER:
		/* for ctdb_call inter-node operations verify that the
		   remote node that sent us the call is running in the
		   same generation instance as this node
		*/
		if (ctdb->vnn_map->generation != hdr->generation) {
			DEBUG(0,(__location__ " ctdb request %u"
				" length %u from node %u to %u had an"
				" invalid generation id:%u while our"
				" generation id is:%u\n", 
				 hdr->reqid, hdr->length, 
				 hdr->srcnode, hdr->destnode, 
				 hdr->generation, ctdb->vnn_map->generation));
			goto done;
		}
	}

	switch (hdr->operation) {
	case CTDB_REQ_CALL:
		ctdb->statistics.node.req_call++;
		ctdb_request_call(ctdb, hdr);
		break;

	case CTDB_REPLY_CALL:
		ctdb->statistics.node.reply_call++;
		ctdb_reply_call(ctdb, hdr);
		break;

	case CTDB_REPLY_ERROR:
		ctdb->statistics.node.reply_error++;
		ctdb_reply_error(ctdb, hdr);
		break;

	case CTDB_REQ_DMASTER:
		ctdb->statistics.node.req_dmaster++;
		ctdb_request_dmaster(ctdb, hdr);
		break;

	case CTDB_REPLY_DMASTER:
		ctdb->statistics.node.reply_dmaster++;
		ctdb_reply_dmaster(ctdb, hdr);
		break;

	case CTDB_REQ_MESSAGE:
		ctdb->statistics.node.req_message++;
		ctdb_request_message(ctdb, hdr);
		break;

	case CTDB_REQ_FINISHED:
		ctdb->statistics.node.req_finished++;
		ctdb_request_finished(ctdb, hdr);
		break;

	case CTDB_REQ_CONTROL:
		ctdb->statistics.node.req_control++;
		ctdb_request_control(ctdb, hdr);
		break;

	case CTDB_REPLY_CONTROL:
		ctdb->statistics.node.reply_control++;
		ctdb_reply_control(ctdb, hdr);
		break;

	case CTDB_REQ_KEEPALIVE:
		ctdb->statistics.keepalive_packets_recv++;
		break;

	default:
		DEBUG(0,("%s: Packet with unknown operation %u\n", 
			 __location__, hdr->operation));
		break;
	}

done:
	talloc_free(tmp_ctx);
}


/*
  called by the transport layer when a packet comes in
*/
static void ctdb_recv_pkt(struct ctdb_context *ctdb, uint8_t *data, uint32_t length)
{
	struct ctdb_req_header *hdr = (struct ctdb_req_header *)data;

	ctdb->statistics.node_packets_recv++;

	/* up the counter for this source node, so we know its alive */
	if (ctdb_validate_vnn(ctdb, hdr->srcnode)) {
		/* as a special case, redirected calls don't increment the rx_cnt */
		if (hdr->operation != CTDB_REQ_CALL ||
		    ((struct ctdb_req_call *)hdr)->hopcount == 0) {
			ctdb->nodes[hdr->srcnode]->rx_cnt++;
		}
	}

	ctdb_input_pkt(ctdb, hdr);
}


/*
  called by the transport layer when a node is dead
*/
void ctdb_node_dead(struct ctdb_node *node)
{
	node->ctdb->num_connected--;
	node->flags &= ~NODE_FLAGS_CONNECTED;
	node->rx_cnt = 0;
	node->dead_count = 0;
	DEBUG(1,("%s: node %s is dead: %u connected\n", 
		 node->ctdb->name, node->name, node->ctdb->num_connected));
	ctdb_daemon_cancel_controls(node->ctdb, node);
}

/*
  called by the transport layer when a node is connected
*/
void ctdb_node_connected(struct ctdb_node *node)
{
	node->ctdb->num_connected++;
	node->dead_count = 0;
	node->flags |= NODE_FLAGS_CONNECTED;
	DEBUG(1,("%s: connected to %s - %u connected\n", 
		 node->ctdb->name, node->name, node->ctdb->num_connected));
}

/*
  wait for all nodes to be connected
*/
void ctdb_daemon_connect_wait(struct ctdb_context *ctdb)
{
	int expected = ctdb->num_nodes - 1;
	if (ctdb->flags & CTDB_FLAG_SELF_CONNECT) {
		expected++;
	}
	while (ctdb->num_connected != expected) {
		DEBUG(3,("ctdb_connect_wait: waiting for %u nodes (have %u)\n", 
			 expected, ctdb->num_connected));
		event_loop_once(ctdb->ev);
	}
	DEBUG(3,("ctdb_connect_wait: got all %u nodes\n", expected));
}

struct queue_next {
	struct ctdb_context *ctdb;
	struct ctdb_req_header *hdr;
};


/*
  trigered when a deferred packet is due
 */
static void queue_next_trigger(struct event_context *ev, struct timed_event *te, 
			       struct timeval t, void *private_data)
{
	struct queue_next *q = talloc_get_type(private_data, struct queue_next);
	ctdb_input_pkt(q->ctdb, q->hdr);
	talloc_free(q);
}	

/*
  defer a packet, so it is processed on the next event loop
  this is used for sending packets to ourselves
 */
static void ctdb_defer_packet(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct queue_next *q;
	q = talloc(ctdb, struct queue_next);
	if (q == NULL) {
		DEBUG(0,(__location__ " Failed to allocate deferred packet\n"));
		return;
	}
	q->ctdb = ctdb;
	q->hdr = talloc_memdup(ctdb, hdr, hdr->length);
	if (q->hdr == NULL) {
		DEBUG(0,("Error copying deferred packet to self\n"));
		return;
	}
#if 0
	/* use this to put packets directly into our recv function */
	ctdb_input_pkt(q->ctdb, q->hdr);
#else
	event_add_timed(ctdb->ev, q, timeval_zero(), queue_next_trigger, q);
#endif
}


/*
  broadcast a packet to all nodes
*/
static void ctdb_broadcast_packet_all(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	int i;
	for (i=0;i<ctdb_get_num_nodes(ctdb);i++) {
		hdr->destnode = ctdb->nodes[i]->vnn;
		ctdb_queue_packet(ctdb, hdr);
	}
}

/*
  broadcast a packet to all nodes in the current vnnmap
*/
static void ctdb_broadcast_packet_vnnmap(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	int i;
	for (i=0;i<ctdb->vnn_map->size;i++) {
		hdr->destnode = ctdb->vnn_map->map[i];
		ctdb_queue_packet(ctdb, hdr);
	}
}

/*
  queue a packet or die
*/
void ctdb_queue_packet(struct ctdb_context *ctdb, struct ctdb_req_header *hdr)
{
	struct ctdb_node *node;

	switch (hdr->destnode) {
	case CTDB_BROADCAST_ALL:
		ctdb_broadcast_packet_all(ctdb, hdr);
		return;
	case CTDB_BROADCAST_VNNMAP:
		ctdb_broadcast_packet_vnnmap(ctdb, hdr);
		return;
	}

	ctdb->statistics.node_packets_sent++;

	if (!ctdb_validate_vnn(ctdb, hdr->destnode)) {
	  	DEBUG(0,(__location__ " cant send to node %u that does not exist\n", 
			 hdr->destnode));
		return;
	}

	node = ctdb->nodes[hdr->destnode];

	if (hdr->destnode == ctdb->vnn && !(ctdb->flags & CTDB_FLAG_SELF_CONNECT)) {
		ctdb_defer_packet(ctdb, hdr);
	} else {
		node->tx_cnt++;
		if (ctdb->methods->queue_pkt(node, (uint8_t *)hdr, hdr->length) != 0) {
			ctdb_fatal(ctdb, "Unable to queue packet\n");
		}
	}
}


static const struct ctdb_upcalls ctdb_upcalls = {
	.recv_pkt       = ctdb_recv_pkt,
	.node_dead      = ctdb_node_dead,
	.node_connected = ctdb_node_connected
};

/*
  initialise the ctdb daemon. 

  NOTE: In current code the daemon does not fork. This is for testing purposes only
  and to simplify the code.
*/
struct ctdb_context *ctdb_init(struct event_context *ev)
{
	struct ctdb_context *ctdb;

	ctdb = talloc_zero(ev, struct ctdb_context);
	ctdb->ev               = ev;
	ctdb->recovery_mode    = CTDB_RECOVERY_NORMAL;
	ctdb->recovery_master  = (uint32_t)-1;
	ctdb->upcalls          = &ctdb_upcalls;
	ctdb->idr              = idr_init(ctdb);
	ctdb->max_lacount      = CTDB_DEFAULT_MAX_LACOUNT;
	ctdb->seqnum_frequency = CTDB_DEFAULT_SEQNUM_FREQUENCY;
	ctdb->node_list_fd     = -1;
	ctdb->monitoring_mode  = CTDB_MONITORING_ACTIVE;

	return ctdb;
}

