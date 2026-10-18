/* SPDX-License-Identifier: GPL-2.0 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_debugfs.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/dma-fence.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/capability.h>
#include <linux/fcntl.h>
#include <linux/fdtable.h>
#include <linux/jiffies.h>
#include <linux/wait.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_prime.h>

#include "lvda.h"
#include "lvda_edid.h"

/*
 * Linux 7.2 split the atomic transaction object: the KMS callbacks that used to
 * take `struct drm_atomic_state *` now take `struct drm_atomic_commit *`, and
 * `struct drm_atomic_state` no longer exists. Alias the transaction type so one
 * source tree builds against both the 6.x LTS and >=7.2 kernels.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
typedef struct drm_atomic_commit lvda_atomic_txn;
#else
typedef struct drm_atomic_state lvda_atomic_txn;
#endif

/*
 * Linux 7.0 added the DRM vblank-timer helpers. With them, lvda emulates a
 * real vblank (fixed-rate when VRR is off, one capped vblank per flip when VRR
 * is on). Older kernels keep the original fake-vblank behaviour, where every
 * flip completes immediately.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define LVDA_HAS_VBLANK_HELPER 1
#include <drm/drm_vblank.h>
#include <drm/drm_vblank_helper.h>
#else
#define LVDA_HAS_VBLANK_HELPER 0
#endif

/* EDID format constants in lvda_edid.h mirror <drm/drm_edid.h>; verify they
 * match. */
static_assert(LVDA_EDID_BLOCK == EDID_LENGTH);
static_assert(LVDA_EDID_EXT_CTA == CEA_EXT);
static_assert(LVDA_EDID_EXT_DISPLAYID == DISPLAYID_EXT);
static_assert(LVDA_EDID_DESC_RANGE == EDID_DETAIL_MONITOR_RANGE);
static_assert(LVDA_EDID_DESC_NAME == EDID_DETAIL_MONITOR_NAME);
static_assert(LVDA_EDID_DESC_LEN == sizeof(struct detailed_timing));
static_assert(LVDA_EDID_NAME_CHARS == sizeof_field(struct detailed_data_string, str));
static_assert(LVDA_EDID_INPUT_DIGITAL == DRM_EDID_INPUT_DIGITAL);
static_assert(LVDA_EDID_DEPTH_8 == DRM_EDID_DIGITAL_DEPTH_8);
static_assert(LVDA_EDID_DEPTH_10 == DRM_EDID_DIGITAL_DEPTH_10);
static_assert(LVDA_EDID_TYPE_DP == DRM_EDID_DIGITAL_TYPE_DP);
static_assert(LVDA_EDID_FEATURE_STANDBY == DRM_EDID_FEATURE_PM_STANDBY);
static_assert(LVDA_EDID_FEATURE_STD_COLOR == DRM_EDID_FEATURE_STANDARD_COLOR);
static_assert(LVDA_EDID_FEATURE_PREFERRED == DRM_EDID_FEATURE_PREFERRED_TIMING);
static_assert(LVDA_EDID_FEATURE_CONT_FREQ == DRM_EDID_FEATURE_CONTINUOUS_FREQ);
static_assert(LVDA_EDID_PT_HSYNC_POS == DRM_EDID_PT_HSYNC_POSITIVE);
static_assert(LVDA_EDID_PT_VSYNC_POS == DRM_EDID_PT_VSYNC_POSITIVE);
static_assert(LVDA_EDID_PT_SEPARATE_SYNC == DRM_EDID_PT_SEPARATE_SYNC);
static_assert(LVDA_EDID_RANGE_LIMITS_ONLY == DRM_EDID_RANGE_LIMITS_ONLY_FLAG);
static_assert(LVDA_EDID_RANGE_OFF_MAX_VFREQ == DRM_EDID_RANGE_OFFSET_MAX_VFREQ);
static_assert(LVDA_EDID_RANGE_OFF_MAX_HFREQ == DRM_EDID_RANGE_OFFSET_MAX_HFREQ);

/* One virtual monitor = one full {plane, CRTC, encoder, connector} chain. */
struct lvda_monitor {
	struct drm_plane primary;
	struct drm_plane cursor;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	u8 edid_bytes[LVDA_EDID_SIZE];
	u16 edid_len;
	bool active;
	void *owner;	/* /dev/lvda file that added it; NULL = free slot */
	u64 generation;
	/* Last requested mode, exposed read-only via debugfs. */
	u8 client_id[LVDA_CLIENT_ID_LEN];
	u32 width;
	u32 height;
	u32 refresh_mhz;
	u32 flags;
	/*
	 * Presentation tracking for LVDA_DRM_WAIT_PRESENT / LVDA_DRM_GET_FRAME.
	 * present_sequence advances once per completed commit touching this
	 * monitor; present_fb holds a reference to the newest presented
	 * primary framebuffer (NULL while the CRTC is off).
	 */
	spinlock_t present_lock;
	wait_queue_head_t present_waitq;
	atomic64_t present_sequence;
	u64 present_timestamp_ns;		/* under present_lock */
	struct drm_framebuffer *present_fb;	/* under present_lock */
#if LVDA_HAS_VBLANK_HELPER
	/* Serializes synthetic vblank generation (periodic and VRR timers). */
	spinlock_t vblank_lock;
	/* One-shot timer enforcing the mode's maximum refresh under VRR. */
	struct hrtimer vrr_hrtimer;
	/* Raw counter for get_vblank_counter: +1 per generated vblank. */
	atomic64_t synthetic_vblank_counter;
	/* CLOCK_MONOTONIC ns of the last generated vblank; 0 = none yet. */
	atomic64_t vrr_vblank_timestamp_ns;
#endif
};

