/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deterministic EDID synthesizer (lvda-SPEC.md §6). Dual-build: the same
 * translation unit is linked into the kernel module and the host test
 * binary, so it leans only on memset/memcpy and the fixed-width typedefs
 * from lvda_edid.h. No floating point, no allocation.
 */

#ifdef __KERNEL__
#include <linux/string.h>
#include <linux/errno.h>
#else
#include <string.h>
#include <errno.h>
#endif
#include "lvda_edid.h"

/* §6.1: salts the 32-bit EDID serial. Exactly 16 bytes, no NUL. */
static const u8 KEY[16] = {
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
	u64 k1 = read_le64(key + 8);
	u64 v0 = 0x736f6d6570736575ULL ^ k0;
	u64 v1 = 0x646f72616e646f6dULL ^ k1;
	u64 v2 = 0x6c7967656e657261ULL ^ k0;
	u64 v3 = 0x7465646279746573ULL ^ k1;
	u64 b = (u64)len << 56;
	const u8 *end = msg + (len & ~(u32)7);
	u32 left = len & 7;
	u32 i;

	for (; msg != end; msg += 8) {
		u64 m = read_le64(msg);

		v3 ^= m;
		SIPROUND;
		SIPROUND;
		v0 ^= m;
	}

	for (i = 0; i < left; i++)
		b |= (u64)msg[i] << (8 * i);

	v3 ^= b;
	SIPROUND;
	SIPROUND;
	v0 ^= b;
	v2 ^= 0xff;
	SIPROUND;
	SIPROUND;
	SIPROUND;
	SIPROUND;

	return v0 ^ v1 ^ v2 ^ v3;
}

/* ---- CVT Reduced-Blanking v1 (§6.3) ---- */

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

/* CVT vsync width (lines) from the aspect ratio of the blanking-rounded
 * width against the active height; see drm_cvt_mode().
 */
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
	u32 hbase = width - (width % 8);	/* character-cell granularity */
	u32 vsync = cvt_vsync(hbase, height);
	u64 frame_period_ns = 1000000000000ULL / refresh_mhz;
	u64 active_period_ns;
	u64 hperiod_ns;
	u64 pixel_clock_khz;
	u32 vbilines, min_vbi;

	if (frame_period_ns < 460000)
		return -EINVAL;
	active_period_ns = frame_period_ns - 460000;

	hperiod_ns = active_period_ns / height;
	if (hperiod_ns == 0)
		return -EOVERFLOW;

	vbilines = (u32)(460000ULL / hperiod_ns) + 1;
	min_vbi = 3 + vsync + 6;	/* VFPORCH + vsync + VBPORCH */
	if (vbilines < min_vbi)
		vbilines = min_vbi;

	t->hdisplay = width;
	t->vdisplay = height;
	t->htotal = hbase + 160;	/* CVT_RB_H_BLANK */
	t->vtotal = height + vbilines;
	t->hsync_end = hbase + 80;	/* hbase + H_BLANK/2 */
	t->hsync_start = t->hsync_end - 32;	/* hsync_end - H_SYNC */
	t->vsync_start = height + 3;	/* VFPORCH */
	t->vsync_end = t->vsync_start + vsync;

	pixel_clock_khz = (u64)t->htotal * 1000000ULL / hperiod_ns;
	t->pixel_clock_khz = (u32)pixel_clock_khz;

	return 0;
}

/* ---- Base block (§6.1) ---- */

/* §6.1: precomputed 10-bit chromaticity packings (no runtime FP). */
static const u8 CHROMA_BT709[10] = {
	0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
};
static const u8 CHROMA_BT2020[10] = {
	0x78, 0xB1, 0xB5, 0x4A, 0x2B, 0xCC, 0x21, 0x0B, 0x50, 0x54,
};

static u8 clamp_u8_min1(u32 v)
{
	if (v == 0)
		return 1;
	if (v > 0xFF)
		return 0xFF;
	return (u8)v;
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

	d[0] = (u8)(pclk & 0xFF);
	d[1] = (u8)((pclk >> 8) & 0xFF);
	d[2] = (u8)(h_active & 0xFF);
	d[3] = (u8)(h_blank & 0xFF);
	d[4] = (u8)((((h_active >> 8) & 0xF) << 4) | ((h_blank >> 8) & 0xF));
	d[5] = (u8)(v_active & 0xFF);
	d[6] = (u8)(v_blank & 0xFF);
	d[7] = (u8)((((v_active >> 8) & 0xF) << 4) | ((v_blank >> 8) & 0xF));
	d[8] = (u8)(hso & 0xFF);
	d[9] = (u8)(hsp & 0xFF);
	d[10] = (u8)(((vso & 0xF) << 4) | (vsp & 0xF));
	d[11] = (u8)((((hso >> 8) & 0x3) << 6) | (((hsp >> 8) & 0x3) << 4) |
		     (((vso >> 4) & 0x3) << 2) | ((vsp >> 4) & 0x3));
	d[12] = (u8)(h_mm & 0xFF);
	d[13] = (u8)(v_mm & 0xFF);
	d[14] = (u8)((((h_mm >> 8) & 0xF) << 4) | ((v_mm >> 8) & 0xF));
	d[15] = 0;	/* h border */
	d[16] = 0;	/* v border */
	d[17] = 0x1E;	/* digital separate sync, hsync+ / vsync+ */
}

