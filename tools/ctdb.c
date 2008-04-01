/* 
   ctdb control tool

   Copyright (C) Andrew Tridgell  2007
   Copyright (C) Ronnie Sahlberg  2007

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
#include "system/time.h"
#include "system/filesys.h"
#include "system/network.h"
#include "system/locale.h"
#include "popt.h"
#include "cmdline.h"
#include "../include/ctdb.h"
#include "../include/ctdb_private.h"

static void usage(void);

static struct {
	int timelimit;
	uint32_t pnn;
	int machinereadable;
	int maxruntime;
} options;

#define TIMELIMIT() timeval_current_ofs(options.timelimit, 0)

/*
  see if a process exists
 */
static int control_process_exists(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t pnn, pid;
	int ret;
	if (argc < 1) {
		usage();
	}

	if (sscanf(argv[0], "%u:%u", &pnn, &pid) != 2) {
		DEBUG(DEBUG_ERR, ("Badly formed pnn:pid\n"));
		return -1;
	}

	ret = ctdb_ctrl_process_exists(ctdb, pnn, pid);
	if (ret == 0) {
		printf("%u:%u exists\n", pnn, pid);
	} else {
		printf("%u:%u does not exist\n", pnn, pid);
	}
	return ret;
}

/*
  display statistics structure
 */
static void show_statistics(struct ctdb_statistics *s)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	int i;
	const char *prefix=NULL;
	int preflen=0;
	const struct {
		const char *name;
		uint32_t offset;
	} fields[] = {
#define STATISTICS_FIELD(n) { #n, offsetof(struct ctdb_statistics, n) }
		STATISTICS_FIELD(num_clients),
		STATISTICS_FIELD(frozen),
		STATISTICS_FIELD(recovering),
		STATISTICS_FIELD(client_packets_sent),
		STATISTICS_FIELD(client_packets_recv),
		STATISTICS_FIELD(node_packets_sent),
		STATISTICS_FIELD(node_packets_recv),
		STATISTICS_FIELD(keepalive_packets_sent),
		STATISTICS_FIELD(keepalive_packets_recv),
		STATISTICS_FIELD(node.req_call),
		STATISTICS_FIELD(node.reply_call),
		STATISTICS_FIELD(node.req_dmaster),
		STATISTICS_FIELD(node.reply_dmaster),
		STATISTICS_FIELD(node.reply_error),
		STATISTICS_FIELD(node.req_message),
		STATISTICS_FIELD(node.req_control),
		STATISTICS_FIELD(node.reply_control),
		STATISTICS_FIELD(client.req_call),
		STATISTICS_FIELD(client.req_message),
		STATISTICS_FIELD(client.req_control),
		STATISTICS_FIELD(timeouts.call),
		STATISTICS_FIELD(timeouts.control),
		STATISTICS_FIELD(timeouts.traverse),
		STATISTICS_FIELD(total_calls),
		STATISTICS_FIELD(pending_calls),
		STATISTICS_FIELD(lockwait_calls),
		STATISTICS_FIELD(pending_lockwait_calls),
		STATISTICS_FIELD(memory_used),
		STATISTICS_FIELD(max_hop_count),
	};
	printf("CTDB version %u\n", CTDB_VERSION);
	for (i=0;i<ARRAY_SIZE(fields);i++) {
		if (strchr(fields[i].name, '.')) {
			preflen = strcspn(fields[i].name, ".")+1;
			if (!prefix || strncmp(prefix, fields[i].name, preflen) != 0) {
				prefix = fields[i].name;
				printf(" %*.*s\n", preflen-1, preflen-1, fields[i].name);
			}
		} else {
			preflen = 0;
		}
		printf(" %*s%-22s%*s%10u\n", 
		       preflen?4:0, "",
		       fields[i].name+preflen, 
		       preflen?0:4, "",
		       *(uint32_t *)(fields[i].offset+(uint8_t *)s));
	}
	printf(" %-30s     %.6f sec\n", "max_call_latency", s->max_call_latency);
	printf(" %-30s     %.6f sec\n", "max_lockwait_latency", s->max_lockwait_latency);
	talloc_free(tmp_ctx);
}

/*
  display remote ctdb statistics combined from all nodes
 */
static int control_statistics_all(struct ctdb_context *ctdb)
{
	int ret, i;
	struct ctdb_statistics statistics;
	uint32_t *nodes;
	uint32_t num_nodes;

	nodes = ctdb_get_connected_nodes(ctdb, TIMELIMIT(), ctdb, &num_nodes);
	CTDB_NO_MEMORY(ctdb, nodes);
	
	ZERO_STRUCT(statistics);

	for (i=0;i<num_nodes;i++) {
		struct ctdb_statistics s1;
		int j;
		uint32_t *v1 = (uint32_t *)&s1;
		uint32_t *v2 = (uint32_t *)&statistics;
		uint32_t num_ints = 
			offsetof(struct ctdb_statistics, __last_counter) / sizeof(uint32_t);
		ret = ctdb_ctrl_statistics(ctdb, nodes[i], &s1);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, ("Unable to get statistics from node %u\n", nodes[i]));
			return ret;
		}
		for (j=0;j<num_ints;j++) {
			v2[j] += v1[j];
		}
		statistics.max_hop_count = 
			MAX(statistics.max_hop_count, s1.max_hop_count);
		statistics.max_call_latency = 
			MAX(statistics.max_call_latency, s1.max_call_latency);
		statistics.max_lockwait_latency = 
			MAX(statistics.max_lockwait_latency, s1.max_lockwait_latency);
	}
	talloc_free(nodes);
	printf("Gathered statistics for %u nodes\n", num_nodes);
	show_statistics(&statistics);
	return 0;
}

/*
  display remote ctdb statistics
 */
static int control_statistics(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct ctdb_statistics statistics;

	if (options.pnn == CTDB_BROADCAST_ALL) {
		return control_statistics_all(ctdb);
	}

	ret = ctdb_ctrl_statistics(ctdb, options.pnn, &statistics);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get statistics from node %u\n", options.pnn));
		return ret;
	}
	show_statistics(&statistics);
	return 0;
}


/*
  reset remote ctdb statistics
 */
static int control_statistics_reset(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_statistics_reset(ctdb, options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to reset statistics on node %u\n", options.pnn));
		return ret;
	}
	return 0;
}


/*
  display uptime of remote node
 */
