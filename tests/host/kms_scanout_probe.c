// SPDX-License-Identifier: GPL-2.0
/*
 * kms_scanout_probe -- step 1: prove lvda's capture path with no GL/seat/session.
 *
 * Pre-req: a monitor must be connected first (lvda-ctl up ...), which
 * synthesizes the EDID and marks Virtual-N connected with the requested mode.
 *
 * This does what a compositor + kmsgrab do between them:
 *   1. become DRM master on the lvda card
 *   2. find the connected virtual connector + its mode (the client's exact mode)
 *   3. allocate a DUMB buffer (lvda's native path), fill a test pattern
 *   4. addFB2 LINEAR, atomic-modeset connector->crtc->plane (ALLOW_MODESET)
 *   5. drmModeGetFB2() on the live plane FB  <- kmsgrab's exact capture call
 *   6. PRIME-export the FB handle to a dmabuf <- what is handed to the encoder
 *
 * PASS == a real scanout is live at the requested mode AND the framebuffer is
 * capturable (GetFB2 -> LINEAR + correct dims/fourcc) AND exportable as a
 * dmabuf: the whole SPEC capture path, end to end.
 *
 * Build: cc kms_scanout_probe.c -o kms_scanout_probe $(pkg-config --cflags --libs libdrm)
 * Run:   sudo ./kms_scanout_probe [/dev/dri/cardN] [WxH]
 *        (auto-finds the lvda card; WxH selects among the connected modes)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static int open_lvda(char *out, size_t n)
{
	for (int i = 0; i < 64; i++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);

		if (fd < 0)
			continue;

		drmVersionPtr v = drmGetVersion(fd);
		int match = v && v->name && strcmp(v->name, "lvda") == 0;

		if (v)
			drmFreeVersion(v);
		if (match) {
			snprintf(out, n, "%s", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* Look up a property id on an object; optionally return its current value. */
static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name,
			uint64_t *cur)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t id = 0;

	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);

		if (p && strcmp(p->name, name) == 0) {
			id = p->prop_id;
			if (cur)
				*cur = props->prop_values[i];
		}
		if (p)
			drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static int g_fail;

static void set_prop(int fd, drmModeAtomicReq *req, uint32_t obj, uint32_t type,
		     const char *name, uint64_t val)
{
	uint32_t pid = prop_id(fd, obj, type, name, NULL);

	if (!pid) {
		fprintf(stderr, "FAIL: missing atomic prop %s on obj %u\n", name, obj);
		g_fail = 1;
		return;
	}
	if (drmModeAtomicAddProperty(req, obj, pid, val) < 0) {
		fprintf(stderr, "FAIL: add atomic prop %s\n", name);
		g_fail = 1;
	}
}

