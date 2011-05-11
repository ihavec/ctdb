/*
   Unix SMB/CIFS implementation.

   Copyright (C) Andrew Tridgell 2005
   Copyright (C) Jelmer Vernooij 2005

     ** NOTE! The following LGPL license applies to the tevent
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "talloc.h"
#include "tevent.h"
#include "tevent_internal.h"
#include "tevent_util.h"
#include <fcntl.h>

/**
  return the number of elements in a string list
*/
size_t ev_str_list_length(const char **list)
{
	size_t ret;
	for (ret=0;list && list[ret];ret++) /* noop */ ;
	return ret;
}

/**
  add an entry to a string list
*/
const char **ev_str_list_add(const char **list, const char *s)
{
	size_t len = ev_str_list_length(list);
	const char **ret;

	ret = talloc_realloc(NULL, list, const char *, len+2);
	if (ret == NULL) return NULL;

	ret[len] = talloc_strdup(ret, s);
	if (ret[len] == NULL) return NULL;

	ret[len+1] = NULL;

	return ret;
}


/**
 Set a fd into blocking/nonblocking mode. Uses POSIX O_NONBLOCK if available,
 else
  if SYSV use O_NDELAY
  if BSD use FNDELAY
**/

int ev_set_blocking(int fd, bool set)
{
	int val;
#ifdef O_NONBLOCK
#define FLAG_TO_SET O_NONBLOCK
#else
#ifdef SYSV
#define FLAG_TO_SET O_NDELAY
#else /* BSD */
#define FLAG_TO_SET FNDELAY
#endif
#endif

	if((val = fcntl(fd, F_GETFL, 0)) == -1)
		return -1;
	if(set) /* Turn blocking on - ie. clear nonblock flag */
		val &= ~FLAG_TO_SET;
	else
		val |= FLAG_TO_SET;
	return fcntl( fd, F_SETFL, val);
#undef FLAG_TO_SET
}

static struct timeval tevent_before_wait_ts;
static struct timeval tevent_after_wait_ts;

/*
 * measure the time difference between multiple arrivals
 * to the point where we wait for new events to come in
 *
 * allows to measure how long it takes to work on a 
 * event
 */
void tevent_before_wait(struct event_context *ev) {

	struct timeval diff;
	struct timeval now = tevent_timeval_current();

	if (!tevent_timeval_is_zero(&tevent_after_wait_ts)) {
		diff = tevent_timeval_until(&tevent_after_wait_ts, &now);
		if (diff.tv_sec > 3) {
			tevent_debug(ev, TEVENT_DEBUG_FATAL,  __location__ 
				     " Handling event took %d seconds!",
				     (int) diff.tv_sec);
		}
	}

	tevent_before_wait_ts = tevent_timeval_current();

}

/*
 * measure how long the select()/epoll() call took
 *
 * allows to measure how long we are waiting for new events
 */
void tevent_after_wait(struct event_context *ev) {

	struct timeval diff;
	struct timeval now = tevent_timeval_current();

	if (!tevent_timeval_is_zero(&tevent_before_wait_ts)) {
		diff = tevent_timeval_until(&tevent_before_wait_ts, &now);
		if (diff.tv_sec > 3) {
			tevent_debug(ev, TEVENT_DEBUG_FATAL,  __location__
				     " No event for %d seconds!",
				     (int) diff.tv_sec);
		}
	}

	tevent_after_wait_ts = tevent_timeval_current();

}