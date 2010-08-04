/*
   ctdb database library

   Copyright (C) Ronnie sahlberg 2010
   Copyright (C) Rusty Russell 2010

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
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <tdb.h>
#include <ctdb_protocol.h>

/**
 * ctdb - a library for accessing tdbs controlled by ctdbd
 *
 * ctdbd (clustered tdb daemon) is a daemon designed to syncronize TDB
 * databases across a cluster.  Using this library, you can communicate with
 * the daemon to access the databases, pass messages across the cluster, and
 * control the daemon itself.
 *
 * The general API is event-driven and asynchronous: you call the
 * *_send functions, supplying callbacks, then when the ctdbd file
 * descriptor is usable, call ctdb_service() to perform read from it
 * and call your callbacks, which use the *_recv functions to unpack
 * the replies from ctdbd.
 *
 * There is also a synchronous wrapper for each function for trivial
 * programs; these can be found in the section marked "Synchronous API".
 */

/**
 * ctdb_log_fn_t - logging function for ctdbd
 * @log_priv: private (typesafe) arg via ctdb_connect
 * @severity: syslog-style severity
 * @format: printf-style format string.
 * @ap: arguments for formatting.
 *
 * The severity passed to log() are as per syslog(3).  In particular,
 * LOG_DEBUG is used for tracing, LOG_WARNING is used for unusual
 * conditions which don't necessarily return an error through the API,
 * LOG_ERR is used for errors such as lost communication with ctdbd or
 * out-of-memory, LOG_ALERT is used for library usage bugs, LOG_CRIT is
 * used for libctdb internal consistency checks.
 *
 * The log() function can be typesafe: the @log_priv arg to
 * ctdb_donnect and signature of log() should match.
 */
typedef void (*ctdb_log_fn_t)(void *log_priv,
			      int severity, const char *format, va_list ap);

/**
 * ctdb_connect - connect to ctdb using the specified domain socket.
 * @addr: the socket address, or NULL for default
 * @log: the logging function
 * @log_priv: the private argument to the logging function.
 *
 * Returns a ctdb context if successful or NULL.  Use ctdb_free() to
 * release the returned ctdb_connection when finished.
 *
 * See Also:
 *	ctdb_log_fn_t, ctdb_log_file()
 */
struct ctdb_connection *ctdb_connect(const char *addr,
				     ctdb_log_fn_t log_fn, void *log_priv);

/**
 * ctdb_log_file - example logging function
 *
 * Logs everything at priority LOG_WARNING or above to the file given (via
 * the log_priv argument, usually stderr).
 */
void ctdb_log_file(FILE *, int, const char *, va_list);

/**
 * ctdb_log_level - level at which to call logging function
 *
 * This variable globally controls filtering on the logging function.
 * It is initialized to LOG_WARNING, meaning that strange but nonfatal
 * events, as well as errors and API misuses are reported.
 *
 * Set it to LOG_DEBUG to receive all messages.
 */
extern int ctdb_log_level;

/***
 *
 *  Asynchronous API
 *
 ***/

/**
 * ctdb_get_fd - get the filedescriptor to select/poll on
 * @ctdb: the ctdb_connection from ctdb_connect.
 *
 * By using poll or select on this file descriptor, you will know when to call
 * ctdb_service().
 *
 * See Also:
 *	ctdb_which_events(), ctdb_service()
 */
int ctdb_get_fd(struct ctdb_connection *ctdb);

/**
 * ctdb_which_events - determine which events ctdb_service wants to see
 * @ctdb: the ctdb_connection from ctdb_connect.
 *
 * This returns POLLIN, possibly or'd with POLLOUT if there are writes
 * pending.  You can set this straight into poll.events.
 *
 * See Also:
 *	ctdb_service()
 */
int ctdb_which_events(struct ctdb_connection *ctdb);

