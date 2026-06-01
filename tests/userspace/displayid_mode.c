/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * DisplayID mode: ADD a 3840x2160@120 monitor and assert the kernel parsed
 * the synthesized DisplayID Type I timing into a 3840x2160@~120 connector
 * mode. Skip with success when /dev/lvda or the
 * lvda DRM card is absent.
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

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "../../uapi/lvda.h"

int main(void)
{
	int lvda_fd = open("/dev/lvda", O_RDWR);
	if (lvda_fd < 0) {
		if (errno == ENOENT || errno == EACCES) {
			printf("SKIP: /dev/lvda absent\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, "open /dev/lvda: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	struct lvda_add req;
	memset(&req, 0, sizeof(req));
	req.width = 3840;
	req.height = 2160;
	req.refresh_mhz = 120000;
	req.flags = 0;
	if (ioctl(lvda_fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "ADD 3840x2160@120: %s\n", strerror(errno));
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	char card[64];
	snprintf(card, sizeof(card), "/dev/dri/card%u",
		 (unsigned)req.drm_card_minor);
	int fd = open(card, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", card, strerror(errno));
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	drmModeResPtr res = drmModeGetResources(fd);
	if (!res || res->count_connectors < 1) {
		fprintf(stderr, "no connectors on %s\n", card);
		if (res)
			drmModeFreeResources(res);
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	int found = 0, preferred = 0;
	for (int i = 0; i < res->count_connectors && !found; i++) {
		drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		for (int m = 0; m < c->count_modes; m++) {
			const drmModeModeInfo *mi = &c->modes[m];

			if (mi->hdisplay == 3840 && mi->vdisplay == 2160 &&
			    mi->vrefresh >= 119 && mi->vrefresh <= 121) {
				found = 1;
				preferred = !!(mi->type & DRM_MODE_TYPE_PREFERRED);
				break;
			}
		}
		drmModeFreeConnector(c);
	}

	drmModeFreeResources(res);
	close(fd);
	close(lvda_fd);

	if (!found) {
		fprintf(stderr,
			"FAIL: connector advertises no 3840x2160@120 mode\n");
		return EXIT_FAILURE;
	}

	printf("PASS: displayid_mode (3840x2160@120 present%s)\n",
	       preferred ? ", preferred" : "");
	return EXIT_SUCCESS;
}
