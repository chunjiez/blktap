/*
 * Copyright (c) 2016, Citrix Systems, Inc.
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *  3. Neither the name of the copyright holder nor the names of its 
 *     contributors may be used to endorse or promote products derived from 
 *     this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include "tapdisk.h"
#include "tapdisk-server.h"
#include "tapdisk-driver.h"
#include "tapdisk-interface.h"
#include "tapdisk-utils.h"
#include "tapdisk-fdreceiver.h"
#include "timeout-math.h"
#include "tapdisk-nbdserver.h"
#include "tapdisk-protocol-new.h"
#include "util.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define INFO(_f, _a...)            tlog_syslog(TLOG_INFO, "nbd: " _f, ##_a)
#define ERROR(_f, _a...)           tlog_syslog(TLOG_WARN, "nbd: " _f, ##_a)

#define N_PASSED_FDS 10
#define TAPDISK_NBDCLIENT_MAX_PATH_LEN 256

#define MAX_NBD_REQS TAPDISK_DATA_REQUESTS
#define NBD_TIMEOUT 30
#define RECV_BUFFER_SIZE 256

/*
 * We'll only ever have one nbdclient fd receiver per tapdisk process, so let's 
 * just store it here globally. We'll also keep track of the passed fds here
 * too.
 */

struct td_fdreceiver *fdreceiver = NULL;

struct tdnbd_passed_fd {
	char                    id[40];
	int                     fd;
} passed_fds[N_PASSED_FDS];

struct nbd_queued_io {
	char                   *buffer;
	int                     len;
	int                     so_far;
};

struct td_nbd_request {
	td_request_t            treq;
	struct nbd_request      nreq;
	int                     timeout_event;
	int                     fake;
	struct nbd_queued_io    header;
	struct nbd_queued_io    body;     /* in or out, depending on whether
					     type is read or write. */
	struct list_head        queue;
};

struct tdnbd_data
{
	int                     writer_event_id;
	struct list_head        sent_reqs;
	struct list_head        pending_reqs;
	struct list_head        free_reqs;
	struct td_nbd_request   requests[MAX_NBD_REQS];
	int                     nr_free_count;

	int                     reader_event_id;
	struct nbd_reply        current_reply;
	struct nbd_queued_io    cur_reply_qio;
	struct td_nbd_request  *curr_reply_req;

	int                     socket;
	/*
	 * TODO tapdisk can talk to an Internet socket or a UNIX domain socket.
	 * Try to group struct members accordingly e.g. in a union.
	 */
	struct sockaddr_in     *remote;
	struct sockaddr_un      remote_un;
	char                   *peer_ip;
	int                     port;
	char                   *name;

	int                     flags;
	int                     closed;
};

int global_id = 0;

static void disable_write_queue(struct tdnbd_data *prv);


/* -- fdreceiver bits and pieces -- */

static void
tdnbd_stash_passed_fd(int fd, char *msg, void *data) 
{
	int free_index = -1;
	int i;
	for (i = 0; i < N_PASSED_FDS; i++)
		/* Check for unused slot before attempting to compare
		 * names so that we never try to compare against the name
		 * of an unused slot */
		if (passed_fds[i].fd == -1 || strncmp(msg, passed_fds[i].id,
					sizeof(passed_fds[i].id) - 1) == 0) {
			free_index = i;
			break;
		}

	if (free_index == -1) {
		ERROR("Error - more than %d fds passed! cannot stash another",
				N_PASSED_FDS);
		close(fd);
		return;
	}

	/* There exists a possibility that the FD we are replacing is still
	 * open. Unconditionally close it here to avoid leaking FDs. Do not
	 * care about errors from close(). */
	if (passed_fds[free_index].fd > -1)
		close(passed_fds[free_index].fd);

	passed_fds[free_index].fd = fd;
	safe_strncpy(passed_fds[free_index].id, msg,
		     sizeof(passed_fds[free_index].id));
}

static int
tdnbd_retrieve_passed_fd(const char *name)
{
	int fd, i;

	for (i = 0; i < N_PASSED_FDS; i++) {
		if (strncmp(name, passed_fds[i].id,
					sizeof(passed_fds[i].id) - 1) == 0) {
			fd = passed_fds[i].fd;
			passed_fds[i].fd = -1;
			return fd;
		}
	}

	ERROR("Couldn't find the fd named: %s", name);

	return -1;
}

