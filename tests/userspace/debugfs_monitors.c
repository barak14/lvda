/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Debugfs introspection: ADD a monitor with a distinctive mode, flags, and
 * client_id, then read /sys/kernel/debug/dri/<minor>/monitors and assert the
 * live row reflects them. Skip with success when /dev/lvda is absent or the
 * debugfs file cannot be read (debugfs unmounted or unprivileged).
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

int main(void)
{
	int lvda_fd = open("/dev/lvda", O_RDWR);
	if (lvda_fd < 0) {
		if (errno == ENOENT || errno == EACCES) {
			printf("SKIP: /dev/lvda absent\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, "open: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	struct lvda_add req;
	memset(&req, 0, sizeof(req));
	req.width = 2560;
	req.height = 1440;
	req.refresh_mhz = 120000;
	req.flags = LVDA_F_HDR | LVDA_F_10BPC;
	static const uint8_t cid[LVDA_CLIENT_ID_LEN] = { 0xde, 0xad, 0xbe, 0xef };
	memcpy(req.client_id, cid, sizeof(req.client_id));
	if (ioctl(lvda_fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "LVDA_IOC_ADD: %s\n", strerror(errno));
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	char path[64];
	snprintf(path, sizeof(path), "/sys/kernel/debug/dri/%u/monitors",
		 (unsigned)req.drm_card_minor);

	FILE *f = fopen(path, "r");
	if (!f) {
		printf("SKIP: cannot read %s: %s\n", path, strerror(errno));
		close(lvda_fd);
		return EXIT_SUCCESS;
	}

	char buf[16384];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	buf[n] = '\0';
	fclose(f);

	static const struct {
		const char *needle;
		const char *what;
	} want[] = {
		{ "client_id",         "header" },
		{ "2560x1440@120.000", "mode" },
		{ "active",            "state" },
		{ "hdr,10bpc",         "flags" },
		{ "deadbeef",          "client_id hex" },
	};

	int rc = 0;
	for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (!strstr(buf, want[i].needle)) {
			fprintf(stderr, "monitors table missing %s (%s)\n",
				want[i].what, want[i].needle);
			rc = 1;
		}
	}

	close(lvda_fd);

	if (rc) {
		fprintf(stderr, "----- %s -----\n%s\n", path, buf);
		printf("FAIL: debugfs_monitors\n");
		return EXIT_FAILURE;
	}
	printf("PASS: debugfs_monitors\n");
	return EXIT_SUCCESS;
}
