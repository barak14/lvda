/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LVDA_PRIV_H
#define LVDA_PRIV_H

/*
 * Internal kernel-side contract between lvda_main.c (char device + ioctl
 * + per-fd monitor ownership) and lvda_kms.c (the persistent DRM card and
 * its on-demand virtual monitors). See lvda-SPEC.md §8, §9.
 */

#include <linux/types.h>

#include "../uapi/lvda.h"

struct device;

/* Pool size: number of virtual monitors the persistent card can light up at
 * once (lvda-SPEC.md §11). Defined, with its module_param(), in
 * lvda_main.c; consumed by lvda_card_register(). */
extern unsigned int lvda_max_monitors;

#define LVDA_MAX_MONITORS 32u

/*
 * Register the single persistent DRM card with n_monitors pre-created virtual
 * monitors, all initially disconnected. parent is the module-lifetime
 * platform device used as the DRM parent (§8). The card stays registered
 * (/dev/dri/cardN live) for the module's lifetime. Returns 0 or -errno.
 */
int lvda_card_register(struct device *parent, unsigned int n_monitors);

/* Unregister and drop the persistent card. DRM refcounting keeps the
 * underlying drm_device alive until other holders (an open /dev/dri/cardN)
 * release it; the file-ops module owner pins the module until then. */
void lvda_card_unregister(void);

/*
 * Light up a free virtual monitor at the requested exact mode: synthesize its
 * EDID, mark its connector connected, fire a hotplug event. owner is an opaque
 * token (the /dev/lvda file) recorded so the monitor is reaped when that file
 * closes. On success *monitor_id (the LVDA_IOC_REMOVE handle), *card_minor,
 * and name[32] (the DRM connector name) are filled. Returns 0 or -errno:
 *   -EINVAL    dimensions/refresh out of range
 *   -EOVERFLOW mode exceeds the DisplayID pixel-clock ceiling
 *   -ENOSPC    no free monitor slot
 *   -ENODEV    card not registered
 */
int lvda_monitor_add(void *owner, const struct lvda_add *req,
		      __u32 *monitor_id, __u64 *generation,
		      __u32 *card_minor, char name[32]);

/* Disable a monitor previously added by the same owner. Returns -EINVAL if
 * monitor_id is out of range or not owned by owner. */
int lvda_monitor_remove(void *owner, __u32 monitor_id);

/* Best-effort rollback for ADD after userspace copy-out failure. The generation
 * prevents deleting a newer monitor that reused the same slot on the same fd. */
void lvda_monitor_abort_add(void *owner, __u32 monitor_id, __u64 generation);

/* Disable every monitor owned by owner (called on /dev/lvda close). */
void lvda_release_owner(void *owner);

#endif /* LVDA_PRIV_H */