struct lvda_device {
	struct drm_device drm;
	struct mutex lock;		/* serializes monitor slot state */
	unsigned int n_monitors;
	struct lvda_monitor monitors[];
};

#define to_lvda(dev)	  container_of(dev, struct lvda_device, drm)
#define to_monitor(conn)  container_of(conn, struct lvda_monitor, connector)
#define crtc_to_monitor(c) container_of(c, struct lvda_monitor, crtc)

/* The single persistent card. */
static struct lvda_device *lvda_card;

static const u32 lvda_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR2101010,
};

static const u32 lvda_cursor_formats[] = {
	DRM_FORMAT_ARGB8888,
};

/* Cursor max edge advertised to userspace via mode_config. */
#define LVDA_CURSOR_MAX 256u

static const u64 lvda_linear_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

/*
 * Extra format modifiers accepted for primary-plane scanout, injected at
 * load time (modprobe.d). lvda never reads pixels, so it can scan out any
 * layout the render GPU produces and the capture consumer can import; the
 * driver just parrots the list into IN_FORMATS so a compositor offers and
 * direct-scans tiled client buffers. Only single-plane layouts work: the
 * generic fb_create path rejects fbs with more planes than the base format
 * (e.g. AMD DCC with metadata planes). tests/host/scanout_modifiers_probe
 * emits a ready-made options line for the installed render GPU.
 */
#define LVDA_MAX_SCANOUT_MODIFIERS 63u

static unsigned long long lvda_scanout_modifiers[LVDA_MAX_SCANOUT_MODIFIERS];
static unsigned int lvda_scanout_modifiers_count;
module_param_array_named(scanout_modifiers, lvda_scanout_modifiers, ullong,
			 &lvda_scanout_modifiers_count, 0444);
MODULE_PARM_DESC(scanout_modifiers,
		 "Extra DRM format modifiers accepted for primary-plane scanout (single-plane layouts only)");

/* {LINEAR, deduped injected modifiers..., INVALID terminator}. */
static u64 lvda_primary_modifiers[LVDA_MAX_SCANOUT_MODIFIERS + 2];

static void lvda_build_primary_modifiers(void)
{
	unsigned int n = 0, i, j;

	lvda_primary_modifiers[n++] = DRM_FORMAT_MOD_LINEAR;

	for (i = 0; i < lvda_scanout_modifiers_count; i++) {
		u64 mod = lvda_scanout_modifiers[i];

		if (mod == DRM_FORMAT_MOD_LINEAR ||
		    mod == DRM_FORMAT_MOD_INVALID) {
			pr_warn("lvda: ignoring scanout_modifiers entry 0x%llx\n",
				mod);
			continue;
		}

		for (j = 0; j < n; j++) {
			if (lvda_primary_modifiers[j] == mod)
				break;
		}
		if (j == n)
			lvda_primary_modifiers[n++] = mod;
	}

	lvda_primary_modifiers[n] = DRM_FORMAT_MOD_INVALID;
}

static_assert(LVDA_DRM_IOCTL_BASE == DRM_IOCTL_BASE);
static_assert(LVDA_DRM_COMMAND_BASE == DRM_COMMAND_BASE);

/*
 * Publish a presentation on every monitor this commit touched. Runs from the
 * commit tail once the flip has completed, so the sequence advances when the
 * new frame is actually on screen (vblank tick, or VRR-capped flip).
 */
static void lvda_publish_presentations(lvda_atomic_txn *state)
{
	struct lvda_device *ldev = to_lvda(state->dev);
	unsigned int i;

	for (i = 0; i < ldev->n_monitors; i++) {
		struct lvda_monitor *mon = &ldev->monitors[i];
		struct drm_crtc_state *crtc_state =
			drm_atomic_get_new_crtc_state(state, &mon->crtc);
		struct drm_plane_state *primary =
			drm_atomic_get_new_plane_state(state, &mon->primary);
		struct drm_framebuffer *old_fb = NULL, *new_fb = NULL;
		bool update_fb = false;

		if (!crtc_state && !primary &&
		    !drm_atomic_get_new_plane_state(state, &mon->cursor))
			continue;

		if (crtc_state && !crtc_state->active) {
			update_fb = true;
		} else if (primary) {
			update_fb = true;
			if (primary->fb && primary->visible &&
			    primary->crtc == &mon->crtc) {
				new_fb = primary->fb;
				drm_framebuffer_get(new_fb);
			}
		}

		spin_lock(&mon->present_lock);
		if (update_fb) {
			old_fb = mon->present_fb;
			mon->present_fb = new_fb;
		}
		mon->present_timestamp_ns = ktime_get_ns();
		atomic64_inc(&mon->present_sequence);
		spin_unlock(&mon->present_lock);

		if (old_fb)
			drm_framebuffer_put(old_fb);
		wake_up_interruptible_all(&mon->present_waitq);
	}
}

static void lvda_atomic_commit_tail(lvda_atomic_txn *state)
{
	struct drm_device *dev = state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, 0);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
#if LVDA_HAS_VBLANK_HELPER
	/*
	 * The CRTC's vblank emulation is the only pacing point: flip_done
	 * completes when the armed event is delivered. Waiting for a vblank
	 * count change instead would stall commits for up to 100 ms under VRR,
	 * where no vblank fires without a flip.
	 */
	drm_atomic_helper_wait_for_flip_done(dev, state);
