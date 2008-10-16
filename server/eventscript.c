/* 
   event script handling

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
#include "system/filesys.h"
#include "system/wait.h"
#include "system/dir.h"
#include "system/locale.h"
#include "../include/ctdb_private.h"
#include "lib/events/events.h"
#include "../common/rb_tree.h"

static struct {
	struct timeval start;
	const char *script_running;
} child_state;

/*
  ctdbd sends us a SIGTERM when we should time out the current script
 */
static void sigterm(int sig)
{
	DEBUG(DEBUG_ERR,("Timed out running script '%s' after %.1f seconds\n", 
		 child_state.script_running, timeval_elapsed(&child_state.start)));
	/* all the child processes will be running in the same process group */
	kill(-getpgrp(), SIGKILL);
	exit(1);
}

struct ctdb_event_script_state {
	struct ctdb_context *ctdb;
	pid_t child;
	void (*callback)(struct ctdb_context *, int, void *);
	int fd[2];
	void *private_data;
	const char *options;
};

/*
  run the event script - varargs version
  this function is called and run in the context of a forked child
  which allows it to do blocking calls such as system()
 */
static int ctdb_event_script_v(struct ctdb_context *ctdb, const char *options)
{
	char *cmdstr;
	int ret;
	struct stat st;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	trbt_tree_t *tree;
	DIR *dir;
	struct dirent *de;
	char *script;
	int count;

	if (ctdb->recovery_mode != CTDB_RECOVERY_NORMAL) {
		/* we guarantee that only some specifically allowed event scripts are run
		   while in recovery */
		const char *allowed_scripts[] = {"startrecovery", "shutdown" };
		int i;
		for (i=0;i<ARRAY_SIZE(allowed_scripts);i++) {
			if (strcmp(options, allowed_scripts[i]) == 0) break;
		}
		if (i == ARRAY_SIZE(allowed_scripts)) {
			DEBUG(0,("Refusing to run event scripts with option '%s' while in recovery\n",
				 options));
			return -1;
		}
	}

	if (setpgid(0,0) != 0) {
		DEBUG(DEBUG_ERR,("Failed to create process group for event scripts - %s\n",
			 strerror(errno)));
		talloc_free(tmp_ctx);
		return -1;		
	}

	signal(SIGTERM, sigterm);

	child_state.start = timeval_current();
	child_state.script_running = "startup";

	/*
	  the service specific event scripts 
	*/
	if (stat(ctdb->event_script_dir, &st) != 0 && 
	    errno == ENOENT) {
		DEBUG(DEBUG_CRIT,("No event script directory found at '%s'\n", ctdb->event_script_dir));
		talloc_free(tmp_ctx);
		return -1;
	}

	/* create a tree to store all the script names in */
	tree = trbt_create(tmp_ctx, 0);

	/* scan all directory entries and insert all valid scripts into the 
	   tree
	*/
	dir = opendir(ctdb->event_script_dir);
	if (dir == NULL) {
		DEBUG(DEBUG_CRIT,("Failed to open event script directory '%s'\n", ctdb->event_script_dir));
		talloc_free(tmp_ctx);
		return -1;
	}

	count = 0;
	while ((de=readdir(dir)) != NULL) {
		int namlen;
		unsigned num;
		char *str;

		namlen = strlen(de->d_name);

		if (namlen < 3) {
			continue;
		}

		if (de->d_name[namlen-1] == '~') {
			/* skip files emacs left behind */
			continue;
		}

		if (de->d_name[2] != '.') {
			continue;
		}

		if (sscanf(de->d_name, "%02u.", &num) != 1) {
			continue;
		}

		/* Make sure the event script is executable */
		str = talloc_asprintf(tree, "%s/%s", ctdb->event_script_dir, de->d_name);
		if (stat(str, &st) != 0) {
			DEBUG(DEBUG_ERR,("Could not stat event script %s. Ignoring this event script\n", str));
			continue;
		}
		if (!(st.st_mode & S_IXUSR)) {
			DEBUG(DEBUG_ERR,("Event script %s is not executable. Ignoring this event script\n", str));
			continue;
		}
		
		
		/* store the event script in the tree */
		trbt_insert32(tree, (num<<16)|count++, talloc_strdup(tree, de->d_name));
	}
	closedir(dir);

	/* fetch the scripts from the tree one by one and execute
	   them
	 */
	while ((script=trbt_findfirstarray32(tree, 1)) != NULL) {
		cmdstr = talloc_asprintf(tmp_ctx, "%s/%s %s", 
				ctdb->event_script_dir,
				script, options);
		CTDB_NO_MEMORY(ctdb, cmdstr);

		DEBUG(DEBUG_INFO,("Executing event script %s\n",cmdstr));

		child_state.start = timeval_current();
		child_state.script_running = cmdstr;

		ret = system(cmdstr);
		/* if the system() call was successful, translate ret into the
		   return code from the command
		*/
		if (ret != -1) {
			ret = WEXITSTATUS(ret);
		}
		/* return an error if the script failed */
		if (ret != 0) {
			DEBUG(DEBUG_ERR,("Event script %s failed with error %d\n", cmdstr, ret));
			talloc_free(tmp_ctx);
			return ret;
		}

		/* remove this script from the tree */
		talloc_free(script);
	}

	child_state.start = timeval_current();
	child_state.script_running = "finished";
	
	talloc_free(tmp_ctx);
	return 0;
}

