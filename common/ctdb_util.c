/* 
   ctdb utility code

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
#include "lib/events/events.h"
#include "lib/tdb/include/tdb.h"
#include "system/network.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "../include/ctdb_private.h"

int LogLevel = DEBUG_NOTICE;
int this_log_level = 0;

/*
  return error string for last error
*/
const char *ctdb_errstr(struct ctdb_context *ctdb)
{
	return ctdb->err_msg;
}


/*
  remember an error message
*/
void ctdb_set_error(struct ctdb_context *ctdb, const char *fmt, ...)
{
	va_list ap;
	talloc_free(ctdb->err_msg);
	va_start(ap, fmt);
	ctdb->err_msg = talloc_vasprintf(ctdb, fmt, ap);
	DEBUG(DEBUG_ERR,("ctdb error: %s\n", ctdb->err_msg));
	va_end(ap);
}

/*
  a fatal internal error occurred - no hope for recovery
*/
void ctdb_fatal(struct ctdb_context *ctdb, const char *msg)
{
	DEBUG(DEBUG_ALERT,("ctdb fatal error: %s\n", msg));
	abort();
}

/*
  parse a IP:port pair
*/
int ctdb_parse_address(struct ctdb_context *ctdb,
		       TALLOC_CTX *mem_ctx, const char *str,
		       struct ctdb_address *address)
{
	struct servent *se;

	setservent(0);
	se = getservbyname("ctdb", "tcp");
	endservent();
	
	address->address = talloc_strdup(mem_ctx, str);
	if (se == NULL) {
		address->port = CTDB_PORT;
	} else {
		address->port = ntohs(se->s_port);
	}
	return 0;
}


/*
  check if two addresses are the same
*/
bool ctdb_same_address(struct ctdb_address *a1, struct ctdb_address *a2)
{
	return strcmp(a1->address, a2->address) == 0 && a1->port == a2->port;
}


/*
  hash function for mapping data to a VNN - taken from tdb
*/
uint32_t ctdb_hash(const TDB_DATA *key)
{
	uint32_t value;	/* Used to compute the hash value.  */
	uint32_t i;	/* Used to cycle through random values. */

	/* Set the initial value from the key size. */
	for (value = 0x238F13AF * key->dsize, i=0; i < key->dsize; i++)
		value = (value + (key->dptr[i] << (i*5 % 24)));

	return (1103515243 * value + 12345);  
}

/*
  a type checking varient of idr_find
 */
static void *_idr_find_type(struct idr_context *idp, int id, const char *type, const char *location)
{
	void *p = idr_find(idp, id);
	if (p && talloc_check_name(p, type) == NULL) {
		DEBUG(DEBUG_ERR,("%s idr_find_type expected type %s  but got %s\n",
			 location, type, talloc_get_name(p)));
		return NULL;
	}
	return p;
}


/*
  update a max latency number
 */
void ctdb_latency(double *latency, struct timeval t)
{
	double l = timeval_elapsed(&t);
	if (l > *latency) {
		*latency = l;
	}
}

uint32_t ctdb_reqid_new(struct ctdb_context *ctdb, void *state)
{
	uint32_t id;

	id  = ctdb->idr_cnt++ & 0xFFFF;
	id |= (idr_get_new(ctdb->idr, state, 0xFFFF)<<16);
	return id;
}

void *_ctdb_reqid_find(struct ctdb_context *ctdb, uint32_t reqid, const char *type, const char *location)
{
	void *p;

	p = _idr_find_type(ctdb->idr, (reqid>>16)&0xFFFF, type, location);
	if (p == NULL) {
		DEBUG(DEBUG_ERR, ("Could not find idr:%u\n",reqid));
	}

	return p;
}


void ctdb_reqid_remove(struct ctdb_context *ctdb, uint32_t reqid)
{
	int ret;

	ret = idr_remove(ctdb->idr, (reqid>>16)&0xFFFF);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Removing idr that does not exist\n"));
	}
}


/*
  form a ctdb_rec_data record from a key/data pair
  
  note that header may be NULL. If not NULL then it is included in the data portion
  of the record
 */
struct ctdb_rec_data *ctdb_marshall_record(TALLOC_CTX *mem_ctx, uint32_t reqid,	
					   TDB_DATA key, 
					   struct ctdb_ltdb_header *header,
					   TDB_DATA data)
{
	size_t length;
	struct ctdb_rec_data *d;

	length = offsetof(struct ctdb_rec_data, data) + key.dsize + 
		data.dsize + (header?sizeof(*header):0);
	d = (struct ctdb_rec_data *)talloc_size(mem_ctx, length);
	if (d == NULL) {
		return NULL;
	}
	d->length = length;
	d->reqid = reqid;
	d->keylen = key.dsize;
	memcpy(&d->data[0], key.dptr, key.dsize);
	if (header) {
		d->datalen = data.dsize + sizeof(*header);
		memcpy(&d->data[key.dsize], header, sizeof(*header));
		memcpy(&d->data[key.dsize+sizeof(*header)], data.dptr, data.dsize);
	} else {
		d->datalen = data.dsize;
		memcpy(&d->data[key.dsize], data.dptr, data.dsize);
	}
	return d;
}

#if HAVE_SCHED_H
#include <sched.h>
#endif

/*
  if possible, make this task real time
 */