#else
	drm_atomic_helper_wait_for_vblanks(dev, state);
#endif
	lvda_publish_presentations(state);
	drm_atomic_helper_cleanup_planes(dev, state);
}

static struct lvda_monitor *lvda_find_monitor(struct drm_device *dev,
					      struct drm_file *file, u32 crtc_id)
{
	struct drm_crtc *crtc = drm_crtc_find(dev, file, crtc_id);

	return crtc ? crtc_to_monitor(crtc) : NULL;
}

static int lvda_wait_present_ioctl(struct drm_device *dev, void *data,
				   struct drm_file *file)
{
	struct lvda_wait_present *req = data;
	struct lvda_monitor *mon;
	u64 requested = req->sequence;

	if (req->reserved || req->timeout_ms > LVDA_PRESENT_MAX_TIMEOUT_MS)
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	mon = lvda_find_monitor(dev, file, req->crtc_id);
	if (!mon)
		return -ENOENT;

	if (req->timeout_ms) {
		long ret = wait_event_interruptible_timeout(
			mon->present_waitq,
			(u64)atomic64_read(&mon->present_sequence) != requested,
			msecs_to_jiffies(req->timeout_ms));

		if (ret < 0)
			return ret;
	}

	spin_lock(&mon->present_lock);
	req->sequence = atomic64_read(&mon->present_sequence);
	req->timestamp_ns = mon->present_timestamp_ns;
	spin_unlock(&mon->present_lock);
	req->flags = req->sequence != requested ? LVDA_PRESENT_CHANGED
						: LVDA_PRESENT_TIMEOUT;
	return 0;
}

/* Install a DMA-BUF fd for obj in the caller. An imported object (the
 * compositor's render-GPU buffer) yields its original DMA-BUF. */
static int lvda_export_obj_fd(struct drm_device *dev, struct drm_file *file,
			      struct drm_gem_object *obj, int *fd)
{
	u32 handle;
	int ret;

	if (!obj)
		return -EINVAL;
	ret = drm_gem_handle_create(file, obj, &handle);
	if (ret)
		return ret;
	ret = drm_gem_prime_handle_to_fd(dev, file, handle,
					 DRM_CLOEXEC | DRM_RDWR, fd);
	drm_gem_handle_delete(file, handle);
	return ret;
}

static int lvda_get_frame_ioctl(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct lvda_frame *req = data;
	struct drm_framebuffer *fb;
	struct lvda_monitor *mon;
	unsigned int i, n;
	int ret = 0;

	for (i = 0; i < ARRAY_SIZE(req->reserved); i++)
		if (req->reserved[i])
			return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	mon = lvda_find_monitor(dev, file, req->crtc_id);
	if (!mon)
		return -ENOENT;

	req->flags = 0;
	req->modifier = 0;
	req->width = req->height = req->fourcc = req->plane_count = 0;
	for (i = 0; i < LVDA_FRAME_MAX_PLANES; i++) {
		req->dma_buf_fds[i] = -1;
		req->pitches[i] = 0;
		req->offsets[i] = 0;
	}

	spin_lock(&mon->present_lock);
	req->sequence = atomic64_read(&mon->present_sequence);
	req->timestamp_ns = mon->present_timestamp_ns;
	fb = mon->present_fb;
	if (fb)
		drm_framebuffer_get(fb);
	spin_unlock(&mon->present_lock);

	if (!fb) {
		req->flags = LVDA_FRAME_EMPTY;
		return 0;
	}

	n = fb->format->num_planes;
	if (!n || n > LVDA_FRAME_MAX_PLANES) {
		ret = -EINVAL;
		goto out;
	}
	for (i = 0; i < n; i++) {
		ret = lvda_export_obj_fd(dev, file, drm_gem_fb_get_obj(fb, i),
					 &req->dma_buf_fds[i]);
		if (ret) {
			req->dma_buf_fds[i] = -1;
			goto out;
		}
		req->pitches[i] = fb->pitches[i];
		req->offsets[i] = fb->offsets[i];
	}
	req->flags = LVDA_FRAME_READY;
	req->width = fb->width;
	req->height = fb->height;
	req->fourcc = fb->format->format;
	req->modifier = (fb->flags & DRM_MODE_FB_MODIFIERS) ?
				fb->modifier : DRM_FORMAT_MOD_INVALID;
	req->plane_count = n;

out:
	drm_framebuffer_put(fb);
	if (ret) {
		for (i = 0; i < LVDA_FRAME_MAX_PLANES; i++) {
			if (req->dma_buf_fds[i] >= 0)
				close_fd(req->dma_buf_fds[i]);
			req->dma_buf_fds[i] = -1;
		}
	}
	return ret;
}

static const struct drm_ioctl_desc lvda_ioctls[] = {
	DRM_IOCTL_DEF_DRV(LVDA_WAIT_PRESENT, lvda_wait_present_ioctl, 0),
	DRM_IOCTL_DEF_DRV(LVDA_GET_FRAME, lvda_get_frame_ioctl, 0),
};

static void lvda_present_cleanup(struct drm_device *dev, void *data)
{
	struct lvda_monitor *mon = data;

	(void)dev;
	if (mon->present_fb)
		drm_framebuffer_put(mon->present_fb);
	mon->present_fb = NULL;
}

