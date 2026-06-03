/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deterministic EDID synthesizer. Built into both the kernel module and the
 * host test binary; uses only memset/memcpy and the fixed-width typedefs from
 * lvda_edid.h.
 */

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <string.h>
#include <errno.h>
#endif
#include "lvda_edid.h"
#include "../uapi/lvda.h"

#define LVDA_SIPHASH_KEY_LEN	16u
#define LVDA_SIPHASH_HALF_KEY	8u
#define LVDA_SIPHASH_LEN_SHIFT	56u
#define LVDA_BYTE_BITS		8u
#define LVDA_U8_MAX		0xFFu
#define LVDA_U32_MAX		0xFFFFFFFFULL
#define LVDA_CHECKSUM_MOD	256u

/* Salts the 32-bit EDID serial. Exactly LVDA_SIPHASH_KEY_LEN bytes, no NUL. */
static const u8 KEY[LVDA_SIPHASH_KEY_LEN] = {
	'l', 'v', 'd', 'a', 'c', '-', 's', 'r',
	'l', '-', 'k', 'e', 'y', '-', 'v', '1',
};

/* ---- SipHash-2-4 (Aumasson/Bernstein reference) ---- */

#define ROTL64(x, b) (u64)(((x) << (b)) | ((x) >> (64 - (b))))

#define SIPROUND							\
	do {								\
		v0 += v1; v1 = ROTL64(v1, 13); v1 ^= v0;		\
		v0 = ROTL64(v0, 32);					\
		v2 += v3; v3 = ROTL64(v3, 16); v3 ^= v2;		\
		v0 += v3; v3 = ROTL64(v3, 21); v3 ^= v0;		\
		v2 += v1; v1 = ROTL64(v1, 17); v1 ^= v2;		\
		v2 = ROTL64(v2, 32);					\
	} while (0)

static u64 read_le64(const u8 *p)
{
	return (u64)p[0] | ((u64)p[1] << 8) | ((u64)p[2] << 16) |
	       ((u64)p[3] << 24) | ((u64)p[4] << 32) | ((u64)p[5] << 40) |
	       ((u64)p[6] << 48) | ((u64)p[7] << 56);
}

static u64 siphash24(const u8 *key, const u8 *msg, u32 len)
{
	u64 k0 = read_le64(key);
	u64 k1 = read_le64(key + LVDA_SIPHASH_HALF_KEY);
	u64 v0 = 0x736f6d6570736575ULL ^ k0;
	u64 v1 = 0x646f72616e646f6dULL ^ k1;
	u64 v2 = 0x6c7967656e657261ULL ^ k0;
	u64 v3 = 0x7465646279746573ULL ^ k1;
	u64 b = (u64)len << LVDA_SIPHASH_LEN_SHIFT;
	const u8 *end = msg + (len & ~(u32)(LVDA_SIPHASH_HALF_KEY - 1));
	u32 left = len & (LVDA_SIPHASH_HALF_KEY - 1);
	u32 i;

	for (; msg != end; msg += 8) {
		u64 m = read_le64(msg);

		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}

	for (i = 0; i < left; i++)
		b |= (u64)msg[i] << (LVDA_BYTE_BITS * i);

	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= LVDA_U8_MAX;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;

	return v0 ^ v1 ^ v2 ^ v3;
}

/* ---- CVT Reduced-Blanking v1 ---- */

struct lvda_timing {
	u32 hdisplay;	/* exact requested width */
	u32 vdisplay;	/* exact requested height */
	u32 htotal;
	u32 vtotal;
	u32 hsync_start;
	u32 hsync_end;
	u32 vsync_start;
	u32 vsync_end;
	u32 pixel_clock_khz;
};

