/* 
   ctdb recovery code

   Copyright (C) Ronnie Sahlberg  2007
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
#include "lib/util/dlinklist.h"
#include "system/network.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "../include/ctdb_private.h"
#include "../common/rb_tree.h"


#define TAKEOVER_TIMEOUT() timeval_current_ofs(ctdb->tunable.takeover_timeout,0)

#define CTDB_ARP_INTERVAL 1
#define CTDB_ARP_REPEAT   3

struct ctdb_takeover_arp {
	struct ctdb_context *ctdb;
	uint32_t count;
	struct sockaddr_in sin;
	struct ctdb_tcp_array *tcparray;
	struct ctdb_vnn *vnn;
};


/*
  lists of tcp endpoints
 */
struct ctdb_tcp_list {
	struct ctdb_tcp_list *prev, *next;
	struct ctdb_tcp_connection connection;
};

/*
  list of clients to kill on IP release
 */
struct ctdb_client_ip {
	struct ctdb_client_ip *prev, *next;
	struct ctdb_context *ctdb;
	struct sockaddr_in ip;
	uint32_t client_id;
};


/*
  send a gratuitous arp
 */
static void ctdb_control_send_arp(struct event_context *ev, struct timed_event *te, 
				  struct timeval t, void *private_data)
{
	struct ctdb_takeover_arp *arp = talloc_get_type(private_data, 
							struct ctdb_takeover_arp);
	int i, s, ret;
	struct ctdb_tcp_array *tcparray;


	ret = ctdb_sys_send_arp(&arp->sin, arp->vnn->vnn_list->iface);
	if (ret != 0) {
		DEBUG(0,(__location__ " sending of arp failed (%s)\n", strerror(errno)));
	}

	s = ctdb_sys_open_sending_socket();
	if (s == -1) {
		DEBUG(0,(__location__ " failed to open raw socket for sending tickles\n"));
		return;
	}

	tcparray = arp->tcparray;
	if (tcparray) {
		for (i=0;i<tcparray->num;i++) {
			DEBUG(2,("sending tcp tickle ack for %u->%s:%u\n",
				 (unsigned)ntohs(tcparray->connections[i].daddr.sin_port), 
				 inet_ntoa(tcparray->connections[i].saddr.sin_addr),
				 (unsigned)ntohs(tcparray->connections[i].saddr.sin_port)));
			ret = ctdb_sys_send_tcp(s, &tcparray->connections[i].saddr, 
						&tcparray->connections[i].daddr, 0, 0, 0);
			if (ret != 0) {
				DEBUG(0,(__location__ " Failed to send tcp tickle ack for %s\n",
					 inet_ntoa(tcparray->connections[i].saddr.sin_addr)));
			}
		}
	}

	close(s);
	arp->count++;

	if (arp->count == CTDB_ARP_REPEAT) {
		talloc_free(arp);
		return;
	}

	event_add_timed(arp->ctdb->ev, arp->ctdb->takeover_ctx, 
			timeval_current_ofs(CTDB_ARP_INTERVAL, 0), 
			ctdb_control_send_arp, arp);
}

struct takeover_callback_state {
	struct ctdb_req_control *c;
	struct sockaddr_in *sin;
	struct ctdb_vnn *vnn;
};

/*
  called when takeip event finishes
 */
static void takeover_ip_callback(struct ctdb_context *ctdb, int status, 
				 void *private_data)
{
	struct takeover_callback_state *state = 
		talloc_get_type(private_data, struct takeover_callback_state);
	struct ctdb_takeover_arp *arp;
	char *ip = inet_ntoa(state->sin->sin_addr);
	struct ctdb_tcp_array *tcparray;

	ctdb_start_monitoring(ctdb);

	if (status != 0) {
		DEBUG(0,(__location__ " Failed to takeover IP %s on interface %s\n",
			 ip, state->vnn->vnn_list->iface));
		ctdb_request_control_reply(ctdb, state->c, NULL, status, NULL);
		talloc_free(state);
		return;
	}

	if (!ctdb->takeover_ctx) {
		ctdb->takeover_ctx = talloc_new(ctdb);
		if (!ctdb->takeover_ctx) goto failed;
	}

	arp = talloc_zero(ctdb->takeover_ctx, struct ctdb_takeover_arp);
	if (!arp) goto failed;
	
	arp->ctdb = ctdb;
	arp->sin = *state->sin;
	arp->vnn = state->vnn;

	tcparray = state->vnn->tcp_array;
	if (tcparray) {
		/* add all of the known tcp connections for this IP to the
		   list of tcp connections to send tickle acks for */
		arp->tcparray = talloc_steal(arp, tcparray);

		state->vnn->tcp_array = NULL;
		state->vnn->tcp_update_needed = true;
	}

	event_add_timed(arp->ctdb->ev, arp->ctdb->takeover_ctx, 
			timeval_zero(), ctdb_control_send_arp, arp);

	/* the control succeeded */
	ctdb_request_control_reply(ctdb, state->c, NULL, 0, NULL);
	talloc_free(state);
	return;

failed:
	ctdb_request_control_reply(ctdb, state->c, NULL, -1, NULL);
	talloc_free(state);
	return;
}

/*
  Find the vnn of the node that has a public ip address
  returns -1 if the address is not known as a public address
 */
static struct ctdb_vnn *find_public_ip_vnn(struct ctdb_context *ctdb, char *ip)
{
	struct ctdb_vnn_list *vnn_list;
	struct ctdb_vnn *vnn;

	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			if (!strcmp(vnn->public_address, ip)) {
				return vnn;
			}
		}
	}
	return NULL;
}


/*
  take over an ip address
 */
