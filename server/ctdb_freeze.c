/* 
   ctdb freeze handling

   Copyright (C) Andrew Tridgell  2007

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
#include "lib/events/events.h"
#include "lib/tdb/include/tdb.h"
#include "system/network.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "../include/ctdb_private.h"
#include "lib/util/dlinklist.h"
#include "db_wrap.h"
#include "../common/rb_tree.h"


/*
  lock all databases
 */
static int ctdb_lock_all_databases(struct ctdb_context *ctdb, uint32_t priority)
{
	struct ctdb_db_context *ctdb_db;
	/* REMOVE later */
	/* This double loop is for backward compatibility and deadlock
	   avoidance for old samba versions that not yet support
	   the set prio call.
	   This code shall be removed later
	*/
	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		if (ctdb_db->priority != priority) {
			continue;
		}
		if (strstr(ctdb_db->db_name, "notify") != NULL) {
			continue;
		}
		DEBUG(DEBUG_INFO,("locking database 0x%08x priority:%u %s\n", ctdb_db->db_id, ctdb_db->priority, ctdb_db->db_name));
		if (tdb_lockall(ctdb_db->ltdb->tdb) != 0) {
			return -1;
		}
	}
	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		if (ctdb_db->priority != priority) {
			continue;
		}
		if (strstr(ctdb_db->db_name, "notify") == NULL) {
			continue;
		}
		DEBUG(DEBUG_INFO,("locking database 0x%08x priority:%u %s\n", ctdb_db->db_id, ctdb_db->priority, ctdb_db->db_name));
		if (tdb_lockall(ctdb_db->ltdb->tdb) != 0) {
			return -1;
		}
	}
	return 0;
}

/*
  a list of control requests waiting for a freeze lock child to get
  the database locks
 */
struct ctdb_freeze_waiter {
	struct ctdb_freeze_waiter *next, *prev;
	struct ctdb_context *ctdb;
	struct ctdb_req_control *c;
	uint32_t priority;
	int32_t status;
};

/* a handle to a freeze lock child process */
struct ctdb_freeze_handle {
	struct ctdb_context *ctdb;
	uint32_t priority;
	pid_t child;
	int fd;
	struct ctdb_freeze_waiter *waiters;
};

/*
  destroy a freeze handle
 */	
static int ctdb_freeze_handle_destructor(struct ctdb_freeze_handle *h)
{
	struct ctdb_context *ctdb = h->ctdb;
	struct ctdb_db_context *ctdb_db;

	DEBUG(DEBUG_ERR,("Release freeze handler for prio %u\n", h->priority));

	/* cancel any pending transactions */
	if (ctdb->freeze_transaction_started) {
		for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
			if (ctdb_db->priority != h->priority) {
				continue;
			}
			tdb_add_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
			if (tdb_transaction_cancel(ctdb_db->ltdb->tdb) != 0) {
				DEBUG(DEBUG_ERR,(__location__ " Failed to cancel transaction for db '%s'\n",
					 ctdb_db->db_name));
			}
			tdb_remove_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
		}
		ctdb->freeze_transaction_started = false;
	}

	ctdb->freeze_mode[h->priority]    = CTDB_FREEZE_NONE;
	ctdb->freeze_handles[h->priority] = NULL;

	kill(h->child, SIGKILL);
	return 0;
}

/*
  called when the child writes its status to us
 */
static void ctdb_freeze_lock_handler(struct event_context *ev, struct fd_event *fde, 
				       uint16_t flags, void *private_data)
{
	struct ctdb_freeze_handle *h = talloc_get_type(private_data, struct ctdb_freeze_handle);
	int32_t status;
	struct ctdb_freeze_waiter *w;

	if (h->ctdb->freeze_mode[h->priority] == CTDB_FREEZE_FROZEN) {
		DEBUG(DEBUG_INFO,("freeze child died - unfreezing\n"));
		talloc_free(h);
		return;
	}

	if (read(h->fd, &status, sizeof(status)) != sizeof(status)) {
		DEBUG(DEBUG_ERR,("read error from freeze lock child\n"));
		status = -1;
	}

	if (status == -1) {
		DEBUG(DEBUG_ERR,("Failed to get locks in ctdb_freeze_child\n"));
		/* we didn't get the locks - destroy the handle */
		talloc_free(h);
		return;
	}

	h->ctdb->freeze_mode[h->priority] = CTDB_FREEZE_FROZEN;

	/* notify the waiters */
	if (h != h->ctdb->freeze_handles[h->priority]) {
		DEBUG(DEBUG_ERR,("lockwait finished but h is not linked\n"));
	}
	while ((w = h->waiters)) {
		w->status = status;
		DLIST_REMOVE(h->waiters, w);
		talloc_free(w);
	}
}