#define LVDA_EDID_MIN_PCLK_KHZ		10u
#define LVDA_EDID_KHZ_PER_MHZ		1000u
#define LVDA_EDID_KHZ_PER_10MHZ		10000u
#define LVDA_EDID_RATE_OFFSET		255u
#define LVDA_EDID_DIM_MIN		LVDA_DIM_MIN
#define LVDA_EDID_DIM_MAX		LVDA_DIM_MAX
#define LVDA_EDID_REFRESH_MHZ_MIN	LVDA_REFRESH_MHZ_MIN
#define LVDA_EDID_REFRESH_MHZ_MAX	LVDA_REFRESH_MHZ_MAX

#define LVDA_CVT_CELL_GRAN		8u
#define LVDA_CVT_RB_MIN_VBLANK_NS	460000u
#define LVDA_CVT_NS_PER_MHZ		1000000000000ULL
#define LVDA_CVT_RB_H_BLANK		160u
#define LVDA_CVT_RB_H_SYNC		32u
#define LVDA_CVT_RB_V_FRONT_PORCH	3u
#define LVDA_CVT_RB_V_BACK_PORCH	6u

/* CVT vsync width (lines) from the aspect ratio of the blanking-rounded
 * width against the active height. */
static u32 cvt_vsync(u32 hbase, u32 vdisp)
{
	if (vdisp % 3 == 0 && vdisp * 4 / 3 == hbase)
		return 4;
	if (vdisp % 9 == 0 && vdisp * 16 / 9 == hbase)
		return 5;
	if (vdisp % 10 == 0 && vdisp * 16 / 10 == hbase)
		return 6;
	if ((vdisp % 4 == 0 && vdisp * 5 / 4 == hbase) ||
	    (vdisp % 9 == 0 && vdisp * 15 / 9 == hbase))
		return 7;
	return 10;
}

static int cvt_rb_v1(u32 width, u32 height, u32 refresh_mhz,
		     struct lvda_timing *t)
{
	u32 hbase = width - (width % LVDA_CVT_CELL_GRAN);
	u32 vsync = cvt_vsync(hbase, height);
	u64 frame_period_ns = LVDA_CVT_NS_PER_MHZ / refresh_mhz;
	u64 active_period_ns;
	u64 hperiod_ns;
	u64 pixel_clock_khz;
	u32 vbilines, min_vbi;

	if (frame_period_ns < LVDA_CVT_RB_MIN_VBLANK_NS)
		return -EINVAL;
	active_period_ns = frame_period_ns - LVDA_CVT_RB_MIN_VBLANK_NS;

	hperiod_ns = active_period_ns / height;
	if (hperiod_ns == 0)
		return -EOVERFLOW;

	vbilines = (u32)(LVDA_CVT_RB_MIN_VBLANK_NS / hperiod_ns) + 1;
	min_vbi = LVDA_CVT_RB_V_FRONT_PORCH + vsync +
		  LVDA_CVT_RB_V_BACK_PORCH;
	if (vbilines < min_vbi)
		vbilines = min_vbi;

	t->hdisplay = width;
	t->vdisplay = height;
	t->htotal = hbase + LVDA_CVT_RB_H_BLANK;
	t->vtotal = height + vbilines;
	t->hsync_end = hbase + LVDA_CVT_RB_H_BLANK / 2;
	t->hsync_start = t->hsync_end - LVDA_CVT_RB_H_SYNC;
	t->vsync_start = height + LVDA_CVT_RB_V_FRONT_PORCH;
	t->vsync_end = t->vsync_start + vsync;

	pixel_clock_khz = (u64)t->htotal * LVDA_EDID_KHZ_PER_MHZ *
			  LVDA_EDID_KHZ_PER_MHZ / hperiod_ns;
	if (pixel_clock_khz < LVDA_EDID_MIN_PCLK_KHZ)
		return -EINVAL;
	if (pixel_clock_khz > LVDA_U32_MAX)
		return -EOVERFLOW;
	t->pixel_clock_khz = (u32)pixel_clock_khz;

	return 0;
}

/* ---- Base block ---- */

/* Precomputed 10-bit chromaticity packings. */
static const u8 CHROMA_BT709[10] = {
	0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
};
static const u8 CHROMA_BT2020[10] = {
	0x78, 0xB1, 0xB5, 0x4A, 0x2B, 0xCC, 0x21, 0x0B, 0x50, 0x54,
};

