/* 
   ctdb over TCP

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
#include "system/network.h"
#include "system/filesys.h"
#include "../include/ctdb_private.h"
#include "ctdb_tcp.h"


/*
  initialise tcp portion of a ctdb node 
*/
static int ctdb_tcp_add_node(struct ctdb_node *node)
{
	struct ctdb_tcp_node *tnode;
	tnode = talloc_zero(node, struct ctdb_tcp_node);
	CTDB_NO_MEMORY(node->ctdb, tnode);

	tnode->fd = -1;
	node->private_data = tnode;

	tnode->queue = ctdb_queue_setup(node->ctdb, node, tnode->fd, CTDB_TCP_ALIGNMENT,
					ctdb_tcp_tnode_cb, node);
	
	return 0;
}

/*
  initialise transport structures
*/
static int ctdb_tcp_initialise(struct ctdb_context *ctdb)
{
	int i;

	/* listen on our own address */
	if (ctdb_tcp_listen(ctdb) != 0) return -1;

	for (i=0; i<ctdb->num_nodes; i++) {
		if (ctdb_tcp_add_node(ctdb->nodes[i]) != 0) {
			DEBUG(0, ("methods->add_node failed at %d\n", i));
			return -1;
		}
	}
	
	return 0;
}

/*
  start the protocol going
*/
static int ctdb_tcp_start(struct ctdb_context *ctdb)
{
	int i;

	/* startup connections to the other servers - will happen on
	   next event loop */
	for (i=0;i<ctdb->num_nodes;i++) {
		struct ctdb_node *node = *(ctdb->nodes + i);
		if (!(ctdb->flags & CTDB_FLAG_SELF_CONNECT) &&
		    ctdb_same_address(&ctdb->address, &node->address)) continue;
		event_add_timed(ctdb->ev, node, timeval_zero(), 
				ctdb_tcp_node_connect, node);
	}

	return 0;
}


/*
  transport packet allocator - allows transport to control memory for packets
*/
static void *ctdb_tcp_allocate_pkt(TALLOC_CTX *mem_ctx, size_t size)
{
	/* tcp transport needs to round to 8 byte alignment to ensure
	   that we can use a length header and 64 bit elements in
	   structures */
	size = (size+(CTDB_TCP_ALIGNMENT-1)) & ~(CTDB_TCP_ALIGNMENT-1);
	return talloc_size(mem_ctx, size);
}


static const struct ctdb_methods ctdb_tcp_methods = {
	.initialise   = ctdb_tcp_initialise,
	.start        = ctdb_tcp_start,
	.queue_pkt    = ctdb_tcp_queue_pkt,
	.add_node     = ctdb_tcp_add_node,
	.allocate_pkt = ctdb_tcp_allocate_pkt
};

/*
  initialise tcp portion of ctdb 
*/
int ctdb_tcp_init(struct ctdb_context *ctdb)
{
	struct ctdb_tcp *ctcp;
	ctcp = talloc_zero(ctdb, struct ctdb_tcp);
	CTDB_NO_MEMORY(ctdb, ctcp);

	ctcp->listen_fd = -1;
	ctdb->private_data = ctcp;
	ctdb->methods = &ctdb_tcp_methods;
	return 0;
}

