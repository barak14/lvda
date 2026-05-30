/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Host-side conformance harness for the deterministic EDID synthesizer.
 * Links lvda_edid.c directly (see Makefile) and exercises the §15.1
 * invariants without any kernel headers. Prints PASS/FAIL per check and
 * exits non-zero if any check fails.
 *
 * Usage:
 *   ./test_edid              run all invariant checks
 *   ./test_edid --dump DIR   write the 5 reference fixtures into DIR
 */

/* popen/pclose/mkdir are POSIX; expose them under a strict C11 build. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../module/lvda_edid.h"

/* Base-block field offsets (lvda-SPEC.md §6). */
#define OFF_SERIAL	0x0C	/* siphash24(client_id) truncated u32 */
#define OFF_VIDEO_IN	0x14	/* 0x80 | (depth<<4) | 0x05 */
#define OFF_CHROMA	0x19	/* 10 bytes BT.709 (SDR) / BT.2020 (HDR) */
#define CHROMA_LEN	10
#define OFF_DTD1	0x36	/* preferred-mode detailed timing */
#define OFF_CTA		0x80	/* CTA-861 extension block */
#define OFF_CTA_DTD	0x82	/* DBC end / DTD offset within extension */
#define OFF_CTA_DBC	0x84	/* first data block */

/* CTA extended data-block tag for HDR Static Metadata (§6). */
#define CTA_EXT_HDR	0x06

static int failures;

static void check(int cond, const char *name)
{
	printf("%s %s\n", cond ? "PASS" : "FAIL", name);
	if (!cond)
		failures++;
}

/* DTD horizontal-active: low 8 bits at +2, high 4 bits in +4 high nibble. */
static unsigned dtd_hactive(const u8 *e)
{
	return e[OFF_DTD1 + 2] | ((unsigned)(e[OFF_DTD1 + 4] & 0xF0) << 4);
}

/* Color bit-depth code in video-input byte bits 6:4 (2=8bpc, 3=10bpc). */
static unsigned video_depth(const u8 *e)
{
	return (e[OFF_VIDEO_IN] >> 4) & 0x07;
}

static unsigned sum_block(const u8 *e, unsigned base)
{
	unsigned s = 0, i;

	for (i = 0; i < 128; i++)
		s += e[base + i];
	return s & 0xFF;
}

/* Walk the CTA data-block collection; true iff an extended block with the
 * given extended tag is present. */
static int cta_has_ext_block(const u8 *e, u8 ext_tag)
{
	unsigned i = OFF_CTA_DBC;
	unsigned end = OFF_CTA + e[OFF_CTA_DTD];

	if (end > LVDA_EDID_SIZE)
		end = LVDA_EDID_SIZE;
	while (i < end) {
		unsigned b = e[i];
		unsigned tag = (b >> 5) & 0x07;
		unsigned len = b & 0x1F;

		if (tag == 7 && i + 1 < end && e[i + 1] == ext_tag)
			return 1;
		i += len + 1;
	}
	return 0;
}

static int synth(const u8 *id, u32 w, u32 h, u32 mhz, int hdr,
		 u8 out[LVDA_EDID_SIZE])
{
	struct lvda_edid_params p = {
		.client_id = id,
		.width = w,
		.height = h,
		.refresh_mhz = mhz,
		.hdr = hdr,
	};

	return lvda_synth_edid(&p, out);
}

/* Fixed key so fixtures and determinism checks reproduce byte-for-byte. */
static const u8 ID_A[16] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};
static const u8 ID_B[16] = {
	0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88,
	0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
};

struct fixture {
	const char *name;
	u32 w, h, mhz;
	int hdr;
};

static const struct fixture FIXTURES[] = {
	{ "1080p60-sdr", 1920, 1080, 60000, 0 },
	{ "1440p144-sdr", 2560, 1440, 144000, 0 },
	{ "1080p120-hdr", 1920, 1080, 120000, 1 },
	{ "4k60-hdr", 3840, 2160, 60000, 1 },
	{ "1366x768-sdr", 1366, 768, 60000, 0 },
	{ "4k120-sdr", 3840, 2160, 120000, 0 },
};
#define N_FIXTURES ((int)(sizeof(FIXTURES) / sizeof(FIXTURES[0])))

