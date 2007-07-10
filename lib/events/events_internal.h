/* 
   Unix SMB/CIFS implementation.

   generalised event loop handling

   Internal structs

   Copyright (C) Stefan Metzmacher 2005
   
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

struct event_ops {
	/* conntext init */
	int (*context_init)(struct event_context *ev);

	/* fd_event functions */
	struct fd_event *(*add_fd)(struct event_context *ev,
				   TALLOC_CTX *mem_ctx,
				   int fd, uint16_t flags,
				   event_fd_handler_t handler,
				   void *private_data);
	uint16_t (*get_fd_flags)(struct fd_event *fde);
	void (*set_fd_flags)(struct fd_event *fde, uint16_t flags);

	/* timed_event functions */
	struct timed_event *(*add_timed)(struct event_context *ev,
					 TALLOC_CTX *mem_ctx,
					 struct timeval next_event,
					 event_timed_handler_t handler,
					 void *private_data);
	/* disk aio event functions */
	struct aio_event *(*add_aio)(struct event_context *ev,
				     TALLOC_CTX *mem_ctx,
				     struct iocb *iocb, 
				     event_aio_handler_t handler, 
				     void *private_data);
	/* signal functions */
	struct signal_event *(*add_signal)(struct event_context *ev, 
					   TALLOC_CTX *mem_ctx,
					   int signum, int sa_flags,
					   event_signal_handler_t handler, 
					   void *private_data);

	/* loop functions */
	int (*loop_once)(struct event_context *ev);
	int (*loop_wait)(struct event_context *ev);
};

struct fd_event {
	struct fd_event *prev, *next;
	struct event_context *event_ctx;
	int fd;
	uint16_t flags; /* see EVENT_FD_* flags */
	event_fd_handler_t handler;
	/* this is private for the specific handler */
	void *private_data;
	/* this is private for the events_ops implementation */
	uint16_t additional_flags;
	void *additional_data;
};

struct timed_event {
	struct timed_event *prev, *next;
	struct event_context *event_ctx;
	struct timeval next_event;
	event_timed_handler_t handler;
	/* this is private for the specific handler */
	void *private_data;
	/* this is private for the events_ops implementation */
	void *additional_data;
};

struct signal_event {
	struct signal_event *prev, *next;
	struct event_context *event_ctx;
	event_signal_handler_t handler;
	void *private_data;
	int signum;
	int sa_flags;
};

/* aio event is private to the aio backend */
struct aio_event;

struct event_context {	
	/* the specific events implementation */
	const struct event_ops *ops;

	/* list of timed events - used by common code */
	struct timed_event *timed_events;

	/* this is private for the events_ops implementation */
	void *additional_data;

	/* number of signal event handlers */
	int num_signal_handlers;

	/* pipe hack used with signal handlers */
	struct fd_event *pipe_fde;
};


bool event_register_backend(const char *name, const struct event_ops *ops);

struct timed_event *common_event_add_timed(struct event_context *, TALLOC_CTX *,
					   struct timeval, event_timed_handler_t, void *);
struct timeval common_event_loop_timer_delay(struct event_context *);

struct signal_event *common_event_add_signal(struct event_context *ev, 
					     TALLOC_CTX *mem_ctx,
					     int signum,
					     int sa_flags,
					     event_signal_handler_t handler, 
					     void *private_data);
int common_event_check_signal(struct event_context *ev);

