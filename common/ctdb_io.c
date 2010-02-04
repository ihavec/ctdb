/* 
   ctdb database library
   Utility functions to read/write blobs of data from a file descriptor
   and handle the case where we might need multiple read/writes to get all the
   data.

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
#include "lib/tdb/include/tdb.h"
#include "lib/events/events.h"
#include "lib/util/dlinklist.h"
#include "system/network.h"
#include "system/filesys.h"
#include "../include/ctdb_private.h"
#include "../include/ctdb.h"

/* structures for packet queueing - see common/ctdb_io.c */
struct ctdb_partial {
	uint8_t *data;
	uint32_t length;
};

struct ctdb_queue_pkt {
	struct ctdb_queue_pkt *next, *prev;
	uint8_t *data;
	uint32_t length;
	uint32_t full_length;
};

struct ctdb_queue {
	struct ctdb_context *ctdb;
	struct ctdb_partial partial; /* partial input packet */
	struct ctdb_queue_pkt *out_queue, *out_queue_tail;
	uint32_t out_queue_length;
	struct fd_event *fde;
	int fd;
	size_t alignment;
	void *private_data;
	ctdb_queue_cb_fn_t callback;
	bool *destroyed;
};



int ctdb_queue_length(struct ctdb_queue *queue)
{
	return queue->out_queue_length;
}

/*
  called when an incoming connection is readable
*/
static void queue_io_read(struct ctdb_queue *queue)
{
	int num_ready = 0;
	ssize_t nread;
	uint8_t *data, *data_base;

	if (ioctl(queue->fd, FIONREAD, &num_ready) != 0) {
		return;
	}
	if (num_ready == 0) {
		/* the descriptor has been closed */
		goto failed;
	}


	queue->partial.data = talloc_realloc_size(queue, queue->partial.data, 
						  num_ready + queue->partial.length);

	if (queue->partial.data == NULL) {
		DEBUG(DEBUG_ERR,("read error alloc failed for %u\n", 
			 num_ready + queue->partial.length));
		goto failed;
	}

	nread = read(queue->fd, queue->partial.data + queue->partial.length, num_ready);
	if (nread <= 0) {
		DEBUG(DEBUG_ERR,("read error nread=%d\n", (int)nread));
		goto failed;
	}


	data = queue->partial.data;
	nread += queue->partial.length;

	queue->partial.data = NULL;
	queue->partial.length = 0;

	if (nread >= 4 && *(uint32_t *)data == nread) {
		/* it is the responsibility of the incoming packet
		 function to free 'data' */
		queue->callback(data, nread, queue->private_data);
		return;
	}

	data_base = data;

	while (nread >= 4 && *(uint32_t *)data <= nread) {
		/* we have at least one packet */
		uint8_t *d2;
		uint32_t len;
		bool destroyed = false;

		len = *(uint32_t *)data;
		if (len == 0) {
			/* bad packet! treat as EOF */
			DEBUG(DEBUG_CRIT,("Invalid packet of length 0\n"));
			goto failed;
		}
		d2 = talloc_memdup(queue, data, len);
		if (d2 == NULL) {
			DEBUG(DEBUG_ERR,("read error memdup failed for %u\n", len));
			/* sigh */
			goto failed;
		}

		queue->destroyed = &destroyed;
		queue->callback(d2, len, queue->private_data);
		/* If callback freed us, don't do anything else. */
		if (destroyed) {
			return;
		}
		queue->destroyed = NULL;

		data += len;
		nread -= len;		
	}

	if (nread > 0) {
		/* we have only part of a packet */
		if (data_base == data) {
			queue->partial.data = data;
			queue->partial.length = nread;
		} else {
			queue->partial.data = talloc_memdup(queue, data, nread);
			if (queue->partial.data == NULL) {
				DEBUG(DEBUG_ERR,("read error memdup partial failed for %u\n", 
					 (unsigned)nread));
				goto failed;
			}
			queue->partial.length = nread;
			talloc_free(data_base);
		}
		return;
	}

	talloc_free(data_base);
	return;

failed:
	queue->callback(NULL, 0, queue->private_data);
}


/* used when an event triggers a dead queue */
static void queue_dead(struct event_context *ev, struct timed_event *te, 
		       struct timeval t, void *private_data)
{
	struct ctdb_queue *queue = talloc_get_type(private_data, struct ctdb_queue);
	queue->callback(NULL, 0, queue->private_data);
}


/*
  called when an incoming connection is writeable
*/
static void queue_io_write(struct ctdb_queue *queue)
{
	while (queue->out_queue) {
		struct ctdb_queue_pkt *pkt = queue->out_queue;
		ssize_t n;
		if (queue->ctdb->flags & CTDB_FLAG_TORTURE) {
			n = write(queue->fd, pkt->data, 1);
		} else {
			n = write(queue->fd, pkt->data, pkt->length);
		}

		if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
			if (pkt->length != pkt->full_length) {
				/* partial packet sent - we have to drop it */
				TLIST_REMOVE(queue->out_queue, queue->out_queue_tail,
					     pkt);
				queue->out_queue_length--;
				talloc_free(pkt);
			}
			talloc_free(queue->fde);
			queue->fde = NULL;
			queue->fd = -1;
			event_add_timed(queue->ctdb->ev, queue, timeval_zero(), 
					queue_dead, queue);
			return;
		}
		if (n <= 0) return;
		
		if (n != pkt->length) {
			pkt->length -= n;
			pkt->data += n;
			return;
		}

		TLIST_REMOVE(queue->out_queue, queue->out_queue_tail, pkt);
		queue->out_queue_length--;
		talloc_free(pkt);
	}

	EVENT_FD_NOT_WRITEABLE(queue->fde);
}

