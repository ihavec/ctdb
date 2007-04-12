/* 
   test of messaging

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
#include "system/network.h"
#include "../include/ctdb.h"
#include "../include/ctdb_private.h"

#define CTDB_SOCKET "/tmp/ctdb.socket.127.0.0.1"


/*
  connect to the unix domain socket
*/
static int ux_socket_connect(const char *name)
{
	struct sockaddr_un addr;
	int fd;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, name, sizeof(addr.sun_path));

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		return -1;
	}
	
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		close(fd);
		return -1;
	}

	return fd;
}

void register_pid_with_daemon(int fd, int pid)
{
	struct ctdb_req_register r;

	bzero(&r, sizeof(r));
	r.hdr.length       = sizeof(r);
	r.hdr.ctdb_magic   = CTDB_MAGIC;
	r.hdr.ctdb_version = CTDB_VERSION;
	r.hdr.operation    = CTDB_REQ_REGISTER;
	r.srvid            = pid;

	/* XXX must deal with partial writes here */
	write(fd, &r, sizeof(r));
}

/* send a command to the cluster to wait until all nodes are connected
   and the cluster is fully operational
 */
int wait_for_cluster(int fd)
{
	struct ctdb_req_connect_wait req;
	struct ctdb_reply_connect_wait rep;
	int cnt, tot;

	/* send a connect wait command to the local node */
	bzero(&req, sizeof(req));
	req.hdr.length       = sizeof(req);
	req.hdr.ctdb_magic   = CTDB_MAGIC;
	req.hdr.ctdb_version = CTDB_VERSION;
	req.hdr.operation    = CTDB_REQ_CONNECT_WAIT;

	/* XXX must deal with partial writes here */
	write(fd, &req, sizeof(req));


	/* read the 4 bytes of length for the pdu */
	cnt=0;
	tot=4;
	while(cnt!=tot){
		int numread;
		numread=read(fd, ((char *)&rep)+cnt, tot-cnt);
		if(numread>0){
			cnt+=numread;
		}
	}
	/* read the rest of the pdu */
	tot=rep.hdr.length;
	while(cnt!=tot){
		int numread;
		numread=read(fd, ((char *)&rep)+cnt, tot-cnt);
		if(numread>0){
			cnt+=numread;
		}
	}

	return rep.vnn;
}


int send_a_message(int fd, int ourvnn, int vnn, int pid, TDB_DATA data)
{
	struct ctdb_req_message r;
	int len, cnt;

	len = offsetof(struct ctdb_req_message, data) + data.dsize;
	r.hdr.length     = len;
	r.hdr.ctdb_magic = CTDB_MAGIC;
	r.hdr.ctdb_version = CTDB_VERSION;
	r.hdr.operation  = CTDB_REQ_MESSAGE;
	r.hdr.destnode   = vnn;
	r.hdr.srcnode    = ourvnn;
	r.hdr.reqid      = 0;
	r.srvid          = pid;
	r.datalen        = data.dsize;
	
	/* write header */
	cnt=write(fd, &r, offsetof(struct ctdb_req_message, data));
	/* write data */
	if(data.dsize){
	    cnt=write(fd, data.dptr, data.dsize);
	}
	return 0;
}

int receive_a_message(int fd, struct ctdb_req_message **preply)
{
	int cnt,tot;
	struct ctdb_req_message *rep;
	uint32_t length;

	/* read the 4 bytes of length for the pdu */
	cnt=0;
	tot=4;
	while(cnt!=tot){
		int numread;
		numread=read(fd, ((char *)&length)+cnt, tot-cnt);
		if(numread>0){
			cnt+=numread;
		}
	}
	
	/* read the rest of the pdu */
	rep = malloc(length);
	rep->hdr.length = length;
	cnt = 0;
	tot = length-4;
	while(cnt!=tot){
		int numread;
		numread=read(fd, ((char *)rep)+cnt, tot-cnt);
		if(numread>0){
			cnt+=numread;
		}
	}

	*preply = rep;
	return 0;
}

int main(int argc, const char *argv[])
{
	int fd, pid, vnn, dstvnn, dstpid;
	TDB_DATA message;
	struct ctdb_req_message *reply;

	/* open the socket to talk to the local ctdb daemon */
	fd=ux_socket_connect(CTDB_SOCKET);
	if (fd==-1) {
		printf("failed to open domain socket\n");
		exit(10);
	}


	/* register our local server id with the daemon so that it knows
	   where to send messages addressed to our local pid.
	 */
	pid=getpid();
	register_pid_with_daemon(fd, pid);


	/* do a connect wait to ensure that all nodes in the cluster are up 
	   and operational.
	   this also tells us the vnn of the local cluster.
	   If someone wants to send us a emssage they should send it to
	   this vnn and our pid
	 */
	vnn=wait_for_cluster(fd);
	printf("our address is vnn:%d pid:%d  if someone wants to send us a message!\n",vnn,pid);


	/* send a message to ourself */
	dstvnn=vnn;
	dstpid=pid;
	message.dptr="Test message";
	message.dsize=strlen(message.dptr)+1;
	send_a_message(fd, vnn, dstvnn, dstpid, message);

	receive_a_message(fd, &reply);

	/* wait for the message to come back */


	return 0;
}
