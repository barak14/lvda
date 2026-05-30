/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * SPEC §15.2 kmsgrab roundtrip: ADD a monitor, attach a LINEAR
 * framebuffer to the primary plane via an atomic modeset, read it back
 * with drmModeGetFB2(), and PRIME-export the buffer as a DMA-BUF. Assert
 * the DRM format and LINEAR modifier survive the round trip (the kmsgrab
 * capture path, §1, §7, §10). Skip with success when /dev/lvda, the
 * lvda DRM card, or DRM-master/atomic is unavailable.
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

/* Look up the property id named `name` on object (id,type). 0 if absent. */
static uint32_t prop_id(int fd, uint32_t obj_id, uint32_t obj_type,
			const char *name)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props)
		return 0;

	uint32_t found = 0;
	for (uint32_t i = 0; i < props->count_props && !found; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, name) == 0)
			found = props->props[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

/* Read uint64 property value named `name` on object. 0 if absent. */
static uint64_t prop_val(int fd, uint32_t obj_id, uint32_t obj_type,
			 const char *name)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props)
		return 0;

	uint64_t val = 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (strcmp(p->name, name) == 0)
			val = props->prop_values[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return val;
}

static int skip(const char *why, int lvda_fd, int fd)
{
	printf("SKIP: %s\n", why);
	if (fd >= 0)
		close(fd);
	if (lvda_fd >= 0)
		close(lvda_fd);
	return EXIT_SUCCESS;
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
	if (fd < 0)
		return skip("cannot open DRM card", lvda_fd, -1);

	drmVersionPtr ver = drmGetVersion(fd);
	if (!ver || !ver->name || strcmp(ver->name, "lvda") != 0) {
		if (ver)
			drmFreeVersion(ver);
		return skip("card is not an lvda card", lvda_fd, fd);
	}
	drmFreeVersion(ver);

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
		return skip("atomic/universal-planes caps unavailable",
			    lvda_fd, fd);

	drmModeResPtr res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "drmModeGetResources: %s\n", strerror(errno));
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	/* Find the connected connector and its preferred mode. */
	uint32_t conn_id = 0;
	uint32_t enc_id = 0;
	drmModeModeInfo mode;
	int have_mode = 0;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnectorPtr c =
			drmModeGetConnector(fd, res->connectors[i]);
		if (!c)
			continue;
		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn_id = c->connector_id;
			enc_id = c->encoder_id;
			int pref = 0;
			for (int m = 0; m < c->count_modes; m++) {
				if (c->modes[m].type &
				    DRM_MODE_TYPE_PREFERRED) {
					pref = m;
					break;
				}
			}
			mode = c->modes[pref];
			have_mode = 1;
		}
		drmModeFreeConnector(c);
		if (have_mode)
			break;
	}
	if (!have_mode) {
		drmModeFreeResources(res);
		return skip("no connected connector with modes", lvda_fd, fd);
	}

	/* Resolve the CRTC from the connector's encoder, else fall back. */
	uint32_t crtc_id = 0;
	if (enc_id) {
		drmModeEncoderPtr enc = drmModeGetEncoder(fd, enc_id);
		if (enc) {
			crtc_id = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
	}
	if (!crtc_id && res->count_crtcs > 0)
		crtc_id = res->crtcs[0];
	if (!crtc_id) {
		drmModeFreeResources(res);
		return skip("no usable CRTC", lvda_fd, fd);
	}

	/* Find the primary plane. */
	drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
	uint32_t plane_id = 0;
	if (planes) {
		for (uint32_t i = 0; i < planes->count_planes && !plane_id;
		     i++) {
			uint64_t type =
				prop_val(fd, planes->planes[i],
					 DRM_MODE_OBJECT_PLANE, "type");
			if (type == DRM_PLANE_TYPE_PRIMARY)
				plane_id = planes->planes[i];
		}
		if (!plane_id && planes->count_planes > 0)
			plane_id = planes->planes[0];
		drmModeFreePlaneResources(planes);
	}
	if (!plane_id) {
		drmModeFreeResources(res);
		return skip("no primary plane", lvda_fd, fd);
	}

	uint32_t w = mode.hdisplay;
	uint32_t h = mode.vdisplay;

	/* Dumb buffer sized to the mode, 32 bpp. */
	uint32_t handle = 0, pitch = 0;
	uint64_t size = 0;
	if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &handle, &pitch,
				    &size) != 0) {
		int e = errno;
		drmModeFreeResources(res);
		if (e == EACCES || e == EOPNOTSUPP || e == EPERM)
			return skip("dumb buffer unavailable (no DRM master)",
				    lvda_fd, fd);
		fprintf(stderr, "CreateDumbBuffer: %s\n", strerror(e));
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	/* Framebuffer with an explicit LINEAR modifier. */
	uint32_t handles[4] = { handle, 0, 0, 0 };
	uint32_t pitches[4] = { pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };
	uint32_t fb_id = 0;
	if (drmModeAddFB2WithModifiers(fd, w, h, DRM_FORMAT_XRGB8888,
				       handles, pitches, offsets, modifiers,
				       &fb_id, DRM_MODE_FB_MODIFIERS) != 0) {
		int e = errno;
		drmModeDestroyDumbBuffer(fd, handle);
		drmModeFreeResources(res);
		fprintf(stderr, "AddFB2WithModifiers: %s\n", strerror(e));
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	/* Mode blob for the CRTC MODE_ID property. */
	uint32_t mode_blob = 0;
	if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode),
				      &mode_blob) != 0) {
		int e = errno;
		drmModeRmFB(fd, fb_id);
		drmModeDestroyDumbBuffer(fd, handle);
		drmModeFreeResources(res);
		fprintf(stderr, "CreatePropertyBlob: %s\n", strerror(e));
		close(fd);
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	/* Build the atomic request. */
	drmModeAtomicReqPtr areq = drmModeAtomicAlloc();
	int rc = EXIT_SUCCESS;
	int done = 0;

	uint32_t p_conn_crtc = prop_id(fd, conn_id, DRM_MODE_OBJECT_CONNECTOR,
				       "CRTC_ID");
	uint32_t p_crtc_mode = prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
				       "MODE_ID");
	uint32_t p_crtc_active = prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC,
					 "ACTIVE");
	uint32_t p_fb = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
	uint32_t p_pcrtc = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE,
				   "CRTC_ID");
	uint32_t p_src_x = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
	uint32_t p_src_y = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
	uint32_t p_src_w = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
	uint32_t p_src_h = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
	uint32_t p_crtc_x = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE,
				    "CRTC_X");
	uint32_t p_crtc_y = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE,
				    "CRTC_Y");
	uint32_t p_crtc_w = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE,
				    "CRTC_W");
	uint32_t p_crtc_h = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE,
				    "CRTC_H");

	if (!areq || !p_conn_crtc || !p_crtc_mode || !p_crtc_active || !p_fb ||
	    !p_pcrtc || !p_src_x || !p_src_y || !p_src_w || !p_src_h ||
	    !p_crtc_x || !p_crtc_y || !p_crtc_w || !p_crtc_h) {
		fprintf(stderr, "missing atomic properties\n");
		rc = EXIT_FAILURE;
		goto cleanup;
	}

	drmModeAtomicAddProperty(areq, conn_id, p_conn_crtc, crtc_id);
	drmModeAtomicAddProperty(areq, crtc_id, p_crtc_mode, mode_blob);
	drmModeAtomicAddProperty(areq, crtc_id, p_crtc_active, 1);
	drmModeAtomicAddProperty(areq, plane_id, p_fb, fb_id);
	drmModeAtomicAddProperty(areq, plane_id, p_pcrtc, crtc_id);
	drmModeAtomicAddProperty(areq, plane_id, p_src_x, 0);
	drmModeAtomicAddProperty(areq, plane_id, p_src_y, 0);
	drmModeAtomicAddProperty(areq, plane_id, p_src_w,
				 (uint64_t)w << 16);
	drmModeAtomicAddProperty(areq, plane_id, p_src_h,
				 (uint64_t)h << 16);
	drmModeAtomicAddProperty(areq, plane_id, p_crtc_x, 0);
	drmModeAtomicAddProperty(areq, plane_id, p_crtc_y, 0);
	drmModeAtomicAddProperty(areq, plane_id, p_crtc_w, w);
	drmModeAtomicAddProperty(areq, plane_id, p_crtc_h, h);

	if (drmModeAtomicCommit(fd, areq, DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL) != 0) {
		int e = errno;
		if (e == EACCES || e == EOPNOTSUPP || e == EPERM ||
		    e == ENOTSUP) {
			printf("SKIP: atomic commit unsupported here: %s\n",
			       strerror(e));
			rc = EXIT_SUCCESS;
			goto cleanup;
		}
		fprintf(stderr, "AtomicCommit: %s\n", strerror(e));
		rc = EXIT_FAILURE;
		goto cleanup;
	}

	/* Read the framebuffer back as kmsgrab would. */
	drmModeFB2Ptr fb2 = drmModeGetFB2(fd, fb_id);
	if (!fb2) {
		fprintf(stderr, "drmModeGetFB2: %s\n", strerror(errno));
		rc = EXIT_FAILURE;
		goto cleanup;
	}

	int ok = 1;
	if (fb2->pixel_format != DRM_FORMAT_XRGB8888) {
		fprintf(stderr, "FB2 pixel_format 0x%08x, want XRGB8888\n",
			fb2->pixel_format);
		ok = 0;
	}
	if (fb2->modifier != DRM_FORMAT_MOD_LINEAR) {
		fprintf(stderr, "FB2 modifier 0x%llx, want LINEAR\n",
			(unsigned long long)fb2->modifier);
		ok = 0;
	}
	if (!(fb2->flags & DRM_MODE_FB_MODIFIERS)) {
		fprintf(stderr, "FB2 flags lack DRM_MODE_FB_MODIFIERS\n");
		ok = 0;
	}
	drmModeFreeFB2(fb2);

	/* PRIME-export the buffer (the kmsgrab DMA-BUF re-export). */
	int dmabuf_fd = -1;
	if (drmPrimeHandleToFD(fd, handle, DRM_CLOEXEC, &dmabuf_fd) != 0 ||
	    dmabuf_fd < 0) {
		fprintf(stderr, "drmPrimeHandleToFD: %s\n", strerror(errno));
		ok = 0;
	} else {
		close(dmabuf_fd);
	}

	if (ok) {
		printf("PASS: kmsgrab_roundtrip (card%u)\n",
		       (unsigned)req.drm_card_minor);
		rc = EXIT_SUCCESS;
	} else {
		printf("FAIL: kmsgrab_roundtrip\n");
		rc = EXIT_FAILURE;
	}
	done = 1;

cleanup:
	(void)done;
	if (areq)
		drmModeAtomicFree(areq);
	if (mode_blob)
		drmModeDestroyPropertyBlob(fd, mode_blob);
	drmModeRmFB(fd, fb_id);
	drmModeDestroyDumbBuffer(fd, handle);
	drmModeFreeResources(res);
	close(fd);
	close(lvda_fd);
	return rc;
}