void
tdnbd_fdreceiver_start()
{
	char fdreceiver_path[TAPDISK_NBDCLIENT_MAX_PATH_LEN];
	int i;

	/* initialise the passed fds list */
	for (i = 0; i < N_PASSED_FDS; i++)
		passed_fds[i].fd = -1;

	snprintf(fdreceiver_path, TAPDISK_NBDCLIENT_MAX_PATH_LEN,
			"%s%d", TAPDISK_NBDCLIENT_LISTEN_SOCK_PATH, getpid());

	fdreceiver = td_fdreceiver_start(fdreceiver_path,
			tdnbd_stash_passed_fd, NULL);

}

void
tdnbd_fdreceiver_stop()
{
	if (fdreceiver)
		td_fdreceiver_stop(fdreceiver);
}

static void
__cancel_req(int i, struct td_nbd_request *pos, int e)
{
	char handle[9];
	memcpy(handle, pos->nreq.handle, 8);
	handle[8] = 0;
	INFO("Entry %d: handle='%s' type=%d, len=%d: %s",
	     i, handle, ntohl(pos->nreq.type), ntohl(pos->nreq.len), strerror(e));

	if (pos->timeout_event >= 0) {
		tapdisk_server_unregister_event(pos->timeout_event);
		pos->timeout_event = -1;
	}

	td_complete_request(pos->treq, e);
}

static void
tdnbd_disable(struct tdnbd_data *prv, int e)
{
	struct td_nbd_request *pos, *q;
	int i = 0;

	INFO("NBD client full-disable");

	tapdisk_server_unregister_event(prv->writer_event_id);
	tapdisk_server_unregister_event(prv->reader_event_id);

	INFO("NBD client cancelling sent reqs");
	list_for_each_entry_safe(pos, q, &prv->sent_reqs, queue)
		__cancel_req(i++, pos, e);

	INFO("NBD client cancelling pending reqs");
	list_for_each_entry_safe(pos, q, &prv->pending_reqs, queue)
		__cancel_req(i++, pos, e);

	INFO("Setting closed");
	prv->closed = 3;
}

/* NBD writer queue */

/* Return code: how much is left to write, or a negative error code */
static int
tdnbd_write_some(int fd, struct nbd_queued_io *data) 
{
	int left = data->len - data->so_far;
	int rc;
	char *code;

	while (left > 0) {
		rc = send(fd, data->buffer + data->so_far, left, 0);

		if (rc == -1) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
				return left;

			code = strerror(errno);
			ERROR("Bad return code %d from send (%s)", rc, 
					(code == 0 ? "unknown" : code));
			return rc;
		}

		if (rc == 0) {
			ERROR("Server shutdown prematurely in write_some");
			return -1;
		}

		left -= rc;
		data->so_far += rc;
	}

	return left;
}

static int
tdnbd_read_some(int fd, struct nbd_queued_io *data)
{
	int left = data->len - data->so_far;
	int rc;
	char *code;

	while (left > 0) {
		rc = recv(fd, data->buffer + data->so_far, left, 0);

		if (rc == -1) {

			if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
				return left;

			code = strerror(errno);
			ERROR("Bad return code %d from send (%s)", rc,
					(code == 0 ? "unknown" : code));
			return rc;
		}

		if (rc == 0) {
			ERROR("Server shutdown prematurely in read_some");
			return -1;
		}

		data->so_far += rc;
		left -= rc;
	}

	return left;
}

static void
tdnbd_writer_cb(event_id_t eb, char mode, void *data)
{
	struct td_nbd_request *pos, *q;
	struct tdnbd_data *prv = data;

	list_for_each_entry_safe(pos, q, &prv->pending_reqs, queue) {
		if (tdnbd_write_some(prv->socket, &pos->header) > 0)
			return;

		if (ntohl(pos->nreq.type) == TAPDISK_NBD_CMD_WRITE) {
			if (tdnbd_write_some(prv->socket, &pos->body) > 0)
				return;
		}

		if (ntohl(pos->nreq.type) == TAPDISK_NBD_CMD_DISC) {
			INFO("sent close request");
			/*
			 * We don't expect a response from a DISC, so move the
			 * request back onto the free list
			 */
			list_move(&pos->queue, &prv->free_reqs);
			prv->nr_free_count++;
			prv->closed = 2;
		} else {
			list_move(&pos->queue, &prv->sent_reqs);
		}
	}

	/* If we're here, we've written everything */

	disable_write_queue(prv);

	if (prv->closed == 2)
		tdnbd_disable(prv, EIO);

	return;
}

