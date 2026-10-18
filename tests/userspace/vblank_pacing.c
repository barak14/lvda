/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vblank pacing: ADD a 100 Hz monitor, modeset it as DRM master, and time
 * atomic page flips to check the emulated vblank.
 *
 *   VRR off: drmWaitVBlank works, back-to-back flips complete once per mode
 *            period, and a flip issued mid-period waits for the next tick.
 *   VRR on:  back-to-back flips are capped at the mode period, a flip issued
 *            after a full idle period completes immediately, and every flip
 *            advances the vblank sequence by exactly one.
 *   VRR off again: the periodic vblank resumes.
 *
 * Skips with success when /dev/lvda, the lvda card, or DRM master is
 * unavailable. Timing bounds are loose enough for a virtme-ng guest.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

#include "../../uapi/lvda.h"

#define FLIPS 30
#define DELAYED_FLIPS 11

static int drm_fd = -1;
static uint32_t crtc_id, plane_id, fb_ids[2];
static uint32_t p_fb, p_vrr;
static double period_ms;
static int failures;

/* Last flip event, filled by the page-flip handler. */
static unsigned int ev_seq;
static int ev_done;

static double now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void sleep_ms(double ms)
{
	struct timespec ts = { .tv_sec = (time_t)(ms / 1e3),
			       .tv_nsec = (long)((ms - (time_t)(ms / 1e3) * 1e3) * 1e6) };

	nanosleep(&ts, NULL);
}