/**
 * ctdb_service - service any I/O and callbacks from ctdbd communication
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @revents: which events are available.
 *
 * This is the core of the library: it read and writes to the ctdbd
 * socket.  It may call callbacks registered with the various _send
 * functions.
 *
 * revents is a bitset: POLLIN and/or POLLOUT may be set to indicate
 * it is worth attempting to read/write the (nonblocking)
 * filedescriptor respectively.
 *
 * Note that the synchronous functions call this internally.
 * Returns false on catastrophic failure.
 */
bool ctdb_service(struct ctdb_connection *ctdb, int revents);

/**
 * struct ctdb_request - handle for an outstanding request
 *
 * This opaque structure returned from various *_send functions gives
 * you a handle by which you can cancel a request.  You can't do
 * anything else with it until the request is completed and it is
 * handed to your callback function.
 */
struct ctdb_request;

/**
 * ctdb_request_free - free a completed request
 *
 * This frees a request: you should only call it once it has been
 * handed to your callback.  For incomplete requests, see ctdb_cancel().
 */
void ctdb_request_free(struct ctdb_connection *ctdb, struct ctdb_request *req);

/**
 * ctdb_callback_t - callback for completed requests.
 *
 * This would normally unpack the request using ctdb_*_recv().  You
 * must free the request using ctdb_request_free().
 *
 * Note that due to macro magic, actual your callback can be typesafe:
 * instead of taking a void *, it can take a type which matches the
 * actual private parameter.
 */
typedef void (*ctdb_callback_t)(struct ctdb_connection *ctdb,
				struct ctdb_request *req, void *private_data);

/**
 * struct ctdb_db - connection to a particular open TDB
 *
 * This represents a particular open database: you receive it from
 * ctdb_attachdb or ctdb_attachdb_recv to manipulate a database.
 *
 * You have to free the handle with ctdb_detach_db() when finished with it.
 */
struct ctdb_db;

/**
 * ctdb_attachdb_send - open a clustered TDB
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @name: the filename of the database (no /).
 * @persistent: whether the database is persistent across ctdbd's life
 * @tdb_flags: the flags to pass to tdb_open.
 * @callback: the callback when we're attached or failed (typesafe)
 * @cbdata: the argument to callback()
 *
 * This function connects to a TDB controlled by ctdbd.  It can create
 * a new TDB if it does not exist, depending on tdb_flags.  Returns
 * the pending request, or NULL on error.
 */
struct ctdb_request *
ctdb_attachdb_send(struct ctdb_connection *ctdb,
		   const char *name, bool persistent, uint32_t tdb_flags,
		   ctdb_callback_t callback, void *cbdata);

/**
 * ctdb_attachdb_recv - read an ctdb_attach reply from ctdbd
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @req: the completed request.
 *
 * This returns NULL if something went wrong, or otherwise the open database.
 */
struct ctdb_db *ctdb_attachdb_recv(struct ctdb_connection *ctdb,
				   struct ctdb_request *req);


/**
 * struct ctdb_lock - a record lock on a clustered TDB database
 *
 * This locks a subset of the database across the entire cluster; it
 * is the fundamental sychronization element for ctdb.  You cannot have
 * more than one lock at once.
 *
 * You MUST NOT block during holding this lock and MUST release it
 * quickly by performing ctdb_release_lock(lock).
 * Do NOT make any system calls that may block while holding the lock.
 *
 * Try to release the lock as quickly as possible.
 */
struct ctdb_lock;

/**
 * ctdb_rrl_callback_t - callback for ctdb_readrecordlock_async
 *
 * This is not the standard ctdb_callback_t, because there is often no
 * request required to access a database record (ie. if it is local already).
 * So the callback is handed the lock directly: it might be NULL if there
 * was an error obtaining the lock.
 *
 * See Also:
 *	ctdb_readrecordlock_async(), ctdb_readrecordlock()
 */
typedef void (*ctdb_rrl_callback_t)(struct ctdb_db *ctdb_db,
				    struct ctdb_lock *lock,
				    TDB_DATA data,
				    void *private_data);