static int
enable_write_queue(struct tdnbd_data *prv)
{
	if (prv->writer_event_id >= 0) 
		return 0;

	prv->writer_event_id = 
		tapdisk_server_register_event(SCHEDULER_POLL_WRITE_FD,
				prv->socket,
				TV_ZERO,
				tdnbd_writer_cb,
				prv);

	return prv->writer_event_id;
}

static void
disable_write_queue(struct tdnbd_data *prv)
{
	if (prv->writer_event_id < 0)
		return;

	tapdisk_server_unregister_event(prv->writer_event_id);

	prv->writer_event_id = -1;
}

static int
tdnbd_queue_request(struct tdnbd_data *prv, int type, uint64_t offset,
		char *buffer, uint32_t length, td_request_t treq, int fake)
{
	if (prv->nr_free_count == 0) 
		return -EBUSY;

	if (prv->closed == 3) {
		td_complete_request(treq, -ETIMEDOUT);
		return -ETIMEDOUT;
	}

	struct td_nbd_request *req = list_entry(prv->free_reqs.next,
			struct td_nbd_request, queue);

	/* fill in the request */

	req->treq = treq;
	int id = global_id++;
	snprintf(req->nreq.handle, 8, "td%05x", id % 0xffff);

	/* Don't time the NBD requests out */
	req->timeout_event = -1;

	req->nreq.magic = htonl(NBD_REQUEST_MAGIC);
	req->nreq.type = htonl(type);
	req->nreq.from = htonll(offset);
	req->nreq.len = htonl(length);
	req->header.buffer = (char *)&req->nreq;
	req->header.len = sizeof(req->nreq);
	req->header.so_far = 0;
	req->body.buffer = buffer;
	req->body.len = length;
	req->body.so_far = 0;
	req->fake = fake;

	list_move_tail(&req->queue, &prv->pending_reqs);
	prv->nr_free_count--;

	if (prv->writer_event_id < 0)
		enable_write_queue(prv);

	return 0;
}

/* NBD Reader callback */

static void
tdnbd_reader_cb(event_id_t eb, char mode, void *data)
{
	char handle[9];
	int do_disable = 0;

	/* Check to see if we're in the middle of reading a response already */
	struct tdnbd_data *prv = data;
	int rc = tdnbd_read_some(prv->socket, &prv->cur_reply_qio);

	if (rc < 0) {
		ERROR("Error reading reply header: %d", rc);
		tdnbd_disable(prv, EIO);
		return;
	}

	if (rc > 0)
		return; /* need more data */

	/* Got a header. */
	if (prv->current_reply.error != 0) {
		ERROR("Error in reply: %d", prv->current_reply.error);
		tdnbd_disable(prv, EIO);
		return;
	}

	/* Have we found the request yet? */
	if (prv->curr_reply_req == NULL) {
		struct td_nbd_request *pos, *q;
		list_for_each_entry_safe(pos, q, &prv->sent_reqs, queue) {
			if (memcmp(pos->nreq.handle, prv->current_reply.handle,
						8) == 0) {
				prv->curr_reply_req = pos;
				break;
			}
		}

		if (prv->curr_reply_req == NULL) {
			memcpy(handle, prv->current_reply.handle, 8);
			handle[8] = 0;

			ERROR("Couldn't find request corresponding to reply "
					"(reply handle='%s')", handle);
			tdnbd_disable(prv, EIO);
			return;
		}
	}

	switch(ntohl(prv->curr_reply_req->nreq.type)) {
	case TAPDISK_NBD_CMD_READ:
		rc = tdnbd_read_some(prv->socket,
				&prv->curr_reply_req->body);

		if (rc < 0) {
			ERROR("Error reading body of request: %d", rc);
			tdnbd_disable(prv, EIO);
			return;
		}

		if (rc > 0)
			return; /* need more data */

		td_complete_request(prv->curr_reply_req->treq, 0);

		break;
	case TAPDISK_NBD_CMD_WRITE:
		td_complete_request(prv->curr_reply_req->treq, 0);

		break;
	default:
		ERROR("Unhandled request response: %d",
				ntohl(prv->curr_reply_req->nreq.type));
		do_disable = 1;
		return;
	} 

	/* remove the state */
	list_move(&prv->curr_reply_req->queue, &prv->free_reqs);
	prv->nr_free_count++;

	prv->cur_reply_qio.so_far = 0;
	if (prv->curr_reply_req->timeout_event >= 0) {
		tapdisk_server_unregister_event(
				prv->curr_reply_req->timeout_event);
	}

	prv->curr_reply_req = NULL;

	/*
	 * NB: do this here otherwise we cancel the request that has just been 
	 * moved
	 */
	if (do_disable)
		tdnbd_disable(prv, EIO);
}