static uint32_t prop_id(uint32_t obj_id, uint32_t obj_type, const char *name)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
	uint32_t found = 0;

	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !found; i++) {
		drmModePropertyPtr p = drmModeGetProperty(drm_fd, props->props[i]);

		if (!p)
			continue;
		if (strcmp(p->name, name) == 0)
			found = props->props[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return found;
}

static uint64_t prop_val(uint32_t obj_id, uint32_t obj_type, const char *name)
{
	drmModeObjectPropertiesPtr props =
		drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
	uint64_t val = 0;

	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props; i++) {
		drmModePropertyPtr p = drmModeGetProperty(drm_fd, props->props[i]);

		if (!p)
			continue;
		if (strcmp(p->name, name) == 0)
			val = props->prop_values[i];
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return val;
}

static void flip_handler(int fd, unsigned int sequence, unsigned int sec,
			 unsigned int usec, unsigned int crtc, void *data)
{
	(void)fd; (void)sec; (void)usec; (void)crtc; (void)data;
	ev_seq = sequence;
	ev_done = 1;
}

/* Wait for the pending flip event. Returns 0, or -1 on a 1 s timeout. */
static int wait_flip(void)
{
	drmEventContext ctx = { .version = 3,
				.page_flip_handler2 = flip_handler };
	struct pollfd pfd = { .fd = drm_fd, .events = POLLIN };

	while (!ev_done) {
		if (poll(&pfd, 1, 1000) <= 0)
			return -1;
		drmHandleEvent(drm_fd, &ctx);
	}
	return 0;
}

/* Queue a nonblocking flip to fb_ids[idx & 1]. */
static int flip(unsigned int idx)
{
	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	int ret;

	drmModeAtomicAddProperty(req, plane_id, p_fb, fb_ids[idx & 1]);
	ev_done = 0;
	ret = drmModeAtomicCommit(drm_fd, req,
				  DRM_MODE_PAGE_FLIP_EVENT |
					  DRM_MODE_ATOMIC_NONBLOCK, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static int set_vrr(int on)
{
	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	int ret;

	drmModeAtomicAddProperty(req, crtc_id, p_vrr, on);
	ret = drmModeAtomicCommit(drm_fd, req, 0, NULL);
	drmModeAtomicFree(req);
	return ret;
}

static void check(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		failures++;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

/* Back-to-back flips: each queued as soon as the previous one completes.
 * Reports intervals between completions and sequence deltas. */
static int run_back_to_back(double *min_iv, double *mean_iv, int *bad_seq)
{
	double prev = 0, sum = 0;
	unsigned int prev_seq = 0;

	*min_iv = 1e9;
	*bad_seq = 0;
	for (unsigned int i = 0; i <= FLIPS; i++) {
		if (flip(i) || wait_flip())
			return -1;
		double t = now_ms();

		if (i) {
			double iv = t - prev;

			sum += iv;
			if (iv < *min_iv)
				*min_iv = iv;
			if (ev_seq - prev_seq != 1)
				(*bad_seq)++;
		}
		prev = t;
		prev_seq = ev_seq;
	}
	*mean_iv = sum / FLIPS;
	return 0;
}

/* Flips queued 1.5 periods after the previous completion. Reports the median
 * queue-to-completion latency and the sequence deltas seen. */
static int run_delayed(double *median_lat, int *seq_delta_min,
		       int *seq_delta_max)
{
	double lat[DELAYED_FLIPS];
	unsigned int prev_seq = 0;

	*seq_delta_min = 1 << 30;
	*seq_delta_max = 0;
	if (flip(0) || wait_flip())
		return -1;
	prev_seq = ev_seq;
	for (unsigned int i = 0; i < DELAYED_FLIPS; i++) {
		sleep_ms(1.5 * period_ms);
		double t0 = now_ms();

		if (flip(i + 1) || wait_flip())
			return -1;
		lat[i] = now_ms() - t0;

		int d = (int)(ev_seq - prev_seq);

		if (d < *seq_delta_min)
			*seq_delta_min = d;
		if (d > *seq_delta_max)
			*seq_delta_max = d;
		prev_seq = ev_seq;
	}
	qsort(lat, DELAYED_FLIPS, sizeof(lat[0]), cmp_double);
	*median_lat = lat[DELAYED_FLIPS / 2];
	return 0;
}

static int wait_vblank_ok(void)
{
	drmVBlank vbl = { .request = { .type = DRM_VBLANK_RELATIVE,
				       .sequence = 1 } };

	return drmWaitVBlank(drm_fd, &vbl) == 0;
}

static int skip(const char *why)
{
	printf("SKIP: vblank_pacing: %s\n", why);
	return EXIT_SUCCESS;
}

int main(void)
{
	int lvda_fd = open("/dev/lvda", O_RDWR);

	if (lvda_fd < 0)
		return skip("/dev/lvda absent");

	struct lvda_add add;

	memset(&add, 0, sizeof(add));
	add.width = 1280;
	add.height = 720;
	add.refresh_mhz = 100000;
	if (ioctl(lvda_fd, LVDA_IOC_ADD, &add) < 0) {
		fprintf(stderr, "LVDA_IOC_ADD: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	char card[64];

	snprintf(card, sizeof(card), "/dev/dri/card%u",
		 (unsigned)add.drm_card_minor);
	drm_fd = open(card, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0)
		return skip("cannot open DRM card");
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
	    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1))
		return skip("atomic caps unavailable");

	drmModeResPtr res = drmModeGetResources(drm_fd);
	drmModeModeInfo mode;
	uint32_t conn_id = 0;

	for (int i = 0; res && i < res->count_connectors && !conn_id; i++) {
		drmModeConnectorPtr c = drmModeGetConnector(drm_fd,
							    res->connectors[i]);

		if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) {
			conn_id = c->connector_id;
			mode = c->modes[0];
			for (int m = 0; m < c->count_modes; m++)
				if (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
					mode = c->modes[m];
					break;
				}
		}
		drmModeFreeConnector(c);
	}
	if (!conn_id)
		return skip("no connected connector");
	crtc_id = res->crtcs[0];

	drmModePlaneResPtr planes = drmModeGetPlaneResources(drm_fd);

	for (uint32_t i = 0; planes && i < planes->count_planes; i++)
		if (prop_val(planes->planes[i], DRM_MODE_OBJECT_PLANE, "type") ==
		    DRM_PLANE_TYPE_PRIMARY) {
			plane_id = planes->planes[i];
			break;
		}
	if (!plane_id)
		return skip("no primary plane");

	period_ms = (double)mode.htotal * mode.vtotal / mode.clock;
	printf("mode %ux%u, period %.3f ms\n", mode.hdisplay, mode.vdisplay,
	       period_ms);

	for (int i = 0; i < 2; i++) {
		uint32_t handle, pitch;
		uint64_t size;
		uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

		if (drmModeCreateDumbBuffer(drm_fd, mode.hdisplay, mode.vdisplay,
					    32, 0, &handle, &pitch, &size))
			return skip("dumb buffers unavailable (no DRM master)");
		handles[0] = handle;
		pitches[0] = pitch;
		if (drmModeAddFB2(drm_fd, mode.hdisplay, mode.vdisplay,
				  DRM_FORMAT_XRGB8888, handles, pitches,
				  offsets, &fb_ids[i], 0)) {
			fprintf(stderr, "AddFB2: %s\n", strerror(errno));
			return EXIT_FAILURE;
		}
	}

	uint32_t mode_blob;

	drmModeCreatePropertyBlob(drm_fd, &mode, sizeof(mode), &mode_blob);
	p_fb = prop_id(plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
	p_vrr = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED");
	if (!p_fb || !p_vrr) {
		fprintf(stderr, "missing FB_ID or VRR_ENABLED property\n");
		return EXIT_FAILURE;
	}

	drmModeAtomicReqPtr req = drmModeAtomicAlloc();
	struct { uint32_t obj, type; const char *name; uint64_t val; } set[] = {
		{ conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", crtc_id },
		{ crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", mode_blob },
		{ crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1 },
		{ crtc_id, DRM_MODE_OBJECT_CRTC, "VRR_ENABLED", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", fb_ids[0] },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", crtc_id },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)mode.hdisplay << 16 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)mode.vdisplay << 16 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0 },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", mode.hdisplay },
		{ plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", mode.vdisplay },
	};

	for (size_t i = 0; i < sizeof(set) / sizeof(set[0]); i++)
		drmModeAtomicAddProperty(req, set[i].obj,
					 prop_id(set[i].obj, set[i].type,
						 set[i].name),
					 set[i].val);
	if (drmModeAtomicCommit(drm_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET,
				NULL)) {
		int e = errno;

		if (e == EACCES || e == EPERM)
			return skip("atomic modeset not permitted");
		fprintf(stderr, "modeset: %s\n", strerror(e));
		return EXIT_FAILURE;
	}
	drmModeAtomicFree(req);

	double min_iv, mean_iv, lat;
	int bad_seq, dmin, dmax;
	char msg[160];

	/* --- VRR off: fixed-rate vblank ------------------------------- */
	check(wait_vblank_ok(), "VRR off: drmWaitVBlank supported");

	if (run_back_to_back(&min_iv, &mean_iv, &bad_seq))
		return check(0, "VRR off: back-to-back flips completed"), EXIT_FAILURE;
	snprintf(msg, sizeof(msg),
		 "VRR off: back-to-back mean interval %.2f ms within 15%% of period",
		 mean_iv);
	check(mean_iv > 0.85 * period_ms && mean_iv < 1.15 * period_ms, msg);
	snprintf(msg, sizeof(msg), "VRR off: %d/%d sequence steps != 1 (<= 3)",
		 bad_seq, FLIPS);
	check(bad_seq <= 3, msg);

	if (run_delayed(&lat, &dmin, &dmax))
		return check(0, "VRR off: delayed flips completed"), EXIT_FAILURE;
	snprintf(msg, sizeof(msg),
		 "VRR off: mid-period flip waits for the tick (median %.2f ms >= 0.3 period)",
		 lat);
	check(lat >= 0.3 * period_ms, msg);

	/* --- VRR on: one capped vblank per flip ----------------------- */
	check(set_vrr(1) == 0, "VRR on: VRR_ENABLED=1 committed");

	if (run_back_to_back(&min_iv, &mean_iv, &bad_seq))
		return check(0, "VRR on: back-to-back flips completed"), EXIT_FAILURE;
	snprintf(msg, sizeof(msg),
		 "VRR on: back-to-back capped (min interval %.2f ms >= 0.9 period)",
		 min_iv);
	check(min_iv >= 0.9 * period_ms, msg);
	snprintf(msg, sizeof(msg),
		 "VRR on: back-to-back mean interval %.2f ms <= 1.3 period", mean_iv);
	check(mean_iv <= 1.3 * period_ms, msg);
	snprintf(msg, sizeof(msg), "VRR on: %d/%d sequence steps != 1 (== 0)",
		 bad_seq, FLIPS);
	check(bad_seq == 0, msg);

	if (run_delayed(&lat, &dmin, &dmax))
		return check(0, "VRR on: delayed flips completed"), EXIT_FAILURE;
	snprintf(msg, sizeof(msg),
		 "VRR on: idle flip completes immediately (median %.2f ms <= 0.2 period)",
		 lat);
	check(lat <= 0.2 * period_ms, msg);
	snprintf(msg, sizeof(msg),
		 "VRR on: idle flips advance sequence by exactly 1 (saw %d..%d)",
		 dmin, dmax);
	check(dmin == 1 && dmax == 1, msg);

	/* --- VRR off again: periodic vblank restored ------------------ */
	check(set_vrr(0) == 0, "VRR off again: VRR_ENABLED=0 committed");
	check(wait_vblank_ok(), "VRR off again: drmWaitVBlank supported");
	if (run_back_to_back(&min_iv, &mean_iv, &bad_seq))
		return check(0, "VRR off again: flips completed"), EXIT_FAILURE;
	snprintf(msg, sizeof(msg),
		 "VRR off again: mean interval %.2f ms within 15%% of period",
		 mean_iv);
	check(mean_iv > 0.85 * period_ms && mean_iv < 1.15 * period_ms, msg);

	printf("%s: vblank_pacing (%d failure(s))\n",
	       failures ? "FAIL" : "PASS", failures);
	close(drm_fd);
	close(lvda_fd);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
