/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LVDA_EDID_H
#define LVDA_EDID_H

/*
 * Deterministic EDID synthesizer. No kernel dependency: compiled both into
 * the module and into the host test binary.
 */

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif

#define LVDA_EDID_BLOCK     128u
#define LVDA_EDID_BASE_SIZE 256u   /* base block + CTA-861 extension */
#define LVDA_EDID_SIZE      384u   /* + DisplayID extension for large modes */

/*
 * The base detailed-timing descriptor has 12-bit active fields (max 4095) and
 * a 16-bit ×10 kHz pixel clock (max 655.35 MHz). A mode beyond either is
 * carried in a DisplayID Type I block instead of being rejected.
 */
#define LVDA_DTD_MAX_ACTIVE 4095u

/*
 * EDID 1.4 / CTA-861 binary-format values used by the synthesizer. Named as
 * plain literals so this header stays kernel-agnostic (the host harness has no
 * <drm/drm_edid.h>). Every value the kernel header also names is pinned to its
 * canonical macro by static_assert in lvda_kms.c, so none of these are free
 * inventions; the mirrored macro is named in each trailing comment.
 */

/* Extension-block tags (block byte 0). */
#define LVDA_EDID_EXT_CTA		0x02u	/* CEA_EXT */
#define LVDA_EDID_EXT_DISPLAYID		0x70u	/* DISPLAYID_EXT */

/* 18-byte display descriptor; byte 3 is the type tag. */
#define LVDA_EDID_DESC_LEN		18u	/* sizeof(struct detailed_timing) */
#define LVDA_EDID_NAME_CHARS		13u	/* detailed_data_string.str[13] */
#define LVDA_EDID_DESC_RANGE		0xFDu	/* EDID_DETAIL_MONITOR_RANGE */
#define LVDA_EDID_DESC_NAME		0xFCu	/* EDID_DETAIL_MONITOR_NAME */
#define LVDA_EDID_DESC_DUMMY		0x10u	/* dummy descriptor; no kernel macro */

/* Video input definition (base byte 0x14): digital, 8/10 bpc, DisplayPort. */
#define LVDA_EDID_INPUT_DIGITAL		0x80u		/* DRM_EDID_INPUT_DIGITAL */
#define LVDA_EDID_DEPTH_8		(2u << 4)	/* DRM_EDID_DIGITAL_DEPTH_8 */
#define LVDA_EDID_DEPTH_10		(3u << 4)	/* DRM_EDID_DIGITAL_DEPTH_10 */
#define LVDA_EDID_TYPE_DP		0x05u		/* DRM_EDID_DIGITAL_TYPE_DP */

/* Feature-support byte (base byte 0x18). */
#define LVDA_EDID_FEATURE_STANDBY	0x80u	/* DRM_EDID_FEATURE_PM_STANDBY */
#define LVDA_EDID_FEATURE_STD_COLOR	0x04u	/* DRM_EDID_FEATURE_STANDARD_COLOR */
#define LVDA_EDID_FEATURE_PREFERRED	0x02u	/* DRM_EDID_FEATURE_PREFERRED_TIMING */
#define LVDA_EDID_FEATURE_CONT_FREQ	0x01u	/* DRM_EDID_FEATURE_CONTINUOUS_FREQ */

/* Detailed-timing flags byte (DTD byte 17): digital separate sync, +h / +v. */
#define LVDA_EDID_PT_HSYNC_POS		(1u << 1)	/* DRM_EDID_PT_HSYNC_POSITIVE */
#define LVDA_EDID_PT_VSYNC_POS		(1u << 2)	/* DRM_EDID_PT_VSYNC_POSITIVE */
#define LVDA_EDID_PT_SEPARATE_SYNC	(3u << 3)	/* DRM_EDID_PT_SEPARATE_SYNC */

/* Display range-limits descriptor bytes. */
#define LVDA_EDID_RANGE_LIMITS_ONLY	0x01u	/* DRM_EDID_RANGE_LIMITS_ONLY_FLAG */
#define LVDA_EDID_RANGE_OFF_MAX_VFREQ	0x02u	/* DRM_EDID_RANGE_OFFSET_MAX_VFREQ */
#define LVDA_EDID_RANGE_OFF_MAX_HFREQ	0x08u	/* DRM_EDID_RANGE_OFFSET_MAX_HFREQ */

struct lvda_edid_params {
	const u8 *client_id;   /* 16 bytes; salts the EDID serial */
	const char *name;      /* monitor name; NULL/empty -> "lvda"; <=13 chars */
	u32 width;             /* active pixels, preserved exactly */
	u32 height;            /* active lines, preserved exactly */
	u32 refresh_mhz;       /* milli-Hz */
	u32 phys_width_mm;     /* EDID physical width; 0 -> derive at 96 DPI */
	u32 phys_height_mm;    /* EDID physical height; 0 -> derive at 96 DPI */
	int hdr;               /* non-zero -> HDR10/PQ + BT.2020 + 10-bit */
	int deep_color;        /* non-zero -> advertise 10-bit even for SDR */
};

/*
 * Synthesize an EDID for the requested mode into out, which MUST be at least
 * LVDA_EDID_SIZE bytes. Modes the base DTD can encode yield a 256-byte EDID
 * (base + CTA-861); larger modes append a DisplayID Type I timing block (384).
 *
 * Returns the byte count written (LVDA_EDID_BASE_SIZE or LVDA_EDID_SIZE), or
 * a negative errno:
 *   -EINVAL    dimensions or refresh out of range
 *   -EOVERFLOW pixel clock exceeds the DisplayID 24-bit field (~167 GHz)
 */
int lvda_synth_edid(const struct lvda_edid_params *p,
		     u8 out[LVDA_EDID_SIZE]);

#endif /* LVDA_EDID_H */