#define LVDA_EDID_HEADER_LEN		8u
#define LVDA_EDID_DESC_TEXT_START	5u
#define LVDA_EDID_LINE_FEED		0x0Au
#define LVDA_EDID_SPACE			0x20u
#define LVDA_EDID_MODEL_YEAR		2026u
#define LVDA_EDID_YEAR_BASE		1990u
#define LVDA_EDID_GAMMA_2_2		120u
#define LVDA_EDID_STD_TIMING_COUNT	16u
#define LVDA_EDID_STD_TIMING_UNUSED	0x01u
#define LVDA_EDID_EST_TIMING_640_480_60	0x20u
#define LVDA_EDID_FALLBACK_WIDTH	1920u
#define LVDA_EDID_FALLBACK_HEIGHT	1080u
#define LVDA_EDID_FALLBACK_REFRESH_MHZ	60000u

enum lvda_edid_base_offset {
	LVDA_BASE_VENDOR_ID = 0x08,
	LVDA_BASE_PRODUCT_CODE = 0x0A,
	LVDA_BASE_SERIAL = 0x0C,
	LVDA_BASE_WEEK = 0x10,
	LVDA_BASE_YEAR = 0x11,
	LVDA_BASE_VERSION = 0x12,
	LVDA_BASE_REVISION = 0x13,
	LVDA_BASE_VIDEO_INPUT = 0x14,
	LVDA_BASE_HSIZE_CM = 0x15,
	LVDA_BASE_VSIZE_CM = 0x16,
	LVDA_BASE_GAMMA = 0x17,
	LVDA_BASE_FEATURES = 0x18,
	LVDA_BASE_CHROMA = 0x19,
	LVDA_BASE_EST_TIMING = 0x23,
	LVDA_BASE_STD_TIMING = 0x26,
	LVDA_BASE_DTD1 = 0x36,
	LVDA_BASE_DTD2 = 0x48,
	LVDA_BASE_DTD3 = 0x5A,
	LVDA_BASE_DTD4 = 0x6C,
	LVDA_BASE_EXT_COUNT = 0x7E,
	LVDA_BASE_CHECKSUM = 0x7F,
};

static u8 clamp_u8_min1(u32 v)
{
	if (v == 0)
		return 1;
	if (v > LVDA_U8_MAX)
		return LVDA_U8_MAX;
	return (u8)v;
}

/* Physical extent in EDID centimetre units: caller millimetres rounded to the
 * nearest cm, or derived from the pixel count at 96 DPI when unset. */
static u8 phys_cm(u32 phys_mm, u32 pixels)
{
	if (phys_mm)
		return clamp_u8_min1((phys_mm + 5) / 10);
	return clamp_u8_min1((u32)((u64)pixels * 254 / (96 * 100)));
}

