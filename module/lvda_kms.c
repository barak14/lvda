/* SPDX-License-Identifier: GPL-2.0 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
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
#include <linux/bits.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "lvda.h"
#include "lvda_edid.h"

/*
 * One virtual monitor = one full {plane, CRTC, encoder, connector} chain so it
 * can be enabled at its own mode independently. possible_crtcs is BIT(slot)
 * because drm_crtc->index is assigned in creation order (§9).
 */
struct lvda_monitor {
	struct drm_plane primary;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	u8 edid_bytes[LVDA_EDID_SIZE];
	u16 edid_len;
	bool active;
	void *owner;	/* /dev/lvda file that added it; NULL = free slot */
	u64 generation;
};

struct lvda_device {
	struct drm_device drm;
	struct mutex lock;		/* serializes monitor slot state */
	unsigned int n_monitors;
	struct lvda_monitor monitors[];
};

#define to_lvda(dev)	  container_of(dev, struct lvda_device, drm)
#define to_monitor(conn)  container_of(conn, struct lvda_monitor, connector)

/* The single persistent card. Set under no lock: registered before the
 * /dev/lvda misc node accepts ioctls and cleared only at module_exit, which
 * the file-ops module owner blocks until every fd is closed. */
static struct lvda_device *lvda_card;

static const u32 lvda_primary_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR2101010,
};

static const u64 lvda_primary_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static void lvda_atomic_commit_tail(struct drm_atomic_state *state)
{
	struct drm_device *dev = state->dev;

	drm_atomic_helper_commit_modeset_disables(dev, state);
	drm_atomic_helper_commit_planes(dev, state, 0);
	drm_atomic_helper_commit_modeset_enables(dev, state);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_wait_for_vblanks(dev, state);
	drm_atomic_helper_cleanup_planes(dev, state);
}

static void lvda_plane_atomic_update(struct drm_plane *plane,
				      struct drm_atomic_state *state)
{
	(void)plane;
	(void)state;
}

static int lvda_plane_atomic_check(struct drm_plane *plane,
				    struct drm_atomic_state *state)
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
						   false, false);
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
};

static const struct drm_crtc_helper_funcs lvda_crtc_helper_funcs = {
	.atomic_check = drm_crtc_helper_atomic_check,
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

/*
 * Align the dumb-buffer scanout pitch to 256 bytes. The shmem helper defaults
 * the pitch to width * cpp, which is only as aligned as the width itself. A
 * streaming host imports the scanned-out framebuffer as a dma-buf and feeds it
 * to a GPU video encoder; those importers (e.g. amdgpu) require a 256-byte
 * pitch. Without this a width not divisible by 64 (e.g. 2400 -> pitch 9600)
 * yields a buffer the encoder rejects, so capture produces no frames.
 */
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

	/* Allocate the object at the aligned pitch directly: the shmem dumb
	 * helper recomputes (and shrinks) the pitch back to width * cpp. */
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
	.name = "lvda",
	.desc = "LVDA virtual display",
	.major = 0,
	.minor = 1,
	.patchlevel = 0,
};

/* Reject a monitor name with non-printable bytes or one longer than the EDID
 * monitor-name descriptor holds. Empty is valid (the synth defaults to lvda). */
static int lvda_validate_name(const __u8 name[16])
{
	unsigned int n;

	for (n = 0; n < 16; n++) {
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

	ret = drm_crtc_init_with_planes(drm, &mon->crtc, &mon->primary, NULL,
					&lvda_crtc_funcs, "lvda-crtc-%u", i);
	if (ret)
		return ret;

	drm_crtc_helper_add(&mon->crtc, &lvda_crtc_helper_funcs);

	/* VRR_ENABLED lets the compositor toggle adaptive sync on the CRTC. */
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

	/* Capability properties a compositor reads off the connector. The
	 * per-monitor EDID gates HDR / wide color gamut: BT.2020 + PQ ride in
	 * the EDID only when LVDA_F_HDR is set, so those light up on HDR
	 * monitors and stay incapable on SDR ones. VRR is always advertised. */
	ret = drm_connector_attach_vrr_capable_property(&mon->connector);
	if (ret)
		return ret;
	drm_connector_set_vrr_capable_property(&mon->connector, true);

	ret = drm_connector_attach_hdr_output_metadata_property(&mon->connector);
	if (ret)
		return ret;

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

	for (i = 0; i < n_monitors; i++) {
		ret = lvda_init_monitor(ldev, i);
		if (ret)
			goto err_put;
	}

	drm_mode_config_reset(drm);

	/* "max bpc" needs the connector state drm_mode_config_reset() just
	 * created. Advertise 8..10 bpc — the depths the scanout formats cover
	 * (XRGB8888 / XBGR2101010) — so the output reports a real color
	 * resolution rather than "unknown". */
	for (i = 0; i < n_monitors; i++) {
		ret = drm_connector_attach_max_bpc_property(
			&ldev->monitors[i].connector, 8, 10);
		if (ret)
			goto err_put;
	}

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
		      __u32 *card_minor, char name[32])
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