/* called when child is finished */
static void ctdb_event_script_handler(struct event_context *ev, struct fd_event *fde, 
				      uint16_t flags, void *p)
{
	struct ctdb_event_script_state *state = 
		talloc_get_type(p, struct ctdb_event_script_state);
	void (*callback)(struct ctdb_context *, int, void *) = state->callback;
	void *private_data = state->private_data;
	struct ctdb_context *ctdb = state->ctdb;
	signed char rt = -1;

	read(state->fd[0], &rt, sizeof(rt));

	talloc_set_destructor(state, NULL);
	talloc_free(state);
	callback(ctdb, rt, private_data);

	ctdb->event_script_timeouts = 0;
}

static void ctdb_ban_self(struct ctdb_context *ctdb, uint32_t ban_period)
{
	int ret;
	struct ctdb_ban_info b;
	TDB_DATA data;

	b.pnn      = ctdb->pnn;
	b.ban_time = ban_period;

	data.dptr = (uint8_t *)&b;
	data.dsize = sizeof(b);

	ret = ctdb_daemon_send_message(ctdb, CTDB_BROADCAST_CONNECTED,
		CTDB_SRVID_BAN_NODE, data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send ban message\n"));
	}
}


/* called when child times out */
static void ctdb_event_script_timeout(struct event_context *ev, struct timed_event *te, 
				      struct timeval t, void *p)
{
	struct ctdb_event_script_state *state = talloc_get_type(p, struct ctdb_event_script_state);
	void (*callback)(struct ctdb_context *, int, void *) = state->callback;
	void *private_data = state->private_data;
	struct ctdb_context *ctdb = state->ctdb;
	char *options;

	DEBUG(DEBUG_ERR,("Event script timed out : %s count : %u\n", state->options, ctdb->event_script_timeouts));

	options = talloc_strdup(ctdb, state->options);
	CTDB_NO_MEMORY_VOID(ctdb, options);

	talloc_free(state);
	if (!strcmp(options, "monitor")) {
		/* if it is a monitor event, we allow it to "hang" a few times
		   before we declare it a failure and ban ourself (and make
		   ourself unhealthy)
		*/
		DEBUG(DEBUG_ERR, (__location__ " eventscript for monitor event timedout.\n"));

		ctdb->event_script_timeouts++;
		if (ctdb->event_script_timeouts > ctdb->tunable.script_ban_count) {
			ctdb->event_script_timeouts = 0;
			DEBUG(DEBUG_ERR, ("Maximum timeout count %u reached for eventscript. Banning self for %d seconds\n", ctdb->tunable.script_ban_count, ctdb->tunable.recovery_ban_period));
			ctdb_ban_self(ctdb, ctdb->tunable.recovery_ban_period);
			callback(ctdb, -1, private_data);
		} else {
		  	callback(ctdb, 0, private_data);
		}
	} else if (!strcmp(options, "startup")) {
		DEBUG(DEBUG_ERR, (__location__ " eventscript for startup event timedout.\n"));
		callback(ctdb, -1, private_data);
	} else {
		/* if it is not a monitor event we ban ourself immediately */
		DEBUG(DEBUG_ERR, (__location__ " eventscript for NON-monitor/NON-startup event timedout. Immediately banning ourself for %d seconds\n", ctdb->tunable.recovery_ban_period));
		ctdb_ban_self(ctdb, ctdb->tunable.recovery_ban_period);
		callback(ctdb, -1, private_data);
	}

	talloc_free(options);
}

/*
  destroy a running event script
 */
static int event_script_destructor(struct ctdb_event_script_state *state)
{
	DEBUG(DEBUG_ERR,(__location__ " Sending SIGTERM to child pid:%d\n", state->child));
	kill(state->child, SIGTERM);
	return 0;
}

/*
  run the event script in the background, calling the callback when 
  finished
 */