/* Wait a certain maximum amount of time for a socket to be readable and then
 * recv() some bytes from it if it is. Returns -ETIMEDOUT if the select times out,
 * otherwise -errno from whatever action failed.
 *
 * Otherwise, returns number of bytes read from the recv() (which could be 0)
 */
static int
tdnbd_wait_recv(int fd, void *buffer, size_t len, int flags)
{
	struct timeval select_tv;
	fd_set socks;
	int rc;

	FD_ZERO(&socks);
	FD_SET(fd, &socks);
	select_tv.tv_sec = 10;
	select_tv.tv_usec = 0;
	rc = TEMP_FAILURE_RETRY(select(fd + 1, &socks, NULL, NULL, &select_tv));
	if (rc < 0) return -errno;
	if (rc == 0) return -ETIMEDOUT;

	rc = TEMP_FAILURE_RETRY(recv(fd, buffer, len, flags));
	if (rc < 0) return -errno;
	return rc;
}

int 
negotiate_client_newstyle_options(int sock, td_driver_t *driver)
{
	int rc;
	struct nbd_new_option new_option;
	char exportname[] = NBD_FIXED_SINGLE_EXPORT;
	new_option.version = htobe64(NBD_OPT_MAGIC);
	new_option.optlen = htobe32(sizeof(exportname));
	new_option.option = htobe32( NBD_OPT_EXPORT_NAME);

	struct nbd_export_name_option_reply handshake_finish;
	static const size_t NO_ZERO_HANDSHAKE_FINISH_SIZE = 10;

	/* Send EXPORTNAME_NAME option request */
	rc = send_fully_or_fail(sock, &new_option, sizeof(new_option));
	if (rc < 0)
	{
		ERROR("Failed to send options to sock");
		goto errout;
	}
	/* Send exportname name */
	rc = send_fully_or_fail(sock, exportname, sizeof(exportname));
	if (rc < 0)
	{
		ERROR("Failed to send export name to sock");
		goto errout;
	}

	/* Collect the results in the handshake finished */
	rc = tdnbd_wait_recv(sock, &handshake_finish, NO_ZERO_HANDSHAKE_FINISH_SIZE, 0);
	if (rc < 0)
	{
		ERROR("Failed to read handshake from sock: %s", strerror(-rc));
		goto errout;
	}

	driver->info.size = be64toh(handshake_finish.exportsize) >> SECTOR_SHIFT;
	driver->info.sector_size = DEFAULT_SECTOR_SIZE;
	driver->info.info = 0;

	rc = fcntl(sock, F_SETFL, O_NONBLOCK);
	if (rc != 0) {
		ERROR("Could not set O_NONBLOCK flag");
		goto errout;
	}

	INFO("Successfully connected to New-style NBD server");

	return 0;

errout:
	close(sock);
	return -1;
}