static void build_dtd(const struct lvda_timing *t, u8 h_cm, u8 v_cm, u8 d[18])
{
	u32 pclk = t->pixel_clock_khz / 10;	/* 10 kHz units */
	u32 h_active = t->hdisplay;
	u32 h_blank = t->htotal - t->hdisplay;
	u32 v_active = t->vdisplay;
	u32 v_blank = t->vtotal - t->vdisplay;
	u32 hso = t->hsync_start - t->hdisplay;	/* h front porch */
	u32 hsp = t->hsync_end - t->hsync_start;
	u32 vso = t->vsync_start - t->vdisplay;	/* v front porch = 3 */
	u32 vsp = t->vsync_end - t->vsync_start;
	u32 h_mm = (u32)h_cm * 10;
	u32 v_mm = (u32)v_cm * 10;

	d[0] = (u8)(pclk & LVDA_U8_MAX);
	d[1] = (u8)((pclk >> 8) & LVDA_U8_MAX);
	d[2] = (u8)(h_active & LVDA_U8_MAX);
	d[3] = (u8)(h_blank & LVDA_U8_MAX);
	d[4] = (u8)((((h_active >> 8) & 0xF) << 4) | ((h_blank >> 8) & 0xF));
	d[5] = (u8)(v_active & LVDA_U8_MAX);
	d[6] = (u8)(v_blank & LVDA_U8_MAX);
	d[7] = (u8)((((v_active >> 8) & 0xF) << 4) | ((v_blank >> 8) & 0xF));
	d[8] = (u8)(hso & LVDA_U8_MAX);
	d[9] = (u8)(hsp & LVDA_U8_MAX);
	d[10] = (u8)(((vso & 0xF) << 4) | (vsp & 0xF));
	d[11] = (u8)((((hso >> 8) & 0x3) << 6) | (((hsp >> 8) & 0x3) << 4) |
		     (((vso >> 4) & 0x3) << 2) | ((vsp >> 4) & 0x3));
	d[12] = (u8)(h_mm & LVDA_U8_MAX);
	d[13] = (u8)(v_mm & LVDA_U8_MAX);
	d[14] = (u8)((((h_mm >> 8) & 0xF) << 4) | ((v_mm >> 8) & 0xF));
	d[15] = 0;	/* h border */
	d[16] = 0;	/* v border */
	d[17] = LVDA_EDID_PT_SEPARATE_SYNC | LVDA_EDID_PT_HSYNC_POS |
		LVDA_EDID_PT_VSYNC_POS;
}

static void build_range_limits(const struct lvda_timing *t, u32 refresh_mhz,
			       u8 d[18])
{
	u32 v_hz = (refresh_mhz + LVDA_EDID_KHZ_PER_MHZ / 2) /
		   LVDA_EDID_KHZ_PER_MHZ;
	u32 hrate_khz = (t->pixel_clock_khz + t->htotal / 2) / t->htotal;
	u32 max_pclk_10mhz = (t->pixel_clock_khz +
			      LVDA_EDID_KHZ_PER_10MHZ - 1) /
			     LVDA_EDID_KHZ_PER_10MHZ;
	u8 vmax, hmax, offsets = 0;
	u32 i;

	if (v_hz < 1)
		v_hz = 1;
	/* EDID 1.4 rate fields are 8-bit; the byte-4 +255 offset flags extend
	 * the max to 510 so the descriptor still covers DisplayID-class modes. */
	if (v_hz > LVDA_U8_MAX) {
		offsets |= LVDA_EDID_RANGE_OFF_MAX_VFREQ;
		vmax = (v_hz - LVDA_EDID_RATE_OFFSET > LVDA_U8_MAX) ?
			LVDA_U8_MAX : (u8)(v_hz - LVDA_EDID_RATE_OFFSET);
	} else {
		vmax = (u8)v_hz;
	}
	if (hrate_khz > LVDA_U8_MAX) {
		offsets |= LVDA_EDID_RANGE_OFF_MAX_HFREQ;
		hmax = (hrate_khz - LVDA_EDID_RATE_OFFSET > LVDA_U8_MAX) ?
			LVDA_U8_MAX : (u8)(hrate_khz - LVDA_EDID_RATE_OFFSET);
	} else {
		hmax = (u8)hrate_khz;
	}

	memset(d, 0, LVDA_EDID_DESC_LEN);
	d[3] = LVDA_EDID_DESC_RANGE;
	d[4] = offsets;		/* 1.4 max-rate +255 offset flags */
	d[5] = 1;		/* min vertical rate (Hz) */
	d[6] = vmax;		/* max vertical rate (Hz; +255 when flagged) */
	d[7] = 1;		/* min horizontal rate (kHz) */
	d[8] = hmax;		/* max horizontal rate (kHz; +255 when flagged) */
	d[9] = (max_pclk_10mhz > LVDA_U8_MAX) ?
	       LVDA_U8_MAX : (u8)max_pclk_10mhz;
	d[10] = LVDA_EDID_RANGE_LIMITS_ONLY;
	d[11] = LVDA_EDID_LINE_FEED;
	for (i = 12; i < LVDA_EDID_DESC_LEN; i++)
		d[i] = LVDA_EDID_SPACE;
}