static int control_uptime(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	int mypnn;
	struct ctdb_uptime *uptime = NULL;
	int tmp, days, hours, minutes, seconds;

	mypnn = ctdb_ctrl_getpnn(ctdb, TIMELIMIT(), options.pnn);
	if (mypnn == -1) {
		return -1;
	}

	ret = ctdb_ctrl_uptime(ctdb, ctdb, TIMELIMIT(), options.pnn, &uptime);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get uptime from node %u\n", options.pnn));
		return ret;
	}

	if (options.machinereadable){
		printf(":Current Node Time:Ctdb Start Time:Last Recovery Time:\n");
		printf(":%u:%u:%u:\n",
			(unsigned int)uptime->current_time.tv_sec,
			(unsigned int)uptime->ctdbd_start_time.tv_sec,
			(unsigned int)uptime->last_recovery_time.tv_sec);
		return 0;
	}

	printf("Current time of node  : %s", ctime(&uptime->current_time.tv_sec));

	tmp = uptime->current_time.tv_sec - uptime->ctdbd_start_time.tv_sec;
	seconds = tmp%60;
	tmp    /= 60;
	minutes = tmp%60;
	tmp    /= 60;
	hours   = tmp%24;
	tmp    /= 24;
	days    = tmp;
	printf("Ctdbd start time      : (%03d %02d:%02d:%02d) %s", days, hours, minutes, seconds, ctime(&uptime->ctdbd_start_time.tv_sec));

	tmp = uptime->current_time.tv_sec - uptime->last_recovery_time.tv_sec;
	seconds = tmp%60;
	tmp    /= 60;
	minutes = tmp%60;
	tmp    /= 60;
	hours   = tmp%24;
	tmp    /= 24;
	days    = tmp;
	printf("Time of last recovery : (%03d %02d:%02d:%02d) %s", days, hours, minutes, seconds, ctime(&uptime->last_recovery_time.tv_sec));

	return 0;
}

/*
  display remote ctdb status
 */
static int control_status(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret;
	struct ctdb_vnn_map *vnnmap=NULL;
	struct ctdb_node_map *nodemap=NULL;
	uint32_t recmode, recmaster;
	int mypnn;

	mypnn = ctdb_ctrl_getpnn(ctdb, TIMELIMIT(), options.pnn);
	if (mypnn == -1) {
		return -1;
	}

	ret = ctdb_ctrl_getnodemap(ctdb, TIMELIMIT(), options.pnn, ctdb, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get nodemap from node %u\n", options.pnn));
		return ret;
	}

	if(options.machinereadable){
		printf(":Node:IP:Disconnected:Banned:Disabled:Unhealthy:\n");
		for(i=0;i<nodemap->num;i++){
			printf(":%d:%s:%d:%d:%d:%d:\n", nodemap->nodes[i].pnn,
				inet_ntoa(nodemap->nodes[i].sin.sin_addr),
			       !!(nodemap->nodes[i].flags&NODE_FLAGS_DISCONNECTED),
			       !!(nodemap->nodes[i].flags&NODE_FLAGS_BANNED),
			       !!(nodemap->nodes[i].flags&NODE_FLAGS_PERMANENTLY_DISABLED),
			       !!(nodemap->nodes[i].flags&NODE_FLAGS_UNHEALTHY));
		}
		return 0;
	}

	printf("Number of nodes:%d\n", nodemap->num);
	for(i=0;i<nodemap->num;i++){
		static const struct {
			uint32_t flag;
			const char *name;
		} flag_names[] = {
			{ NODE_FLAGS_DISCONNECTED,          "DISCONNECTED" },
			{ NODE_FLAGS_PERMANENTLY_DISABLED,  "DISABLED" },
			{ NODE_FLAGS_BANNED,                "BANNED" },
			{ NODE_FLAGS_UNHEALTHY,             "UNHEALTHY" },
		};
		char *flags_str = NULL;
		int j;
		for (j=0;j<ARRAY_SIZE(flag_names);j++) {
			if (nodemap->nodes[i].flags & flag_names[j].flag) {
				if (flags_str == NULL) {
					flags_str = talloc_strdup(ctdb, flag_names[j].name);
				} else {
					flags_str = talloc_asprintf_append(flags_str, "|%s",
									   flag_names[j].name);
				}
				CTDB_NO_MEMORY_FATAL(ctdb, flags_str);
			}
		}
		if (flags_str == NULL) {
			flags_str = talloc_strdup(ctdb, "OK");
			CTDB_NO_MEMORY_FATAL(ctdb, flags_str);
		}
		printf("pnn:%d %-16s %s%s\n", nodemap->nodes[i].pnn,
		       inet_ntoa(nodemap->nodes[i].sin.sin_addr),
		       flags_str,
		       nodemap->nodes[i].pnn == mypnn?" (THIS NODE)":"");
		talloc_free(flags_str);
	}

	ret = ctdb_ctrl_getvnnmap(ctdb, TIMELIMIT(), options.pnn, ctdb, &vnnmap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get vnnmap from node %u\n", options.pnn));
		return ret;
	}
	if (vnnmap->generation == INVALID_GENERATION) {
		printf("Generation:INVALID\n");
	} else {
		printf("Generation:%d\n",vnnmap->generation);
	}
	printf("Size:%d\n",vnnmap->size);
	for(i=0;i<vnnmap->size;i++){
		printf("hash:%d lmaster:%d\n", i, vnnmap->map[i]);
	}

	ret = ctdb_ctrl_getrecmode(ctdb, ctdb, TIMELIMIT(), options.pnn, &recmode);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get recmode from node %u\n", options.pnn));
		return ret;
	}
	printf("Recovery mode:%s (%d)\n",recmode==CTDB_RECOVERY_NORMAL?"NORMAL":"RECOVERY",recmode);

	ret = ctdb_ctrl_getrecmaster(ctdb, ctdb, TIMELIMIT(), options.pnn, &recmaster);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get recmaster from node %u\n", options.pnn));
		return ret;
	}
	printf("Recovery master:%d\n",recmaster);

	return 0;
}

/*
  get a list of all tickles for this pnn
 */
