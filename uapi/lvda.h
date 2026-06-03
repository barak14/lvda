/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_LVDA_H
#define _UAPI_LINUX_LVDA_H

/*
 * lvda userspace ABI. Single source of truth — the module #includes this
 * header directly; there is no second declaration.
 *
 * Model: the module registers ONE persistent DRM card at load time, holding a
 * fixed pool of virtual monitors that all start disconnected. LVDA_IOC_ADD
 * enables one at an exact mode (synthesizes its EDID, marks the connector
 * connected, fires a hotplug event); LVDA_IOC_REMOVE disables it. A monitor
 * is owned by the /dev/lvda file that added it and is disabled when that file
 * is closed — the open fd is the liveness signal (no heartbeat, no watchdog).
 */

#include <linux/types.h>
#include <linux/ioctl.h>

#define LVDA_IOC_MAGIC 'V'

#define LVDA_PROTOCOL_MAJOR 2
#define LVDA_PROTOCOL_MINOR 2
#define LVDA_PROTOCOL_PATCH 0

/* Feature flags for lvda_add.flags. */
#define LVDA_F_HDR   (1u << 0)   /* declare HDR10/PQ + BT.2020 + 10-bit in EDID */
#define LVDA_F_10BPC (1u << 1)   /* advertise 10-bit color depth even for SDR */
#define LVDA_F_ALL   (LVDA_F_HDR | LVDA_F_10BPC)

/*
 * Inclusive request bounds. Out-of-range dimensions or refresh fail ADD with
 * -EINVAL; a timing whose pixel clock exceeds even DisplayID fails with
 * -EOVERFLOW.
 */
#define LVDA_DIM_MIN          1u
#define LVDA_DIM_MAX          8192u
#define LVDA_REFRESH_MHZ_MIN  1000u
#define LVDA_REFRESH_MHZ_MAX  1000000u

/*
 * Physical-size bounds (millimetres) for lvda_add.phys_{width,height}_mm. Zero
 * derives the size from the pixel count at 96 DPI; a nonzero value is rounded
 * to the EDID 1 cm granularity.
 */
#define LVDA_PHYS_MM_MIN      10u
#define LVDA_PHYS_MM_MAX      2550u

/* Usable bytes of lvda_add.name (EDID monitor-name descriptor capacity). */
#define LVDA_CLIENT_ID_LEN      16u
#define LVDA_NAME_FIELD_LEN     16u
#define LVDA_NAME_MAX           13u
#define LVDA_CONNECTOR_NAME_LEN 32u

/*
 * Enable a virtual monitor on the persistent lvda card at an exact mode.
 * The monitor is owned by this open file; LVDA_IOC_REMOVE or close(2)
 * disables it. client_id only salts the EDID serial (stable identity across
 * reconnects) and is NOT a lookup key. monitor_id is the handle passed back
 * to LVDA_IOC_REMOVE; connector_name is the DRM connector (e.g. "Virtual-1")
 * a capturer targets. phys_width_mm/phys_height_mm set the EDID physical size
 * (0 = derive at 96 DPI); name sets the monitor name (empty = "lvda").
 */
struct lvda_add {
	/* IN */
	__u8  client_id[LVDA_CLIENT_ID_LEN];
	__u32 width;            /* 1..8192 */
	__u32 height;           /* 1..8192 */
	__u32 refresh_mhz;      /* 1000..1000000 (milli-Hz) */
	__u32 flags;            /* LVDA_F_* */
	__u32 phys_width_mm;    /* EDID physical width; 0 = derive at 96 DPI */
	__u32 phys_height_mm;   /* EDID physical height; 0 = derive at 96 DPI */
	__u8  name[LVDA_NAME_FIELD_LEN]; /* <=LVDA_NAME_MAX chars; "" = "lvda" */
	__u32 reserved[2];      /* MBZ */
	/* OUT */
	__u32 monitor_id;       /* handle for LVDA_IOC_REMOVE */
	__u32 drm_card_minor;   /* N in /dev/dri/cardN (persistent) */
	__u8  connector_name[LVDA_CONNECTOR_NAME_LEN];
};

struct lvda_remove {
	/* IN */
	__u32 monitor_id;       /* as returned by LVDA_IOC_ADD */
	__u32 reserved;         /* MBZ */
};

struct lvda_version {
	__u8 major;
	__u8 minor;
	__u8 patch;
	__u8 flags;             /* supported LVDA_F_* bits */
};

#define LVDA_IOC_ADD      _IOWR(LVDA_IOC_MAGIC, 1, struct lvda_add)
#define LVDA_IOC_REMOVE   _IOW (LVDA_IOC_MAGIC, 2, struct lvda_remove)
#define LVDA_IOC_VERSION  _IOR (LVDA_IOC_MAGIC, 3, struct lvda_version)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct lvda_add)     == 104, "lvda_add ABI drift");
_Static_assert(sizeof(struct lvda_remove)  == 8,  "lvda_remove ABI drift");
_Static_assert(sizeof(struct lvda_version) == 4,  "lvda_version ABI drift");
#endif

#endif /* _UAPI_LINUX_LVDA_H */