int32_t ctdb_control_takeover_ip(struct ctdb_context *ctdb, 
				 struct ctdb_req_control *c,
				 TDB_DATA indata, 
				 bool *async_reply)
{
	int ret;
	struct takeover_callback_state *state;
	struct ctdb_public_ip *pip = (struct ctdb_public_ip *)indata.dptr;
	char *ip = inet_ntoa(pip->sin.sin_addr);
	struct ctdb_vnn *vnn;

	/* update out vnn list */
	vnn->pnn = pip->pnn;
	vnn = find_public_ip_vnn(ctdb, ip);
	if (vnn == NULL) {
		DEBUG(0,("takeoverip called for an ip '%s' that is not a public address\n", ip));
		return 0;
	}
	vnn->pnn = pip->pnn;

	/* if our kernel already has this IP, do nothing */
	if (ctdb_sys_have_ip(ip)) {
		return 0;
	}

	state = talloc(ctdb, struct takeover_callback_state);
	CTDB_NO_MEMORY(ctdb, state);

	state->c = talloc_steal(ctdb, c);
	state->sin = talloc(ctdb, struct sockaddr_in);       
	CTDB_NO_MEMORY(ctdb, state->sin);
	*state->sin = pip->sin;

	state->vnn = vnn;

	DEBUG(0,("Takeover of IP %s/%u on interface %s\n", 
		 ip, vnn->public_netmask_bits, 
		 vnn->vnn_list->iface));

	ctdb_stop_monitoring(ctdb);

	ret = ctdb_event_script_callback(ctdb, 
					 timeval_current_ofs(ctdb->tunable.script_timeout, 0),
					 state, takeover_ip_callback, state,
					 "takeip %s %s %u",
					 vnn->vnn_list->iface, 
					 ip,
					 vnn->public_netmask_bits);
	if (ret != 0) {
		DEBUG(0,(__location__ " Failed to takeover IP %s on interface %s\n",
			 ip, vnn->vnn_list->iface));
		talloc_free(state);
		return -1;
	}

	/* tell ctdb_control.c that we will be replying asynchronously */
	*async_reply = true;

	return 0;
}

/*
  kill any clients that are registered with a IP that is being released
 */
static void release_kill_clients(struct ctdb_context *ctdb, struct in_addr in)
{
	struct ctdb_client_ip *ip;

	for (ip=ctdb->client_ip_list; ip; ip=ip->next) {
		if (ip->ip.sin_addr.s_addr == in.s_addr) {
			struct ctdb_client *client = ctdb_reqid_find(ctdb, 
								     ip->client_id, 
								     struct ctdb_client);
			if (client->pid != 0) {
				DEBUG(0,(__location__ " Killing client pid %u for IP %s on client_id %u\n",
					 (unsigned)client->pid, inet_ntoa(in),
					 ip->client_id));
				kill(client->pid, SIGKILL);
			}
		}
	}
}

/*
  called when releaseip event finishes
 */
static void release_ip_callback(struct ctdb_context *ctdb, int status, 
				void *private_data)
{
	struct takeover_callback_state *state = 
		talloc_get_type(private_data, struct takeover_callback_state);
	char *ip = inet_ntoa(state->sin->sin_addr);
	TDB_DATA data;

	ctdb_start_monitoring(ctdb);

	/* send a message to all clients of this node telling them
	   that the cluster has been reconfigured and they should
	   release any sockets on this IP */
	data.dptr = (uint8_t *)ip;
	data.dsize = strlen(ip)+1;

	ctdb_daemon_send_message(ctdb, ctdb->pnn, CTDB_SRVID_RELEASE_IP, data);

	/* kill clients that have registered with this IP */
	release_kill_clients(ctdb, state->sin->sin_addr);
	
	/* the control succeeded */
	ctdb_request_control_reply(ctdb, state->c, NULL, 0, NULL);
	talloc_free(state);
}

/*
  release an ip address
 */
int32_t ctdb_control_release_ip(struct ctdb_context *ctdb, 
				struct ctdb_req_control *c,
				TDB_DATA indata, 
				bool *async_reply)
{
	int ret;
	struct takeover_callback_state *state;
	struct ctdb_public_ip *pip = (struct ctdb_public_ip *)indata.dptr;
	char *ip = inet_ntoa(pip->sin.sin_addr);
	struct ctdb_vnn *vnn;

	/* update our vnn list */
	vnn = find_public_ip_vnn(ctdb, ip);
	if (vnn == NULL) {
		DEBUG(0,("releaseip called for an ip '%s' that is not a public address\n", ip));
		return 0;
	}
	vnn->pnn = pip->pnn;

	if (!ctdb_sys_have_ip(ip)) {
		return 0;
	}

	DEBUG(0,("Release of IP %s/%u on interface %s\n", 
		 ip, vnn->public_netmask_bits, 
		 vnn->vnn_list->iface));

	/* stop any previous arps */
	talloc_free(ctdb->takeover_ctx);
	ctdb->takeover_ctx = NULL;

	state = talloc(ctdb, struct takeover_callback_state);
	CTDB_NO_MEMORY(ctdb, state);

	state->c = talloc_steal(state, c);
	state->sin = talloc(state, struct sockaddr_in);       
	CTDB_NO_MEMORY(ctdb, state->sin);
	*state->sin = pip->sin;

	state->vnn = vnn;

	ctdb_stop_monitoring(ctdb);

	ret = ctdb_event_script_callback(ctdb, 
					 timeval_current_ofs(ctdb->tunable.script_timeout, 0),
					 state, release_ip_callback, state,
					 "releaseip %s %s %u",
					 vnn->vnn_list->iface, 
					 ip,
					 vnn->public_netmask_bits);
	if (ret != 0) {
		DEBUG(0,(__location__ " Failed to release IP %s on interface %s\n",
			 ip, vnn->vnn_list->iface));
		talloc_free(state);
		return -1;
	}

	/* tell the control that we will be reply asynchronously */
	*async_reply = true;

	return 0;
}