static int control_get_tickles(struct ctdb_context *ctdb, int argc, const char **argv)
{
	struct ctdb_control_tcp_tickle_list *list;
	struct sockaddr_in ip;
	int i, ret;

	if (argc < 1) {
		usage();
	}

	ip.sin_family = AF_INET;
	if (inet_aton(argv[0], &ip.sin_addr) == 0) {
		DEBUG(DEBUG_ERR,("Wrongly formed ip address '%s'\n", argv[0]));
		return -1;
	}

	ret = ctdb_ctrl_get_tcp_tickles(ctdb, TIMELIMIT(), options.pnn, ctdb, &ip, &list);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to list tickles\n"));
		return -1;
	}

	printf("Tickles for ip:%s\n", inet_ntoa(list->ip.sin_addr));
	printf("Num tickles:%u\n", list->tickles.num);
	for (i=0;i<list->tickles.num;i++) {
		printf("SRC: %s:%u   ", inet_ntoa(list->tickles.connections[i].saddr.sin_addr), ntohs(list->tickles.connections[i].saddr.sin_port));
		printf("DST: %s:%u\n", inet_ntoa(list->tickles.connections[i].daddr.sin_addr), ntohs(list->tickles.connections[i].daddr.sin_port));
	}

	talloc_free(list);
	
	return 0;
}

/*
  move/failover an ip address to a specific node
 */
static int control_moveip(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t pnn;
	struct sockaddr_in ip;
	uint32_t value;
	struct ctdb_all_public_ips *ips;
	struct ctdb_public_ip pip;
	TDB_DATA data;
	struct ctdb_node_map *nodemap=NULL;
	int i, ret;

	if (argc < 2) {
		usage();
	}

	ip.sin_family = AF_INET;
	if (inet_aton(argv[0], &ip.sin_addr) == 0) {
		DEBUG(DEBUG_ERR,("Wrongly formed ip address '%s'\n", argv[0]));
		return -1;
	}


	ret = ctdb_ctrl_getnodemap(ctdb, TIMELIMIT(), CTDB_CURRENT_NODE, ctdb, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get nodemap from local node\n"));
		return ret;
	}

	if (sscanf(argv[1], "%u", &pnn) != 1) {
		DEBUG(DEBUG_ERR, ("Badly formed pnn\n"));
		return -1;
	}

	ret = ctdb_ctrl_get_tunable(ctdb, TIMELIMIT(), CTDB_CURRENT_NODE, "DeterministicIPs", &value);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to get tunable variable 'DeterministicIPs' from local node\n"));
		return -1;
	}
	if (value != 0) {
		DEBUG(DEBUG_ERR, ("The tunable 'DeterministicIPs' is set. You can only move ip addresses when this feature is disabled\n"));
		return -1;
	}

	ret = ctdb_ctrl_get_tunable(ctdb, TIMELIMIT(), CTDB_CURRENT_NODE, "NoIPFailback", &value);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to get tunable variable 'NoIPFailback' from local node\n"));
		return -1;
	}
	if (value == 0) {
		DEBUG(DEBUG_ERR, ("The tunable 'NoIPFailback' is NOT set. You can only move ip addresses when this feature is enabled\n"));
		return -1;
	}

	/* read the public ip list from the node */
	ret = ctdb_ctrl_get_public_ips(ctdb, TIMELIMIT(), pnn, ctdb, &ips);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get public ip list from node %u\n", pnn));
		return -1;
	}

	for (i=0;i<ips->num;i++) {
		if (ctdb_same_ip(&ip, &ips->ips[i].sin)) {
			break;
		}
	}
	if (i==ips->num) {
		DEBUG(DEBUG_ERR, ("Node %u can not host ip address '%s'\n",
			pnn, inet_ntoa(ip.sin_addr)));
		return -1;
	}
	if (ips->ips[i].pnn == pnn) {
		DEBUG(DEBUG_ERR, ("Host %u is already hosting '%s'\n",
			pnn, inet_ntoa(ips->ips[i].sin.sin_addr)));
		return -1;
	}

	/* send a moveip message to the recovery master */
	pip.pnn = pnn;
	pip.sin.sin_family = AF_INET;
	pip.sin.sin_addr   = ips->ips[i].sin.sin_addr;
	data.dsize = sizeof(pip);
	data.dptr = (unsigned char *)&pip;


	/* send release ip to all nodes */
	if (ctdb_client_async_control(ctdb, CTDB_CONTROL_RELEASE_IP,
			list_of_active_nodes(ctdb, nodemap, ctdb, true),
			TIMELIMIT(), false, data) != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Unable to send 'ReleaseIP' to all nodes.\n"));
		return -1;
	}

	return 0;
}

/*
  add a public ip address to a node
 */
static int control_addip(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	int len;
	unsigned mask;
	struct sockaddr_in addr;
	struct ctdb_control_ip_iface *pub;

	if (argc != 2) {
		usage();
	}

	if (!parse_ip_mask(argv[0], &addr, &mask)) {
		DEBUG(DEBUG_ERR, ("Badly formed ip/mask : %s\n", argv[0]));
		return -1;
	}

	len = offsetof(struct ctdb_control_ip_iface, iface) + strlen(argv[1]);
	pub = talloc_size(ctdb, len); 
	CTDB_NO_MEMORY(ctdb, pub);

	pub->sin   = addr;
	pub->mask  = mask;
	pub->len   = strlen(argv[1])+1;
	memcpy(&pub->iface[0], argv[1], strlen(argv[1])+1);

	ret = ctdb_ctrl_add_public_ip(ctdb, TIMELIMIT(), options.pnn, pub);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to add public ip to node %u\n", options.pnn));
		return ret;
	}

	return 0;
}

/*
  delete a public ip address from a node
 */
static int control_delip(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct sockaddr_in addr;
	struct ctdb_control_ip_iface pub;

	if (argc != 1) {
		usage();
	}

	addr.sin_family = AF_INET;
	if (inet_aton(argv[0], &addr.sin_addr) == 0) {
		DEBUG(DEBUG_ERR,("Wrongly formed ip address '%s'\n", argv[0]));
		return -1;
	}

	pub.sin   = addr;
	pub.mask  = 0;
	pub.len   = 0;

	ret = ctdb_ctrl_del_public_ip(ctdb, TIMELIMIT(), options.pnn, &pub);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to del public ip from node %u\n", options.pnn));
		return ret;
	}

	return 0;
}

/*
  kill a tcp connection
 */
static int kill_tcp(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct ctdb_control_killtcp killtcp;

	if (argc < 2) {
		usage();
	}

	if (!parse_ip_port(argv[0], &killtcp.src)) {
		DEBUG(DEBUG_ERR, ("Bad IP:port '%s'\n", argv[0]));
		return -1;
	}

	if (!parse_ip_port(argv[1], &killtcp.dst)) {
		DEBUG(DEBUG_ERR, ("Bad IP:port '%s'\n", argv[1]));
		return -1;
	}

	ret = ctdb_ctrl_killtcp(ctdb, TIMELIMIT(), options.pnn, &killtcp);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to killtcp from node %u\n", options.pnn));
		return ret;
	}

	return 0;
}