int main(int argc, char **argv)
{
	char path[64] = {0};
	int want_w = 0, want_h = 0;

	for (int i = 1; i < argc; i++) {
		if (strchr(argv[i], 'x') &&
		    sscanf(argv[i], "%dx%d", &want_w, &want_h) == 2)
			continue;
		snprintf(path, sizeof(path), "%s", argv[i]);
	}

	int fd = path[0] ? open(path, O_RDWR | O_CLOEXEC)
			 : open_lvda(path, sizeof(path));
	if (fd < 0) {
		fprintf(stderr, "FAIL: no lvda card (modprobe lvda + lvda-ctl up?)\n");
		return 1;
	}
	printf("card: %s\n", path);

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "FAIL: atomic/universal-planes caps "
				"(not DRM master? a compositor may hold the card)\n");
		return 1;
	}

	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		fprintf(stderr, "FAIL: drmModeGetResources\n");
		return 1;
	}

	drmModeConnector *conn = NULL;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);

		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			conn = c;
			break;
		}
		if (c)
			drmModeFreeConnector(c);
	}
	if (!conn) {
		fprintf(stderr, "FAIL: no connected lvda connector "
				"(run: lvda-ctl up --width W --height H --fps N)\n");
		return 1;
	}

	drmModeModeInfo mode = conn->modes[0];
	if (want_w && want_h) {
		for (int i = 0; i < conn->count_modes; i++) {
			if (conn->modes[i].hdisplay == want_w &&
			    conn->modes[i].vdisplay == want_h) {
				mode = conn->modes[i];
				break;
			}
		}
	}
	printf("connector: id=%u modes=%d\n", conn->connector_id, conn->count_modes);
	printf("mode: %ux%u@%uHz\n", mode.hdisplay, mode.vdisplay, mode.vrefresh);

	uint32_t crtc_id = 0;
	for (int e = 0; e < conn->count_encoders && !crtc_id; e++) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);

		if (!enc)
			continue;
		for (int b = 0; b < res->count_crtcs; b++) {
			if (enc->possible_crtcs & (1u << b)) {
				crtc_id = res->crtcs[b];
				break;
			}
		}
		drmModeFreeEncoder(enc);
	}
	if (!crtc_id) {
		fprintf(stderr, "FAIL: no usable crtc for connector\n");
		return 1;
	}
	printf("crtc: id=%u\n", crtc_id);

	uint32_t crtc_bit = 0;
	for (int b = 0; b < res->count_crtcs; b++)
		if (res->crtcs[b] == crtc_id)
			crtc_bit = 1u << b;

	uint32_t plane_id = 0;
	drmModePlaneRes *planes = drmModeGetPlaneResources(fd);
	for (uint32_t i = 0; planes && i < planes->count_planes && !plane_id; i++) {
		drmModePlane *pl = drmModeGetPlane(fd, planes->planes[i]);

		if (!pl)
			continue;
		uint64_t type = 0;

		prop_id(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type);
		if ((pl->possible_crtcs & crtc_bit) && type == DRM_PLANE_TYPE_PRIMARY)
			plane_id = pl->plane_id;
		drmModeFreePlane(pl);
	}
	if (planes)
		drmModeFreePlaneResources(planes);
	if (!plane_id) {
		fprintf(stderr, "FAIL: no primary plane for crtc\n");
		return 1;
	}
	printf("plane: id=%u (primary)\n", plane_id);

	struct drm_mode_create_dumb creq = {
		.width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32,
	};
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
		fprintf(stderr, "FAIL: create dumb buffer\n");
		return 1;
	}

	uint32_t handles[4] = { creq.handle, 0, 0, 0 };
	uint32_t pitches[4] = { creq.pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint64_t mods[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };
	uint32_t fb_id = 0;
	if (drmModeAddFB2WithModifiers(fd, mode.hdisplay, mode.vdisplay,
				       DRM_FORMAT_XRGB8888, handles, pitches,
				       offsets, mods, &fb_id,
				       DRM_MODE_FB_MODIFIERS)) {
		fprintf(stderr, "FAIL: addFB2 (LINEAR XRGB8888)\n");
		return 1;
	}

	struct drm_mode_map_dumb mreq = { .handle = creq.handle };
	if (!drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
		void *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, mreq.offset);

		if (map != MAP_FAILED) {
			for (uint32_t y = 0; y < mode.vdisplay; y++) {
				uint32_t *row = (uint32_t *)((char *)map + y * creq.pitch);

				for (uint32_t x = 0; x < mode.hdisplay; x++)
					row[x] = ((x ^ y) & 0xff) ? 0xff1a99e6 : 0xff000000;
			}
			munmap(map, creq.size);
		}
	}

	uint32_t blob_id = 0;
	if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob_id)) {
		fprintf(stderr, "FAIL: create mode blob\n");
		return 1;
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	set_prop(fd, req, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id);
	set_prop(fd, req, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob_id);
	set_prop(fd, req, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", fb_id);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)mode.hdisplay << 16);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)mode.vdisplay << 16);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", mode.hdisplay);
	set_prop(fd, req, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", mode.vdisplay);
	if (g_fail)
		return 1;

	int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
	if (ret) {
		fprintf(stderr, "FAIL: atomic modeset commit (%d)\n", ret);
		return 1;
	}
	printf("modeset: committed %ux%u onto crtc %u\n",
	       mode.hdisplay, mode.vdisplay, crtc_id);

	uint64_t live_fb = 0;
	prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", &live_fb);
	if (live_fb != fb_id) {
		fprintf(stderr, "FAIL: live plane FB (%lu) != committed FB (%u)\n",
			(unsigned long)live_fb, fb_id);
		return 1;
	}

	drmModeFB2 *fb = drmModeGetFB2(fd, (uint32_t)live_fb);
	if (!fb) {
		fprintf(stderr, "FAIL: drmModeGetFB2 (the kmsgrab capture call)\n");
		return 1;
	}
	printf("capture (GetFB2): %ux%u fourcc=%.4s modifier=0x%llx handle=%u pitch=%u\n",
	       fb->width, fb->height, (char *)&fb->pixel_format,
	       (unsigned long long)fb->modifier, fb->handles[0], fb->pitches[0]);

	int dmabuf = -1;
	if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &dmabuf) ||
	    dmabuf < 0) {
		fprintf(stderr, "FAIL: PRIME export of scanout FB (the encoder handoff)\n");
		drmModeFreeFB2(fb);
		return 1;
	}
	printf("PRIME export: dmabuf fd=%d (handed to vaapi/nvenc/vulkan in real use)\n",
	       dmabuf);

	int ok = (fb->width == mode.hdisplay && fb->height == mode.vdisplay &&
		  fb->modifier == DRM_FORMAT_MOD_LINEAR &&
		  fb->pixel_format == DRM_FORMAT_XRGB8888);
	close(dmabuf);
	drmModeFreeFB2(fb);

	if (!ok) {
		fprintf(stderr, "FAIL: captured FB attrs mismatch requested mode/format\n");
		return 1;
	}

	printf("\nPASS: scanout live at %ux%u; FB captured (LINEAR XRGB8888) + PRIME-exported on %s\n",
	       mode.hdisplay, mode.vdisplay, path);
	return 0;
}
