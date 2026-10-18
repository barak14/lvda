/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Presentation events: LVDA_DRM_WAIT_PRESENT and LVDA_DRM_GET_FRAME.
 *
 *   - before any modeset, GET_FRAME reports EMPTY;
 *   - after a modeset it exports the presented framebuffer, and the DMA-BUF
 *     is the same buffer the client scanned out (same dma-buf inode);
 *   - a blocked WAIT_PRESENT wakes when a queued page flip completes, and
 *     the frame then exported is the flipped-to buffer;
 *   - with no commits, WAIT_PRESENT times out after ~timeout_ms;
 *   - a buffer PRIME-imported from another card (vgem in the VM) is exported
 *     as its original DMA-BUF (zero-copy);
 *   - bad input is rejected; turning the CRTC off makes GET_FRAME EMPTY.
 *
 * Needs root (CAP_SYS_ADMIN, DRM master). Skips with success otherwise.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

#include "../../uapi/lvda.h"

static int fd = -1;
static int failures;
static uint32_t crtc_id, plane_id, conn_id;
static drmModeModeInfo mode;

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, type);
	uint32_t found = 0;

	for (uint32_t i = 0; props && i < props->count_props && !found; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);

		if (p && strcmp(p->name, name) == 0)
			found = props->props[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static uint64_t prop_val(uint32_t obj, uint32_t type, const char *name)
{
	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(fd, obj, type);
	uint64_t val = 0;

	for (uint32_t i = 0; props && i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(fd, props->props[i]);

		if (p && strcmp(p->name, name) == 0)
			val = props->prop_values[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return val;
}

static ino_t inode_of(int f)
{
	struct stat st;

	return fstat(f, &st) == 0 ? st.st_ino : 0;
}

static int get_frame(struct lvda_frame *fr)
{
	memset(fr, 0, sizeof(*fr));
	fr->crtc_id = crtc_id;
	return ioctl(fd, DRM_IOCTL_LVDA_GET_FRAME, fr);
}

static void close_frame(struct lvda_frame *fr)
{
	for (unsigned int i = 0; i < LVDA_FRAME_MAX_PLANES; i++)
		if (fr->dma_buf_fds[i] >= 0)
			close(fr->dma_buf_fds[i]);
}

static int wait_present(uint64_t seq, uint32_t timeout_ms,
			struct lvda_wait_present *w)
{
	memset(w, 0, sizeof(*w));
	w->crtc_id = crtc_id;
	w->sequence = seq;
	w->timeout_ms = timeout_ms;
	return ioctl(fd, DRM_IOCTL_LVDA_WAIT_PRESENT, w);
}

/* Dumb buffer on card `cfd` with an fb on the lvda card (via PRIME when
 * cfd is foreign). Returns the fb id and the buffer's own DMA-BUF fd. */
static uint32_t make_fb(int cfd, int *dmabuf_out)
{
	uint32_t handle, pitch, lvda_handle;
	uint64_t size;
	int dmabuf;

	if (drmModeCreateDumbBuffer(cfd, mode.hdisplay, mode.vdisplay, 32, 0,
				    &handle, &pitch, &size))
		return 0;
	if (drmPrimeHandleToFD(cfd, handle, DRM_CLOEXEC | DRM_RDWR, &dmabuf))
		return 0;
	if (cfd == fd)
		lvda_handle = handle;
	else if (drmPrimeFDToHandle(fd, dmabuf, &lvda_handle))
		return 0;

	uint32_t handles[4] = { lvda_handle }, pitches[4] = { pitch };
	uint32_t offsets[4] = { 0 }, fb = 0;

	if (drmModeAddFB2(fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &fb, 0))
		return 0;
	*dmabuf_out = dmabuf;
	return fb;
}

static int commit_fb(uint32_t fb, uint32_t flags)
{
	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	int ret;

	drmModeAtomicAddProperty(req, plane_id,
				 prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID"), fb);
	ret = drmModeAtomicCommit(fd, req, flags, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static int modeset(uint32_t fb, int on)
{
	uint32_t blob = 0;
	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	int ret;

	if (on)
		drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob);
	struct { uint32_t obj, type; const char *name; uint64_t val; } set[] = {
		{ conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", on ? crtc_id : 0 },
		{ crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", blob },
		{ crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", on },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", on ? fb : 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", on ? crtc_id : 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", on ? (uint64_t)mode.hdisplay << 16 : 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", on ? (uint64_t)mode.vdisplay << 16 : 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", on ? mode.hdisplay : 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", on ? mode.vdisplay : 0 },
	};

	for (size_t i = 0; i < sizeof(set) / sizeof(set[0]); i++)
		drmModeAtomicAddProperty(req, set[i].obj,
					 prop_id(set[i].obj, set[i].type, set[i].name),
					 set[i].val);
	ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static int open_foreign(unsigned int lvda_minor)
{
	for (int i = 0; i < 64; i++) {
		char path[32];
		int f;

		if ((unsigned int)i == lvda_minor)
			continue;
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		f = open(path, O_RDWR | O_CLOEXEC);
		if (f < 0)
			continue;
		drmVersionPtr v = drmGetVersion(f);
		int lvda = v && v->name && strcmp(v->name, "lvda") == 0;

		drmFreeVersion(v);
		if (!lvda)
			return f;
		close(f);
	}
	return -1;
}

static int skip(const char *why)
{
	printf("SKIP: present_frame: %s\n", why);
	return EXIT_SUCCESS;
}

int main(void)
{
	int lvda_fd = open("/dev/lvda", O_RDWR);
	struct lvda_add add;
	struct lvda_frame fr;
	struct lvda_wait_present w;
	char card[64], msg[160];

	if (lvda_fd < 0)
		return skip("/dev/lvda absent");
	memset(&add, 0, sizeof(add));
	add.width = 1280;
	add.height = 720;
	add.refresh_mhz = 60000;
	if (ioctl(lvda_fd, LVDA_IOC_ADD, &add) < 0) {
		fprintf(stderr, "LVDA_IOC_ADD: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	snprintf(card, sizeof(card), "/dev/dri/card%u", add.drm_card_minor);
	fd = open(card, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return skip("cannot open DRM card");
	if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
		return skip("atomic caps unavailable");

	drmModeResPtr res = drmModeGetResources(fd);

	for (int i = 0; res && i < res->count_connectors && !conn_id; i++) {
		drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);

		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			conn_id = c->connector_id;
			mode = c->modes[0];
		}
		drmModeFreeConnector(c);
	}
	if (!conn_id)
		return skip("no connected connector");
	crtc_id = res->crtcs[0];
	drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);

	for (uint32_t i = 0; planes && i < planes->count_planes && !plane_id; i++)
		if (prop_val(planes->planes[i], DRM_MODE_OBJECT_PLANE, "type") ==
		    DRM_PLANE_TYPE_PRIMARY)
			plane_id = planes->planes[i];
	if (!plane_id)
		return skip("no primary plane");

	/* --- before any modeset -------------------------------------- */
	if (wait_present(0, 0, &w)) {
		if (errno == EACCES || errno == EPERM)
			return skip("CAP_SYS_ADMIN required");
		fprintf(stderr, "WAIT_PRESENT: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	uint64_t seq0 = w.sequence;

	check(w.flags == (seq0 ? LVDA_PRESENT_CHANGED : LVDA_PRESENT_TIMEOUT),
	      "query reports CHANGED iff the sequence differs");
	check(get_frame(&fr) == 0 && fr.flags == LVDA_FRAME_EMPTY &&
	      fr.dma_buf_fds[0] == -1,
	      "GET_FRAME before modeset is EMPTY");

	/* --- input validation ---------------------------------------- */
	memset(&w, 0, sizeof(w));
	w.crtc_id = crtc_id;
	w.reserved = 1;
	check(ioctl(fd, DRM_IOCTL_LVDA_WAIT_PRESENT, &w) < 0 && errno == EINVAL,
	      "WAIT_PRESENT rejects nonzero reserved");
	memset(&w, 0, sizeof(w));
	w.crtc_id = crtc_id;
	w.timeout_ms = LVDA_PRESENT_MAX_TIMEOUT_MS + 1;
	check(ioctl(fd, DRM_IOCTL_LVDA_WAIT_PRESENT, &w) < 0 && errno == EINVAL,
	      "WAIT_PRESENT rejects timeout above the maximum");
	memset(&w, 0, sizeof(w));
	w.crtc_id = 0xdeadbeef;
	check(ioctl(fd, DRM_IOCTL_LVDA_WAIT_PRESENT, &w) < 0 && errno == ENOENT,
	      "WAIT_PRESENT rejects an unknown CRTC");
	memset(&fr, 0, sizeof(fr));
	fr.crtc_id = crtc_id;
	fr.reserved[3] = 1;
	check(ioctl(fd, DRM_IOCTL_LVDA_GET_FRAME, &fr) < 0 && errno == EINVAL,
	      "GET_FRAME rejects nonzero reserved");

	/* --- modeset: frame exported, same buffer -------------------- */
	int dmabuf[2];
	uint32_t fb[2] = { make_fb(fd, &dmabuf[0]), make_fb(fd, &dmabuf[1]) };

	if (!fb[0] || !fb[1])
		return skip("dumb buffers unavailable (no DRM master)");
	if (modeset(fb[0], 1)) {
		fprintf(stderr, "modeset: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	check(get_frame(&fr) == 0 && fr.flags == LVDA_FRAME_READY &&
	      fr.sequence > seq0,
	      "after modeset GET_FRAME is READY with a newer sequence");
	check(fr.width == mode.hdisplay && fr.height == mode.vdisplay &&
	      fr.fourcc == DRM_FORMAT_XRGB8888 && fr.plane_count == 1 &&
	      fr.pitches[0] >= mode.hdisplay * 4u && fr.dma_buf_fds[1] == -1,
	      "frame layout matches the framebuffer");
	check(fr.modifier == DRM_FORMAT_MOD_INVALID,
	      "fb added without modifiers reports DRM_FORMAT_MOD_INVALID");
	check(fr.dma_buf_fds[0] >= 0 &&
	      inode_of(fr.dma_buf_fds[0]) == inode_of(dmabuf[0]),
	      "exported DMA-BUF is the scanned-out buffer");
	uint64_t seq1 = fr.sequence;

	close_frame(&fr);

	/* --- a queued flip wakes a blocked waiter -------------------- */
	check(commit_fb(fb[1], DRM_MODE_ATOMIC_NONBLOCK) == 0,
	      "nonblocking flip queued");
	double t0 = now_ms();

	check(wait_present(seq1, 1000, &w) == 0 &&
	      w.flags == LVDA_PRESENT_CHANGED && w.sequence == seq1 + 1,
	      "WAIT_PRESENT wakes on the flip, sequence +1");
	snprintf(msg, sizeof(msg), "woke within one refresh (%.2f ms)", now_ms() - t0);
	check(now_ms() - t0 < 40.0, msg);
	check(w.timestamp_ns > 0, "presentation timestamp set");
	check(get_frame(&fr) == 0 && fr.flags == LVDA_FRAME_READY &&
	      fr.sequence == w.sequence &&
	      inode_of(fr.dma_buf_fds[0]) == inode_of(dmabuf[1]),
	      "GET_FRAME after the wake exports the flipped-to buffer");
	close_frame(&fr);

	/* --- no commits: timeout ------------------------------------- */
	t0 = now_ms();
	check(wait_present(w.sequence, 50, &w) == 0 &&
	      w.flags == LVDA_PRESENT_TIMEOUT,
	      "WAIT_PRESENT times out with no commits");
	snprintf(msg, sizeof(msg), "timeout honoured (%.2f ms >= 40)", now_ms() - t0);
	check(now_ms() - t0 >= 40.0, msg);

	/* --- PRIME-imported buffer exports its original DMA-BUF ------ */
	int foreign = open_foreign(add.drm_card_minor);

	if (foreign < 0) {
		printf("skip: no foreign card for the PRIME check (modprobe vgem?)\n");
	} else {
		int vdmabuf;
		uint32_t vfb = make_fb(foreign, &vdmabuf);

		check(vfb && commit_fb(vfb, 0) == 0, "flip to a vgem-imported fb");
		check(get_frame(&fr) == 0 && fr.flags == LVDA_FRAME_READY &&
		      inode_of(fr.dma_buf_fds[0]) == inode_of(vdmabuf),
		      "imported fb is exported as the exporter's own DMA-BUF");
		close_frame(&fr);
	}

	/* --- CRTC off: EMPTY ----------------------------------------- */
	check(modeset(0, 0) == 0, "CRTC disabled");
	check(get_frame(&fr) == 0 && fr.flags == LVDA_FRAME_EMPTY,
	      "GET_FRAME with the CRTC off is EMPTY");

	printf("%s: present_frame (%d failure(s))\n",
	       failures ? "FAIL" : "PASS", failures);
	close(fd);
	close(lvda_fd);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