/*
  send a gratious arp
 */
static int control_gratious_arp(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct sockaddr_in sin;

	if (argc < 2) {
		usage();
	}

	sin.sin_family = AF_INET;
	if (inet_aton(argv[0], &sin.sin_addr) == 0) {
		DEBUG(DEBUG_ERR,("Wrongly formed ip address '%s'\n", argv[0]));
		return -1;
	}

	ret = ctdb_ctrl_gratious_arp(ctdb, TIMELIMIT(), options.pnn, &sin, argv[1]);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to send gratious_arp from node %u\n", options.pnn));
		return ret;
	}

	return 0;
}

/*
  register a server id
 */
static int regsrvid(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct ctdb_server_id server_id;

	if (argc < 3) {
		usage();
	}

	server_id.pnn       = strtoul(argv[0], NULL, 0);
	server_id.type      = strtoul(argv[1], NULL, 0);
	server_id.server_id = strtoul(argv[2], NULL, 0);

	ret = ctdb_ctrl_register_server_id(ctdb, TIMELIMIT(), &server_id);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to register server_id from node %u\n", options.pnn));
		return ret;
	}
	return -1;
}

/*
  unregister a server id
 */
static int unregsrvid(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct ctdb_server_id server_id;

	if (argc < 3) {
		usage();
	}

	server_id.pnn       = strtoul(argv[0], NULL, 0);
	server_id.type      = strtoul(argv[1], NULL, 0);
	server_id.server_id = strtoul(argv[2], NULL, 0);

	ret = ctdb_ctrl_unregister_server_id(ctdb, TIMELIMIT(), &server_id);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to unregister server_id from node %u\n", options.pnn));
		return ret;
	}
	return -1;
}

/*
  check if a server id exists
 */
static int chksrvid(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t status;
	int ret;
	struct ctdb_server_id server_id;

	if (argc < 3) {
		usage();
	}

	server_id.pnn       = strtoul(argv[0], NULL, 0);
	server_id.type      = strtoul(argv[1], NULL, 0);
	server_id.server_id = strtoul(argv[2], NULL, 0);

	ret = ctdb_ctrl_check_server_id(ctdb, TIMELIMIT(), options.pnn, &server_id, &status);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to check server_id from node %u\n", options.pnn));
		return ret;
	}

	if (status) {
		printf("Server id %d:%d:%d EXISTS\n", server_id.pnn, server_id.type, server_id.server_id);
	} else {
		printf("Server id %d:%d:%d does NOT exist\n", server_id.pnn, server_id.type, server_id.server_id);
	}
	return 0;
}

/*
  get a list of all server ids that are registered on a node
 */
static int getsrvids(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret;
	struct ctdb_server_id_list *server_ids;

	ret = ctdb_ctrl_get_server_id_list(ctdb, ctdb, TIMELIMIT(), options.pnn, &server_ids);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get server_id list from node %u\n", options.pnn));
		return ret;
	}

	for (i=0; i<server_ids->num; i++) {
		printf("Server id %d:%d:%d\n", 
			server_ids->server_ids[i].pnn, 
			server_ids->server_ids[i].type, 
			server_ids->server_ids[i].server_id); 
	}

	return -1;
}

/*
  send a tcp tickle ack
 */
static int tickle_tcp(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int s, ret;
	struct sockaddr_in src, dst;

	if (argc < 2) {
		usage();
	}

	if (!parse_ip_port(argv[0], &src)) {
		DEBUG(DEBUG_ERR, ("Bad IP:port '%s'\n", argv[0]));
		return -1;
	}

	if (!parse_ip_port(argv[1], &dst)) {
		DEBUG(DEBUG_ERR, ("Bad IP:port '%s'\n", argv[1]));
		return -1;
	}

	s = ctdb_sys_open_sending_socket();
	if (s == -1) {
		DEBUG(DEBUG_ERR, ("Failed to open socket for sending tickle\n"));
		return 0;
	}

	ret = ctdb_sys_send_tcp(s, &src, &dst, 0, 0, 0);
	close(s);
	if (ret==0) {
		return 0;
	}
	DEBUG(DEBUG_ERR, ("Error while sending tickle ack\n"));

	return -1;
}


/*
  display public ip status
 */
static int control_ip(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	struct ctdb_all_public_ips *ips;

	/* read the public ip list from this node */
	ret = ctdb_ctrl_get_public_ips(ctdb, TIMELIMIT(), options.pnn, tmp_ctx, &ips);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get public ips from node %u\n", options.pnn));
		talloc_free(tmp_ctx);
		return ret;
	}

	if (options.machinereadable){
		printf(":Public IP:Node:\n");
	} else {
		printf("Public IPs on node %u\n", options.pnn);
	}

	for (i=1;i<=ips->num;i++) {
		if (options.machinereadable){
			printf(":%s:%d:\n", inet_ntoa(ips->ips[ips->num-i].sin.sin_addr), ips->ips[ips->num-i].pnn);
		} else {
			printf("%s %d\n", inet_ntoa(ips->ips[ips->num-i].sin.sin_addr), ips->ips[ips->num-i].pnn);
		}
	}

	talloc_free(tmp_ctx);
	return 0;
}

/*
  display pid of a ctdb daemon
 */
static int control_getpid(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t pid;
	int ret;

	ret = ctdb_ctrl_getpid(ctdb, TIMELIMIT(), options.pnn, &pid);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get daemon pid from node %u\n", options.pnn));
		return ret;
	}
	printf("Pid:%d\n", pid);

	return 0;
}

/*
  disable a remote node
 */
static int control_disable(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_ctrl_modflags(ctdb, TIMELIMIT(), options.pnn, NODE_FLAGS_PERMANENTLY_DISABLED, 0);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to disable node %u\n", options.pnn));
		return ret;
	}

	return 0;
}

/*
  enable a disabled remote node
 */
static int control_enable(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_ctrl_modflags(ctdb, TIMELIMIT(), options.pnn, 0, NODE_FLAGS_PERMANENTLY_DISABLED);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to enable node %u\n", options.pnn));
		return ret;
	}

	return 0;
}