static int add_public_address(struct ctdb_context *ctdb, int ip0, int ip1, int ip2, int ip3, int nm, char *iface)
{
	struct ctdb_vnn      *vnn;
	struct ctdb_vnn_list *vnn_list;
	const char *public_address;

	/* first find the entry for this interface   if we have one */
	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		if (!strcmp(iface, vnn_list->iface)) {
			break;
		}
	}

	/* If we dont have a vnn_list for this interface, we must create one */
	if (vnn_list == NULL) {
		vnn_list = talloc_zero(ctdb, struct ctdb_vnn_list);
		CTDB_NO_MEMORY_FATAL(ctdb, vnn_list);
		vnn_list->iface = talloc_strdup(vnn_list, iface);
		vnn_list->next = ctdb->vnn_list;
		ctdb->vnn_list = vnn_list;
	}		


	/* Verify that we dont have an entry for this ip yet */
	public_address = talloc_asprintf(vnn_list, "%d.%d.%d.%d", ip0, ip1, ip2, ip3);
	CTDB_NO_MEMORY_FATAL(ctdb, public_address);
	for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
		if (!strcmp(public_address, vnn->public_address)) {
			DEBUG(0,("Same ip '%s' specified multiple times in the public address list \n", public_address));
			exit(1);
		}		
	}

	
	/* make sure the netmask is ok */
	if (nm > 32) {
		DEBUG(0, ("Illegal netmask for IP %s\n", public_address));
		return -1;
	}

	/* create a new vnn structure for this ip address */
	vnn = talloc_zero(vnn_list, struct ctdb_vnn);
	CTDB_NO_MEMORY_FATAL(ctdb, vnn);
	vnn->vnn_list            = vnn_list;
	vnn->public_address      = talloc_steal(vnn, public_address);
	vnn->public_netmask_bits = nm;
	vnn->pnn        = -1;
	
	vnn->next     = vnn_list->vnn;
	vnn_list->vnn = vnn;	

	vnn_list->num_ips++;
	return 0;
}


/*
  setup the event script directory
*/
int ctdb_set_event_script_dir(struct ctdb_context *ctdb, const char *script_dir)
{
	ctdb->event_script_dir = talloc_strdup(ctdb, script_dir);
	CTDB_NO_MEMORY(ctdb, ctdb->event_script_dir);
	return 0;
}

/*
  setup the public address lists from a file
*/
int ctdb_set_public_addresses(struct ctdb_context *ctdb, const char *alist)
{
	char **lines;
	int nlines;
	int i;

	lines = file_lines_load(alist, &nlines, ctdb);
	if (lines == NULL) {
		ctdb_set_error(ctdb, "Failed to load public address list '%s'\n", alist);
		return -1;
	}
	while (nlines > 0 && strcmp(lines[nlines-1], "") == 0) {
		nlines--;
	}

	for (i=0;i<nlines;i++) {
		int ip0, ip1, ip2, ip3, nm;
		char iface[256];

		if (sscanf(lines[i], "%d.%d.%d.%d/%d %255s", &ip0, &ip1, &ip2, &ip3, &nm, iface) != 6) {
			DEBUG(0,("Badly formed line '%s' in public address list\n", lines[i]));
			talloc_free(lines);
			return -1;
		}

		if (add_public_address(ctdb, ip0, ip1, ip2, ip3, nm, iface)) {
			DEBUG(0,("Failed to add '%s' to the public address list\n", lines[i]));
			talloc_free(lines);
			return -1;
		}
	}

	talloc_free(lines);
	return 0;
}


/* Given a physical node and an interface, return the number of
   public addresses that is currently assigned to this node/interface.
*/
static int node_ip_coverage(struct ctdb_context *ctdb, 
	int32_t pnn, struct ctdb_vnn_list *vnn_list)
{
	int num=0;
	struct ctdb_vnn *vnn;

	for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
		if (vnn->pnn == pnn) {
			num++;
		}
	}
	return num;
}

/* search the vnn list for a node to takeover vnn.
   pick the node that currently are serving the least number of vnns for
   this interface so that the vnns get spread out evenly.
*/
static int find_takeover_node(struct ctdb_context *ctdb, 
		struct ctdb_node_map *nodemap, uint32_t mask, 
		struct ctdb_vnn_list *vnn_list, struct ctdb_vnn *vnn)
{
	int pnn, min, num;
	int i;

	pnn    = -1;
	for (i=0;i<nodemap->num;i++) {
		if (nodemap->nodes[i].flags & mask) {
			/* This node is not healty and can not be used to serve
			   a public address 
			*/
			continue;
		}

		num = node_ip_coverage(ctdb, i, vnn_list);
		/* was this the first node we checked ? */
		if (pnn == -1) {
			pnn = i;
			min  = num;
		} else {
			if (num < min) {
				pnn = i;
				min  = num;
			}
		}
	}	
	if (pnn == -1) {
		DEBUG(0,(__location__ " Could not find node to take over public address '%s'\n", vnn->public_address));
		return -1;
	}

	vnn->pnn = pnn;
	return 0;
}

/*
  make any IP alias changes for public addresses that are necessary 
 */
int ctdb_takeover_run(struct ctdb_context *ctdb, struct ctdb_node_map *nodemap)
{
	int i, num_healthy;
	int ret;
	struct ctdb_public_ip ip;
	uint32_t mask;
	struct ctdb_vnn_list *vnn_list;
	struct ctdb_vnn *vnn;
	int maxnode, maxnum, minnode, minnum, num;

	ZERO_STRUCT(ip);

	/* Count how many completely healthy nodes we have */
	num_healthy = 0;
	for (i=0;i<nodemap->num;i++) {
		if (!(nodemap->nodes[i].flags & (NODE_FLAGS_INACTIVE|NODE_FLAGS_DISABLED))) {
			num_healthy++;
		}
	}

	if (num_healthy > 0) {
		/* We have healthy nodes, so only consider them for 
		   serving public addresses
		*/
		mask = NODE_FLAGS_INACTIVE|NODE_FLAGS_DISABLED;
	} else {
		/* We didnt have any completely healthy nodes so
		   use "disabled" nodes as a fallback
		*/
		mask = NODE_FLAGS_INACTIVE;
	}


	/* mark all public addresses with a masked node as being served by
	   node -1
	*/
	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			if (vnn->pnn == -1) {
				continue;
			}
			if (nodemap->nodes[vnn->pnn].flags & mask) {
				vnn->pnn = -1;
			}
		}
	}


	/* now we must redistribute all public addresses with takeover node
	   -1 among the nodes available
	*/
	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