#if LVDA_HAS_VBLANK_HELPER
/*
 * Vblank emulation ported from Vibeshine's VKMS fork (vibeshine-drm:
 * vkms_crtc.c and vibeshine_drm_vrr.h, GPL-2.0+, Copyright (c) 2026 Chase
 * Payne; derived from the upstream VKMS driver).
 *
 * VRR off: the DRM vblank timer fires at the mode's refresh and flips
 * complete on the next tick.
 *
 * VRR on: the periodic timer is stopped. Each flip generates exactly one
 * vblank, immediately if at least one mode period has passed since the
 * previous vblank, otherwise when that period elapses. Flips are never
 * delayed beyond the mode's maximum refresh rate. A synthetic raw counter
 * keeps DRM from inferring zero or several vblanks from the irregular
 * timestamps.
 */

/* Earliest legal presentation time for a VRR flip. A zero previous timestamp
 * permits the first presentation immediately; saturation keeps a stale value
 * from wrapping into the past. */
static u64 lvda_vrr_deadline_ns(u64 previous_ns, u64 now_ns, u64 period_ns)
{
	u64 deadline_ns;

	if (!previous_ns || !period_ns)
		return now_ns;
	if (previous_ns > U64_MAX - period_ns)
		return U64_MAX;

	deadline_ns = previous_ns + period_ns;
	return deadline_ns > now_ns ? deadline_ns : now_ns;
}

/* handle_vblank_timeout for the DRM vblank timer, and the VRR timer body.
 * Also called from atomic_flush (process context), hence irqsave. */
static bool lvda_crtc_handle_vblank(struct drm_crtc *crtc)
{
	struct lvda_monitor *mon = crtc_to_monitor(crtc);
	unsigned long flags;
	bool fence_cookie, ret;

	fence_cookie = dma_fence_begin_signalling();
	spin_lock_irqsave(&mon->vblank_lock, flags);
	atomic64_set(&mon->vrr_vblank_timestamp_ns, ktime_get_ns());
	atomic64_inc(&mon->synthetic_vblank_counter);
	ret = drm_crtc_handle_vblank(crtc);
	spin_unlock_irqrestore(&mon->vblank_lock, flags);
	dma_fence_end_signalling(fence_cookie);

	return ret;
}

static enum hrtimer_restart lvda_vrr_vblank_fire(struct hrtimer *timer)
{
	struct lvda_monitor *mon = container_of(timer, struct lvda_monitor,
						vrr_hrtimer);

	lvda_crtc_handle_vblank(&mon->crtc);
	return HRTIMER_NORESTART;
}

static void lvda_schedule_vrr_vblank(struct drm_crtc *crtc)
{
	struct drm_vblank_crtc *vblank = drm_crtc_vblank_crtc(crtc);
	struct lvda_monitor *mon = crtc_to_monitor(crtc);
	u64 now_ns = ktime_get_ns();
	u64 period_ns = max_t(int, READ_ONCE(vblank->framedur_ns), 0);
	u64 deadline_ns;

	deadline_ns = lvda_vrr_deadline_ns(
		atomic64_read(&mon->vrr_vblank_timestamp_ns), now_ns, period_ns);
	if (deadline_ns <= now_ns) {
		lvda_crtc_handle_vblank(crtc);
		return;
	}

	hrtimer_start(&mon->vrr_hrtimer, ns_to_ktime(deadline_ns),
		      HRTIMER_MODE_ABS);
}

static u32 lvda_crtc_get_vblank_counter(struct drm_crtc *crtc)
{
	return (u32)atomic64_read(
		&crtc_to_monitor(crtc)->synthetic_vblank_counter);
}

static bool lvda_crtc_get_vblank_timestamp(struct drm_crtc *crtc,
					   int *max_error,
					   ktime_t *vblank_time,
					   bool in_vblank_irq)
{
	u64 timestamp_ns;

	if (!READ_ONCE(crtc->state->vrr_enabled))
		return drm_crtc_vblank_helper_get_vblank_timestamp_from_timer(
			crtc, max_error, vblank_time, in_vblank_irq);

	/* Under VRR the real generation time is the vblank time. */
	timestamp_ns = atomic64_read(
		&crtc_to_monitor(crtc)->vrr_vblank_timestamp_ns);
	if (!timestamp_ns)
		return false;
	*max_error = 0;
	*vblank_time = ns_to_ktime(timestamp_ns);
	return true;
}

static void lvda_crtc_atomic_flush(struct drm_crtc *crtc,
				   lvda_atomic_txn *state)
{
	struct lvda_monitor *mon = crtc_to_monitor(crtc);
	struct drm_crtc_state *old_state =
		drm_atomic_get_old_crtc_state(state, crtc);
	struct drm_crtc_state *new_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event;
	bool vrr_disabled = old_state && old_state->vrr_enabled &&
			    !new_state->vrr_enabled;
	bool vrr_flip = false;

	spin_lock_irq(&crtc->dev->event_lock);
	event = new_state->event;
	new_state->event = NULL;
	if (event) {
		if (drm_crtc_vblank_get(crtc) == 0) {
			drm_crtc_arm_vblank_event(crtc, event);
			vrr_flip = new_state->vrr_enabled;
		} else {
			drm_crtc_send_vblank_event(crtc, event);
		}
	}
	spin_unlock_irq(&crtc->dev->event_lock);

	/*
	 * Adaptive sync removes the fixed cadence, not the mode's maximum
	 * refresh. Stop the periodic timer (drm_crtc_vblank_get() may just
	 * have started it) and generate one capped vblank per flip; restore
	 * the periodic timer when VRR turns off.
	 */
	if (new_state->vrr_enabled && old_state && old_state->active)
		drm_crtc_vblank_cancel_timer(crtc);
	if (vrr_flip) {
		lvda_schedule_vrr_vblank(crtc);
	} else if (vrr_disabled) {
		hrtimer_cancel(&mon->vrr_hrtimer);
		atomic64_set(&mon->vrr_vblank_timestamp_ns, 0);
		if (new_state->active)
			drm_crtc_vblank_start_timer(crtc);
	}
}