static int dump_fixtures(const char *dir)
{
	int i, rc = 0;

	mkdir(dir, 0755); /* ignore EEXIST */
	for (i = 0; i < N_FIXTURES; i++) {
		const struct fixture *f = &FIXTURES[i];
		u8 e[LVDA_EDID_SIZE];
		char path[512];
		FILE *fp;
		int r = synth(ID_A, f->w, f->h, f->mhz, f->hdr, e);

		if (r < 0) {
			fprintf(stderr, "dump: synth %s failed: %d\n",
				f->name, r);
			rc = 1;
			continue;
		}
		snprintf(path, sizeof(path), "%s/%s.bin", dir, f->name);
		fp = fopen(path, "wb");
		if (!fp) {
			fprintf(stderr, "dump: open %s failed\n", path);
			rc = 1;
			continue;
		}
		if (fwrite(e, 1, (size_t)r, fp) != (size_t)r)
			rc = 1;
		fclose(fp);
		printf("wrote %s\n", path);
	}
	return rc;
}

/* Compare against vectors/<name>.bin if present; skip silently when absent. */
static void check_fixture(const struct fixture *f)
{
	u8 e[LVDA_EDID_SIZE], ref[LVDA_EDID_SIZE];
	char path[512];
	char label[128];
	FILE *fp;
	size_t n;

	snprintf(path, sizeof(path), "vectors/%s.bin", f->name);
	fp = fopen(path, "rb");
	if (!fp) {
		printf("SKIP fixture %s (absent)\n", f->name);
		return;
	}
	n = fread(ref, 1, sizeof(ref), fp);
	fclose(fp);
	snprintf(label, sizeof(label), "fixture %s", f->name);
	{
		int len = synth(ID_A, f->w, f->h, f->mhz, f->hdr, e);

		check(len > 0 && (size_t)len == n && memcmp(e, ref, n) == 0,
		      label);
	}
}

/* Pipe each EDID through edid-decode --check when available; never fail. */
static int have_edid_decode(void)
{
	return system("command -v edid-decode >/dev/null 2>&1") == 0;
}

static void edid_decode_warn(const u8 *e, const char *name)
{
	FILE *fp = popen("edid-decode --check >/dev/null 2>&1", "w");
	int status;

	if (!fp)
		return;
	fwrite(e, 1, LVDA_EDID_SIZE, fp);
	status = pclose(fp);
	if (status != 0)
		printf("WARN edid-decode --check non-zero for %s (%d)\n",
		       name, status);
}

