// SPDX-License-Identifier: GPL-2.0
/*
 * prime_import_probe -- step 2: prove lvda scans out a GPU-allocated buffer.
 *
 * Pre-req: a monitor must be connected first (lvda-ctl up ...) and no
 * compositor may hold the lvda card (the probe becomes DRM master).
 *
 * This does what a compositor's zero-copy multi-GPU path would do:
 *   1. allocate a LINEAR gbm_bo on a real render GPU (amdgpu/nvidia/...)
 *   2. export it as a dma-buf (gbm_bo_get_fd)
 *   3. PRIME-import the dma-buf into lvda  <- exercises dma_buf attach + map
 *   4. addFB2 LINEAR over the imported handle
 *   5. atomic-modeset it onto the connected virtual connector
 *   6. drmModeGetFB2 + PRIME re-export      <- kmsgrab's capture sequence
 *   7. compare dma-buf inodes: the re-export must hand back the ORIGINAL
 *      render-GPU buffer (same struct dma_buf), proving zero-copy identity
 *      render -> scanout -> capture -> encoder.
 *
 * PASS == a render-GPU buffer is live on the lvda plane AND the captured
 * dma-buf IS the render-GPU buffer (same inode), end to end.
 *
 * Build: cc prime_import_probe.c -o prime_import_probe \
 *            $(pkg-config --cflags --libs libdrm gbm)
 * Run:   sudo ./prime_import_probe [/dev/dri/cardN] [/dev/dri/renderDN]
 *        (auto-finds the lvda card and the first render node)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gbm.h>
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

static int open_render(char *out, size_t n)
{
	for (int i = 128; i < 192; i++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);

		if (fd < 0)
			continue;

		drmVersionPtr v = drmGetVersion(fd);

		if (v) {
			printf("render: %s (%s)\n", path, v->name);
			drmFreeVersion(v);
		}
		snprintf(out, n, "%s", path);
		return fd;
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
	char card_path[64] = {0}, render_path[64] = {0};

	for (int i = 1; i < argc; i++) {
		if (strstr(argv[i], "renderD"))
			snprintf(render_path, sizeof(render_path), "%s", argv[i]);
		else
			snprintf(card_path, sizeof(card_path), "%s", argv[i]);
	}

	int fd = card_path[0] ? open(card_path, O_RDWR | O_CLOEXEC)
			      : open_lvda(card_path, sizeof(card_path));
	if (fd < 0) {
		fprintf(stderr, "FAIL: no lvda card (modprobe lvda + lvda-ctl up?)\n");
		return 1;
	}
	printf("card: %s\n", card_path);

	int rfd = render_path[0] ? open(render_path, O_RDWR | O_CLOEXEC)
				 : open_render(render_path, sizeof(render_path));
	if (rfd < 0) {
		fprintf(stderr, "SKIP: no render node (no real GPU on this host)\n");
		return 0;
	}

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
	printf("mode: %ux%u@%uHz\n", mode.hdisplay, mode.vdisplay, mode.vrefresh);

	struct gbm_device *gbm = gbm_create_device(rfd);
	if (!gbm) {
		fprintf(stderr, "FAIL: gbm_create_device on %s\n", render_path);
		return 1;
	}

	struct gbm_bo *bo = gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay,
					  GBM_FORMAT_XRGB8888,
					  GBM_BO_USE_LINEAR);
	if (!bo) {
		fprintf(stderr, "FAIL: gbm_bo_create LINEAR %ux%u on render GPU\n",
			mode.hdisplay, mode.vdisplay);
		return 1;
	}
	uint32_t pitch = gbm_bo_get_stride(bo);

	uint32_t map_pitch = 0;
	void *map_data = NULL;
	void *map = gbm_bo_map(bo, 0, 0, mode.hdisplay, mode.vdisplay,
			       GBM_BO_TRANSFER_WRITE, &map_pitch, &map_data);
	if (map) {
		for (uint32_t y = 0; y < mode.vdisplay; y++) {
			uint32_t *row = (uint32_t *)((char *)map + y * map_pitch);

			for (uint32_t x = 0; x < mode.hdisplay; x++)
				row[x] = ((x ^ y) & 0xff) ? 0xffe6991a : 0xff000000;
		}
		gbm_bo_unmap(bo, map_data);
	}

	int dmabuf = gbm_bo_get_fd(bo);
	if (dmabuf < 0) {
		fprintf(stderr, "FAIL: gbm_bo_get_fd (render GPU dma-buf export)\n");
		return 1;
	}
	printf("export: render-GPU bo %ux%u pitch=%u -> dmabuf fd=%d\n",
	       mode.hdisplay, mode.vdisplay, pitch, dmabuf);

	uint32_t handle = 0;
	if (drmPrimeFDToHandle(fd, dmabuf, &handle)) {
		fprintf(stderr, "FAIL: PRIME import into lvda (errno=%d %s)\n",
			errno, strerror(errno));
		return 1;
	}
	printf("import: lvda gem handle=%u\n", handle);

	uint32_t handles[4] = { handle, 0, 0, 0 };
	uint32_t pitches[4] = { pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint64_t mods[4] = { DRM_FORMAT_MOD_LINEAR, 0, 0, 0 };
	uint32_t fb_id = 0;
	if (drmModeAddFB2WithModifiers(fd, mode.hdisplay, mode.vdisplay,
				       DRM_FORMAT_XRGB8888, handles, pitches,
				       offsets, mods, &fb_id,
				       DRM_MODE_FB_MODIFIERS)) {
		fprintf(stderr, "FAIL: addFB2 over imported handle (errno=%d %s)\n",
			errno, strerror(errno));
		return 1;
	}
	printf("addFB2: fb=%u (LINEAR XRGB8888, imported)\n", fb_id);

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
	if (ret == -EACCES || ret == -EPERM) {
		printf("\nPARTIAL PASS: import + addFB2 of a render-GPU buffer "
		       "work, but a compositor holds DRM master so the scanout "
		       "commit was skipped\n");
		return 0;
	}
	if (ret) {
		fprintf(stderr, "FAIL: atomic modeset commit of imported FB (%d)\n", ret);
		return 1;
	}
	printf("modeset: imported FB live on crtc %u\n", crtc_id);

	drmModeFB2 *fb = drmModeGetFB2(fd, fb_id);
	if (!fb) {
		fprintf(stderr, "FAIL: drmModeGetFB2 (the kmsgrab capture call)\n");
		return 1;
	}

	int reexport = -1;
	if (drmPrimeHandleToFD(fd, fb->handles[0], DRM_CLOEXEC | DRM_RDWR, &reexport) ||
	    reexport < 0) {
		fprintf(stderr, "FAIL: PRIME re-export of imported scanout FB\n");
		drmModeFreeFB2(fb);
		return 1;
	}

	struct stat orig_st, reexp_st;
	if (fstat(dmabuf, &orig_st) || fstat(reexport, &reexp_st)) {
		fprintf(stderr, "FAIL: fstat dma-buf fds\n");
		return 1;
	}
	printf("capture: GetFB2 modifier=0x%llx; re-export inode=%llu original inode=%llu\n",
	       (unsigned long long)fb->modifier,
	       (unsigned long long)reexp_st.st_ino,
	       (unsigned long long)orig_st.st_ino);

	int ok = (orig_st.st_ino == reexp_st.st_ino &&
		  fb->modifier == DRM_FORMAT_MOD_LINEAR &&
		  fb->width == mode.hdisplay && fb->height == mode.vdisplay);
	close(reexport);
	drmModeFreeFB2(fb);
	if (!ok) {
		fprintf(stderr, "FAIL: captured dma-buf is not the render-GPU buffer\n");
		return 1;
	}

	printf("\nPASS: render-GPU buffer imported, scanned out at %ux%u, and "
	       "captured as the SAME dma-buf (zero-copy) on %s\n",
	       mode.hdisplay, mode.vdisplay, card_path);
	return 0;
}