static void lvda_crtc_atomic_enable(struct drm_crtc *crtc,
				    lvda_atomic_txn *state)
{
	struct lvda_monitor *mon = crtc_to_monitor(crtc);

	(void)state;
	drm_crtc_vblank_on(crtc);

	/* drm_crtc_vblank_on() may start the periodic timer; VRR doesn't
	 * use it. */
	if (crtc->state->vrr_enabled) {
		atomic64_set(&mon->vrr_vblank_timestamp_ns, ktime_get_ns());
		drm_crtc_vblank_cancel_timer(crtc);
	}
}

static void lvda_crtc_atomic_disable(struct drm_crtc *crtc,
				     lvda_atomic_txn *state)
{
	struct lvda_monitor *mon = crtc_to_monitor(crtc);

	(void)state;
	hrtimer_cancel(&mon->vrr_hrtimer);
	atomic64_set(&mon->vrr_vblank_timestamp_ns, 0);
	drm_crtc_vblank_off(crtc);
}

static void lvda_vrr_timer_cleanup(struct drm_device *dev, void *data)
{
	struct lvda_monitor *mon = data;

	(void)dev;
	hrtimer_cancel(&mon->vrr_hrtimer);
}
#endif /* LVDA_HAS_VBLANK_HELPER */

static void lvda_plane_atomic_update(struct drm_plane *plane,
				      lvda_atomic_txn *state)
{
	(void)plane;
	(void)state;
}

static int lvda_plane_atomic_check(struct drm_plane *plane,
				    lvda_atomic_txn *state)
{
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;

	plane_state = drm_atomic_get_new_plane_state(state, plane);
	if (!plane_state)
		return -EINVAL;

	if (!plane_state->crtc)
		return 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, plane_state->crtc);
	if (!crtc_state)
		return -EINVAL;

	return drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						   DRM_PLANE_NO_SCALING,
						   DRM_PLANE_NO_SCALING,
						   plane->type == DRM_PLANE_TYPE_CURSOR,
						   false);
}

static int lvda_connector_get_modes(struct drm_connector *connector)
{
	struct lvda_device *ldev = to_lvda(connector->dev);
	struct lvda_monitor *mon = to_monitor(connector);
	const struct drm_edid *edid = NULL;
	int count;

	mutex_lock(&ldev->lock);
	if (mon->active)
		edid = drm_edid_alloc(mon->edid_bytes, mon->edid_len);
	mutex_unlock(&ldev->lock);

	drm_edid_connector_update(connector, edid);
	count = edid ? drm_edid_connector_add_modes(connector) : 0;
	drm_edid_free(edid);

	return count;
}

static enum drm_connector_status
lvda_connector_detect(struct drm_connector *connector, bool force)
{
	struct lvda_device *ldev = to_lvda(connector->dev);
	struct lvda_monitor *mon = to_monitor(connector);
	bool active;

	(void)force;

	mutex_lock(&ldev->lock);
	active = mon->active;
	mutex_unlock(&ldev->lock);

	return active ? connector_status_connected
		      : connector_status_disconnected;
}

static const struct drm_plane_funcs lvda_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static const struct drm_plane_helper_funcs lvda_plane_helper_funcs = {
	.atomic_check = lvda_plane_atomic_check,
	.atomic_update = lvda_plane_atomic_update,
};

static const struct drm_crtc_funcs lvda_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
#if LVDA_HAS_VBLANK_HELPER
	.get_vblank_counter = lvda_crtc_get_vblank_counter,
	.enable_vblank = drm_crtc_vblank_helper_enable_vblank_timer,
	.disable_vblank = drm_crtc_vblank_helper_disable_vblank_timer,
	.get_vblank_timestamp = lvda_crtc_get_vblank_timestamp,
#endif
};

static const struct drm_crtc_helper_funcs lvda_crtc_helper_funcs = {
	.atomic_check = drm_crtc_helper_atomic_check,
#if LVDA_HAS_VBLANK_HELPER
	.atomic_flush = lvda_crtc_atomic_flush,
	.atomic_enable = lvda_crtc_atomic_enable,
	.atomic_disable = lvda_crtc_atomic_disable,
	.handle_vblank_timeout = lvda_crtc_handle_vblank,
#endif
};

static const struct drm_encoder_funcs lvda_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_connector_funcs lvda_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.detect = lvda_connector_detect,
	.reset = drm_atomic_helper_connector_reset,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs lvda_connector_helper_funcs = {
	.get_modes = lvda_connector_get_modes,
};

static const struct drm_mode_config_funcs lvda_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static const struct drm_mode_config_helper_funcs lvda_mode_config_helpers = {
	.atomic_commit_tail = lvda_atomic_commit_tail,
};

DEFINE_DRM_GEM_FOPS(lvda_fops);

/* Dumb-buffer scanout pitch alignment, in bytes. */
#define LVDA_FB_PITCH_ALIGN 256u