static int
tdnbd_nbd_negotiate_old(struct tdnbd_data *prv, td_driver_t *driver)
{
	int rc;
	char buffer[RECV_BUFFER_SIZE];
	uint64_t size;
	uint32_t flags;
	int padbytes = 124;
	int sock = prv->socket;

	/*
	 * NBD OLD-style negotiation protocol:
	 *
	 * Server sends 'NBDMAGIC'
	 * then it sends 0x00420281861253L
	 * then it sends a 64 bit bigendian size <-- YOU ARE HERE
	 * then it sends a 32 bit bigendian flags
	 * then it sends 124 bytes of nothing
	 */

	/*
	 * We need to limit the time we spend in this function as we're still
	 * using blocking IO at this point
	 */

	rc = tdnbd_wait_recv(sock, &size, sizeof(size), 0);
	if (rc < 0) {
		ERROR("Error in nbd_negotiate: %s", strerror(-rc));
		goto errout;
	}
	if (rc < sizeof(size)) {
		ERROR("Short read in OLD negotiation(3) (%d)\n", rc);
		goto errout;
	}

	INFO("Got size: %"PRIu64"", ntohll(size));

	driver->info.size = ntohll(size) >> SECTOR_SHIFT;
	driver->info.sector_size = DEFAULT_SECTOR_SIZE;
	driver->info.info = 0;

	rc = tdnbd_wait_recv(sock, &flags, sizeof(flags), 0);
	if (rc < 0) {
		ERROR("Error in nbd_negotiate: %s", strerror(-rc));
		goto errout;
	}
	if (rc < sizeof(flags)) {
		ERROR("Short read in OLD negotiation(4) (%d)\n", rc);
		goto errout;
	}

	INFO("Got flags: %"PRIu32"", ntohl(flags));

	while (padbytes > 0) {
		rc = tdnbd_wait_recv(sock, buffer, padbytes, 0);
		if (rc < 0) {
			ERROR("Error in nbd_negotiate: %s", strerror(-rc));
			goto errout;
		}
		padbytes -= rc;
	}

	rc = fcntl(sock, F_SETFL, O_NONBLOCK);

	if (rc != 0) {
		ERROR("Could not set O_NONBLOCK flag");
		goto errout;
	}
	INFO("Successfully connected to Old-style NBD server");

	return 0;

errout:
	close(sock);
	return -1;
}

static int
tdnbd_nbd_negotiate_new(struct tdnbd_data *prv, td_driver_t *driver)
{
	int rc;
	uint16_t gflags;
	uint32_t cflags = htobe32(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);
	int sock = prv->socket;

	/*
	 * NBD NEW-style negotiation protocol:
	 *
	 * Server sends 'NBDMAGIC'
	 * then it sends 'IHAVEOPT'
	 * then it sends 16 bits of server handshake flags <-- YOU ARE HERE
	 * then it expects 32 bits of client handshake flags
	 * then we send additional options
	 */

	/* Receive NBD flags */
	rc = tdnbd_wait_recv(sock, &gflags, sizeof(gflags), 0);
	if (rc < 0) {
		ERROR("Error in nbd_negotiate: %s", strerror(-rc));
		goto errout;
	}
	if (rc < sizeof(gflags)) {
		ERROR("Short read in NEW negotiation(3) (%d)\n", rc);
		goto errout;
	}

	/* Send back flags*/
	rc = send(sock, &cflags, sizeof(cflags), 0);
	if (rc < sizeof(cflags)) {
		ERROR("Failed to send client flags");
		goto errout;
	}

	return negotiate_client_newstyle_options(sock, driver);

errout:
	close(sock);
	return -1;
}

static int
tdnbd_nbd_negotiate(struct tdnbd_data *prv, td_driver_t *driver)
{
	int rc;
	uint64_t magic;
	int sock = prv->socket;

	/* Read the NBD opening magic number, which is the same for all protocol
	 * versions */
	rc = tdnbd_wait_recv(sock, &magic, sizeof(magic), 0);
	if (rc < 0) {
		ERROR("Error in nbd_negotiate: %s", strerror(-rc));
		goto errout;
	}
	if (rc < sizeof(magic)) {
		ERROR("Short read in negotiation(1) (wanted %ld got %d)\n", sizeof(magic), rc);
		goto errout;
	}
	if (htonll(NBD_MAGIC) != magic) {
		ERROR("Error in NBD negotiation: wanted '0x%" PRIx64 "' got '0x%" PRIx64 "'", htonll(NBD_MAGIC), magic);
		goto errout;
	}

	/* Read the second magic number, which tells us which NBD protocol the
	 * server is offering. */
	rc = tdnbd_wait_recv(sock, &magic, sizeof(magic), 0);
	if (rc < 0) {
		ERROR("Error in nbd_negotiate: %s", strerror(-rc));
		goto errout;
	}
	if (rc < sizeof(magic)) {
		ERROR("Short read in negotiation(2) (wanted %ld got %d)\n", sizeof(magic), rc);
		goto errout;
	}

	if (htonll(NBD_OLD_VERSION) == magic)
		return tdnbd_nbd_negotiate_old(prv, driver);
	if (htonll(NBD_OPT_MAGIC) == magic)
		return tdnbd_nbd_negotiate_new(prv, driver);

	ERROR("Unknown NBD MAGIC 2: Wanted '0x%" PRIx64 "' or '0x%" PRIx64 "', got '0x%" PRIx64 "'",
			htonll(NBD_OLD_VERSION), htonll(NBD_OPT_MAGIC), magic);

errout:
	close(sock);
	return -1;
}