/*
  ban a node from the cluster
 */
static int control_ban(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct ctdb_ban_info b;
	TDB_DATA data;
	uint32_t ban_time;

	if (argc < 1) {
		usage();
	}

	ban_time = strtoul(argv[0], NULL, 0);

	b.pnn = options.pnn;
	b.ban_time = ban_time;

	data.dptr = (uint8_t *)&b;
	data.dsize = sizeof(b);

	ret = ctdb_send_message(ctdb, options.pnn, CTDB_SRVID_BAN_NODE, data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to ban node %u\n", options.pnn));
		return -1;
	}
	
	return 0;
}


/*
  unban a node from the cluster
 */
static int control_unban(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	TDB_DATA data;

	data.dptr = (uint8_t *)&options.pnn;
	data.dsize = sizeof(uint32_t);

	ret = ctdb_send_message(ctdb, options.pnn, CTDB_SRVID_UNBAN_NODE, data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to to unban node %u\n", options.pnn));
		return -1;
	}
	
	return 0;
}


/*
  shutdown a daemon
 */
static int control_shutdown(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_ctrl_shutdown(ctdb, TIMELIMIT(), options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to shutdown node %u\n", options.pnn));
		return ret;
	}

	return 0;
}

/*
  trigger a recovery
 */
static int control_recover(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_ctrl_freeze(ctdb, TIMELIMIT(), options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to freeze node\n"));
		return ret;
	}

	ret = ctdb_ctrl_setrecmode(ctdb, TIMELIMIT(), options.pnn, CTDB_RECOVERY_ACTIVE);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to set recovery mode\n"));
		return ret;
	}

	return 0;
}


/*
  display monitoring mode of a remote node
 */
static int control_getmonmode(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t monmode;
	int ret;

	ret = ctdb_ctrl_getmonmode(ctdb, TIMELIMIT(), options.pnn, &monmode);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get monmode from node %u\n", options.pnn));
		return ret;
	}
	printf("Monitoring mode:%s (%d)\n",monmode==CTDB_MONITORING_ACTIVE?"ACTIVE":"DISABLED",monmode);

	return 0;
}


/*
  disable monitoring on a  node
 */
static int control_disable_monmode(struct ctdb_context *ctdb, int argc, const char **argv)
{
	
	int ret;

	ret = ctdb_ctrl_disable_monmode(ctdb, TIMELIMIT(), options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to disable monmode on node %u\n", options.pnn));
		return ret;
	}
	printf("Monitoring mode:%s\n","DISABLED");

	return 0;
}

/*
  enable monitoring on a  node
 */
static int control_enable_monmode(struct ctdb_context *ctdb, int argc, const char **argv)
{
	
	int ret;

	ret = ctdb_ctrl_enable_monmode(ctdb, TIMELIMIT(), options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to enable monmode on node %u\n", options.pnn));
		return ret;
	}
	printf("Monitoring mode:%s\n","ACTIVE");

	return 0;
}

/*
  display remote list of keys/data for a db
 */
static int control_catdb(struct ctdb_context *ctdb, int argc, const char **argv)
{
	const char *db_name;
	struct ctdb_db_context *ctdb_db;
	int ret;

	if (argc < 1) {
		usage();
	}

	db_name = argv[0];
	ctdb_db = ctdb_attach(ctdb, db_name, false);

	if (ctdb_db == NULL) {
		DEBUG(DEBUG_ERR,("Unable to attach to database '%s'\n", db_name));
		return -1;
	}

	/* traverse and dump the cluster tdb */
	ret = ctdb_dump_db(ctdb_db, stdout);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to dump database\n"));
		return -1;
	}
	talloc_free(ctdb_db);

	printf("Dumped %d records\n", ret);
	return 0;
}


/*
  display a list of the databases on a remote ctdb
 */
static int control_getdbmap(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret;
	struct ctdb_dbid_map *dbmap=NULL;

	ret = ctdb_ctrl_getdbmap(ctdb, TIMELIMIT(), options.pnn, ctdb, &dbmap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get dbids from node %u\n", options.pnn));
		return ret;
	}

	printf("Number of databases:%d\n", dbmap->num);
	for(i=0;i<dbmap->num;i++){
		const char *path;
		const char *name;
		bool persistent;

		ctdb_ctrl_getdbpath(ctdb, TIMELIMIT(), options.pnn, dbmap->dbs[i].dbid, ctdb, &path);
		ctdb_ctrl_getdbname(ctdb, TIMELIMIT(), options.pnn, dbmap->dbs[i].dbid, ctdb, &name);
		persistent = dbmap->dbs[i].persistent;
		printf("dbid:0x%08x name:%s path:%s %s\n", dbmap->dbs[i].dbid, name, 
		       path, persistent?"PERSISTENT":"");
	}

	return 0;
}

/*
  get the filename of the reclock file
 */
static int control_getreclock(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret, fd;
	const char *reclock;
	struct ctdb_node_map *nodemap=NULL;
	char *pnnfile;

	ret = ctdb_ctrl_getreclock(ctdb, TIMELIMIT(), options.pnn, ctdb, &reclock);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get reclock file from node %u\n", options.pnn));
		return ret;
	}

	ret = ctdb_ctrl_getnodemap(ctdb, TIMELIMIT(), options.pnn, ctdb, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get nodemap from node %u\n", options.pnn));
		return ret;
	}


	pnnfile = talloc_asprintf(ctdb, "%s.pnn", reclock);
	CTDB_NO_MEMORY(ctdb, pnnfile);

	fd = open(pnnfile, O_RDONLY);
	if (fd == -1) {
		DEBUG(DEBUG_CRIT,(__location__ " Failed to open reclock pnn file %s - (%s)\n", 
			 pnnfile, strerror(errno)));
		exit(10);
	}


	printf("Reclock file : %s\n", reclock);
	for (i=0; i<nodemap->num; i++) {
		int count;

		count = ctdb_read_pnn_lock(fd, nodemap->nodes[i].pnn);

		printf("pnn:%d %-16s", nodemap->nodes[i].pnn,
		       inet_ntoa(nodemap->nodes[i].sin.sin_addr));
		if (count == -1) {
			printf(" NOT ACTIVE\n");
		} else {
			printf(" ACTIVE with %d connections\n", count);
		}
	}

	close(fd);
	return 0;
}


/*
  check if the local node is recmaster or not
  it will return 1 if this node is the recmaster and 0 if it is not
  or if the local ctdb daemon could not be contacted
 */
