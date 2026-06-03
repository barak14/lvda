/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * ADD a monitor, open the lvda DRM card, walk the primary plane IN_FORMATS
 * blob, and assert the modifier set is exactly {DRM_FORMAT_MOD_LINEAR} and
 * the format set contains the three formats XRGB8888, ARGB8888, XBGR2101010.
 * Skip with success when /dev/lvda or the lvda DRM card is absent.
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
#include <drm_mode.h>
#include <drm_fourcc.h>

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
	req.width = 1920;
	req.height = 1080;
	req.refresh_mhz = 60000;
	req.flags = 0;
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

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		printf("SKIP: atomic/universal-planes caps unavailable: %s\n",
		       strerror(errno));
		close(fd);
		close(lvda_fd);
		return EXIT_SUCCESS;
	}

	drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
	if (!planes || planes->count_planes == 0) {
		fprintf(stderr, "no planes on %s\n", card);
		if (planes)
			drmModeFreePlaneResources(planes);
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	int rc = 0;
	int checked = 0;

	for (uint32_t pi = 0; pi < planes->count_planes; pi++) {
		uint32_t plane_id = planes->planes[pi];
		drmModeObjectPropertiesPtr props =
			drmModeObjectGetProperties(fd, plane_id,
						   DRM_MODE_OBJECT_PLANE);
		if (!props)
			continue;

		uint64_t in_formats_blob = 0;
		uint64_t plane_type = UINT64_MAX;
		int have_blob = 0;
		for (uint32_t i = 0; i < props->count_props; i++) {
			drmModePropertyPtr p =
				drmModeGetProperty(fd, props->props[i]);
			if (!p)
				continue;
			if (strcmp(p->name, "IN_FORMATS") == 0) {
				in_formats_blob = props->prop_values[i];
				have_blob = 1;
			}
			if (strcmp(p->name, "type") == 0)
				plane_type = props->prop_values[i];
			drmModeFreeProperty(p);
			if (have_blob && plane_type != UINT64_MAX)
				break;
		}
		drmModeFreeObjectProperties(props);

		if (plane_type != DRM_PLANE_TYPE_PRIMARY)
			continue;

		if (!have_blob)
			continue;

		drmModePropertyBlobPtr blob =
			drmModeGetPropertyBlob(fd, (uint32_t)in_formats_blob);
		if (!blob || !blob->data) {
			fprintf(stderr, "plane %u: IN_FORMATS blob missing\n",
				plane_id);
			rc = 1;
			if (blob)
				drmModeFreePropertyBlob(blob);
			continue;
		}

		const struct drm_format_modifier_blob *fb = blob->data;
		const uint32_t *fmts =
			(const uint32_t *)((const char *)fb +
					   fb->formats_offset);
		const struct drm_format_modifier *mods =
			(const struct drm_format_modifier *)
				((const char *)fb + fb->modifiers_offset);

		/* Every advertised modifier must be LINEAR. */
		for (uint32_t m = 0; m < fb->count_modifiers; m++) {
			if (mods[m].modifier != DRM_FORMAT_MOD_LINEAR) {
				fprintf(stderr,
					"plane %u: non-LINEAR modifier "
					"0x%llx\n", plane_id,
					(unsigned long long)mods[m].modifier);
				rc = 1;
			}
		}

		/* Required formats must all be present. */
		int has_xrgb = 0, has_argb = 0, has_xbgr2101010 = 0;
		for (uint32_t f = 0; f < fb->count_formats; f++) {
			switch (fmts[f]) {
			case DRM_FORMAT_XRGB8888:
				has_xrgb = 1;
				break;
			case DRM_FORMAT_ARGB8888:
				has_argb = 1;
				break;
			case DRM_FORMAT_XBGR2101010:
				has_xbgr2101010 = 1;
				break;
			default:
				break;
			}
		}
		if (!has_xrgb) {
			fprintf(stderr, "plane %u: XRGB8888 missing\n",
				plane_id);
			rc = 1;
		}
		if (!has_argb) {
			fprintf(stderr, "plane %u: ARGB8888 missing\n",
				plane_id);
			rc = 1;
		}
		if (!has_xbgr2101010) {
			fprintf(stderr, "plane %u: XBGR2101010 missing\n",
				plane_id);
			rc = 1;
		}

		printf("plane %u: %u formats, %u modifiers (LINEAR only)\n",
		       plane_id, fb->count_formats, fb->count_modifiers);
		checked++;
		drmModeFreePropertyBlob(blob);
	}

	drmModeFreePlaneResources(planes);
	close(fd);
	close(lvda_fd);

	if (checked == 0) {
		fprintf(stderr, "no plane exposed IN_FORMATS\n");
		return EXIT_FAILURE;
	}
	if (rc) {
		printf("FAIL: modifier_negotiation\n");
		return EXIT_FAILURE;
	}
	printf("PASS: modifier_negotiation\n");
	return EXIT_SUCCESS;
}
