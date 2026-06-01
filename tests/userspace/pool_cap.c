/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Multiple monitors may be added on one fd. ADD until the pool is exhausted
 * (-ENOSPC), then REMOVE one and confirm the freed slot can be reused. Any
 * ADD failing with something other than ENOSPC — notably EBUSY — fails the
 * test. Skip with success when /dev/lvda is absent.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../uapi/lvda.h"

#define MAX_TRY 128

static void fill(struct lvda_add *req)
{
	memset(req, 0, sizeof(*req));
	req->width = 1920;
	req->height = 1080;
	req->refresh_mhz = 60000;
}

int main(void)
{
	int fd = open("/dev/lvda", O_RDWR);
	uint32_t ids[MAX_TRY];
	int n = 0, enospc = 0, i;
	struct lvda_add req;
	struct lvda_remove rm;

	if (fd < 0) {
		if (errno == ENOENT || errno == EACCES) {
			printf("SKIP: /dev/lvda absent\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, "open: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	for (i = 0; i < MAX_TRY; i++) {
		fill(&req);
		errno = 0;
		if (ioctl(fd, LVDA_IOC_ADD, &req) == 0) {
			ids[n++] = req.monitor_id;
			continue;
		}
		if (errno == ENOSPC) {
			enospc = 1;
			break;
		}
		fprintf(stderr, "ADD #%d failed with %s (want success or ENOSPC)\n",
			n + 1, strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (n < 1) {
		fprintf(stderr, "no monitor could be added\n");
		close(fd);
		return EXIT_FAILURE;
	}
	if (!enospc) {
		fprintf(stderr, "pool not exhausted after %d adds\n", MAX_TRY);
		close(fd);
		return EXIT_FAILURE;
	}

	printf("pool_cap: %d monitor(s) on one fd before ENOSPC\n", n);

	memset(&rm, 0, sizeof(rm));
	rm.monitor_id = ids[0];
	if (ioctl(fd, LVDA_IOC_REMOVE, &rm) < 0) {
		fprintf(stderr, "REMOVE: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	fill(&req);
	if (ioctl(fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "ADD after REMOVE should reuse the slot: %s\n",
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	printf("pool_cap: freed slot reused (monitor_id=%u)\n",
	       (unsigned)req.monitor_id);

	close(fd);
	return EXIT_SUCCESS;
}