void ctdb_set_scheduler(struct ctdb_context *ctdb)
{
#if HAVE_SCHED_SETSCHEDULER	
	struct sched_param p;
	if (ctdb->saved_scheduler_param == NULL) {
		ctdb->saved_scheduler_param = talloc_size(ctdb, sizeof(p));
	}
	
	if (sched_getparam(0, (struct sched_param *)ctdb->saved_scheduler_param) == -1) {
		DEBUG(DEBUG_ERR,("Unable to get old scheduler params\n"));
		return;
	}

	p = *(struct sched_param *)ctdb->saved_scheduler_param;
	p.sched_priority = 1;

	if (sched_setscheduler(0, SCHED_FIFO, &p) == -1) {
		DEBUG(DEBUG_CRIT,("Unable to set scheduler to SCHED_FIFO (%s)\n", 
			 strerror(errno)));
	} else {
		DEBUG(DEBUG_NOTICE,("Set scheduler to SCHED_FIFO\n"));
	}
#endif
}

/*
  restore previous scheduler parameters
 */
void ctdb_restore_scheduler(struct ctdb_context *ctdb)
{
#if HAVE_SCHED_SETSCHEDULER	
	if (ctdb->saved_scheduler_param == NULL) {
		ctdb_fatal(ctdb, "No saved scheduler parameters\n");
	}
	if (sched_setscheduler(0, SCHED_OTHER, (struct sched_param *)ctdb->saved_scheduler_param) == -1) {
		ctdb_fatal(ctdb, "Unable to restore old scheduler parameters\n");
	}
#endif
}

void set_nonblocking(int fd)
{
	unsigned v;
	v = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, v | O_NONBLOCK);
}

void set_close_on_exec(int fd)
{
	unsigned v;
	v = fcntl(fd, F_GETFD, 0);
        fcntl(fd, F_SETFD, v | FD_CLOEXEC);
}


/*
  parse a ip:num pair with the given separator
 */
static bool parse_ip_num(const char *s, struct in_addr *addr, unsigned *num, const char sep)
{
	const char *p;
	char *endp = NULL;
	char buf[16];

	p = strchr(s, sep);
	if (p == NULL) {
		return false;
	}

	if (p - s > 15) {
		return false;
	}

	*num = strtoul(p+1, &endp, 10);
	if (endp == NULL || *endp != 0) {
		/* trailing garbage */
		return false;
	}

	strlcpy(buf, s, 1+p-s);

	if (inet_aton(buf, addr) == 0) {
		return false;
	}

	return true;
}


static bool parse_ipv4(const char *s, unsigned port, ctdb_sock_addr *saddr)
{
	saddr->ip.sin_family = AF_INET;
	saddr->ip.sin_port   = htons(port);

	if (inet_pton(AF_INET, s, &saddr->ip.sin_addr) != 1) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to translate %s into sin_addr\n", s));
		return false;
	}

	return true;
}

static bool parse_ipv6(const char *s, unsigned port, ctdb_sock_addr *saddr)
{
	saddr->ip6.sin6_family   = AF_INET6;
	saddr->ip6.sin6_port     = htons(port);
	saddr->ip6.sin6_flowinfo = 0;
	saddr->ip6.sin6_scope_id = 0;

	if (inet_pton(AF_INET6, s, &saddr->ip6.sin6_addr) != 1) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to translate %s into sin6_addr\n", s));
		return false;
	}

	return true;
}
/*
  parse a ip:port pair
 */
bool parse_ip_port(const char *addr, ctdb_sock_addr *saddr)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	char *s, *p;
	unsigned port;
	char *endp = NULL;
	bool ret;

	s = talloc_strdup(tmp_ctx, addr);
	if (s == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " Failed strdup()\n"));
		talloc_free(tmp_ctx);
		return false;
	}

	p = rindex(s, ':');
	if (p == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " This addr: %s does not contain a port number\n", s));
		talloc_free(tmp_ctx);
		return false;
	}

	port = strtoul(p+1, &endp, 10);
	if (endp == NULL || *endp != 0) {
		/* trailing garbage */
		DEBUG(DEBUG_ERR, (__location__ " Trailing garbage after the port in %s\n", s));
		talloc_free(tmp_ctx);
		return false;
	}
	*p = 0;


	/* now is this a ipv4 or ipv6 address ?*/
	p = index(s, ':');
	if (p == NULL) {
		ret = parse_ipv4(s, port, saddr);
	} else {
		ret = parse_ipv6(s, port, saddr);
	}

	talloc_free(tmp_ctx);
	return ret;
}

/*
  parse a ip/mask pair
 */
bool parse_ip_mask(const char *s, struct sockaddr_in *ip, unsigned *mask)
{
	if (!parse_ip_num(s, &ip->sin_addr, mask, '/')) {
		return false;
	}
	if (*mask > 32) {
		return false;
	}
	ip->sin_family = AF_INET;
	ip->sin_port   = 0;
	return true;
}

/*
  compare two sockaddr_in structures - matching only on IP
 */
bool ctdb_same_ip(const struct sockaddr_in *ip1, const struct sockaddr_in *ip2)
{
	return ip1->sin_family == ip2->sin_family &&
		ip1->sin_addr.s_addr == ip2->sin_addr.s_addr;
}

/*
  compare two sockaddr_in structures
 */
bool ctdb_same_sockaddr(const struct sockaddr_in *ip1, const struct sockaddr_in *ip2)
{
	return ctdb_same_ip(ip1, ip2) && ip1->sin_port == ip2->sin_port;
}



void ctdb_block_signal(int signum)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set,signum);
	sigprocmask(SIG_BLOCK,&set,NULL);
}

void ctdb_unblock_signal(int signum)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set,signum);
	sigprocmask(SIG_UNBLOCK,&set,NULL);
}