/**
 * ctdb_readrecordlock_async - read and lock a record
 * @ctdb_db: the database handle from ctdb_attachdb/ctdb_attachdb_recv.
 * @key: the key of the record to lock.
 * @callback: the callback once the record is locked (typesafe).
 * @cbdata: the argument to callback()
 *
 * This returns true on success.  Commonly, we can obtain the record
 * immediately and so the callback will be invoked.  Otherwise a request
 * will be queued to ctdbd for the record.
 *
 * If failure is immediate, false is returned.  Otherwise, the callback
 * may receive a NULL lock arg to indicate asynchronous failure.
 */
bool ctdb_readrecordlock_async(struct ctdb_db *ctdb_db, TDB_DATA key,
			       ctdb_rrl_callback_t callback, void *cbdata);

/**
 * ctdb_writerecord - write a locked record in a TDB
 * @ctdb_db: the database handle from ctdb_attachdb/ctdb_attachdb_recv.
 * @lock: the lock from ctdb_readrecordlock/ctdb_readrecordlock_recv
 * @data: the new data to place in the record.
 */
bool ctdb_writerecord(struct ctdb_db *ctdb_db,
		      struct ctdb_lock *lock, TDB_DATA data);

/**
 * ctdb_release_lock - release a record lock on a TDB
 * @ctdb_db: the database handle from ctdb_attachdb/ctdb_attachdb_recv.
 * @lock: the lock from ctdb_readrecordlock/ctdb_readrecordlock_async
 */
void ctdb_release_lock(struct ctdb_db *ctdb_db, struct ctdb_lock *lock);

/**
 * ctdb_message_fn_t - messaging callback for ctdb messages
 *
 * ctdbd provides a simple messaging API; you can register for a particular
 * 64-bit id on which you want to send messages, and send to other ids.
 *
 * See Also:
 *	ctdb_set_message_handler_send()
 */
typedef void (*ctdb_message_fn_t)(struct ctdb_connection *,
				  uint64_t srvid, TDB_DATA data, void *);

/**
 * ctdb_set_message_handler_send - register for messages to a srvid
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @srvid: the 64 bit identifier for our messages.
 * @handler: the callback when we receive such a message (typesafe)
 * @callback: the callback when ctdb replies to our message (typesafe)
 * @cbdata: the argument to callback() and handler()
 *
 * Note: our callback will always be called before handler.
 *
 * See Also:
 *	ctdb_set_message_handler_recv(), ctdb_remove_message_handler_send()
 */
struct ctdb_request *
ctdb_set_message_handler_send(struct ctdb_connection *ctdb, uint64_t srvid,
			      ctdb_message_fn_t handler,
			      ctdb_callback_t callback,
			      void *cbdata);

/**
 * ctdb_set_message_handler_recv - read a set_message_handler result
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @req: the completed request
 *
 * If this returns true, the registered handler may be called from the next
 * ctdb_service().  If this returns false, the registration failed.
 */
bool ctdb_set_message_handler_recv(struct ctdb_connection *ctdb,
				   struct ctdb_request *handle);

/**
 * ctdb_remove_message_handler_send - unregister for messages to a srvid
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @srvid: the 64 bit identifier for our messages.
 * @callback: the callback when ctdb replies to our message (typesafe)
 * @cbdata: the argument to callback()
 *
 * This undoes a successful ctdb_set_message_handler or
 * ctdb_set_message_handler_recv.
 */
struct ctdb_request *
ctdb_remove_message_handler_send(struct ctdb_connection *ctdb, uint64_t srvid,
				 ctdb_callback_t callback, void *cbdata);

/**
 * ctdb_remove_message_handler_recv - read a remove_message_handler result
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @req: the completed request
 *
 * After this returns true, the registered handler will no longer be called.
 * If this returns false, the de-registration failed.
 */
bool ctdb_remove_message_handler_recv(struct ctdb_request *handle);