static int
tdnbd_connect_import_session(struct tdnbd_data *prv, td_driver_t* driver)
{
	int sock;
	int opt = 1;
	int rc;

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock < 0) {
		ERROR("Could not create socket: %s\n", strerror(errno));
		return -1;
	}

	rc = setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void *)&opt,
			sizeof(opt));
	if (rc < 0) {
		ERROR("Could not set TCP_NODELAY: %s\n", strerror(errno));
		close(sock);
		return -1;
	}

	prv->remote = (struct sockaddr_in *)malloc(
			sizeof(struct sockaddr_in));
	if (!prv->remote) {
		ERROR("struct sockaddr_in malloc failure\n");
		close(sock);
		return -1;
	}
	prv->remote->sin_family = AF_INET;
	rc = inet_pton(AF_INET, prv->peer_ip, &(prv->remote->sin_addr.s_addr));
	if (rc < 0) {
		ERROR("Could not create inaddr: %s\n", strerror(errno));
		free(prv->remote);
		prv->remote = NULL;
		close(sock);
		return -1;
	}
	else if (rc == 0) {
		ERROR("inet_pton parse error\n");
		free(prv->remote);
		prv->remote = NULL;
		close(sock);
		return -1;
	}
	prv->remote->sin_port = htons(prv->port);

	if (connect(sock, (struct sockaddr *)prv->remote,
				sizeof(struct sockaddr)) < 0) {
		ERROR("Could not connect to peer: %s\n", strerror(errno));
		free(prv->remote);
		close(sock);
		return -1;
	}

	prv->socket = sock;

	return tdnbd_nbd_negotiate(prv, driver);
}

/* -- interface -- */

static int tdnbd_close(td_driver_t*);

static int
tdnbd_open(td_driver_t* driver, const char* name,
	   struct td_vbd_encryption *encryption, td_flag_t flags)
{
	struct tdnbd_data *prv;
	char peer_ip[256];
	int port;
	int rc;
	int i;
	struct stat buf;

	driver->info.sector_size = 512;
	driver->info.info = 0;

	prv = (struct tdnbd_data *)driver->data;
	memset(prv, 0, sizeof(struct tdnbd_data));

	INFO("Opening nbd export to %s (flags=%x)\n", name, flags);

	prv->writer_event_id = -1;
	INIT_LIST_HEAD(&prv->sent_reqs);
	INIT_LIST_HEAD(&prv->pending_reqs);
	INIT_LIST_HEAD(&prv->free_reqs);
	for (i = 0; i < MAX_NBD_REQS; i++) {
		INIT_LIST_HEAD(&prv->requests[i].queue);
		prv->requests[i].timeout_event = -1;
		list_add(&prv->requests[i].queue, &prv->free_reqs);
	}
	prv->nr_free_count = MAX_NBD_REQS;
	prv->cur_reply_qio.buffer = (char *)&prv->current_reply;
	prv->cur_reply_qio.len = sizeof(struct nbd_reply);