try_again:
		/* loop over all vnn's and find a physical node to cover for 
		   each unassigned vnn.
		*/
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			if (vnn->pnn == -1) {
				if (find_takeover_node(ctdb, nodemap, mask, vnn_list, vnn)) {
					DEBUG(0,("Failed to find node to cover ip %s\n", vnn->public_address));
					return -1;
				}
			}
		}

		/* Get the highest and lowes number of vnn's a valid node
		   covers for this interface 
		*/
		maxnode = -1;
		minnode = -1;
		for (i=0;i<nodemap->num;i++) {
			if (nodemap->nodes[i].flags & mask) {
				continue;
			}
			num = node_ip_coverage(ctdb, i, vnn_list);
			if (maxnode == -1) {
				maxnode = i;
				maxnum  = num;
			} else {
				if (num > maxnum) {
					maxnode = i;
					maxnum  = num;
				}
			}
			if (minnode == -1) {
				minnode = i;
				minnum  = num;
			} else {
				if (num < minnum) {
					minnode = i;
					minnum  = num;
				}
			}
		}
		if (maxnode == -1) {
			DEBUG(0,(__location__ " Could not find maxnode\n"));
			return -1;
		}

		/* if the spread between the smallest and largest coverage by
		   a node is >=2 we steal one of the ips from the node with the
		   most coverage to even things out a bit
		*/
		if (maxnum > minnum+1) {
			/* mark one of maxnode's vnn's as unassigned and try
			   again
			*/
			for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
				if (vnn->pnn == maxnode) {
					vnn->pnn = -1;
					goto try_again;
				}
			}
		}
	}



	/* at this point ->pnn is the node which will own each IP */

	/* now tell all nodes to delete any alias that they should not
	   have.  This will be a NOOP on nodes that don't currently
	   hold the given alias */
	for (i=0;i<nodemap->num;i++) {
		/* don't talk to unconnected nodes, but do talk to banned nodes */
		if (nodemap->nodes[i].flags & NODE_FLAGS_DISCONNECTED) {
			continue;
		}

		for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
			for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
				if (vnn->pnn == nodemap->nodes[i].pnn) {
					/* This node should be serving this
					   vnn so dont tell it to release the ip
					*/
					continue;
				}

				ip.pnn = vnn->pnn;
				ip.sin.sin_family = AF_INET;
				inet_aton(vnn->public_address, &ip.sin.sin_addr);

				ret = ctdb_ctrl_release_ip(ctdb, TAKEOVER_TIMEOUT(),
							   nodemap->nodes[i].pnn, 
							   &ip);
				if (ret != 0) {
					DEBUG(0,("Failed to tell vnn %u to release IP %s\n",
						 nodemap->nodes[i].pnn,
						 vnn->public_address));
					return -1;
				}
			}
		}
	}

	/* tell all nodes to get their own IPs */
	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			if (vnn->pnn == -1) {
				/* this IP won't be taken over */
				continue;
			}
			ip.pnn = vnn->pnn;
			ip.sin.sin_family = AF_INET;
			inet_aton(vnn->public_address, &ip.sin.sin_addr);

			ret = ctdb_ctrl_takeover_ip(ctdb, TAKEOVER_TIMEOUT(), 
					    vnn->pnn, 
					    &ip);
			if (ret != 0) {
				DEBUG(0,("Failed asking vnn %u to take over IP %s\n",
					 vnn->pnn, 
					 vnn->public_address));
				return -1;
			}
		}
	}

	return 0;
}


/*
  destroy a ctdb_client_ip structure
 */
static int ctdb_client_ip_destructor(struct ctdb_client_ip *ip)
{
	DLIST_REMOVE(ip->ctdb->client_ip_list, ip);
	return 0;
}

/*
  called by a client to inform us of a TCP connection that it is managing
  that should tickled with an ACK when IP takeover is done
 */
int32_t ctdb_control_tcp_client(struct ctdb_context *ctdb, uint32_t client_id,
				TDB_DATA indata)
{
	struct ctdb_client *client = ctdb_reqid_find(ctdb, client_id, struct ctdb_client);
	struct ctdb_control_tcp *p = (struct ctdb_control_tcp *)indata.dptr;
	struct ctdb_tcp_list *tcp;
	struct ctdb_control_tcp_vnn t;
	int ret;
	TDB_DATA data;
	struct ctdb_client_ip *ip;
	char *addr;
	struct ctdb_vnn *vnn;

	addr = inet_ntoa(p->dest.sin_addr);

	vnn = find_public_ip_vnn(ctdb, addr);
	if (vnn == NULL) {
		DEBUG(3,("Could not add client IP %s. This is not a public address.\n", addr)); 
		return 0;
	}

	ip = talloc(client, struct ctdb_client_ip);
	CTDB_NO_MEMORY(ctdb, ip);

	ip->ctdb = ctdb;
	ip->ip = p->dest;
	ip->client_id = client_id;
	talloc_set_destructor(ip, ctdb_client_ip_destructor);
	DLIST_ADD(ctdb->client_ip_list, ip);

	tcp = talloc(client, struct ctdb_tcp_list);
	CTDB_NO_MEMORY(ctdb, tcp);

	tcp->connection.saddr = p->src;
	tcp->connection.daddr = p->dest;

	DLIST_ADD(client->tcp_list, tcp);

	t.src  = p->src;
	t.dest = p->dest;

	data.dptr = (uint8_t *)&t;
	data.dsize = sizeof(t);

	DEBUG(2,("registered tcp client for %u->%s:%u\n",
		 (unsigned)ntohs(p->dest.sin_port), 
		 inet_ntoa(p->src.sin_addr),
		 (unsigned)ntohs(p->src.sin_port)));

	/* tell all nodes about this tcp connection */
	ret = ctdb_daemon_send_control(ctdb, CTDB_BROADCAST_CONNECTED, 0, 
				       CTDB_CONTROL_TCP_ADD,
				       0, CTDB_CTRL_FLAG_NOREPLY, data, NULL, NULL);
	if (ret != 0) {
		DEBUG(0,(__location__ " Failed to send CTDB_CONTROL_TCP_ADD\n"));
		return -1;
	}

	return 0;
}