/*
  create a child which gets locks on all the open databases, then calls the callback telling the parent
  that it is done
 */
static struct ctdb_freeze_handle *ctdb_freeze_lock(struct ctdb_context *ctdb, uint32_t priority)
{
	struct ctdb_freeze_handle *h;
	int fd[2];
	struct fd_event *fde;

	h = talloc_zero(ctdb, struct ctdb_freeze_handle);
	CTDB_NO_MEMORY_NULL(ctdb, h);

	h->ctdb     = ctdb;
	h->priority = priority;

	if (pipe(fd) == -1) {
		DEBUG(DEBUG_ERR,("Failed to create pipe for ctdb_freeze_lock\n"));
		talloc_free(h);
		return NULL;
	}
	
	h->child = fork();
	if (h->child == -1) {
		DEBUG(DEBUG_ERR,("Failed to fork child for ctdb_freeze_lock\n"));
		talloc_free(h);
		return NULL;
	}

	if (h->child == 0) {
		int ret;

		/* in the child */
		close(fd[0]);

		ret = ctdb_lock_all_databases(ctdb, priority);
		if (ret != 0) {
			_exit(0);
		}

		ret = write(fd[1], &ret, sizeof(ret));
		if (ret != sizeof(ret)) {
			DEBUG(DEBUG_ERR, (__location__ " Failed to write to socket from freeze child. ret:%d errno:%u\n", ret, errno));
			_exit(1);
		}

		while (1) {
			sleep(1);
			if (kill(ctdb->ctdbd_pid, 0) != 0) {
				DEBUG(DEBUG_ERR,("Parent died. Exiting lock wait child\n"));

				_exit(0);
			}
		}
	}

	talloc_set_destructor(h, ctdb_freeze_handle_destructor);

	close(fd[1]);
	set_close_on_exec(fd[0]);

	h->fd = fd[0];


	fde = event_add_fd(ctdb->ev, h, h->fd, EVENT_FD_READ|EVENT_FD_AUTOCLOSE, 
			   ctdb_freeze_lock_handler, h);
	if (fde == NULL) {
		DEBUG(DEBUG_ERR,("Failed to setup fd event for ctdb_freeze_lock\n"));
		close(fd[0]);
		talloc_free(h);
		return NULL;
	}

	return h;
}

/*
  destroy a waiter for a freeze mode change
 */
static int ctdb_freeze_waiter_destructor(struct ctdb_freeze_waiter *w)
{
	ctdb_request_control_reply(w->ctdb, w->c, NULL, w->status, NULL);
	return 0;
}

/*
  start the freeze process for a certain priority
 */
int ctdb_start_freeze(struct ctdb_context *ctdb, uint32_t priority)
{
	if (priority == 0) {
		DEBUG(DEBUG_ERR,("Freeze priority 0 requested, remapping to priority 1\n"));
		priority = 1;
	}

	if ((priority < 1) || (priority > NUM_DB_PRIORITIES)) {
		DEBUG(DEBUG_ERR,(__location__ " Invalid db priority : %u\n", priority));
		return -1;
	}

	if (ctdb->freeze_mode[priority] == CTDB_FREEZE_FROZEN) {
		/* we're already frozen */
		return 0;
	}

	DEBUG(DEBUG_ERR, ("Freeze priority %u\n", priority));

	/* Stop any vacuuming going on: we don't want to wait. */
	ctdb_stop_vacuuming(ctdb);

	/* if there isn't a freeze lock child then create one */
	if (ctdb->freeze_handles[priority] == NULL) {
		ctdb->freeze_handles[priority] = ctdb_freeze_lock(ctdb, priority);
		CTDB_NO_MEMORY(ctdb, ctdb->freeze_handles[priority]);
		ctdb->freeze_mode[priority] = CTDB_FREEZE_PENDING;
	}

	return 0;
}

/*
  freeze the databases
 */
int32_t ctdb_control_freeze(struct ctdb_context *ctdb, struct ctdb_req_control *c, bool *async_reply)
{
	struct ctdb_freeze_waiter *w;
	uint32_t priority;

	priority = (uint32_t)c->srvid;

	if (priority == 0) {
		DEBUG(DEBUG_ERR,("Freeze priority 0 requested, remapping to priority 1\n"));
		priority = 1;
	}

	if ((priority < 1) || (priority > NUM_DB_PRIORITIES)) {
		DEBUG(DEBUG_ERR,(__location__ " Invalid db priority : %u\n", priority));
		return -1;
	}

	if (ctdb->freeze_mode[priority] == CTDB_FREEZE_FROZEN) {
		DEBUG(DEBUG_ERR, ("Freeze priority %u\n", priority));
		/* we're already frozen */
		return 0;
	}

	if (ctdb_start_freeze(ctdb, priority) != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to start freezing databases with priority %u\n", priority));
		return -1;
	}

	/* add ourselves to list of waiters */
	if (ctdb->freeze_handles[priority] == NULL) {
		DEBUG(DEBUG_ERR,("No freeze lock handle when adding a waiter\n"));
		return -1;
	}

	w = talloc(ctdb->freeze_handles[priority], struct ctdb_freeze_waiter);
	CTDB_NO_MEMORY(ctdb, w);
	w->ctdb     = ctdb;
	w->c        = talloc_steal(w, c);
	w->priority = priority;
	w->status   = -1;
	talloc_set_destructor(w, ctdb_freeze_waiter_destructor);
	DLIST_ADD(ctdb->freeze_handles[priority]->waiters, w);

	/* we won't reply till later */
	*async_reply = True;
	return 0;
}