static int control_isnotrecmaster(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t mypnn, recmaster;
	int ret;

	mypnn = ctdb_ctrl_getpnn(ctdb, TIMELIMIT(), options.pnn);
	if (mypnn == -1) {
		printf("Failed to get pnn of node\n");
		return 1;
	}

	ret = ctdb_ctrl_getrecmaster(ctdb, ctdb, TIMELIMIT(), options.pnn, &recmaster);
	if (ret != 0) {
		printf("Failed to get the recmaster\n");
		return 1;
	}

	if (recmaster != mypnn) {
		printf("this node is not the recmaster\n");
		return 1;
	}

	printf("this node is the recmaster\n");
	return 0;
}

/*
  ping a node
 */
static int control_ping(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	struct timeval tv = timeval_current();
	ret = ctdb_ctrl_ping(ctdb, options.pnn);
	if (ret == -1) {
		printf("Unable to get ping response from node %u\n", options.pnn);
	} else {
		printf("response from %u time=%.6f sec  (%d clients)\n", 
		       options.pnn, timeval_elapsed(&tv), ret);
	}
	return 0;
}


/*
  get a tunable
 */
static int control_getvar(struct ctdb_context *ctdb, int argc, const char **argv)
{
	const char *name;
	uint32_t value;
	int ret;

	if (argc < 1) {
		usage();
	}

	name = argv[0];
	ret = ctdb_ctrl_get_tunable(ctdb, TIMELIMIT(), options.pnn, name, &value);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to get tunable variable '%s'\n", name));
		return -1;
	}

	printf("%-19s = %u\n", name, value);
	return 0;
}

/*
  set a tunable
 */
static int control_setvar(struct ctdb_context *ctdb, int argc, const char **argv)
{
	const char *name;
	uint32_t value;
	int ret;

	if (argc < 2) {
		usage();
	}

	name = argv[0];
	value = strtoul(argv[1], NULL, 0);

	ret = ctdb_ctrl_set_tunable(ctdb, TIMELIMIT(), options.pnn, name, value);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to set tunable variable '%s'\n", name));
		return -1;
	}
	return 0;
}

/*
  list all tunables
 */
static int control_listvars(struct ctdb_context *ctdb, int argc, const char **argv)
{
	uint32_t count;
	const char **list;
	int ret, i;

	ret = ctdb_ctrl_list_tunables(ctdb, TIMELIMIT(), options.pnn, ctdb, &list, &count);
	if (ret == -1) {
		DEBUG(DEBUG_ERR, ("Unable to list tunable variables\n"));
		return -1;
	}

	for (i=0;i<count;i++) {
		control_getvar(ctdb, 1, &list[i]);
	}

	talloc_free(list);
	
	return 0;
}

static struct {
	int32_t	level;
	const char *description;
} debug_levels[] = {
	{DEBUG_EMERG,	"EMERG"},
	{DEBUG_ALERT,	"ALERT"},
	{DEBUG_CRIT,	"CRIT"},
	{DEBUG_ERR,	"ERR"},
	{DEBUG_WARNING,	"WARNING"},
	{DEBUG_NOTICE,	"NOTICE"},
	{DEBUG_INFO,	"INFO"},
	{DEBUG_DEBUG,	"DEBUG"}
};

static const char *get_debug_by_level(int32_t level)
{
	int i;

	for (i=0;i<ARRAY_SIZE(debug_levels);i++) {
		if (debug_levels[i].level == level) {
			return debug_levels[i].description;
		}
	}
	return "Unknown";
}

static int32_t get_debug_by_desc(const char *desc)
{
	int i;

	for (i=0;i<ARRAY_SIZE(debug_levels);i++) {
		if (!strcmp(debug_levels[i].description, desc)) {
			return debug_levels[i].level;
		}
	}
	return DEBUG_ERR;
}

/*
  display debug level on a node
 */
static int control_getdebug(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	int32_t level;

	ret = ctdb_ctrl_get_debuglevel(ctdb, options.pnn, &level);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get debuglevel response from node %u\n", options.pnn));
		return ret;
	} else {
		if (options.machinereadable){
			printf(":Name:Level:\n");
			printf(":%s:%d:\n",get_debug_by_level(level),level);
		} else {
			printf("Node %u is at debug level %s (%u)\n", options.pnn, get_debug_by_level(level), level);
		}
	}
	return 0;
}


/*
  set debug level on a node or all nodes
 */
static int control_setdebug(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	uint32_t level;

	if (argc < 1) {
		usage();
	}

	if (isalpha(argv[0][0])) { 
		level = get_debug_by_desc(argv[0]);
	} else {
		level = strtoul(argv[0], NULL, 0);
	}

	ret = ctdb_ctrl_set_debuglevel(ctdb, options.pnn, level);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to set debug level on node %u\n", options.pnn));
	}
	return 0;
}


/*
  freeze a node
 */
static int control_freeze(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_ctrl_freeze(ctdb, TIMELIMIT(), options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to freeze node %u\n", options.pnn));
	}		
	return 0;
}

/*
  thaw a node
 */
static int control_thaw(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;

	ret = ctdb_ctrl_thaw(ctdb, TIMELIMIT(), options.pnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to thaw node %u\n", options.pnn));
	}		
	return 0;
}


/*
  attach to a database
 */
static int control_attach(struct ctdb_context *ctdb, int argc, const char **argv)
{
	const char *db_name;
	struct ctdb_db_context *ctdb_db;

	if (argc < 1) {
		usage();
	}
	db_name = argv[0];

	ctdb_db = ctdb_attach(ctdb, db_name, false);
	if (ctdb_db == NULL) {
		DEBUG(DEBUG_ERR,("Unable to attach to database '%s'\n", db_name));
		return -1;
	}

	return 0;
}

/*
  dump memory usage
 */
static int control_dumpmemory(struct ctdb_context *ctdb, int argc, const char **argv)
{
	TDB_DATA data;
	int ret;
	int32_t res;
	char *errmsg;
	TALLOC_CTX *tmp_ctx = talloc_new(ctdb);
	ret = ctdb_control(ctdb, options.pnn, 0, CTDB_CONTROL_DUMP_MEMORY,
			   0, tdb_null, tmp_ctx, &data, &res, NULL, &errmsg);
	if (ret != 0 || res != 0) {
		DEBUG(DEBUG_ERR,("Failed to dump memory - %s\n", errmsg));
		talloc_free(tmp_ctx);
		return -1;
	}
	write(1, data.dptr, data.dsize);
	talloc_free(tmp_ctx);
	return 0;
}