static int lvda_dumb_create(struct drm_file *file, struct drm_device *drm,
			    struct drm_mode_create_dumb *args)
{
	u64 bits, min_pitch, pitch, size;
	struct drm_gem_shmem_object *shmem;
	u32 handle;
	int ret;

	if (!args->width || !args->height || !args->bpp || args->flags)
		return -EINVAL;

	bits = (u64)args->width * args->bpp;
	min_pitch = (bits + 7) / 8;
	pitch = ALIGN(min_pitch, (u64)LVDA_FB_PITCH_ALIGN);
	if (pitch > (u64)~0U)
		return -EINVAL;

	if (check_mul_overflow(pitch, (u64)args->height, &size))
		return -EINVAL;
	if ((u64)(size_t)size != size)
		return -EINVAL;

	args->pitch = (u32)pitch;

	shmem = drm_gem_shmem_create(drm, (size_t)size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);
	args->size = shmem->base.size;

	ret = drm_gem_handle_create(file, &shmem->base, &handle);
	drm_gem_object_put(&shmem->base);
	if (ret)
		return ret;

	args->handle = handle;
	return 0;
}

static const struct drm_driver lvda_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.dumb_create = lvda_dumb_create,
	.fops = &lvda_fops,
	.ioctls = lvda_ioctls,
	.num_ioctls = ARRAY_SIZE(lvda_ioctls),
	.name = "lvda",
	.desc = "LVDA virtual display",
	.major = 0,
	.minor = 1,
	.patchlevel = 0,
};

static int lvda_monitors_show(struct seq_file *m, void *unused)
{
	struct drm_debugfs_entry *entry = m->private;
	struct lvda_device *ldev = to_lvda(entry->dev);
	unsigned int i;

	(void)unused;

	seq_printf(m, "%-4s %-6s %-19s %-11s %-4s %-12s %-4s %s\n",
		   "slot", "state", "mode", "flags", "gen", "connector",
		   "edid", "client_id");

	mutex_lock(&ldev->lock);
	for (i = 0; i < ldev->n_monitors; i++) {
		struct lvda_monitor *mon = &ldev->monitors[i];
		char mode[24] = "-";
		char flags[12] = "-";
		char cid[2 * LVDA_CLIENT_ID_LEN + 1] = "-";

		if (mon->active) {
			snprintf(mode, sizeof(mode), "%ux%u@%u.%03u",
				 mon->width, mon->height,
				 mon->refresh_mhz / 1000,
				 mon->refresh_mhz % 1000);
			scnprintf(flags, sizeof(flags), "%s%s%s",
				  mon->flags & LVDA_F_HDR ? "hdr" : "",
				  (mon->flags & LVDA_F_ALL) == LVDA_F_ALL ?
					  "," : "",
				  mon->flags & LVDA_F_10BPC ? "10bpc" : "");
			if (!flags[0])
				strscpy(flags, "-", sizeof(flags));
			snprintf(cid, sizeof(cid), "%*phN",
				 (int)sizeof(mon->client_id), mon->client_id);
		}

		seq_printf(m, "%-4u %-6s %-19s %-11s %-4llu %-12s %-4u %s\n",
			   i, mon->active ? "active" : "free", mode, flags,
			   mon->generation, mon->connector.name,
			   mon->edid_len, cid);
	}
	mutex_unlock(&ldev->lock);

	return 0;
}

static const struct drm_debugfs_info lvda_debugfs_list[] = {
	{ "monitors", lvda_monitors_show, 0 },
};

/* Reject a monitor name with non-printable bytes or one longer than the EDID
 * monitor-name descriptor holds. Empty is valid. */
static int lvda_validate_name(const __u8 name[LVDA_NAME_FIELD_LEN])
{
	unsigned int n;

	for (n = 0; n < LVDA_NAME_FIELD_LEN; n++) {
		if (!name[n])
			break;
		if (name[n] < 0x20 || name[n] > 0x7E)
			return -EINVAL;
	}
	if (n > LVDA_NAME_MAX)
		return -EINVAL;
	return 0;
}

static int lvda_validate_add(const struct lvda_add *req)
{
	if (req->width < LVDA_DIM_MIN || req->width > LVDA_DIM_MAX)
		return -EINVAL;

	if (req->height < LVDA_DIM_MIN || req->height > LVDA_DIM_MAX)
		return -EINVAL;

	if (req->refresh_mhz < LVDA_REFRESH_MHZ_MIN ||
	    req->refresh_mhz > LVDA_REFRESH_MHZ_MAX)
		return -EINVAL;

	if (req->flags & ~LVDA_F_ALL)
		return -EINVAL;

	if (req->reserved[0] || req->reserved[1])
		return -EINVAL;

	if (req->phys_width_mm &&
	    (req->phys_width_mm < LVDA_PHYS_MM_MIN ||
	     req->phys_width_mm > LVDA_PHYS_MM_MAX))
		return -EINVAL;

	if (req->phys_height_mm &&
	    (req->phys_height_mm < LVDA_PHYS_MM_MIN ||
	     req->phys_height_mm > LVDA_PHYS_MM_MAX))
		return -EINVAL;

	if (lvda_validate_name(req->name))
		return -EINVAL;

	return 0;
}