/*
  block until we are frozen, used during daemon startup
 */
bool ctdb_blocking_freeze(struct ctdb_context *ctdb)
{
	int i;

	for (i=1; i<=NUM_DB_PRIORITIES; i++) {
		if (ctdb_start_freeze(ctdb, i)) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to freeze databases of prio %u\n", i));
			continue;
		}

		/* block until frozen */
		while (ctdb->freeze_mode[i] == CTDB_FREEZE_PENDING) {
			event_loop_once(ctdb->ev);
		}
	}

	return 0;
}


static void thaw_priority(struct ctdb_context *ctdb, uint32_t priority)
{
	DEBUG(DEBUG_ERR,("Thawing priority %u\n", priority));

	/* cancel any pending transactions */
	if (ctdb->freeze_transaction_started) {
		struct ctdb_db_context *ctdb_db;

		for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
			tdb_add_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
			if (tdb_transaction_cancel(ctdb_db->ltdb->tdb) != 0) {
				DEBUG(DEBUG_ERR,(__location__ " Failed to cancel transaction for db '%s'\n",
					 ctdb_db->db_name));
			}
			tdb_remove_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
		}
	}
	ctdb->freeze_transaction_started = false;

#if 0
	/* this hack can be used to get a copy of the databases at the end of a recovery */
	system("mkdir -p /var/ctdb.saved; /usr/bin/rsync --delete -a /var/ctdb/ /var/ctdb.saved/$$ 2>&1 > /dev/null");
#endif

#if 0
	/* and this one for local testing */
	system("mkdir -p test.db.saved; /usr/bin/rsync --delete -a test.db/ test.db.saved/$$ 2>&1 > /dev/null");
#endif

	if (ctdb->freeze_handles[priority] != NULL) {
		talloc_free(ctdb->freeze_handles[priority]);
		ctdb->freeze_handles[priority] = NULL;
	}
}

/*
  thaw the databases
 */
int32_t ctdb_control_thaw(struct ctdb_context *ctdb, uint32_t priority)
{

	if (priority > NUM_DB_PRIORITIES) {
		DEBUG(DEBUG_ERR,(__location__ " Invalid db priority : %u\n", priority));
		return -1;
	}

	if (priority == 0) {
		int i;
		for (i=1;i<=NUM_DB_PRIORITIES; i++) {
			thaw_priority(ctdb, i);
		}
	} else {
		thaw_priority(ctdb, priority);
	}

	ctdb_call_resend_all(ctdb);
	return 0;
}


/*
  start a transaction on all databases - used for recovery
 */
int32_t ctdb_control_transaction_start(struct ctdb_context *ctdb, uint32_t id)
{
	struct ctdb_db_context *ctdb_db;
	int i;

	for (i=1;i<=NUM_DB_PRIORITIES; i++) {
		if (ctdb->freeze_mode[i] != CTDB_FREEZE_FROZEN) {
			DEBUG(DEBUG_ERR,(__location__ " Failed transaction_start while not frozen\n"));
			return -1;
		}
	}

	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		int ret;

		tdb_add_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);

		if (ctdb->freeze_transaction_started) {
			if (tdb_transaction_cancel(ctdb_db->ltdb->tdb) != 0) {
				DEBUG(DEBUG_ERR,(__location__ " Failed to cancel transaction for db '%s'\n",
					 ctdb_db->db_name));
				/* not a fatal error */
			}
		}

		ret = tdb_transaction_start(ctdb_db->ltdb->tdb);

		tdb_remove_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);

		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to start transaction for db '%s'\n",
				 ctdb_db->db_name));
			return -1;
		}
	}

	ctdb->freeze_transaction_started = true;
	ctdb->freeze_transaction_id = id;

	return 0;
}

/*
  cancel a transaction for all databases - used for recovery
 */