/*
  handler for memory dumps
*/
static void mem_dump_handler(struct ctdb_context *ctdb, uint64_t srvid, 
			     TDB_DATA data, void *private_data)
{
	write(1, data.dptr, data.dsize);
	exit(0);
}

/*
  dump memory usage on the recovery daemon
 */
static int control_rddumpmemory(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int ret;
	TDB_DATA data;
	struct rd_memdump_reply rd;

	rd.pnn = ctdb_ctrl_getpnn(ctdb, TIMELIMIT(), CTDB_CURRENT_NODE);
	if (rd.pnn == -1) {
		DEBUG(DEBUG_ERR, ("Failed to get pnn of local node\n"));
		return -1;
	}
	rd.srvid = getpid();

	/* register a message port for receiveing the reply so that we
	   can receive the reply
	*/
	ctdb_set_message_handler(ctdb, rd.srvid, mem_dump_handler, NULL);


	data.dptr = (uint8_t *)&rd;
	data.dsize = sizeof(rd);

	ret = ctdb_send_message(ctdb, options.pnn, CTDB_SRVID_MEM_DUMP, data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to send memdump request message to %u\n", options.pnn));
		return -1;
	}

	/* this loop will terminate when we have received the reply */
	while (1) {	
		event_loop_once(ctdb->ev);
	}

	return 0;
}

/*
  list all nodes in the cluster
 */
static int control_listnodes(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret;
	struct ctdb_node_map *nodemap=NULL;

	ret = ctdb_ctrl_getnodemap(ctdb, TIMELIMIT(), options.pnn, ctdb, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get nodemap from node %u\n", options.pnn));
		return ret;
	}

	for(i=0;i<nodemap->num;i++){
		printf("%s\n", inet_ntoa(nodemap->nodes[i].sin.sin_addr));
	}

	return 0;
}

/*
  reload the nodes file on the local node
 */
static int control_reload_nodes_file(struct ctdb_context *ctdb, int argc, const char **argv)
{
	int i, ret;
	int mypnn;
	struct ctdb_node_map *nodemap=NULL;

	mypnn = ctdb_ctrl_getpnn(ctdb, TIMELIMIT(), CTDB_CURRENT_NODE);
	if (mypnn == -1) {
		DEBUG(DEBUG_ERR, ("Failed to read pnn of local node\n"));
		return -1;
	}

	ret = ctdb_ctrl_getnodemap(ctdb, TIMELIMIT(), CTDB_CURRENT_NODE, ctdb, &nodemap);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Unable to get nodemap from local node\n"));
		return ret;
	}

	/* reload the nodes file on all remote nodes */
	for (i=0;i<nodemap->num;i++) {
		if (nodemap->nodes[i].pnn == mypnn) {
			continue;
		}
		DEBUG(DEBUG_NOTICE, ("Reloading nodes file on node %u\n", nodemap->nodes[i].pnn));
		ret = ctdb_ctrl_reload_nodes_file(ctdb, TIMELIMIT(),
			nodemap->nodes[i].pnn);
		if (ret != 0) {
			DEBUG(DEBUG_ERR, ("ERROR: Failed to reload nodes file on node %u. You MUST fix that node manually!\n", nodemap->nodes[i].pnn));
		}
	}

	/* reload the nodes file on the local node */
	DEBUG(DEBUG_NOTICE, ("Reloading nodes file on node %u\n", mypnn));
	ret = ctdb_ctrl_reload_nodes_file(ctdb, TIMELIMIT(), mypnn);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("ERROR: Failed to reload nodes file on node %u. You MUST fix that node manually!\n", mypnn));
	}

	return 0;
}


static const struct {
	const char *name;
	int (*fn)(struct ctdb_context *, int, const char **);
	bool auto_all;
	const char *msg;
	const char *args;
} ctdb_commands[] = {
	{ "status",          control_status,            true,  "show node status" },
	{ "uptime",          control_uptime,            true,  "show node uptime" },
	{ "ping",            control_ping,              true,  "ping all nodes" },
	{ "getvar",          control_getvar,            true,  "get a tunable variable",               "<name>"},
	{ "setvar",          control_setvar,            true,  "set a tunable variable",               "<name> <value>"},
	{ "listvars",        control_listvars,          true,  "list tunable variables"},
	{ "statistics",      control_statistics,        false, "show statistics" },
	{ "statisticsreset", control_statistics_reset,  true,  "reset statistics"},
	{ "ip",              control_ip,                true,  "show which public ip's that ctdb manages" },
	{ "process-exists",  control_process_exists,    true,  "check if a process exists on a node",  "<pid>"},
	{ "getdbmap",        control_getdbmap,          true,  "show the database map" },
	{ "catdb",           control_catdb,             true,  "dump a database" ,                     "<dbname>"},
	{ "getmonmode",      control_getmonmode,        true,  "show monitoring mode" },
	{ "disablemonitor",      control_disable_monmode,        true,  "set monitoring mode to DISABLE" },
	{ "enablemonitor",      control_enable_monmode,        true,  "set monitoring mode to ACTIVE" },
	{ "setdebug",        control_setdebug,          true,  "set debug level",                      "<EMERG|ALERT|CRIT|ERR|WARNING|NOTICE|INFO|DEBUG>" },
	{ "getdebug",        control_getdebug,          true,  "get debug level" },
	{ "attach",          control_attach,            true,  "attach to a database",                 "<dbname>" },
	{ "dumpmemory",      control_dumpmemory,        true,  "dump memory map to stdout" },
	{ "rddumpmemory",    control_rddumpmemory,      true,  "dump memory map from the recovery daemon to stdout" },
	{ "getpid",          control_getpid,            true,  "get ctdbd process ID" },
	{ "disable",         control_disable,           true,  "disable a nodes public IP" },
	{ "enable",          control_enable,            true,  "enable a nodes public IP" },
	{ "ban",             control_ban,               true,  "ban a node from the cluster",          "<bantime|0>"},
	{ "unban",           control_unban,             true,  "unban a node from the cluster" },
	{ "shutdown",        control_shutdown,          true,  "shutdown ctdbd" },
	{ "recover",         control_recover,           true,  "force recovery" },
	{ "freeze",          control_freeze,            true,  "freeze all databases" },
	{ "thaw",            control_thaw,              true,  "thaw all databases" },
	{ "isnotrecmaster",  control_isnotrecmaster,    false,  "check if the local node is recmaster or not" },
	{ "killtcp",         kill_tcp,                  false, "kill a tcp connection.", "<srcip:port> <dstip:port>" },
	{ "gratiousarp",     control_gratious_arp,      false, "send a gratious arp", "<ip> <interface>" },
	{ "tickle",          tickle_tcp,                false, "send a tcp tickle ack", "<srcip:port> <dstip:port>" },
	{ "gettickles",      control_get_tickles,       false, "get the list of tickles registered for this ip", "<ip>" },

	{ "regsrvid",        regsrvid,			false, "register a server id", "<pnn> <type> <id>" },
	{ "unregsrvid",      unregsrvid,		false, "unregister a server id", "<pnn> <type> <id>" },
	{ "chksrvid",        chksrvid,			false, "check if a server id exists", "<pnn> <type> <id>" },
	{ "getsrvids",       getsrvids,			false, "get a list of all server ids"},
	{ "vacuum",          ctdb_vacuum,		false, "vacuum the databases of empty records", "[max_records]"},
	{ "repack",          ctdb_repack,		false, "repack all databases", "[max_freelist]"},
	{ "listnodes",       control_listnodes,		false, "list all nodes in the cluster"},
	{ "reloadnodes",     control_reload_nodes_file,		false, "reload the nodes file and restart the transport on all nodes"},
	{ "getreclock",      control_getreclock,        false,  "get the path to the reclock file" },
	{ "moveip",          control_moveip,		false, "move/failover an ip address to another node", "<ip> <node>"},
	{ "addip",           control_addip,		false, "add a ip address to a node", "<ip/mask> <iface>"},
	{ "delip",           control_delip,		false, "delete an ip address from a node", "<ip>"},
};

