/*
 * Example program to demonstrate the libctdb api
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This program needs to be linked with libtdb and libctdb
 * (You need these packages installed: libtdb libtdb-devel
 *  ctdb and ctdb-devel)
 *
 * This program can then be compiled using
 *    gcc -o tst tst.c -ltdb -lctdb
 *
 *
 */
#include <stdio.h>
#include <stdint.h>
#include <poll.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <err.h>
#include <stdbool.h>
#include <tdb.h>
#include <ctdb.h>

TDB_DATA key;

void msg_h(struct ctdb_connection *ctdb, uint64_t srvid, TDB_DATA data, void *private_data)
{
	printf("Message received on port %d : %s\n", (int)srvid, data.dptr);
}

static void pnn_cb(struct ctdb_connection *ctdb,
		   struct ctdb_request *req, void *private)
{
	int status;
	uint32_t pnn;

	status = ctdb_getpnn_recv(req, &pnn);
	ctdb_request_free(req);
	if (status != 0) {
		printf("Error reading PNN\n");
		return;
	}
	printf("status:%d pnn:%d\n", status, pnn);
}

static void rm_cb(struct ctdb_connection *ctdb,
		  struct ctdb_request *req, void *private)
{
	int status;
	uint32_t rm;

	status = ctdb_getrecmaster_recv(req, &rm);
	ctdb_request_free(req);
	if (status != 0) {
		printf("Error reading RECMASTER\n");
		return;
	}

	printf("GETRECMASTER ASYNC: status:%d recmaster:%d\n", status, rm);
}

/*
 * example on how to first read(non-existing recortds are implicitely created
 * on demand) a record and change it in the callback.
 * This forms the atom for the read-modify-write cycle.
 *
 * Pure read, or pure write are just special cases of this cycle.
 */
static void rrl_cb(struct ctdb_connection *ctdb,
		  struct ctdb_request *req, void *private)
{
	struct ctdb_lock *lock;
	TDB_DATA outdata;
	TDB_DATA data;
	char tmp[256];

	lock = ctdb_readrecordlock_recv(private, req, &outdata);
	if (!lock) {
		printf("rrl_cb returned error\n");
		return;
	}

	printf("rrl size:%d data:%.*s\n", outdata.dsize,
	       outdata.dsize, outdata.dptr);
	if (outdata.dsize == 0) {
		tmp[0] = 0;
	} else {
		strcpy(tmp, outdata.dptr);
	}
	strcat(tmp, "*");

	data.dptr  = tmp;
	data.dsize = strlen(tmp) + 1;
	ctdb_writerecord(lock, data);

	printf("Wrote new record : %s\n", tmp);

	ctdb_release_lock(lock);
}

static bool registered = false;
void message_handler_cb(struct ctdb_connection *ctdb,
			struct ctdb_request *req, void *private)
{
	if (ctdb_set_message_handler_recv(ctdb, req) != 0) {
		err(1, "registering message");
	}
	ctdb_request_free(req);
	printf("Message handler registered\n");
	registered = true;
}

int main(int argc, char *argv[])
{
	struct ctdb_connection *ctdb_connection;
	struct ctdb_request *handle;
	struct ctdb_db *ctdb_db_context;
	struct pollfd pfd;
	uint32_t recmaster;
	int ret;
	TDB_DATA msg;

	ctdb_connection = ctdb_connect("/tmp/ctdb.socket");
	if (!ctdb_connection)
		err(1, "Connecting to /tmp/ctdb.socket");

	pfd.fd = ctdb_get_fd(ctdb_connection);

	handle = ctdb_set_message_handler_send(ctdb_connection, 55, msg_h,
					       message_handler_cb, NULL);
	if (handle == NULL) {
		printf("Failed to register message port\n");
		exit(10);
	}

	/* Hack for testing: this makes sure registration goes out. */
	while (!registered) {
		ctdb_service(ctdb_connection, POLLIN|POLLOUT);
	}

	msg.dptr="HelloWorld";
	msg.dsize = strlen(msg.dptr);

	ret = ctdb_send_message(ctdb_connection, 0, 55, msg);
	if (ret != 0) {
		printf("Failed to send message. Aborting\n");
		exit(10);
	}

	handle = ctdb_getrecmaster_send(ctdb_connection, 0, rm_cb, NULL);
	if (handle == NULL) {
		printf("Failed to send get_recmaster control\n");
		exit(10);
	}

	ctdb_db_context = ctdb_attachdb(ctdb_connection, "test_test.tdb", 0, 0);
	if (!ctdb_db_context) {
		printf("Failed to attach to database\n");
		exit(10);
	}

	/*
	 * SYNC call with callback to read the recmaster
	 * calls the blocking sync function.
	 * Avoid this mode for performance critical tasks
	 */
	ret = ctdb_getrecmaster(ctdb_connection, CTDB_CURRENT_NODE, &recmaster);
	if (ret != 0) {
		printf("Failed to receive response to getrecmaster\n");
		exit(10);
	}
	printf("GETRECMASTER SYNC: status:%d recmaster:%d\n", ret, recmaster);


	handle = ctdb_getpnn_send(ctdb_connection, CTDB_CURRENT_NODE,
				  pnn_cb, NULL);
	if (handle == NULL) {
		printf("Failed to send get_pnn control\n");
		exit(10);
	}

	if (!ctdb_readrecordlock_send(ctdb_db_context, key, &handle,
				      rrl_cb, ctdb_db_context)) {
		printf("Failed to send READRECORDLOCK\n");
		exit(10);
	}
	if (handle) {
		printf("READRECORDLOCK is async\n");
	}
	for (;;) {

	  pfd.events = ctdb_which_events(ctdb_connection);
	  if (poll(&pfd, 1, -1) < 0) {
	    printf("Poll failed");
	    exit(10);
	  }
	  if (ctdb_service(ctdb_connection, pfd.revents) < 0) {
		  err(1, "Failed to service");
	  }
	}

	return 0;
}