static void build_name_descriptor(const char *name, u8 d[18])
{
	static const char fallback[] = "lvda";
	u32 n = 0, i;

	if (!name || !name[0])
		name = fallback;

	memset(d, 0, LVDA_EDID_DESC_LEN);
	d[3] = LVDA_EDID_DESC_NAME;
	while (n < LVDA_EDID_NAME_CHARS && name[n]) {
		d[LVDA_EDID_DESC_TEXT_START + n] = (u8)name[n];
		n++;
	}
	i = LVDA_EDID_DESC_TEXT_START + n;
	if (n < LVDA_EDID_NAME_CHARS)
		d[i++] = LVDA_EDID_LINE_FEED;
	while (i < LVDA_EDID_DESC_LEN)
		d[i++] = LVDA_EDID_SPACE;
}

static void build_dummy_descriptor(u8 d[18])
{
	memset(d, 0, LVDA_EDID_DESC_LEN);
	d[3] = LVDA_EDID_DESC_DUMMY;
}

static u8 edid_checksum(const u8 *block)
{
	u32 sum = 0;
	u32 i;

	for (i = 0; i < LVDA_EDID_BLOCK - 1; i++)
		sum += block[i];
	return (u8)((LVDA_CHECKSUM_MOD - (sum & LVDA_U8_MAX)) & LVDA_U8_MAX);
}

static void build_base_block(const struct lvda_edid_params *p,
			     const struct lvda_timing *dtd,
			     const struct lvda_timing *range,
			     int preferred, u8 ext_count, u8 *b)
{
	static const u8 header[LVDA_EDID_HEADER_LEN] = {
		0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
	};
	/* PNP "LVD": each letter (c-'@')&0x1F, packed (a<<10)|(b<<5)|c. */
	u32 pnp = (((u32)('L' - '@') & 0x1F) << 10) |
		  (((u32)('V' - '@') & 0x1F) << 5) |
		  ((u32)('D' - '@') & 0x1F);
	/* EDID serial: siphash24(KEY, client_id), or 0 for an all-zero client_id. */
	u32 serial = 0;
	{
		u32 i;
		for (i = 0; i < sizeof(KEY); i++)
			if (p->client_id[i]) {
				serial = (u32)siphash24(KEY, p->client_id,
							 sizeof(KEY));
				break;
			}
	}
	u8 depth_bits = (p->hdr || p->deep_color) ?
			LVDA_EDID_DEPTH_10 : LVDA_EDID_DEPTH_8;
	u8 h_cm = phys_cm(p->phys_width_mm, p->width);
	u8 v_cm = phys_cm(p->phys_height_mm, p->height);

	memset(b, 0, LVDA_EDID_BLOCK);
	memcpy(b, header, LVDA_EDID_HEADER_LEN);

	b[LVDA_BASE_VENDOR_ID] = (u8)((pnp >> 8) & LVDA_U8_MAX);
	b[LVDA_BASE_VENDOR_ID + 1] = (u8)(pnp & LVDA_U8_MAX);
	b[LVDA_BASE_PRODUCT_CODE] = 0x01;
	b[LVDA_BASE_PRODUCT_CODE + 1] = 0x00;
	b[LVDA_BASE_SERIAL] = (u8)(serial & LVDA_U8_MAX);
	b[LVDA_BASE_SERIAL + 1] = (u8)((serial >> 8) & LVDA_U8_MAX);
	b[LVDA_BASE_SERIAL + 2] = (u8)((serial >> 16) & LVDA_U8_MAX);
	b[LVDA_BASE_SERIAL + 3] = (u8)((serial >> 24) & LVDA_U8_MAX);
	b[LVDA_BASE_WEEK] = LVDA_U8_MAX;
	b[LVDA_BASE_YEAR] = LVDA_EDID_MODEL_YEAR - LVDA_EDID_YEAR_BASE;
	b[LVDA_BASE_VERSION] = 0x01;
	b[LVDA_BASE_REVISION] = 0x04;
	b[LVDA_BASE_VIDEO_INPUT] =
		(u8)(LVDA_EDID_INPUT_DIGITAL | depth_bits | LVDA_EDID_TYPE_DP);
	b[LVDA_BASE_HSIZE_CM] = h_cm;
	b[LVDA_BASE_VSIZE_CM] = v_cm;
	b[LVDA_BASE_GAMMA] = LVDA_EDID_GAMMA_2_2;
	/* Standby (cosmetic) + sRGB default (SDR only) + preferred native
	 * timing (when the base DTD carries the real mode) + continuous freq. */
	b[LVDA_BASE_FEATURES] = (u8)(LVDA_EDID_FEATURE_STANDBY |
				     (p->hdr ? 0 : LVDA_EDID_FEATURE_STD_COLOR) |
				     (preferred ? LVDA_EDID_FEATURE_PREFERRED : 0) |
				     LVDA_EDID_FEATURE_CONT_FREQ);
	memcpy(b + LVDA_BASE_CHROMA, p->hdr ? CHROMA_BT2020 : CHROMA_BT709,
	       sizeof(CHROMA_BT709));
	b[LVDA_BASE_EST_TIMING] = LVDA_EDID_EST_TIMING_640_480_60;
	b[LVDA_BASE_EST_TIMING + 1] = 0x00;
	b[LVDA_BASE_EST_TIMING + 2] = 0x00;
	memset(b + LVDA_BASE_STD_TIMING, LVDA_EDID_STD_TIMING_UNUSED,
	       LVDA_EDID_STD_TIMING_COUNT);

	build_dtd(dtd, h_cm, v_cm, b + LVDA_BASE_DTD1);
	build_range_limits(range, p->refresh_mhz, b + LVDA_BASE_DTD2);
	build_name_descriptor(p->name, b + LVDA_BASE_DTD3);
	build_dummy_descriptor(b + LVDA_BASE_DTD4);

	b[LVDA_BASE_EXT_COUNT] = ext_count;
	b[LVDA_BASE_CHECKSUM] = edid_checksum(b);
}