/*
  called when an incoming connection is readable or writeable
*/
static void queue_io_handler(struct event_context *ev, struct fd_event *fde, 
			     uint16_t flags, void *private_data)
{
	struct ctdb_queue *queue = talloc_get_type(private_data, struct ctdb_queue);

	if (flags & EVENT_FD_READ) {
		queue_io_read(queue);
	} else {
		queue_io_write(queue);
	}
}


/*
  queue a packet for sending
*/
int ctdb_queue_send(struct ctdb_queue *queue, uint8_t *data, uint32_t length)
{
	struct ctdb_queue_pkt *pkt;
	uint32_t length2, full_length;

	if (queue->alignment) {
		/* enforce the length and alignment rules from the tcp packet allocator */
		length2 = (length+(queue->alignment-1)) & ~(queue->alignment-1);
		*(uint32_t *)data = length2;
	} else {
		length2 = length;
	}

	if (length2 != length) {
		memset(data+length, 0, length2-length);
	}

	full_length = length2;
	
	/* if the queue is empty then try an immediate write, avoiding
	   queue overhead. This relies on non-blocking sockets */
	if (queue->out_queue == NULL && queue->fd != -1 &&
	    !(queue->ctdb->flags & CTDB_FLAG_TORTURE)) {
		ssize_t n = write(queue->fd, data, length2);
		if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
			talloc_free(queue->fde);
			queue->fde = NULL;
			queue->fd = -1;
			event_add_timed(queue->ctdb->ev, queue, timeval_zero(), 
					queue_dead, queue);
			/* yes, we report success, as the dead node is 
			   handled via a separate event */
			return 0;
		}
		if (n > 0) {
			data += n;
			length2 -= n;
		}
		if (length2 == 0) return 0;
	}

	pkt = talloc(queue, struct ctdb_queue_pkt);
	CTDB_NO_MEMORY(queue->ctdb, pkt);

	pkt->data = talloc_memdup(pkt, data, length2);
	CTDB_NO_MEMORY(queue->ctdb, pkt->data);

	pkt->length = length2;
	pkt->full_length = full_length;

	if (queue->out_queue == NULL && queue->fd != -1) {
		EVENT_FD_WRITEABLE(queue->fde);
	}

	TLIST_ADD_END(queue->out_queue, queue->out_queue_tail, pkt);

	queue->out_queue_length++;

	if (queue->ctdb->tunable.verbose_memory_names != 0) {
		struct ctdb_req_header *hdr = (struct ctdb_req_header *)pkt->data;
		switch (hdr->operation) {
		case CTDB_REQ_CONTROL: {
			struct ctdb_req_control *c = (struct ctdb_req_control *)hdr;
			talloc_set_name(pkt, "ctdb_queue_pkt: control opcode=%u srvid=%llu datalen=%u",
					(unsigned)c->opcode, (unsigned long long)c->srvid, (unsigned)c->datalen);
			break;
		}
		case CTDB_REQ_MESSAGE: {
			struct ctdb_req_message *m = (struct ctdb_req_message *)hdr;
			talloc_set_name(pkt, "ctdb_queue_pkt: message srvid=%llu datalen=%u",
					(unsigned long long)m->srvid, (unsigned)m->datalen);
			break;
		}
		default:
			talloc_set_name(pkt, "ctdb_queue_pkt: operation=%u length=%u src=%u dest=%u",
					(unsigned)hdr->operation, (unsigned)hdr->length, 
					(unsigned)hdr->srcnode, (unsigned)hdr->destnode);
			break;
		}
	}

	return 0;
}


/*
  setup the fd used by the queue
 */
int ctdb_queue_set_fd(struct ctdb_queue *queue, int fd)
{
	queue->fd = fd;
	talloc_free(queue->fde);
	queue->fde = NULL;

	if (fd != -1) {
		queue->fde = event_add_fd(queue->ctdb->ev, queue, fd, EVENT_FD_READ|EVENT_FD_AUTOCLOSE, 
					  queue_io_handler, queue);
		if (queue->fde == NULL) {
			return -1;
		}

		if (queue->out_queue) {
			EVENT_FD_WRITEABLE(queue->fde);		
		}
	}

	return 0;
}

/* If someone sets up this pointer, they want to know if the queue is freed */
static int queue_destructor(struct ctdb_queue *queue)
{
	if (queue->destroyed != NULL)
		*queue->destroyed = true;
	return 0;
}

/*
  setup a packet queue on a socket
 */
struct ctdb_queue *ctdb_queue_setup(struct ctdb_context *ctdb,
				    TALLOC_CTX *mem_ctx, int fd, int alignment,
				    
				    ctdb_queue_cb_fn_t callback,
				    void *private_data)
{
	struct ctdb_queue *queue;

	queue = talloc_zero(mem_ctx, struct ctdb_queue);
	CTDB_NO_MEMORY_NULL(ctdb, queue);

	queue->ctdb = ctdb;
	queue->fd = fd;
	queue->alignment = alignment;
	queue->private_data = private_data;
	queue->callback = callback;
	if (fd != -1) {
		if (ctdb_queue_set_fd(queue, fd) != 0) {
			talloc_free(queue);
			return NULL;
		}
	}
	talloc_set_destructor(queue, queue_destructor);

	return queue;
}