/*
  see if two sockaddr_in are the same
 */
static bool same_sockaddr_in(struct sockaddr_in *in1, struct sockaddr_in *in2)
{
	return in1->sin_family == in2->sin_family &&
		in1->sin_port == in2->sin_port &&
		in1->sin_addr.s_addr == in2->sin_addr.s_addr;
}

/*
  find a tcp address on a list
 */
static struct ctdb_tcp_connection *ctdb_tcp_find(struct ctdb_tcp_array *array, 
					   struct ctdb_tcp_connection *tcp)
{
	int i;

	if (array == NULL) {
		return NULL;
	}

	for (i=0;i<array->num;i++) {
		if (same_sockaddr_in(&array->connections[i].saddr, &tcp->saddr) &&
		    same_sockaddr_in(&array->connections[i].daddr, &tcp->daddr)) {
			return &array->connections[i];
		}
	}
	return NULL;
}

/*
  called by a daemon to inform us of a TCP connection that one of its
  clients managing that should tickled with an ACK when IP takeover is
  done
 */
int32_t ctdb_control_tcp_add(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_control_tcp_vnn *p = (struct ctdb_control_tcp_vnn *)indata.dptr;
	struct ctdb_tcp_array *tcparray;
	struct ctdb_tcp_connection tcp;
	struct ctdb_vnn *vnn;
	char *addr;

	addr = inet_ntoa(p->dest.sin_addr);
	vnn = find_public_ip_vnn(ctdb, addr);
	if (vnn == NULL) {
		DEBUG(0,(__location__ " got TCP_ADD control for an address which is not a public address '%s'\n", addr));
		return-1;
	}


	tcparray = vnn->tcp_array;

	/* If this is the first tickle */
	if (tcparray == NULL) {
		tcparray = talloc_size(ctdb->nodes, 
			offsetof(struct ctdb_tcp_array, connections) +
			sizeof(struct ctdb_tcp_connection) * 1);
		CTDB_NO_MEMORY(ctdb, tcparray);
		vnn->tcp_array = tcparray;

		tcparray->num = 0;
		tcparray->connections = talloc_size(tcparray, sizeof(struct ctdb_tcp_connection));
		CTDB_NO_MEMORY(ctdb, tcparray->connections);

		tcparray->connections[tcparray->num].saddr = p->src;
		tcparray->connections[tcparray->num].daddr = p->dest;
		tcparray->num++;
		return 0;
	}


	/* Do we already have this tickle ?*/
	tcp.saddr = p->src;
	tcp.daddr = p->dest;
	if (ctdb_tcp_find(vnn->tcp_array, &tcp) != NULL) {
		DEBUG(4,("Already had tickle info for %s:%u for vnn:%u\n",
			 inet_ntoa(tcp.daddr.sin_addr),
			 ntohs(tcp.daddr.sin_port),
			 vnn->pnn));
		return 0;
	}

	/* A new tickle, we must add it to the array */
	tcparray->connections = talloc_realloc(tcparray, tcparray->connections,
					struct ctdb_tcp_connection,
					tcparray->num+1);
	CTDB_NO_MEMORY(ctdb, tcparray->connections);

	vnn->tcp_array = tcparray;
	tcparray->connections[tcparray->num].saddr = p->src;
	tcparray->connections[tcparray->num].daddr = p->dest;
	tcparray->num++;
				
	DEBUG(2,("Added tickle info for %s:%u from vnn %u\n",
		 inet_ntoa(tcp.daddr.sin_addr),
		 ntohs(tcp.daddr.sin_port),
		 vnn->pnn));

	return 0;
}


/*
  called by a daemon to inform us of a TCP connection that one of its
  clients managing that should tickled with an ACK when IP takeover is
  done
 */
static void ctdb_remove_tcp_connection(struct ctdb_context *ctdb, struct ctdb_tcp_connection *conn)
{
	struct ctdb_tcp_connection *tcpp;
	struct ctdb_vnn *vnn = find_public_ip_vnn(ctdb, inet_ntoa(conn->daddr.sin_addr));

	if (vnn == NULL) {
		DEBUG(0,(__location__ " unable to find public address %s\n", inet_ntoa(conn->daddr.sin_addr)));
		return;
	}

	/* if the array is empty we cant remove it
	   and we dont need to do anything
	 */
	if (vnn->tcp_array == NULL) {
		DEBUG(2,("Trying to remove tickle that doesnt exist (array is empty) %s:%u\n",
			 inet_ntoa(conn->daddr.sin_addr),
			 ntohs(conn->daddr.sin_port)));
		return;
	}


	/* See if we know this connection
	   if we dont know this connection  then we dont need to do anything
	 */
	tcpp = ctdb_tcp_find(vnn->tcp_array, conn);
	if (tcpp == NULL) {
		DEBUG(2,("Trying to remove tickle that doesnt exist %s:%u\n",
			 inet_ntoa(conn->daddr.sin_addr),
			 ntohs(conn->daddr.sin_port)));
		return;
	}


	/* We need to remove this entry from the array.
           Instead of allocating a new array and copying data to it
	   we cheat and just copy the last entry in the existing array
	   to the entry that is to be removed and just shring the 
	   ->num field
	 */
	*tcpp = vnn->tcp_array->connections[vnn->tcp_array->num - 1];
	vnn->tcp_array->num--;

	/* If we deleted the last entry we also need to remove the entire array
	 */
	if (vnn->tcp_array->num == 0) {
		talloc_free(vnn->tcp_array);
		vnn->tcp_array = NULL;
	}		

	vnn->tcp_update_needed = true;

	DEBUG(2,("Removed tickle info for %s:%u\n",
		 inet_ntoa(conn->saddr.sin_addr),
		 ntohs(conn->saddr.sin_port)));
}


/*
  called when a daemon restarts - send all tickes for all public addresses
  we are serving immediately to the new node.
 */
int32_t ctdb_control_startup(struct ctdb_context *ctdb, uint32_t vnn)
{
/*XXX here we should send all tickes we are serving to the new node */
	return 0;
}


/*
  called when a client structure goes away - hook to remove
  elements from the tcp_list in all daemons
 */
