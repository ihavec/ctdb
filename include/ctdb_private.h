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


/*
  an installed ctdb remote call
*/
struct ctdb_registered_call {
	struct ctdb_registered_call *next, *prev;
	uint32_t id;
	ctdb_fn_t fn;
};

/*
  this address structure might need to be generalised later for some
  transports
*/
struct ctdb_address {
	const char *address;
	int port;
};

/*
  state associated with one node
*/
struct ctdb_node {
	struct ctdb_context *ctdb;
	struct ctdb_address address;
	const char *name; /* for debug messages */
	void *private; /* private to transport */
	uint32_t vnn;
};

/*
  transport specific methods
*/
struct ctdb_methods {
	int (*start)(struct ctdb_context *); /* start protocol processing */	
	int (*add_node)(struct ctdb_node *); /* setup a new node */	
	int (*queue_pkt)(struct ctdb_node *, uint8_t *data, uint32_t length);
};

/*
  transport calls up to the ctdb layer
*/
struct ctdb_upcalls {
	/* recv_pkt is called when a packet comes in */
	void (*recv_pkt)(struct ctdb_context *, uint8_t *data, uint32_t length);

	/* node_dead is called when an attempt to send to a node fails */
	void (*node_dead)(struct ctdb_node *);

	/* node_connected is called when a connection to a node is established */
	void (*node_connected)(struct ctdb_node *);
};

/* main state of the ctdb daemon */
struct ctdb_context {
	struct event_context *ev;
	struct ctdb_address address;
	const char *name;
	uint32_t vnn; /* our own vnn */
	uint32_t num_nodes;
	struct idr_context *idr;
	struct ctdb_node **nodes; /* array of nodes in the cluster - indexed by vnn */
	struct ctdb_registered_call *calls; /* list of registered calls */
	char *err_msg;
	struct tdb_context *ltdb;
	const struct ctdb_methods *methods; /* transport methods */
	const struct ctdb_upcalls *upcalls; /* transport upcalls */
	void *private; /* private to transport */
};

#define CTDB_NO_MEMORY(ctdb, p) do { if (!(p)) { \
          ctdb_set_error(ctdb, "Out of memory at %s:%d", __FILE__, __LINE__); \
	  return -1; }} while (0)

/* arbitrary maximum timeout for ctdb operations */
#define CTDB_REQ_TIMEOUT 10


/*
  operation IDs
*/
enum ctdb_operation {
	CTDB_OP_CALL = 0
};

/*
  packet structures
*/
struct ctdb_req_header {
	uint32_t _length; /* ignored by datagram transports */
	uint32_t operation;
	uint32_t destnode;
	uint32_t srcnode;
	uint32_t reqid;
	uint32_t reqtimeout;
};

struct ctdb_reply_header {
	uint32_t _length; /* ignored by datagram transports */
	uint32_t operation;
	uint32_t destnode;
	uint32_t srcnode;
	uint32_t reqid;
};

struct ctdb_req_call {
	struct ctdb_req_header hdr;
	uint32_t callid;
	uint32_t keylen;
	uint32_t calldatalen;
	uint8_t data[0]; /* key[] followed by calldata[] */
};

struct ctdb_reply_call {
	struct ctdb_reply_header hdr;
	uint32_t datalen;
	uint8_t  data[0];
};

/* internal prototypes */
void ctdb_set_error(struct ctdb_context *ctdb, const char *fmt, ...);
bool ctdb_same_address(struct ctdb_address *a1, struct ctdb_address *a2);
int ctdb_parse_address(struct ctdb_context *ctdb,
		       TALLOC_CTX *mem_ctx, const char *str,
		       struct ctdb_address *address);
uint32_t ctdb_hash(TDB_DATA *key);