static int ctdb_event_script_callback_v(struct ctdb_context *ctdb, 
					struct timeval timeout,
					TALLOC_CTX *mem_ctx,
					void (*callback)(struct ctdb_context *, int, void *),
					void *private_data,
					const char *fmt, va_list ap)
{
	struct ctdb_event_script_state *state;
	int ret;

	state = talloc(mem_ctx, struct ctdb_event_script_state);
	CTDB_NO_MEMORY(ctdb, state);

	state->ctdb = ctdb;
	state->callback = callback;
	state->private_data = private_data;
	state->options = talloc_vasprintf(state, fmt, ap);
	CTDB_NO_MEMORY(ctdb, state->options);
	
	ret = pipe(state->fd);
	if (ret != 0) {
		talloc_free(state);
		return -1;
	}

	state->child = fork();

	if (state->child == (pid_t)-1) {
		close(state->fd[0]);
		close(state->fd[1]);
		talloc_free(state);
		return -1;
	}

	if (state->child == 0) {
		signed char rt;

		close(state->fd[0]);
		if (ctdb->do_setsched) {
			ctdb_restore_scheduler(ctdb);
		}
		set_close_on_exec(state->fd[1]);
		rt = ctdb_event_script_v(ctdb, state->options);
		while ((ret = write(state->fd[1], &rt, sizeof(rt))) != sizeof(rt)) {
			sleep(1);
		}
		_exit(rt);
	}

	talloc_set_destructor(state, event_script_destructor);

	close(state->fd[1]);

	event_add_fd(ctdb->ev, state, state->fd[0], EVENT_FD_READ|EVENT_FD_AUTOCLOSE,
		     ctdb_event_script_handler, state);

	if (!timeval_is_zero(&timeout)) {
		event_add_timed(ctdb->ev, state, timeout, ctdb_event_script_timeout, state);
	} else {
		DEBUG(DEBUG_ERR, (__location__ " eventscript %s called with no timeout\n", state->options));
	}

	return 0;
}


/*
  run the event script in the background, calling the callback when 
  finished
 */
int ctdb_event_script_callback(struct ctdb_context *ctdb, 
			       struct timeval timeout,
			       TALLOC_CTX *mem_ctx,
			       void (*callback)(struct ctdb_context *, int, void *),
			       void *private_data,
			       const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = ctdb_event_script_callback_v(ctdb, timeout, mem_ctx, callback, private_data, fmt, ap);
	va_end(ap);

	return ret;
}


struct callback_status {
	bool done;
	int status;
};

/*
  called when ctdb_event_script() finishes
 */
static void event_script_callback(struct ctdb_context *ctdb, int status, void *private_data)
{
	struct callback_status *s = (struct callback_status *)private_data;
	s->done = true;
	s->status = status;
}

/*
  run the event script, waiting for it to complete. Used when the caller doesn't want to 
  continue till the event script has finished.
 */
int ctdb_event_script(struct ctdb_context *ctdb, const char *fmt, ...)
{
	va_list ap;
	int ret;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	struct callback_status status;

	va_start(ap, fmt);
	ret = ctdb_event_script_callback_v(ctdb, timeval_zero(), tmp_ctx, event_script_callback, &status, fmt, ap);
	va_end(ap);

	if (ret != 0) {
		talloc_free(tmp_ctx);
		return ret;
	}

	status.status = -1;
	status.done = false;

	while (status.done == false && event_loop_once(ctdb->ev) == 0) /* noop */;

	talloc_free(tmp_ctx);

	return status.status;
}


struct eventscript_callback_state {
	struct ctdb_req_control *c;
};

/*
  called when takeip event finishes
 */
static void run_eventscripts_callback(struct ctdb_context *ctdb, int status, 
				 void *private_data)
{
	struct eventscript_callback_state *state = 
		talloc_get_type(private_data, struct eventscript_callback_state);

	ctdb_enable_monitoring(ctdb);

	if (status != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to forcibly run eventscripts\n"));
		ctdb_request_control_reply(ctdb, state->c, NULL, status, NULL);
		talloc_free(state);
		return;
	}

	/* the control succeeded */
	ctdb_request_control_reply(ctdb, state->c, NULL, 0, NULL);
	talloc_free(state);
	return;
}

/*
  A control to force running of the eventscripts from the ctdb client tool
*/
int32_t ctdb_run_eventscripts(struct ctdb_context *ctdb,
		struct ctdb_req_control *c,
		TDB_DATA indata, bool *async_reply)
{
	int ret;
	struct eventscript_callback_state *state;

	/* kill off any previous invokations of forced eventscripts */
	if (ctdb->eventscripts_ctx) {
		talloc_free(ctdb->eventscripts_ctx);
	}
	ctdb->eventscripts_ctx = talloc_new(ctdb);
	CTDB_NO_MEMORY(ctdb, ctdb->eventscripts_ctx);

	state = talloc(ctdb->eventscripts_ctx, struct eventscript_callback_state);
	CTDB_NO_MEMORY(ctdb, state);

	state->c = talloc_steal(state, c);

	DEBUG(DEBUG_NOTICE,("Forced running of eventscripts with arguments %s\n", indata.dptr));

	if (ctdb->recovery_mode != CTDB_RECOVERY_NORMAL) {
		DEBUG(DEBUG_ERR, (__location__ " Aborted running eventscript \"%s\" while in RECOVERY mode\n", indata.dptr));
		return -1;
	}

	ctdb_disable_monitoring(ctdb);

	ret = ctdb_event_script_callback(ctdb, 
			 timeval_current_ofs(ctdb->tunable.script_timeout, 0),
			 state, run_eventscripts_callback, state,
			 (const char *)indata.dptr);

	if (ret != 0) {
		ctdb_enable_monitoring(ctdb);
		DEBUG(DEBUG_ERR,(__location__ " Failed to run eventscripts with arguments %s\n", indata.dptr));
		talloc_free(state);
		return -1;
	}

	/* tell ctdb_control.c that we will be replying asynchronously */
	*async_reply = true;

	return 0;
}