void ctdb_takeover_client_destructor_hook(struct ctdb_client *client)
{
	while (client->tcp_list) {
		struct ctdb_tcp_list *tcp = client->tcp_list;
		DLIST_REMOVE(client->tcp_list, tcp);
		ctdb_remove_tcp_connection(client->ctdb, &tcp->connection);
	}
}


/*
  release all IPs on shutdown
 */
void ctdb_release_all_ips(struct ctdb_context *ctdb)
{
	struct ctdb_vnn_list *vnn_list;
	struct ctdb_vnn *vnn;

	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			if (ctdb_sys_have_ip(vnn->public_address)) {
				struct in_addr in;
				ctdb_event_script(ctdb, "releaseip %s %s %u",
						  vnn_list->iface, 
						  vnn->public_address,
						  vnn->public_netmask_bits);
				if (inet_aton(vnn->public_address, &in) != 0) {
					release_kill_clients(ctdb, in);
				}
			}
		}
	}
}


/*
  get list of public IPs
 */
int32_t ctdb_control_get_public_ips(struct ctdb_context *ctdb, 
				    struct ctdb_req_control *c, TDB_DATA *outdata)
{
	int i, num, len;
	struct ctdb_all_public_ips *ips;
	struct ctdb_vnn_list *vnn_list;
	struct ctdb_vnn *vnn;

	/* count how many public ip structures we have */
	num = 0;
	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			num++;
		}
	}

	len = offsetof(struct ctdb_all_public_ips, ips) + 
		num*sizeof(struct ctdb_public_ip);
	ips = talloc_zero_size(outdata, len);
	CTDB_NO_MEMORY(ctdb, ips);

	outdata->dsize = len;
	outdata->dptr  = (uint8_t *)ips;

	ips->num = num;
	i = 0;
	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			ips->ips[i].pnn   = vnn->pnn;
			ips->ips[i].sin.sin_family = AF_INET;
			inet_aton(vnn->public_address, 
				  &ips->ips[i].sin.sin_addr);
			i++;
		}
	}

	return 0;
}



/* 
   structure containing the listening socket and the list of tcp connections
   that the ctdb daemon is to kill
*/
struct ctdb_kill_tcp {
	struct ctdb_vnn_list *vnn_list;
	struct ctdb_context *ctdb;
	int capture_fd;
	int sending_fd;
	struct fd_event *fde;
	trbt_tree_t *connections;
	void *private_data;
};

/*
  a tcp connection that is to be killed
 */
struct ctdb_killtcp_con {
	struct sockaddr_in src;
	struct sockaddr_in dst;
	int count;
	struct ctdb_kill_tcp *killtcp;
};

/* this function is used to create a key to represent this socketpair
   in the killtcp tree.
   this key is used to insert and lookup matching socketpairs that are
   to be tickled and RST
*/
#define KILLTCP_KEYLEN	4
static uint32_t *killtcp_key(struct sockaddr_in *src, struct sockaddr_in *dst)
{
	static uint32_t key[KILLTCP_KEYLEN];

	key[0]	= dst->sin_addr.s_addr;
	key[1]	= src->sin_addr.s_addr;
	key[2]	= dst->sin_port;
	key[3]	= src->sin_port;

	return key;
}

/*
  called when we get a read event on the raw socket
 */
static void capture_tcp_handler(struct event_context *ev, struct fd_event *fde, 
				uint16_t flags, void *private_data)
{
	struct ctdb_kill_tcp *killtcp = talloc_get_type(private_data, struct ctdb_kill_tcp);
	struct ctdb_killtcp_con *con;
	struct sockaddr_in src, dst;
	uint32_t ack_seq, seq;

	if (!(flags & EVENT_FD_READ)) {
		return;
	}

	if (ctdb_sys_read_tcp_packet(killtcp->capture_fd,
				killtcp->private_data,
				&src, &dst,
				&ack_seq, &seq) != 0) {
		/* probably a non-tcp ACK packet */
		return;
	}

	/* check if we have this guy in our list of connections
	   to kill
	*/
	con = trbt_lookuparray32(killtcp->connections, 
			KILLTCP_KEYLEN, killtcp_key(&src, &dst));
	if (con == NULL) {
		/* no this was some other packet we can just ignore */
		return;
	}

	/* This one has been tickled !
	   now reset him and remove him from the list.
	 */
	DEBUG(1, ("sending a tcp reset to kill connection :%d -> %s:%d\n", ntohs(con->dst.sin_port), inet_ntoa(con->src.sin_addr), ntohs(con->src.sin_port)));

	ctdb_sys_send_tcp(killtcp->sending_fd, &con->dst, 
			  &con->src, ack_seq, seq, 1);
	talloc_free(con);
}


/* when traversing the list of all tcp connections to send tickle acks to
   (so that we can capture the ack coming back and kill the connection
    by a RST)
   this callback is called for each connection we are currently trying to kill
*/
static void tickle_connection_traverse(void *param, void *data)
{
	struct ctdb_killtcp_con *con = talloc_get_type(data, struct ctdb_killtcp_con);
	struct ctdb_kill_tcp *killtcp = talloc_get_type(param, struct ctdb_kill_tcp);

	/* have tried too many times, just give up */
	if (con->count >= 5) {
		talloc_free(con);
		return;
	}

	/* othervise, try tickling it again */
	con->count++;
	ctdb_sys_send_tcp(killtcp->sending_fd, &con->dst, &con->src, 0, 0, 0);
}


/* 
   called every second until all sentenced connections have been reset
 */
static void ctdb_tickle_sentenced_connections(struct event_context *ev, struct timed_event *te, 
					      struct timeval t, void *private_data)
{
	struct ctdb_kill_tcp *killtcp = talloc_get_type(private_data, struct ctdb_kill_tcp);


	/* loop over all connections sending tickle ACKs */
	trbt_traversearray32(killtcp->connections, KILLTCP_KEYLEN, tickle_connection_traverse, killtcp);


	/* If there are no more connections to kill we can remove the
	   entire killtcp structure
	 */
	if ( (killtcp->connections == NULL) || 
	     (killtcp->connections->root == NULL) ) {
		talloc_free(killtcp);
		return;
	}