/* ---- CTA-861 extension ---- */

static void build_cta_block(const struct lvda_edid_params *p, u8 *b)
{
	u32 q = 4;

	memset(b, 0, LVDA_EDID_BLOCK);
	b[0x00] = LVDA_EDID_EXT_CTA;
	b[0x01] = 0x03;		/* CTA revision 3 */
	b[0x03] = 0x00;		/* no native DTDs, no audio/YCbCr claims */

	/* Video Capability Data Block (extended tag 0x00). */
	b[q++] = 0xE2;
	b[q++] = 0x00;
	b[q++] = 0xCF;

	/* Colorimetry Data Block (extended tag 0x05). */
	b[q++] = 0xE3;
	b[q++] = 0x05;
	b[q++] = p->hdr ? 0xC0 : 0x00;	/* BT.2020 RGB|YCC for HDR */
	b[q++] = 0x20;			/* sRGB acknowledge */

	/* HDR Static Metadata Data Block (extended tag 0x06), iff HDR. */
	if (p->hdr) {
		b[q++] = 0xE6;
		b[q++] = 0x06;
		b[q++] = 0x05;	/* EOTF: SDR (bit0) + PQ/ST 2084 (bit2) */
		b[q++] = 0x01;	/* Static Metadata Type 1 */
		b[q++] = 0x96;	/* desired max luminance ~1000 nits */
		b[q++] = 0x7E;	/* max frame-average ~400 nits */
		b[q++] = 0x05;	/* min luminance ~0.05 nits */
	}

	b[0x02] = (u8)q;	/* DTD offset = end of data-block collection */
	b[0x7F] = edid_checksum(b);
}

/* DisplayID Type I detailed timing: 24-bit pixel clock (10 kHz units) +
 * 16-bit active fields, carried in its own EDID extension block (tag 0x70). */
