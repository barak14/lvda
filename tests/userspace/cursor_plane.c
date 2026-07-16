/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Cursor plane: ADD a monitor on the lvda card, then assert the card exposes
 * a DRM_PLANE_TYPE_CURSOR plane that advertises ARGB8888. Skip with success
 * when /dev/lvda or the lvda DRM card is absent.
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
#include <drm_fourcc.h>
#include <drm_mode.h>

#include "../../uapi/lvda.h"

/* Read the enum "type" property value of a plane. Returns 1 on success. */
static int plane_type(int fd, uint32_t plane_id, uint64_t *type)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
	int found = 0;

	if (!props)
		return 0;

	for (uint32_t i = 0; i < props->count_props && !found; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);

		if (!p)
			continue;
		if (strcmp(p->name, "type") == 0) {
			*type = props->prop_values[i];
			found = 1;
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

/* Returns 1 if the plane advertises the given fourcc format. */
static int plane_has_format(drmModePlanePtr pl, uint32_t fourcc)
{
	for (uint32_t i = 0; i < pl->count_formats; i++)
		if (pl->formats[i] == fourcc)
			return 1;
	return 0;
}

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
	req.width = 1920;
	req.height = 1080;
	req.refresh_mhz = 60000;
	if (ioctl(lvda_fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "LVDA_IOC_ADD: %s\n", strerror(errno));
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	char card[64];
	snprintf(card, sizeof(card), "/dev/dri/card%u",
		 (unsigned)req.drm_card_minor);

	int fd = open(card, O_RDWR);
	if (fd < 0) {
		printf("SKIP: cannot open %s: %s\n", card, strerror(errno));
		close(lvda_fd);
		return EXIT_SUCCESS;
	}

	drmVersionPtr ver = drmGetVersion(fd);
	if (!ver || !ver->name || strcmp(ver->name, "lvda") != 0) {
		printf("SKIP: %s is not an lvda card\n", card);
		if (ver)
			drmFreeVersion(ver);
		close(fd);
		close(lvda_fd);
		return EXIT_SUCCESS;
	}
	drmFreeVersion(ver);

	/* Primary and cursor planes are only enumerated with universal planes. */
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
		printf("SKIP: universal planes unavailable: %s\n",
		       strerror(errno));
		close(fd);
		close(lvda_fd);
		return EXIT_SUCCESS;
	}

	drmModePlaneResPtr pres = drmModeGetPlaneResources(fd);
	if (!pres || pres->count_planes == 0) {
		fprintf(stderr, "no planes on %s\n", card);
		if (pres)
			drmModeFreePlaneResources(pres);
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	int cursors = 0, cursor_argb = 0;
	int rc = 0;

	for (uint32_t i = 0; i < pres->count_planes; i++) {
		uint64_t type = 0;

		if (!plane_type(fd, pres->planes[i], &type))
			continue;
		if (type != DRM_PLANE_TYPE_CURSOR)
			continue;

		cursors++;

		drmModePlanePtr pl = drmModeGetPlane(fd, pres->planes[i]);
		if (pl) {
			if (plane_has_format(pl, DRM_FORMAT_ARGB8888))
				cursor_argb++;
			drmModeFreePlane(pl);
		}
	}

	if (cursors == 0) {
		fprintf(stderr, "no DRM_PLANE_TYPE_CURSOR plane on %s\n", card);
		rc = 1;
	} else if (cursor_argb != cursors) {
		fprintf(stderr,
			"%d of %d cursor plane(s) lack ARGB8888\n",
			cursors - cursor_argb, cursors);
		rc = 1;
	}

	printf("found %d cursor plane(s), %d with ARGB8888\n",
	       cursors, cursor_argb);

	drmModeFreePlaneResources(pres);
	close(fd);
	close(lvda_fd);

	if (rc) {
		printf("FAIL: cursor_plane\n");
		return EXIT_FAILURE;
	}
	printf("PASS: cursor_plane\n");
	return EXIT_SUCCESS;
}