	/* try tickling them again in a seconds time
	 */
	event_add_timed(killtcp->ctdb->ev, killtcp, timeval_current_ofs(1, 0), 
			ctdb_tickle_sentenced_connections, killtcp);
}

/*
  destroy the killtcp structure
 */
static int ctdb_killtcp_destructor(struct ctdb_kill_tcp *killtcp)
{
	if (killtcp->sending_fd != -1) {
		close(killtcp->sending_fd);
		killtcp->sending_fd = -1;
	}
	killtcp->vnn_list->killtcp = NULL;
	return 0;
}


/* nothing fancy here, just unconditionally replace any existing
   connection structure with the new one.

   dont even free the old one if it did exist, that one is talloc_stolen
   by the same node in the tree anyway and will be deleted when the new data 
   is deleted
*/
static void *add_killtcp_callback(void *parm, void *data)
{
	return parm;
}

/*
  add a tcp socket to the list of connections we will kill on failover
 */
static int ctdb_killtcp_add_connection(struct ctdb_context *ctdb, 
				       struct sockaddr_in *src, struct sockaddr_in *dst)
{
	struct ctdb_kill_tcp *killtcp;
	struct ctdb_killtcp_con *con;
	char *addr;
	struct ctdb_vnn *vnn;
	struct ctdb_vnn_list *vnn_list;

	addr = inet_ntoa(dst->sin_addr);

	vnn = find_public_ip_vnn(ctdb, addr);
	if (vnn == NULL) {
		DEBUG(0,(__location__ " Could not killtcp, '%s' is not a public address\n", addr)); 
		return -1;
	}

	vnn_list = vnn->vnn_list;
	killtcp = vnn_list->killtcp;
	
	/* If this is the first connection to kill we must allocate
	   a new structure
	 */
	if (killtcp == NULL) {
		killtcp = talloc_zero(ctdb, struct ctdb_kill_tcp);
		CTDB_NO_MEMORY(ctdb, killtcp);

		killtcp->vnn_list    = vnn_list;
		killtcp->ctdb        = ctdb;
		killtcp->capture_fd  = -1;
		killtcp->sending_fd  = -1;
		killtcp->connections= trbt_create(killtcp, 0);

		vnn_list->killtcp        = killtcp;
		talloc_set_destructor(killtcp, ctdb_killtcp_destructor);
	}



	/* create a structure that describes this connection we want to
	   RST and store it in killtcp->connections
	*/
	con = talloc(killtcp, struct ctdb_killtcp_con);
	CTDB_NO_MEMORY(ctdb, con);
	con->src     = *src;
	con->dst     = *dst;
	con->count   = 0;
	con->killtcp = killtcp;


	trbt_insertarray32_callback(killtcp->connections,
			KILLTCP_KEYLEN, killtcp_key(&con->dst, &con->src),
			add_killtcp_callback, con);

	/* 
	   If we dont have a socket to send from yet we must create it
	 */
	if (killtcp->sending_fd == -1) {
		killtcp->sending_fd = ctdb_sys_open_sending_socket();
		if (killtcp->sending_fd == -1) {
			DEBUG(0,(__location__ " Failed to open sending socket for killtcp\n"));
			goto failed;
		}
	}

	/* 
	   If we dont have a socket to listen on yet we must create it
	 */
	if (killtcp->capture_fd == -1) {
		killtcp->capture_fd = ctdb_sys_open_capture_socket(vnn_list->iface, &killtcp->private_data);
		if (killtcp->capture_fd == -1) {
			DEBUG(0,(__location__ " Failed to open capturing socket for killtcp\n"));
			goto failed;
		}
	}


	if (killtcp->fde == NULL) {
		killtcp->fde = event_add_fd(ctdb->ev, killtcp, killtcp->capture_fd, 
					    EVENT_FD_READ | EVENT_FD_AUTOCLOSE, 
					    capture_tcp_handler, killtcp);

		/* We also need to set up some events to tickle all these connections
		   until they are all reset
		*/
		event_add_timed(ctdb->ev, killtcp, timeval_current_ofs(1, 0), 
				ctdb_tickle_sentenced_connections, killtcp);
	}

	/* tickle him once now */
	ctdb_sys_send_tcp(killtcp->sending_fd, &con->dst, &con->src, 0, 0, 0);

	return 0;

failed:
	talloc_free(vnn_list->killtcp);
	vnn_list->killtcp = NULL;
	return -1;
}

/*
  called by a daemon to inform us of a TCP connection that one of its
  clients managing that should reset when IP takeover is done
 */
int32_t ctdb_control_kill_tcp(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_control_killtcp *killtcp = (struct ctdb_control_killtcp *)indata.dptr;

	return ctdb_killtcp_add_connection(ctdb, &killtcp->src, &killtcp->dst);
}

/*
  called by a daemon to inform us of the entire list of TCP tickles for
  a particular public address.
  this control should only be sent by the node that is currently serving
  that public address.
 */
