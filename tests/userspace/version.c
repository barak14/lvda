/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * SPEC §15.2 version: LVDA_IOC_VERSION reports the protocol major and
 * advertises HDR support in flags. Skip with success when /dev/lvda is
 * absent.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../uapi/lvda.h"

int main(void)
{
	int fd = open("/dev/lvda", O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT || errno == EACCES) {
			printf("SKIP: /dev/lvda absent\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, "open: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	struct lvda_version ver;
	memset(&ver, 0, sizeof(ver));
	if (ioctl(fd, LVDA_IOC_VERSION, &ver) < 0) {
		fprintf(stderr, "LVDA_IOC_VERSION: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	printf("version: %u.%u.%u flags=0x%02x\n", ver.major, ver.minor,
	       ver.patch, ver.flags);

	if (ver.major != LVDA_PROTOCOL_MAJOR) {
		fprintf(stderr, "major=%u, want %u\n", ver.major,
			LVDA_PROTOCOL_MAJOR);
		close(fd);
		return EXIT_FAILURE;
	}
	if (!(ver.flags & LVDA_F_HDR)) {
		fprintf(stderr, "flags=0x%02x missing LVDA_F_HDR\n", ver.flags);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