static void build_range_limits(const struct lvda_timing *t, u32 refresh_mhz,
			       u8 d[18])
{
	u32 v_hz = (refresh_mhz + 500) / 1000;
	u32 hrate_khz = (t->pixel_clock_khz + t->htotal / 2) / t->htotal;
	u32 max_pclk_10mhz = (t->pixel_clock_khz + 9999) / 10000;
	u8 vmax, hmax, offsets = 0;
	int i;

	if (v_hz < 1)
		v_hz = 1;
	/* EDID 1.4 rate fields are 8-bit; the byte-4 +255 offset flags extend
	 * the max to 510 so the descriptor still covers DisplayID-class modes. */
	if (v_hz > 0xFF) {
		offsets |= 0x02;	/* DRM_EDID_RANGE_OFFSET_MAX_VFREQ */
		vmax = (v_hz - 255 > 0xFF) ? 0xFF : (u8)(v_hz - 255);
	} else {
		vmax = (u8)v_hz;
	}
	if (hrate_khz > 0xFF) {
		offsets |= 0x08;	/* DRM_EDID_RANGE_OFFSET_MAX_HFREQ */
		hmax = (hrate_khz - 255 > 0xFF) ? 0xFF : (u8)(hrate_khz - 255);
	} else {
		hmax = (u8)hrate_khz;
	}

	memset(d, 0, 18);
	d[3] = 0xFD;		/* display range limits tag */
	d[4] = offsets;		/* 1.4 max-rate +255 offset flags */
	d[5] = 1;		/* min vertical rate (Hz) */
	d[6] = vmax;		/* max vertical rate (Hz; +255 when flagged) */
	d[7] = 1;		/* min horizontal rate (kHz) */
	d[8] = hmax;		/* max horizontal rate (kHz; +255 when flagged) */
	d[9] = (max_pclk_10mhz > 0xFF) ? 0xFF : (u8)max_pclk_10mhz;
	d[10] = 0x01;		/* video timing support: Range Limits Only */
	d[11] = 0x0A;		/* line feed */
	for (i = 12; i < 18; i++)
		d[i] = 0x20;
}

static void build_name_descriptor(u8 d[18])
{
	static const char name[] = "lvda";
	u32 n = (u32)(sizeof(name) - 1);	/* 4 */
	u32 i;

	memset(d, 0, 18);
	d[3] = 0xFC;		/* monitor name tag */
	memcpy(d + 5, name, n);
	d[5 + n] = 0x0A;
	for (i = 5 + n + 1; i < 18; i++)
		d[i] = 0x20;
}

static void build_dummy_descriptor(u8 d[18])
{
	memset(d, 0, 18);
	d[3] = 0x10;		/* dummy descriptor tag */
}

static u8 edid_checksum(const u8 *block)
{
	u32 sum = 0;
	u32 i;

	for (i = 0; i < 127; i++)
		sum += block[i];
	return (u8)((256 - (sum & 0xFF)) & 0xFF);
}