int main(int argc, char **argv)
{
	u8 a[LVDA_EDID_SIZE], b[LVDA_EDID_SIZE];
	u8 sdr[LVDA_EDID_SIZE], hdr[LVDA_EDID_SIZE];
	int i, rc;

	if (argc >= 3 && strcmp(argv[1], "--dump") == 0)
		return dump_fixtures(argv[2]);

	/* 1. Serial is a deterministic function of client_id. */
	rc = synth(ID_A, 1920, 1080, 60000, 0, a);
	check(rc > 0, "synth 1080p60 sdr ok");
	rc = synth(ID_A, 1920, 1080, 60000, 0, b);
	check(rc > 0 && memcmp(a + OFF_SERIAL, b + OFF_SERIAL, 4) == 0,
	      "serial stable for same client_id");
	rc = synth(ID_B, 1920, 1080, 60000, 0, b);
	check(rc > 0 && memcmp(a + OFF_SERIAL, b + OFF_SERIAL, 4) != 0,
	      "serial differs for different client_id");

	/* 2. Full determinism: identical params -> byte-identical output. */
	rc = synth(ID_A, 1920, 1080, 60000, 0, b);
	check(rc > 0 && memcmp(a, b, LVDA_EDID_BASE_SIZE) == 0,
	      "deterministic 256-byte output");

	/* 3. Base and CTA checksums each sum to zero mod 256. */
	check(sum_block(a, 0x00) == 0, "base checksum (byte 0x7F)");
	check(sum_block(a, OFF_CTA) == 0, "cta checksum (byte 0xFF)");

	/* 4. Exact requested width survives into the DTD. */
	rc = synth(ID_A, 1366, 768, 60000, 0, a);
	check(rc > 0 && dtd_hactive(a) == 1366, "DTD hactive == 1366");

	/* 5. A mode beyond the base DTD (8192x4320@120) is carried via a
	 * 384-byte EDID with a DisplayID Type I timing instead of rejected. */
	rc = synth(ID_A, 8192, 4320, 120000, 0, b);
	check(rc == (int)LVDA_EDID_SIZE, "8192x4320@120 -> 384-byte DisplayID EDID");

	/* 5b. 4096 wide overflows the DTD 12-bit active field but fits DisplayID. */
	rc = synth(ID_A, 4096, 2160, 60000, 0, b);
	check(rc == (int)LVDA_EDID_SIZE, "4096-wide -> DisplayID EDID");

	/* 5c. 4K@120 (~1.1 GHz): base declares 2 extensions; the DisplayID
	 * extension (tag 0x70) carries a Display Parameters block (tag 0x01)
	 * followed by a Type I detailed-timing block (tag 0x03). */
	rc = synth(ID_A, 3840, 2160, 120000, 0, b);
	check(rc == (int)LVDA_EDID_SIZE, "4K@120 -> DisplayID EDID");
	check(rc > 0 && b[0x7E] == 0x02, "4K@120 base block: 2 extensions");
	check(rc > 0 && b[LVDA_EDID_BASE_SIZE] == 0x70, "DisplayID ext tag 0x70");
	check(rc > 0 && b[LVDA_EDID_BASE_SIZE + 5] == 0x01,
	      "DisplayID Display Parameters block");
	check(rc > 0 && b[LVDA_EDID_BASE_SIZE + 20] == 0x03,
	      "DisplayID Type I detailed-timing block");

	/* 5d. A clock beyond the DisplayID 24-bit field is genuinely rejected. */
	rc = synth(ID_A, 16384, 16384, 1000000, 0, b);
	check(rc == -EOVERFLOW, "16384^2@1000Hz -> -EOVERFLOW (beyond DisplayID)");

	/* 6. HDR vs SDR: depth, chromaticity, and HDR metadata block. */
	rc = synth(ID_A, 1920, 1080, 60000, 0, sdr);
	check(rc > 0, "synth sdr ok");
	rc = synth(ID_A, 1920, 1080, 60000, 1, hdr);
	check(rc > 0, "synth hdr ok");
	check(video_depth(sdr) == 2, "sdr video depth == 8bpc (010)");
	check(video_depth(hdr) == 3, "hdr video depth == 10bpc (011)");
	check(memcmp(sdr + OFF_CHROMA, hdr + OFF_CHROMA, CHROMA_LEN) != 0,
	      "hdr chromaticity differs from sdr");
	check(cta_has_ext_block(hdr, CTA_EXT_HDR),
	      "hdr CTA carries HDR static-metadata block");
	check(!cta_has_ext_block(sdr, CTA_EXT_HDR),
	      "sdr CTA has no HDR static-metadata block");

	/* 7. Range validation. */
	check(synth(ID_A, 0, 1080, 60000, 0, b) == -EINVAL,
	      "width 0 -> -EINVAL");
	check(synth(ID_A, 1920, 1080, 500, 0, b) == -EINVAL,
	      "refresh 500 mHz -> -EINVAL");

	/* 8. Optional fixture byte-comparison. */
	for (i = 0; i < N_FIXTURES; i++)
		check_fixture(&FIXTURES[i]);

	/* Optional external validation, warn-only. */
	if (have_edid_decode()) {
		for (i = 0; i < N_FIXTURES; i++) {
			const struct fixture *f = &FIXTURES[i];
			u8 e[LVDA_EDID_SIZE];

			if (synth(ID_A, f->w, f->h, f->mhz, f->hdr, e) == 0)
				edid_decode_warn(e, f->name);
		}
	}

	if (failures) {
		printf("\n%d check(s) FAILED\n", failures);
		return 1;
	}
	printf("\nall checks passed\n");
	return 0;
}