/*
  show usage message
 */
static void usage(void)
{
	int i;
	printf(
"Usage: ctdb [options] <control>\n" \
"Options:\n" \
"   -n <node>          choose node number, or 'all' (defaults to local node)\n"
"   -Y                 generate machinereadable output\n"
"   -t <timelimit>     set timelimit for control in seconds (default %u)\n", options.timelimit);
	printf("Controls:\n");
	for (i=0;i<ARRAY_SIZE(ctdb_commands);i++) {
		printf("  %-15s %-27s  %s\n", 
		       ctdb_commands[i].name, 
		       ctdb_commands[i].args?ctdb_commands[i].args:"",
		       ctdb_commands[i].msg);
	}
	exit(1);
}


static void ctdb_alarm(int sig)
{
	printf("Maximum runtime exceeded - exiting\n");
	_exit(0);
}

/*
  main program
*/
int main(int argc, const char *argv[])
{
	struct ctdb_context *ctdb;
	char *nodestring = NULL;
	struct poptOption popt_options[] = {
		POPT_AUTOHELP
		POPT_CTDB_CMDLINE
		{ "timelimit", 't', POPT_ARG_INT, &options.timelimit, 0, "timelimit", "integer" },
		{ "node",      'n', POPT_ARG_STRING, &nodestring, 0, "node", "integer|all" },
		{ "machinereadable", 'Y', POPT_ARG_NONE, &options.machinereadable, 0, "enable machinereadable output", NULL },
		{ "maxruntime", 'T', POPT_ARG_INT, &options.maxruntime, 0, "die if runtime exceeds this limit (in seconds)", "integer" },
		POPT_TABLEEND
	};
	int opt;
	const char **extra_argv;
	int extra_argc = 0;
	int ret=-1, i;
	poptContext pc;
	struct event_context *ev;
	const char *control;

	setlinebuf(stdout);
	
	/* set some defaults */
	options.maxruntime = 0;
	options.timelimit = 3;
	options.pnn = CTDB_CURRENT_NODE;

	pc = poptGetContext(argv[0], argc, argv, popt_options, POPT_CONTEXT_KEEP_FIRST);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		default:
			DEBUG(DEBUG_ERR, ("Invalid option %s: %s\n", 
				poptBadOption(pc, 0), poptStrerror(opt)));
			exit(1);
		}
	}

	/* setup the remaining options for the main program to use */
	extra_argv = poptGetArgs(pc);
	if (extra_argv) {
		extra_argv++;
		while (extra_argv[extra_argc]) extra_argc++;
	}

	if (extra_argc < 1) {
		usage();
	}

	if (options.maxruntime != 0) {
		signal(SIGALRM, ctdb_alarm);
		alarm(options.maxruntime);
	}

	/* setup the node number to contact */
	if (nodestring != NULL) {
		if (strcmp(nodestring, "all") == 0) {
			options.pnn = CTDB_BROADCAST_ALL;
		} else {
			options.pnn = strtoul(nodestring, NULL, 0);
		}
	}

	control = extra_argv[0];

	ev = event_context_init(NULL);

	/* initialise ctdb */
	ctdb = ctdb_cmdline_client(ev);
	if (ctdb == NULL) {
		DEBUG(DEBUG_ERR, ("Failed to init ctdb\n"));
		exit(1);
	}

	for (i=0;i<ARRAY_SIZE(ctdb_commands);i++) {
		if (strcmp(control, ctdb_commands[i].name) == 0) {
			int j;

			if (options.pnn == CTDB_CURRENT_NODE) {
				int pnn;
				pnn = ctdb_ctrl_getpnn(ctdb, TIMELIMIT(), options.pnn);		
				if (pnn == -1) {
					return -1;
				}
				options.pnn = pnn;
			}

			if (ctdb_commands[i].auto_all && 
			    options.pnn == CTDB_BROADCAST_ALL) {
				uint32_t *nodes;
				uint32_t num_nodes;
				ret = 0;

				nodes = ctdb_get_connected_nodes(ctdb, TIMELIMIT(), ctdb, &num_nodes);
				CTDB_NO_MEMORY(ctdb, nodes);
	
				for (j=0;j<num_nodes;j++) {
					options.pnn = nodes[j];
					ret |= ctdb_commands[i].fn(ctdb, extra_argc-1, extra_argv+1);
				}
				talloc_free(nodes);
			} else {
				ret = ctdb_commands[i].fn(ctdb, extra_argc-1, extra_argv+1);
			}
			break;
		}
	}

	if (i == ARRAY_SIZE(ctdb_commands)) {
		DEBUG(DEBUG_ERR, ("Unknown control '%s'\n", control));
		exit(1);
	}

	return ret;
}