static void build_base_block(const struct lvda_edid_params *p,
			     const struct lvda_timing *dtd,
			     const struct lvda_timing *range,
			     int preferred, u8 ext_count, u8 *b)
{
	static const u8 header[8] = {
		0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
	};
	/* PNP "LVD": each letter (c-'@')&0x1F, packed (a<<10)|(b<<5)|c. */
	u32 pnp = (((u32)('L' - '@') & 0x1F) << 10) |
		  (((u32)('V' - '@') & 0x1F) << 5) |
		  ((u32)('D' - '@') & 0x1F);
	/* §6.1: the EDID serial is normally siphash24(KEY, client_id, 16),
	 * giving a stable per-client identity that's salt-dependent. As a
	 * documented sentinel for 'no caller identity supplied', an all-zero
	 * client_id maps to serial 0; this lets userspace templates (e.g.
	 * display-manager monitors.xml configs that pin the ambient lvda
	 * connector) target a stable identity without depending on KEY. */
	u32 serial = 0;
	{
		int i;
		for (i = 0; i < 16; i++)
			if (p->client_id[i]) {
				serial = (u32)siphash24(KEY, p->client_id, 16);
				break;
			}
	}
	u8 depth = p->hdr ? 0x3 : 0x2;	/* 10 bpc (HDR) / 8 bpc (SDR) */
	u8 h_cm = clamp_u8_min1((u32)((u64)p->width * 254 / (96 * 100)));
	u8 v_cm = clamp_u8_min1((u32)((u64)p->height * 254 / (96 * 100)));

	memset(b, 0, 128);
	memcpy(b, header, 8);

	b[0x08] = (u8)((pnp >> 8) & 0xFF);	/* big-endian PNP */
	b[0x09] = (u8)(pnp & 0xFF);
	b[0x0A] = 0x01;		/* product code u16 LE = 0x0001 */
	b[0x0B] = 0x00;
	b[0x0C] = (u8)(serial & 0xFF);		/* serial LE */
	b[0x0D] = (u8)((serial >> 8) & 0xFF);
	b[0x0E] = (u8)((serial >> 16) & 0xFF);
	b[0x0F] = (u8)((serial >> 24) & 0xFF);
	b[0x10] = 0xFF;		/* week: model-year flag */
	b[0x11] = 36;		/* year = 2026 - 1990 */
	b[0x12] = 0x01;		/* EDID version */
	b[0x13] = 0x04;		/* EDID revision */
	b[0x14] = (u8)(0x80 | (depth << 4) | 0x05);	/* digital, DP */
	b[0x15] = h_cm;
	b[0x16] = v_cm;
	b[0x17] = 120;		/* gamma 2.2 */
	/* standby + sRGB-default(iff SDR) + preferred-native(iff the base DTD
	 * carries the real mode) + cont-freq. 0x80 = standby (cosmetic). */
	b[0x18] = (u8)(0x80 | (p->hdr ? 0 : 0x04) |
		       (preferred ? 0x02 : 0) | 0x01);
	memcpy(b + 0x19, p->hdr ? CHROMA_BT2020 : CHROMA_BT709, 10);
	b[0x23] = 0x20;		/* established timings I: 640x480@60 */
	b[0x24] = 0x00;
	b[0x25] = 0x00;
	memset(b + 0x26, 0x01, 16);	/* standard timings: unused markers */

	build_dtd(dtd, h_cm, v_cm, b + 0x36);
	build_range_limits(range, p->refresh_mhz, b + 0x48);
	build_name_descriptor(b + 0x5A);
	build_dummy_descriptor(b + 0x6C);

	b[0x7E] = ext_count;	/* extension blocks following the base */
	b[0x7F] = edid_checksum(b);
}

/* ---- CTA-861 extension (§6.2) ---- */

static void build_cta_block(const struct lvda_edid_params *p, u8 *b)
{
	u32 q = 4;

	memset(b, 0, 128);
	b[0x00] = 0x02;		/* CTA extension tag */
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

/* DisplayID Type I detailed timing (lvda-SPEC.md §6.4): 24-bit pixel clock
 * (10 kHz units) + 16-bit active fields, so it encodes any mode the base DTD
 * cannot. Carried in its own EDID extension block (tag 0x70). */
#define LVDA_DTD_MAX_PCLK_KHZ 655350u		/* base DTD: u16 × 10 kHz */
#define LVDA_DID_MAX_PCLK_KHZ 167772160u	/* DisplayID: 24-bit × 10 kHz */

static int timing_fits_dtd(const struct lvda_timing *t)
{
	return t->hdisplay <= LVDA_DTD_MAX_ACTIVE &&
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
	u8 h_cm = clamp_u8_min1((u32)((u64)t->hdisplay * 254 / (96 * 100)));
	u8 v_cm = clamp_u8_min1((u32)((u64)t->vdisplay * 254 / (96 * 100)));
	u32 h_mm10 = (u32)h_cm * 100;		/* 0.1 mm units, matches base cm */
	u32 v_mm10 = (u32)v_cm * 100;
	u32 aspect = (t->hdisplay * 100 + t->vdisplay / 2) / t->vdisplay;
	u8 depth = p->hdr ? 9 : 7;		/* bpc - 1: 10 bpc (HDR) / 8 bpc */
	u32 sum = 0;
	u32 i;

	if (aspect < 100)
		aspect = 100;
	if (aspect > 355)		/* (ratio-1)*100 must fit the u8 field */
		aspect = 355;

	memset(b, 0, 128);
	b[0x00] = 0x70;		/* DISPLAYID_EXT */

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
	if (p->width < 1 || p->width > 16384)
		return -EINVAL;
	if (p->height < 1 || p->height > 16384)
		return -EINVAL;
	if (p->refresh_mhz < 1000 || p->refresh_mhz > 1000000)
		return -EINVAL;

	ret = cvt_rb_v1(p->width, p->height, p->refresh_mhz, &t);
	if (ret)
		return ret;

	if (timing_fits_dtd(&t)) {
		build_base_block(p, &t, &t, 1, 1, out);
		build_cta_block(p, out + LVDA_EDID_BLOCK);
		return LVDA_EDID_BASE_SIZE;
	}

	/* Too large for the base DTD: carry the real mode in a DisplayID block,
	 * leaving a representable 1080p60 fallback in the non-preferred DTD. The
	 * base range-limits descriptor still reflects the real mode. */
	if (t.pixel_clock_khz > LVDA_DID_MAX_PCLK_KHZ)
		return -EOVERFLOW;

	ret = cvt_rb_v1(1920, 1080, 60000, &fallback);
	if (ret)
		return ret;

	build_base_block(p, &fallback, &t, 0, 2, out);
	build_cta_block(p, out + LVDA_EDID_BLOCK);
	build_displayid_block(p, &t, out + LVDA_EDID_BASE_SIZE);

	return LVDA_EDID_SIZE;
}
