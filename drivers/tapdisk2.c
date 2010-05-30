/*
 * Copyright (c) 2008, XenSource Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of XenSource Inc. nor the names of its contributors
 *       may be used to endorse or promote products derived from this software
 *       without specific prior written permission.
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
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "tapdisk.h"
#include "tapdisk-utils.h"
#include "tapdisk-server.h"
#include "tapdisk-control.h"

static void
usage(const char *app, int err)
{
	fprintf(stderr,
		"usage: %s [-h] [-l <syslog>] "
		"-u <uuid> -c <control socket>\n", app);
	exit(err);
}

int
main(int argc, char *argv[])
{
	char *control;
	int c, uuid, err, nodaemon;
	const char *facility;

	uuid     = -1;
	control  = NULL;
	facility = "daemon";
	nodaemon = 0;

	while ((c = getopt(argc, argv, "l:u:c:Dh")) != -1) {
		switch (c) {
		case 'l':
			facility = optarg;
			break;
		case 'u':
			uuid = atoi(optarg);
			break;
		case 'c':
			control = optarg;
			break;
		case 'D':
			nodaemon = 1;
			break;
		case 'h':
			usage(argv[0], 0);
		default:
			usage(argv[0], EINVAL);
		}
	}

	if (optind != argc || uuid == -1 || !control)
		usage(argv[0], EINVAL);

	chdir("/");
	tapdisk_start_logging("tapdisk2", facility);

	err = tapdisk_server_init();
	if (err) {
		DPRINTF("failed to initialize server: %d\n", err);
		goto out;
	}

	err = tapdisk_control_open(uuid, control);
	if (err) {
		DPRINTF("failed to open control socket: %d\n", err);
		goto out;
	}

	if (!nodaemon) {
		err = daemon(0, 0);
		if (err) {
			DPRINTF("failed to daemonize: %d\n", errno);
			goto out;
		}
	}

	err = tapdisk_server_complete();
	if (err) {
		DPRINTF("failed to complete server: %d\n", err);
		goto out;
	}

	err = tapdisk_server_run();

out:
	tapdisk_control_close();
	tapdisk_stop_logging();
	return err;
}