int32_t ctdb_control_transaction_cancel(struct ctdb_context *ctdb)
{
	struct ctdb_db_context *ctdb_db;

	DEBUG(DEBUG_ERR,(__location__ " recovery transaction cancelled called\n"));

	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		tdb_add_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);

		if (tdb_transaction_cancel(ctdb_db->ltdb->tdb) != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to cancel transaction for db '%s'\n",  ctdb_db->db_name));
			/* not a fatal error */
		}

		tdb_remove_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
	}

	ctdb->freeze_transaction_started = false;

	return 0;
}

/*
  commit transactions on all databases
 */
int32_t ctdb_control_transaction_commit(struct ctdb_context *ctdb, uint32_t id)
{
	struct ctdb_db_context *ctdb_db;
	int i;
	int healthy_nodes = 0;

	for (i=1;i<=NUM_DB_PRIORITIES; i++) {
		if (ctdb->freeze_mode[i] != CTDB_FREEZE_FROZEN) {
			DEBUG(DEBUG_ERR,(__location__ " Failed transaction_start while not frozen\n"));
			return -1;
		}
	}

	if (!ctdb->freeze_transaction_started) {
		DEBUG(DEBUG_ERR,(__location__ " transaction not started\n"));
		return -1;
	}

	if (id != ctdb->freeze_transaction_id) {
		DEBUG(DEBUG_ERR,(__location__ " incorrect transaction id 0x%x in commit\n", id));
		return -1;
	}

	DEBUG(DEBUG_DEBUG,(__location__ " num_nodes[%d]\n", ctdb->num_nodes));
	for (i=0; i < ctdb->num_nodes; i++) {
		DEBUG(DEBUG_DEBUG,(__location__ " node[%d].flags[0x%X]\n",
				   i, ctdb->nodes[i]->flags));
		if (ctdb->nodes[i]->flags == 0) {
			healthy_nodes++;
		}
	}
	DEBUG(DEBUG_INFO,(__location__ " healthy_nodes[%d]\n", healthy_nodes));

	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		int ret;

		tdb_add_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
		ret = tdb_transaction_commit(ctdb_db->ltdb->tdb);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to commit transaction for db '%s'. Cancel all transactions and resetting transaction_started to false.\n",
				 ctdb_db->db_name));
			goto fail;
		}
		tdb_remove_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);

		ret = ctdb_update_persistent_health(ctdb, ctdb_db, NULL, healthy_nodes);
		if (ret != 0) {
			DEBUG(DEBUG_CRIT,(__location__ " Failed to update persistent health for db '%s'. "
					 "Cancel all remaining transactions and resetting transaction_started to false.\n",
					 ctdb_db->db_name));
			goto fail;
		}
	}

	ctdb->freeze_transaction_started = false;
	ctdb->freeze_transaction_id = 0;

	return 0;

fail:
	/* cancel any pending transactions */
	for (ctdb_db=ctdb->db_list;ctdb_db;ctdb_db=ctdb_db->next) {
		tdb_add_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
		if (tdb_transaction_cancel(ctdb_db->ltdb->tdb) != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to cancel transaction for db '%s'\n",
				 ctdb_db->db_name));
		}
		tdb_remove_flags(ctdb_db->ltdb->tdb, TDB_NOLOCK);
	}
	ctdb->freeze_transaction_started = false;

	return -1;
}

/*
  wipe a database - only possible when in a frozen transaction
 */
int32_t ctdb_control_wipe_database(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_control_wipe_database w = *(struct ctdb_control_wipe_database *)indata.dptr;
	struct ctdb_db_context *ctdb_db;

	ctdb_db = find_ctdb_db(ctdb, w.db_id);
	if (!ctdb_db) {
		DEBUG(DEBUG_ERR,(__location__ " Unknown db 0x%x\n", w.db_id));
		return -1;
	}

	if (ctdb->freeze_mode[ctdb_db->priority] != CTDB_FREEZE_FROZEN) {
		DEBUG(DEBUG_ERR,(__location__ " Failed transaction_start while not frozen\n"));
		return -1;
	}

	if (!ctdb->freeze_transaction_started) {
		DEBUG(DEBUG_ERR,(__location__ " transaction not started\n"));
		return -1;
	}

	if (w.transaction_id != ctdb->freeze_transaction_id) {
		DEBUG(DEBUG_ERR,(__location__ " incorrect transaction id 0x%x in commit\n", w.transaction_id));
		return -1;
	}

	if (tdb_wipe_all(ctdb_db->ltdb->tdb) != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to wipe database for db '%s'\n",
			 ctdb_db->db_name));
		return -1;
	}

	if (!ctdb_db->persistent) {
		talloc_free(ctdb_db->delete_queue);
		ctdb_db->delete_queue = trbt_create(ctdb_db, 0);
		if (ctdb_db->delete_queue == NULL) {
			DEBUG(DEBUG_ERR, (__location__ " Failed to re-create "
					  "the vacuum tree.\n"));
			return -1;
		}
	}

	return 0;
}
