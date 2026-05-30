/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LVDA_EDID_H
#define LVDA_EDID_H

/*
 * Deterministic EDID synthesizer. No kernel dependency: compiled both into
 * the module and into the host test binary. See lvda-SPEC.md §6.
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

struct lvda_edid_params {
	const u8 *client_id;   /* 16 bytes; salts the EDID serial */
	u32 width;             /* active pixels, preserved exactly */
	u32 height;            /* active lines, preserved exactly */
	u32 refresh_mhz;       /* milli-Hz */
	int hdr;               /* non-zero -> HDR10/PQ + BT.2020 + 10-bit */
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