/**
 * ctdb_send_message - send a message via ctdbd
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @pnn: the physical node number to send to
 * @srvid: the 64 bit identifier for this message type.
 * @data: the data to send
 *
 * This allows arbitrary messages to be sent across the cluster to those
 * listening (via ctdb_set_message_handler et al).
 *
 * This queues a message to be sent: you will need to call
 * ctdb_service() to actually send the message.  There is no callback
 * because there is no acknowledgement.
 *
 * See Also:
 *	ctdb_getpnn_send(), ctdb_getpnn()
 */
bool ctdb_send_message(struct ctdb_connection *ctdb, uint32_t pnn, uint64_t srvid, TDB_DATA data);

/**
 * ctdb_getpnn_send - read the pnn number of a node.
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @destnode: the destination node (see below)
 * @callback: the callback when ctdb replies to our message (typesafe)
 * @cbdata: the argument to callback()
 *
 * There are several special values for destnode, detailed in
 * ctdb_protocol.h, particularly CTDB_CURRENT_NODE which means the
 * local ctdbd.
 */
struct ctdb_request *
ctdb_getpnn_send(struct ctdb_connection *ctdb,
		 uint32_t destnode,
		 ctdb_callback_t callback,
		 void *cbdata);
/**
 * ctdb_getpnn_recv - read an ctdb_getpnn reply from ctdbd
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @req: the completed request.
 * @pnn: a pointer to the pnn to fill in
 *
 * This returns false if something went wrong, or otherwise fills in pnn.
 */
bool ctdb_getpnn_recv(struct ctdb_connection *ctdb,
		      struct ctdb_request *req, uint32_t *pnn);


/**
 * ctdb_getrecmaster_send - read the recovery master of a node
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @destnode: the destination node (see below)
 * @callback: the callback when ctdb replies to our message (typesafe)
 * @cbdata: the argument to callback()
 *
 * There are several special values for destnode, detailed in
 * ctdb_protocol.h, particularly CTDB_CURRENT_NODE which means the
 * local ctdbd.
 */
struct ctdb_request *
ctdb_getrecmaster_send(struct ctdb_connection *ctdb,
			uint32_t destnode,
			    ctdb_callback_t callback, void *cbdata);

/**
 * ctdb_getrecmaster_recv - read an ctdb_getrecmaster reply from ctdbd
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @req: the completed request.
 * @recmaster: a pointer to the recmaster to fill in
 *
 * This returns false if something went wrong, or otherwise fills in
 * recmaster.
 */
bool ctdb_getrecmaster_recv(struct ctdb_connection *ctdb,
			    struct ctdb_request *handle,
			    uint32_t *recmaster);

/**
 * ctdb_cancel - cancel an uncompleted request
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @req: the uncompleted request.
 *
 * This cancels a request, returning true.  You may not cancel a
 * request which has already been completed (ie. once its callback has
 * been called); you should simply use ctdb_request_free() in that case.
 */
void ctdb_cancel(struct ctdb_connection *ctdb, struct ctdb_request *req);

/***
 *
 *  Synchronous API
 *
 ***/

/**
 * ctdb_attachdb - open a clustered TDB (synchronous)
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @name: the filename of the database (no /).
 * @persistent: whether the database is persistent across ctdbd's life
 * @tdb_flags: the flags to pass to tdb_open.
 *
 * Do a ctdb_attachdb_send and wait for it to complete.
 * Returns NULL on failure.
 */
struct ctdb_db *ctdb_attachdb(struct ctdb_connection *ctdb,
			      const char *name, bool persistent,
			      uint32_t tdb_flags);

/**
 * ctdb_readrecordlock - read and lock a record (synchronous)
 * @ctdb_db: the database handle from ctdb_attachdb/ctdb_attachdb_recv.
 * @key: the key of the record to lock.
 * @req: a pointer to the request, if one is needed.
 *
 * Do a ctdb_readrecordlock_send and wait for it to complete.
 * Returns NULL on failure.
 */
struct ctdb_lock *ctdb_readrecordlock(struct ctdb_db *ctdb_db, TDB_DATA key,
				      TDB_DATA *data);