#define LVDA_DTD_MAX_PCLK_KHZ 655350u		/* base DTD: u16 × 10 kHz */
#define LVDA_DID_MAX_PCLK_KHZ 167772160u	/* DisplayID: 24-bit × 10 kHz */

static int timing_fits_dtd(const struct lvda_timing *t)
{
	return t->pixel_clock_khz >= LVDA_EDID_MIN_PCLK_KHZ &&
	       t->hdisplay <= LVDA_DTD_MAX_ACTIVE &&
	       t->vdisplay <= LVDA_DTD_MAX_ACTIVE &&
	       t->pixel_clock_khz <= LVDA_DTD_MAX_PCLK_KHZ;
}

static void build_displayid_block(const struct lvda_edid_params *p,
				  const struct lvda_timing *t, u8 *b)
{
	u32 pclk = t->pixel_clock_khz / 10 - 1;		/* 10 kHz units; field+1 */
	u32 hactive = t->hdisplay - 1;
	u32 hblank = (t->htotal - t->hdisplay) - 1;
	u32 hfront = (t->hsync_start - t->hdisplay) - 1;
	u32 hsw = (t->hsync_end - t->hsync_start) - 1;
	u32 vactive = t->vdisplay - 1;
	u32 vblank = (t->vtotal - t->vdisplay) - 1;
	u32 vfront = (t->vsync_start - t->vdisplay) - 1;
	u32 vsw = (t->vsync_end - t->vsync_start) - 1;
	u8 h_cm = phys_cm(p->phys_width_mm, t->hdisplay);
	u8 v_cm = phys_cm(p->phys_height_mm, t->vdisplay);
	u32 h_mm10 = (u32)h_cm * 100;		/* 0.1 mm units, matches base cm */
	u32 v_mm10 = (u32)v_cm * 100;
	u32 aspect = (t->hdisplay * 100 + t->vdisplay / 2) / t->vdisplay;
	u8 depth = (p->hdr || p->deep_color) ? 9 : 7;	/* bpc-1: 10 / 8 bpc */
	u32 sum = 0;
	u32 i;

	if (aspect < 100)
		aspect = 100;
	if (aspect > 355)		/* (ratio-1)*100 must fit the u8 field */
		aspect = 355;

	memset(b, 0, LVDA_EDID_BLOCK);
	b[0x00] = LVDA_EDID_EXT_DISPLAYID;

	/* DisplayID base-section header. */
	b[0x01] = 0x12;		/* structure version 1.2 */
	b[0x02] = 38;		/* payload: 15B params block + 23B timing block */
	b[0x03] = 0x03;		/* product type: monitor */
	b[0x04] = 0x00;		/* no further sections */

	/* Display Parameters Data Block (tag 0x01), 12-byte payload. */
	b[0x05] = 0x01;		/* DATA_BLOCK_DISPLAY_PARAMETERS */
	b[0x06] = 0x00;		/* revision 0 */
	b[0x07] = 12;
	b[0x08] = (u8)(h_mm10 & 0xFF);
	b[0x09] = (u8)((h_mm10 >> 8) & 0xFF);
	b[0x0A] = (u8)(v_mm10 & 0xFF);
	b[0x0B] = (u8)((v_mm10 >> 8) & 0xFF);
	b[0x0C] = (u8)(t->hdisplay & 0xFF);		/* native horizontal pixels */
	b[0x0D] = (u8)((t->hdisplay >> 8) & 0xFF);
	b[0x0E] = (u8)(t->vdisplay & 0xFF);		/* native vertical pixels */
	b[0x0F] = (u8)((t->vdisplay >> 8) & 0xFF);
	b[0x10] = 0x00;		/* feature flags: none */
	b[0x11] = 0xFF;		/* transfer gamma: undefined (use base block) */
	b[0x12] = (u8)(aspect - 100);	/* aspect ratio = field/100 + 1 */
	b[0x13] = (u8)((depth << 4) | depth);	/* bpc: overall | native */

	/* Type I detailed-timing data block (tag 0x03), one 20-byte timing. */
	b[0x14] = 0x03;		/* DATA_BLOCK_TYPE_1_DETAILED_TIMING */
	b[0x15] = 0x00;		/* revision 0 */
	b[0x16] = 20;
	b[0x17] = (u8)(pclk & 0xFF);
	b[0x18] = (u8)((pclk >> 8) & 0xFF);
	b[0x19] = (u8)((pclk >> 16) & 0xFF);
	b[0x1A] = 0x80;		/* preferred timing */
	b[0x1B] = (u8)(hactive & 0xFF);
	b[0x1C] = (u8)((hactive >> 8) & 0xFF);
	b[0x1D] = (u8)(hblank & 0xFF);
	b[0x1E] = (u8)((hblank >> 8) & 0xFF);
	b[0x1F] = (u8)(hfront & 0xFF);
	b[0x20] = (u8)(((hfront >> 8) & 0x7F) | 0x80);	/* bit15: hsync positive */
	b[0x21] = (u8)(hsw & 0xFF);
	b[0x22] = (u8)((hsw >> 8) & 0xFF);
	b[0x23] = (u8)(vactive & 0xFF);
	b[0x24] = (u8)((vactive >> 8) & 0xFF);
	b[0x25] = (u8)(vblank & 0xFF);
	b[0x26] = (u8)((vblank >> 8) & 0xFF);
	b[0x27] = (u8)(vfront & 0xFF);
	b[0x28] = (u8)(((vfront >> 8) & 0x7F) | 0x80);	/* bit15: vsync positive */
	b[0x29] = (u8)(vsw & 0xFF);
	b[0x2A] = (u8)((vsw >> 8) & 0xFF);

	/* DisplayID section checksum: bytes [1..43] sum to 0 mod 256. */
	for (i = 1; i <= 42; i++)
		sum += b[i];
	b[0x2B] = (u8)((256 - (sum & 0xFF)) & 0xFF);

	b[0x7F] = edid_checksum(b);	/* EDID extension-block checksum */
}

