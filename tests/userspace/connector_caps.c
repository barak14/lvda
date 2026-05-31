/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Connector capability properties: ADD a monitor on the lvda card, then
 * assert every connector exposes the optional capability properties a
 * compositor reads to advertise features — vrr_capable (immutable, == 1),
 * Colorspace (enum carrying BT2020_RGB and BT2020_YCC), HDR_OUTPUT_METADATA
 * (blob) — and that every CRTC exposes the atomic VRR_ENABLED property. These
 * are attached at connector init independent of LVDA_F_HDR, so flags == 0
 * still exercises them. Skip with success when /dev/lvda or the lvda DRM card
 * is absent.
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

#include "../../uapi/lvda.h"

/* Locate a property by name on a mode object. Returns 1 and stores its id and
 * current value when present, else 0. */
static int find_prop(int fd, uint32_t obj_id, uint32_t obj_type,
		     const char *name, uint32_t *prop_id, uint64_t *value)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(fd, obj_id, obj_type);
	int found = 0;

	if (!props)
		return 0;

	for (uint32_t i = 0; i < props->count_props && !found; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, name) == 0) {
			found = 1;
			if (prop_id)
				*prop_id = props->props[i];
			if (value)
				*value = props->prop_values[i];
		}
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

/* Returns 1 if the enum property exposes an entry named `entry`. */
static int enum_has(int fd, uint32_t prop_id, const char *entry)
{
	drmModePropertyPtr p = drmModeGetProperty(fd, prop_id);
	int has = 0;

	if (!p)
		return 0;
	if (p->flags & DRM_MODE_PROP_ENUM) {
		for (int j = 0; j < p->count_enums; j++) {
			if (strcmp(p->enums[j].name, entry) == 0) {
				has = 1;
				break;
			}
		}
	}
	drmModeFreeProperty(p);
	return has;
}

/* Reads a range property's [min, max] bounds. Returns 1 on success. */
static int range_bounds(int fd, uint32_t prop_id, uint64_t *min, uint64_t *max)
{
	drmModePropertyPtr p = drmModeGetProperty(fd, prop_id);
	int ok = 0;

	if (!p)
		return 0;
	if ((p->flags & DRM_MODE_PROP_RANGE) && p->count_values == 2) {
		*min = p->values[0];
		*max = p->values[1];
		ok = 1;
	}
	drmModeFreeProperty(p);
	return ok;
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

	/* VRR_ENABLED is an atomic CRTC property — only listed with the cap. */
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
		printf("SKIP: atomic cap unavailable: %s\n", strerror(errno));
		close(fd);
		close(lvda_fd);
		return EXIT_SUCCESS;
	}

	drmModeResPtr res = drmModeGetResources(fd);
	if (!res || res->count_connectors == 0 || res->count_crtcs == 0) {
		fprintf(stderr, "no connectors/crtcs on %s\n", card);
		if (res)
			drmModeFreeResources(res);
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	int rc = 0;

	for (int c = 0; c < res->count_connectors; c++) {
		uint32_t id = res->connectors[c];
		uint32_t cs_prop = 0;
		uint64_t vrr = 0;

		if (!find_prop(fd, id, DRM_MODE_OBJECT_CONNECTOR,
			       "vrr_capable", NULL, &vrr)) {
			fprintf(stderr, "connector %u: vrr_capable missing\n",
				id);
			rc = 1;
		} else if (vrr != 1) {
			fprintf(stderr,
				"connector %u: vrr_capable=%llu, want 1\n",
				id, (unsigned long long)vrr);
			rc = 1;
		}

		if (!find_prop(fd, id, DRM_MODE_OBJECT_CONNECTOR,
			       "HDR_OUTPUT_METADATA", NULL, NULL)) {
			fprintf(stderr,
				"connector %u: HDR_OUTPUT_METADATA missing\n",
				id);
			rc = 1;
		}

		if (!find_prop(fd, id, DRM_MODE_OBJECT_CONNECTOR,
			       "Colorspace", &cs_prop, NULL)) {
			fprintf(stderr, "connector %u: Colorspace missing\n",
				id);
			rc = 1;
		} else {
			if (!enum_has(fd, cs_prop, "BT2020_RGB")) {
				fprintf(stderr,
					"connector %u: Colorspace lacks "
					"BT2020_RGB\n", id);
				rc = 1;
			}
			if (!enum_has(fd, cs_prop, "BT2020_YCC")) {
				fprintf(stderr,
					"connector %u: Colorspace lacks "
					"BT2020_YCC\n", id);
				rc = 1;
			}
		}

		uint32_t bpc_prop = 0;
		if (!find_prop(fd, id, DRM_MODE_OBJECT_CONNECTOR,
			       "max bpc", &bpc_prop, NULL)) {
			fprintf(stderr, "connector %u: max bpc missing\n", id);
			rc = 1;
		} else {
			uint64_t lo = 0, hi = 0;
			if (!range_bounds(fd, bpc_prop, &lo, &hi) ||
			    lo != 8 || hi != 10) {
				fprintf(stderr,
					"connector %u: max bpc range "
					"[%llu;%llu], want [8;10]\n", id,
					(unsigned long long)lo,
					(unsigned long long)hi);
				rc = 1;
			}
		}
	}

	for (int c = 0; c < res->count_crtcs; c++) {
		if (!find_prop(fd, res->crtcs[c], DRM_MODE_OBJECT_CRTC,
			       "VRR_ENABLED", NULL, NULL)) {
			fprintf(stderr, "crtc %u: VRR_ENABLED missing\n",
				res->crtcs[c]);
			rc = 1;
		}
	}

	printf("checked %d connector(s), %d crtc(s)\n",
	       res->count_connectors, res->count_crtcs);

	drmModeFreeResources(res);
	close(fd);
	close(lvda_fd);

	if (rc) {
		printf("FAIL: connector_caps\n");
		return EXIT_FAILURE;
	}
	printf("PASS: connector_caps\n");
	return EXIT_SUCCESS;
}