/**
 * ctdb_set_message_handler - register for messages to a srvid (synchronous)
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @srvid: the 64 bit identifier for our messages.
 * @handler: the callback when we receive such a message (typesafe)
 * @cbdata: the argument to handler()
 *
 * If this returns true, the message handler can be called from any
 * ctdb_service() (which is also called indirectly by other
 * synchronous functions).  If this returns false, the registration
 * failed.
 */
bool ctdb_set_message_handler(struct ctdb_connection *ctdb, uint64_t srvid,
			     ctdb_message_fn_t handler, void *cbdata);


/**
 * ctdb_remove_message_handler - deregister for messages (synchronous)
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @srvid: the 64 bit identifier for our messages.
 *
 * If this returns true, the message handler will no longer be called.
 * If this returns false, the deregistration failed.
 */
bool ctdb_remove_message_handler(struct ctdb_connection *ctdb, uint64_t srvid);

/**
 * ctdb_getpnn - read the pnn number of a node (synchronous)
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @destnode: the destination node (see below)
 * @pnn: a pointer to the pnn to fill in
 *
 * There are several special values for destnode, detailed in
 * ctdb_protocol.h, particularly CTDB_CURRENT_NODE which means the
 * local ctdbd.
 *
 * Returns true and fills in *pnn on success.
 */
bool ctdb_getpnn(struct ctdb_connection *ctdb,
		 uint32_t destnode,
		 uint32_t *pnn);

/**
 * ctdb_getrecmaster - read the recovery master of a node (synchronous)
 * @ctdb: the ctdb_connection from ctdb_connect.
 * @destnode: the destination node (see below)
 * @recmaster: a pointer to the recmaster to fill in
 *
 * There are several special values for destnode, detailed in
 * ctdb_protocol.h, particularly CTDB_CURRENT_NODE which means the
 * local ctdbd.
 *
 * Returns true and fills in *recmaster on success.
 */
bool ctdb_getrecmaster(struct ctdb_connection *ctdb,
		       uint32_t destnode,
		       uint32_t *recmaster);

/* These ugly macro wrappers make the callbacks typesafe. */
#include <ctdb_typesafe_cb.h>
#define ctdb_sendcb(cb, cbdata)						\
	 typesafe_cb_preargs(void, (cb), (cbdata),			\
			     struct ctdb_connection *, struct ctdb_request *)

#define ctdb_connect(addr, log, logpriv)				\
	ctdb_connect((addr),						\
		     typesafe_cb_postargs(void, (log), (logpriv),	\
					  int, const char *, va_list),	\
		     (logpriv))


#define ctdb_attachdb_send(ctdb, name, persistent, tdb_flags, cb, cbdata) \
	ctdb_attachdb_send((ctdb), (name), (persistent), (tdb_flags),	\
			   ctdb_sendcb((cb), (cbdata)), (cbdata))

#define ctdb_readrecordlock_async(_ctdb_db, key, cb, cbdata)		\
	ctdb_readrecordlock_async((_ctdb_db), (key),			\
		typesafe_cb_preargs(void, (cb), (cbdata),		\
				    struct ctdb_db *, struct ctdb_lock *, \
				    TDB_DATA), (cbdata))

#define ctdb_set_message_handler_send(ctdb, srvid, handler, cb, cbdata)	\
	ctdb_set_message_handler_send((ctdb), (srvid), (handler),	\
	      ctdb_sendcb((cb), (cbdata)), (cbdata))

#define ctdb_remove_message_handler_send(ctdb, srvid, cb, cbdata)	\
	ctdb_remove_message_handler_send((ctdb), (srvid),		\
	      ctdb_sendcb((cb), (cbdata)), (cbdata))

#define ctdb_getpnn_send(ctdb, destnode, cb, cbdata)			\
	ctdb_getpnn_send((ctdb), (destnode),				\
			 ctdb_sendcb((cb), (cbdata)), (cbdata))

#define ctdb_getrecmaster_send(ctdb, destnode, cb, cbdata)		\
	ctdb_getrecmaster_send((ctdb), (destnode),			\
			       ctdb_sendcb((cb), (cbdata)), (cbdata))
#endif