int lvda_synth_edid(const struct lvda_edid_params *p,
		     u8 out[LVDA_EDID_SIZE])
{
	struct lvda_timing t, fallback;
	int ret;

	if (!p || !p->client_id || !out)
		return -EINVAL;
	if (p->width < LVDA_EDID_DIM_MIN || p->width > LVDA_EDID_DIM_MAX)
		return -EINVAL;
	if (p->height < LVDA_EDID_DIM_MIN || p->height > LVDA_EDID_DIM_MAX)
		return -EINVAL;
	if (p->refresh_mhz < LVDA_EDID_REFRESH_MHZ_MIN ||
	    p->refresh_mhz > LVDA_EDID_REFRESH_MHZ_MAX)
		return -EINVAL;

	ret = cvt_rb_v1(p->width, p->height, p->refresh_mhz, &t);
	if (ret)
		return ret;

	if (timing_fits_dtd(&t)) {
		build_base_block(p, &t, &t, 1, 1, out);
		build_cta_block(p, out + LVDA_EDID_BLOCK);
		return LVDA_EDID_BASE_SIZE;
	}

	/* Mode too large for the base DTD: carry it in a DisplayID block with a
	 * 1080p60 fallback in the non-preferred base DTD. */
	if (t.pixel_clock_khz > LVDA_DID_MAX_PCLK_KHZ)
		return -EOVERFLOW;

	ret = cvt_rb_v1(LVDA_EDID_FALLBACK_WIDTH, LVDA_EDID_FALLBACK_HEIGHT,
			LVDA_EDID_FALLBACK_REFRESH_MHZ, &fallback);
	if (ret)
		return ret;

	build_base_block(p, &fallback, &t, 0, 2, out);
	build_cta_block(p, out + LVDA_EDID_BLOCK);
	build_displayid_block(p, &t, out + LVDA_EDID_BASE_SIZE);

	return LVDA_EDID_SIZE;
}