int32_t ctdb_control_set_tcp_tickle_list(struct ctdb_context *ctdb, TDB_DATA indata)
{
	struct ctdb_control_tcp_tickle_list *list = (struct ctdb_control_tcp_tickle_list *)indata.dptr;
	struct ctdb_tcp_array *tcparray;
	char *addr;
	struct ctdb_vnn *vnn;

	/* We must at least have tickles.num or else we cant verify the size
	   of the received data blob
	 */
	if (indata.dsize < offsetof(struct ctdb_control_tcp_tickle_list, 
					tickles.connections)) {
		DEBUG(0,("Bad indata in ctdb_control_set_tcp_tickle_list. Not enough data for the tickle.num field\n"));
		return -1;
	}

	/* verify that the size of data matches what we expect */
	if (indata.dsize < offsetof(struct ctdb_control_tcp_tickle_list, 
				tickles.connections)
			 + sizeof(struct ctdb_tcp_connection)
				 * list->tickles.num) {
		DEBUG(0,("Bad indata in ctdb_control_set_tcp_tickle_list\n"));
		return -1;
	}	

	addr = inet_ntoa(list->ip.sin_addr);

	vnn = find_public_ip_vnn(ctdb, addr);
	if (vnn == NULL) {
		DEBUG(0,(__location__ " Could not set tcp tickle list, '%s' is not a public address\n", addr)); 
		return 1;
	}

	/* remove any old ticklelist we might have */
	talloc_free(vnn->tcp_array);
	vnn->tcp_array = NULL;

	tcparray = talloc(ctdb->nodes, struct ctdb_tcp_array);
	CTDB_NO_MEMORY(ctdb, tcparray);

	tcparray->num = list->tickles.num;

	tcparray->connections = talloc_array(tcparray, struct ctdb_tcp_connection, tcparray->num);
	CTDB_NO_MEMORY(ctdb, tcparray->connections);

	memcpy(tcparray->connections, &list->tickles.connections[0], 
	       sizeof(struct ctdb_tcp_connection)*tcparray->num);

	/* We now have a new fresh tickle list array for this vnn */
	vnn->tcp_array = talloc_steal(vnn, tcparray);
	
	return 0;
}

/*
  called to return the full list of tickles for the puclic address associated 
  with the provided vnn
 */
int32_t ctdb_control_get_tcp_tickle_list(struct ctdb_context *ctdb, TDB_DATA indata, TDB_DATA *outdata)
{
	struct sockaddr_in *ip = (struct sockaddr_in *)indata.dptr;
	struct ctdb_control_tcp_tickle_list *list;
	struct ctdb_tcp_array *tcparray;
	int num;
	char *addr;
	struct ctdb_vnn *vnn;


	addr = inet_ntoa(ip->sin_addr);

	vnn = find_public_ip_vnn(ctdb, addr);
	if (vnn == NULL) {
		DEBUG(0,(__location__ " Could not get tcp tickle list, '%s' is not a public address\n", addr)); 
		return 1;
	}

	tcparray = vnn->tcp_array;
	if (tcparray) {
		num = tcparray->num;
	} else {
		num = 0;
	}

	outdata->dsize = offsetof(struct ctdb_control_tcp_tickle_list, 
				tickles.connections)
			+ sizeof(struct ctdb_tcp_connection) * num;

	outdata->dptr  = talloc_size(outdata, outdata->dsize);
	CTDB_NO_MEMORY(ctdb, outdata->dptr);
	list = (struct ctdb_control_tcp_tickle_list *)outdata->dptr;

	list->ip = *ip;
	list->tickles.num = num;
	if (num) {
		memcpy(&list->tickles.connections[0], tcparray->connections, 
			sizeof(struct ctdb_tcp_connection) * num);
	}

	return 0;
}


/*
  set the list of all tcp tickles for a public address
 */
static int ctdb_ctrl_set_tcp_tickles(struct ctdb_context *ctdb, 
			      struct timeval timeout, uint32_t destnode, 
			      struct sockaddr_in *ip,
			      struct ctdb_tcp_array *tcparray)
{
	int ret, num;
	TDB_DATA data;
	struct ctdb_control_tcp_tickle_list *list;

	if (tcparray) {
		num = tcparray->num;
	} else {
		num = 0;
	}

	data.dsize = offsetof(struct ctdb_control_tcp_tickle_list, 
				tickles.connections) +
			sizeof(struct ctdb_tcp_connection) * num;
	data.dptr = talloc_size(ctdb, data.dsize);
	CTDB_NO_MEMORY(ctdb, data.dptr);

	list = (struct ctdb_control_tcp_tickle_list *)data.dptr;
	list->ip = *ip;
	list->tickles.num = num;
	if (tcparray) {
		memcpy(&list->tickles.connections[0], tcparray->connections, sizeof(struct ctdb_tcp_connection) * num);
	}

	ret = ctdb_daemon_send_control(ctdb, CTDB_BROADCAST_CONNECTED, 0, 
				       CTDB_CONTROL_SET_TCP_TICKLE_LIST,
				       0, CTDB_CTRL_FLAG_NOREPLY, data, NULL, NULL);
	if (ret != 0) {
		DEBUG(0,(__location__ " ctdb_control for set tcp tickles failed\n"));
		return -1;
	}

	talloc_free(data.dptr);

	return ret;
}


/*
  perform tickle updates if required
 */
static void ctdb_update_tcp_tickles(struct event_context *ev, 
				struct timed_event *te, 
				struct timeval t, void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);
	int ret;
	struct ctdb_vnn_list *vnn_list;
	struct ctdb_vnn *vnn;

	for (vnn_list=ctdb->vnn_list;vnn_list;vnn_list=vnn_list->next) {
		struct sockaddr_in ip;

		for (vnn=vnn_list->vnn;vnn;vnn=vnn->next) {
			/* we only send out updates for public addresses that 
			   we have taken over
			 */
			if (ctdb->pnn != vnn->pnn) {
				continue;
			}
			/* We only send out the updates if we need to */
			if (!vnn->tcp_update_needed) {
				continue;
			}
			inet_aton(vnn->public_address, &ip.sin_addr);
			ret = ctdb_ctrl_set_tcp_tickles(ctdb, 
					TAKEOVER_TIMEOUT(),
					CTDB_BROADCAST_CONNECTED,
					&ip,
					vnn->tcp_array);

			if (ret != 0) {
				DEBUG(0,("Failed to send the tickle update for public address %s\n", vnn->public_address));
			}
		}
	}

	event_add_timed(ctdb->ev, ctdb->tickle_update_context,
			     timeval_current_ofs(ctdb->tunable.tickle_update_interval, 0), 
			     ctdb_update_tcp_tickles, ctdb);
}		
	

/*
  start periodic update of tcp tickles
 */
void ctdb_start_tcp_tickle_update(struct ctdb_context *ctdb)
{
	ctdb->tickle_update_context = talloc_new(ctdb);

	event_add_timed(ctdb->ev, ctdb->tickle_update_context,
			     timeval_current_ofs(ctdb->tunable.tickle_update_interval, 0), 
			     ctdb_update_tcp_tickles, ctdb);
}
