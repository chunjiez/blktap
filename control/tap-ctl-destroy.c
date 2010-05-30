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
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "tap-ctl.h"
#include "blktap2.h"

static void
usage(void)
{
	printf("usage: destroy <-i id> <-m minor>\n");
}

int
_tap_ctl_destroy(const int id, const int minor)
{
	int err;

	err = _tap_ctl_close(id, minor, 0);
	if (err)
		return err;

	err = _tap_ctl_detach(id, minor);
	if (err)
		return err;

	err = _tap_ctl_free(minor);
	if (err)
		return err;

	return 0;
}

int
tap_ctl_destroy(int argc, char **argv)
{
	int c, id, minor;

	id    = -1;
	minor = -1;

	optind = 0;
	while ((c = getopt(argc, argv, "i:m:h")) != -1) {
		switch (c) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'm':
			minor = atoi(optarg);
			break;
		case 'h':
			usage();
			return 0;
		}
	}

	if (id == -1 || minor == -1) {
		usage();
		return EINVAL;
	}

	return _tap_ctl_destroy(id, minor);
}