	bzero(&buf, sizeof(buf));
	rc = stat(name, &buf);
	if (!rc && S_ISSOCK(buf.st_mode)) {
		int len = 0;
		if ((prv->socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
			ERROR("failed to create UNIX domain socket: %s\n",
					strerror(errno));
			return -1;
		}
		prv->remote_un.sun_family = AF_UNIX;
		safe_strncpy(prv->remote_un.sun_path, name, sizeof(prv->remote_un.sun_path));
		len = strlen(prv->remote_un.sun_path)
			+ sizeof(prv->remote_un.sun_family);
		if ((rc = connect(prv->socket, (struct sockaddr*)&prv->remote_un, len)
					== -1)) {
			ERROR("failed to connect to %s: %s\n", name, strerror(errno));
			return -1;
		}
		rc = tdnbd_nbd_negotiate(prv, driver);
		if (rc) {
			ERROR("failed to negotiate with the NBD server\n");
			return -1;
		}
	} else {
		rc = sscanf(name, "%255[^:]:%d", peer_ip, &port);
		if (rc == 2) {
			prv->peer_ip = strdup(peer_ip);
			if (!prv->peer_ip) {
				ERROR("Failure to malloc for NBD destination");
				return -1;
			}
			prv->port = port;
			prv->name = NULL;
			INFO("Export peer=%s port=%d\n", prv->peer_ip, prv->port);
			if (tdnbd_connect_import_session(prv, driver) < 0)
				return -1;

		} else {
			prv->socket = tdnbd_retrieve_passed_fd(name);
			if (prv->socket < 0) {
				ERROR("Couldn't find fd named: %s", name);
				return -1;
			}
			INFO("Found passed fd. Connecting...");
			prv->remote = NULL;
			prv->peer_ip = NULL;
			prv->name = strdup(name);
			prv->port = -1;
			if (tdnbd_nbd_negotiate(prv, driver) < 0) {
				ERROR("Failed to negotiate");
				return -1;
			}
		}
	}

	prv->reader_event_id =
		tapdisk_server_register_event(SCHEDULER_POLL_READ_FD,
				prv->socket, TV_ZERO,
				tdnbd_reader_cb,
				(void *)prv);

	prv->flags = flags;
	prv->closed = 0;

	if (flags & TD_OPEN_SECONDARY)
		INFO("Opening in secondary mode: Read requests will be "
				"forwarded");

	return 0;

}

static int
tdnbd_close(td_driver_t* driver)
{
	struct tdnbd_data *prv = (struct tdnbd_data *)driver->data;
	td_request_t treq;

	bzero(&treq, sizeof(treq));

	if (prv->closed == 3) {
		INFO("NBD close: already decided that the connection is dead.");
		if (prv->socket >= 0)
			close(prv->socket);
		prv->socket = -1;
		return 0;
	}

	/* Send a close packet */

	INFO("Sending disconnect request");
	tdnbd_queue_request(prv, TAPDISK_NBD_CMD_DISC, 0, 0, 0, treq, 0);

	INFO("Switching socket to blocking IO mode");
	fcntl(prv->socket, F_SETFL, fcntl(prv->socket, F_GETFL) & ~O_NONBLOCK);

	INFO("Writing disconnection request");
	tdnbd_writer_cb(0, 0, prv);

	INFO("Written");

	if (prv->peer_ip) {
		free(prv->peer_ip);
		prv->peer_ip = NULL;
	}

	if (prv->name) {
		tdnbd_stash_passed_fd(prv->socket, prv->name, 0);
		free(prv->name);
	} else {
		if (prv->socket >= 0)
			close(prv->socket);
		prv->socket = -1;
	}

	return 0;
}

static void
tdnbd_queue_read(td_driver_t* driver, td_request_t treq)
{
	struct tdnbd_data *prv = (struct tdnbd_data *)driver->data;
	int      size    = treq.secs * driver->info.sector_size;
	uint64_t offset  = treq.sec * (uint64_t)driver->info.sector_size;

	if (prv->flags & TD_OPEN_SECONDARY)
		td_forward_request(treq);
	else
		tdnbd_queue_request(prv, TAPDISK_NBD_CMD_READ, offset, treq.buf, size,
				treq, 0);
}

static void
tdnbd_queue_write(td_driver_t* driver, td_request_t treq)
{
	struct tdnbd_data *prv = (struct tdnbd_data *)driver->data;
	int      size    = treq.secs * driver->info.sector_size;
	uint64_t offset  = treq.sec * (uint64_t)driver->info.sector_size;

	tdnbd_queue_request(prv, TAPDISK_NBD_CMD_WRITE,
			offset, treq.buf, size, treq, 0);
}

static int
tdnbd_get_parent_id(td_driver_t* driver, td_disk_id_t* id)
{
	return TD_NO_PARENT;
}

static int
tdnbd_validate_parent(td_driver_t *driver,
		td_driver_t *parent, td_flag_t flags)
{
	return -EINVAL;
}

struct tap_disk tapdisk_nbd = {
	.disk_type          = "tapdisk_nbd",
	.private_data_size  = sizeof(struct tdnbd_data),
	.flags              = 0,
	.td_open            = tdnbd_open,
	.td_close           = tdnbd_close,
	.td_queue_read      = tdnbd_queue_read,
	.td_queue_write     = tdnbd_queue_write,
	.td_get_parent_id   = tdnbd_get_parent_id,
	.td_validate_parent = tdnbd_validate_parent,
};