static int lvda_init_mode_config(struct drm_device *drm)
{
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = LVDA_DIM_MIN;
	drm->mode_config.min_height = LVDA_DIM_MIN;
	drm->mode_config.max_width = LVDA_DIM_MAX;
	drm->mode_config.max_height = LVDA_DIM_MAX;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.cursor_width = LVDA_CURSOR_MAX;
	drm->mode_config.cursor_height = LVDA_CURSOR_MAX;
	drm->mode_config.funcs = &lvda_mode_config_funcs;
	drm->mode_config.helper_private = &lvda_mode_config_helpers;

	return 0;
}

static int lvda_init_monitor(struct lvda_device *ldev, unsigned int i)
{
	struct drm_device *drm = &ldev->drm;
	struct lvda_monitor *mon = &ldev->monitors[i];
	int ret;

	ret = drm_universal_plane_init(drm, &mon->primary, BIT(i),
				       &lvda_plane_funcs,
				       lvda_primary_formats,
				       ARRAY_SIZE(lvda_primary_formats),
				       lvda_primary_modifiers,
				       DRM_PLANE_TYPE_PRIMARY,
				       "lvda-primary-%u", i);
	if (ret)
		return ret;

	drm_plane_helper_add(&mon->primary, &lvda_plane_helper_funcs);

	ret = drm_universal_plane_init(drm, &mon->cursor, BIT(i),
				       &lvda_plane_funcs,
				       lvda_cursor_formats,
				       ARRAY_SIZE(lvda_cursor_formats),
				       lvda_linear_modifiers,
				       DRM_PLANE_TYPE_CURSOR,
				       "lvda-cursor-%u", i);
	if (ret)
		return ret;

	drm_plane_helper_add(&mon->cursor, &lvda_plane_helper_funcs);

	ret = drm_crtc_init_with_planes(drm, &mon->crtc, &mon->primary,
					&mon->cursor, &lvda_crtc_funcs,
					"lvda-crtc-%u", i);
	if (ret)
		return ret;

	drm_crtc_helper_add(&mon->crtc, &lvda_crtc_helper_funcs);

#if LVDA_HAS_VBLANK_HELPER
	spin_lock_init(&mon->vblank_lock);
	atomic64_set(&mon->synthetic_vblank_counter, 0);
	atomic64_set(&mon->vrr_vblank_timestamp_ns, 0);
	hrtimer_setup(&mon->vrr_hrtimer, lvda_vrr_vblank_fire,
		      CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	ret = drmm_add_action_or_reset(drm, lvda_vrr_timer_cleanup, mon);
	if (ret)
		return ret;
#endif

	spin_lock_init(&mon->present_lock);
	init_waitqueue_head(&mon->present_waitq);
	atomic64_set(&mon->present_sequence, 0);
	mon->present_timestamp_ns = 0;
	mon->present_fb = NULL;
	/* Registered after drmm_mode_config_init(), so this runs before the
	 * mode-config cleanup that would flag a still-referenced fb. */
	ret = drmm_add_action_or_reset(drm, lvda_present_cleanup, mon);
	if (ret)
		return ret;

	/* VRR_ENABLED atomic CRTC property. */
	drm_object_attach_property(&mon->crtc.base,
				   drm->mode_config.prop_vrr_enabled, 0);

	ret = drm_encoder_init(drm, &mon->encoder, &lvda_encoder_funcs,
			       DRM_MODE_ENCODER_VIRTUAL, "lvda-encoder-%u", i);
	if (ret)
		return ret;

	mon->encoder.possible_crtcs = BIT(i);

	ret = drm_connector_init(drm, &mon->connector, &lvda_connector_funcs,
				 DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret)
		return ret;

	drm_connector_helper_add(&mon->connector,
				 &lvda_connector_helper_funcs);

	ret = drm_connector_attach_encoder(&mon->connector, &mon->encoder);
	if (ret)
		return ret;

	drm_connector_attach_edid_property(&mon->connector);

	/* Capability properties a compositor reads off the connector. */
	ret = drm_connector_attach_vrr_capable_property(&mon->connector);
	if (ret)
		return ret;
	drm_connector_set_vrr_capable_property(&mon->connector, true);

	/* Linux 7.2 made this attach unconditional and dropped the error path. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
	drm_connector_attach_hdr_output_metadata_property(&mon->connector);
#else
	ret = drm_connector_attach_hdr_output_metadata_property(&mon->connector);
	if (ret)
		return ret;
#endif

	ret = drm_mode_create_dp_colorspace_property(&mon->connector, 0);
	if (ret)
		return ret;
	ret = drm_connector_attach_colorspace_property(&mon->connector);
	if (ret)
		return ret;
	mon->connector.polled = DRM_CONNECTOR_POLL_HPD;
	mon->connector.status = connector_status_disconnected;

	return 0;
}

int lvda_card_register(struct device *parent, unsigned int n_monitors)
{
	struct lvda_device *ldev;
	struct drm_device *drm;
	unsigned int i;
	int ret;

	if (n_monitors < 1 || n_monitors > LVDA_MAX_MONITORS)
		return -EINVAL;

	if (lvda_card)
		return -EBUSY;

	ldev = __drm_dev_alloc(parent, &lvda_drm_driver,
			       struct_size(ldev, monitors, n_monitors),
			       offsetof(struct lvda_device, drm));
	if (IS_ERR(ldev))
		return PTR_ERR(ldev);

	drm = &ldev->drm;
	ldev->n_monitors = n_monitors;
	ret = drmm_mutex_init(drm, &ldev->lock);
	if (ret)
		goto err_put;

	ret = lvda_init_mode_config(drm);
	if (ret)
		goto err_put;

	lvda_build_primary_modifiers();

	for (i = 0; i < n_monitors; i++) {
		ret = lvda_init_monitor(ldev, i);
		if (ret)
			goto err_put;
	}

#if LVDA_HAS_VBLANK_HELPER
	/* get_vblank_counter reports a full 32-bit synthetic counter. */
	drm->max_vblank_count = U32_MAX;
	ret = drm_vblank_init(drm, n_monitors);
	if (ret)
		goto err_put;
#endif

	drm_mode_config_reset(drm);

	/* Advertise 8..10 bpc on each connector. */
	for (i = 0; i < n_monitors; i++) {
		ret = drm_connector_attach_max_bpc_property(
			&ldev->monitors[i].connector, 8, 10);
		if (ret)
			goto err_put;
	}

	drm_debugfs_add_files(drm, lvda_debugfs_list,
			      ARRAY_SIZE(lvda_debugfs_list));

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_put;

	lvda_card = ldev;
	return 0;

err_put:
	drm_dev_put(drm);
	return ret;
}

void lvda_card_unregister(void)
{
	struct lvda_device *ldev = lvda_card;

	if (!ldev)
		return;

	lvda_card = NULL;
	drm_dev_unregister(&ldev->drm);
	drm_dev_put(&ldev->drm);
}

int lvda_monitor_add(void *owner, const struct lvda_add *req,
		      __u32 *monitor_id, __u64 *generation,
		      __u32 *card_minor, char name[LVDA_CONNECTOR_NAME_LEN])
{
	struct lvda_device *ldev = lvda_card;
	struct lvda_monitor *mon = NULL;
	u8 edid_buf[LVDA_EDID_SIZE];
	u64 gen;
	unsigned int i;
	int len;

	if (!ldev)
		return -ENODEV;

	len = lvda_validate_add(req);
	if (len)
		return len;

	len = lvda_synth_edid(&(struct lvda_edid_params) {
		.client_id = req->client_id,
		.name = req->name[0] ? (const char *)req->name : NULL,
		.width = req->width,
		.height = req->height,
		.refresh_mhz = req->refresh_mhz,
		.phys_width_mm = req->phys_width_mm,
		.phys_height_mm = req->phys_height_mm,
		.hdr = !!(req->flags & LVDA_F_HDR),
		.deep_color = !!(req->flags & LVDA_F_10BPC),
	}, edid_buf);
	if (len < 0)
		return len;

	mutex_lock(&ldev->lock);
	for (i = 0; i < ldev->n_monitors; i++) {
		if (!ldev->monitors[i].owner) {
			mon = &ldev->monitors[i];
			break;
		}
	}
	if (!mon) {
		mutex_unlock(&ldev->lock);
		return -ENOSPC;
	}

	memcpy(mon->edid_bytes, edid_buf, len);
	mon->edid_len = len;
	memcpy(mon->client_id, req->client_id, sizeof(mon->client_id));
	mon->width = req->width;
	mon->height = req->height;
	mon->refresh_mhz = req->refresh_mhz;
	mon->flags = req->flags;
	gen = ++mon->generation;
	mon->active = true;
	mon->owner = owner;
	mon->connector.status = connector_status_connected;
	mutex_unlock(&ldev->lock);

	*monitor_id = i;
	*generation = gen;
	*card_minor = ldev->drm.primary->index;
	strscpy(name, mon->connector.name, 32);

	drm_kms_helper_hotplug_event(&ldev->drm);

	return 0;
}

int lvda_monitor_remove(void *owner, __u32 monitor_id)
{
	struct lvda_device *ldev = lvda_card;
	struct lvda_monitor *mon;

	if (!ldev)
		return -ENODEV;

	if (monitor_id >= ldev->n_monitors)
		return -EINVAL;

	mon = &ldev->monitors[monitor_id];

	mutex_lock(&ldev->lock);
	if (mon->owner != owner) {
		mutex_unlock(&ldev->lock);
		return -EINVAL;
	}
	mon->active = false;
	mon->owner = NULL;
	mon->connector.status = connector_status_disconnected;
	mutex_unlock(&ldev->lock);

	drm_kms_helper_hotplug_event(&ldev->drm);

	return 0;
}

void lvda_monitor_abort_add(void *owner, __u32 monitor_id, __u64 generation)
{
	struct lvda_device *ldev = lvda_card;
	struct lvda_monitor *mon;
	bool changed = false;

	if (!ldev)
		return;

	if (monitor_id >= ldev->n_monitors)
		return;

	mon = &ldev->monitors[monitor_id];

	mutex_lock(&ldev->lock);
	if (mon->owner == owner && mon->generation == generation) {
		mon->active = false;
		mon->owner = NULL;
		mon->connector.status = connector_status_disconnected;
		changed = true;
	}
	mutex_unlock(&ldev->lock);

	if (changed)
		drm_kms_helper_hotplug_event(&ldev->drm);
}

void lvda_release_owner(void *owner)
{
	struct lvda_device *ldev = lvda_card;
	bool changed = false;
	unsigned int i;

	if (!ldev)
		return;

	mutex_lock(&ldev->lock);
	for (i = 0; i < ldev->n_monitors; i++) {
		if (ldev->monitors[i].owner == owner) {
			ldev->monitors[i].active = false;
			ldev->monitors[i].owner = NULL;
			ldev->monitors[i].connector.status =
				connector_status_disconnected;
			changed = true;
		}
	}
	mutex_unlock(&ldev->lock);

	if (changed)
		drm_kms_helper_hotplug_event(&ldev->drm);
}
