/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * EDID bytes: ADD a monitor, read its EDID back from sysfs
 * (/sys/class/drm/card<minor>-<connector>/edid), and validate the base +
 * CTA checksums, the active resolution in the preferred DTD, and the video
 * input byte. One case for SDR, one for HDR. No libdrm — sysfs only.
 * Skip with success when /dev/lvda is absent.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../../uapi/lvda.h"

#define EDID_LEN 256
#define NS_PER_MS 1000000L
#define EDID_READ_TRIES 10
#define EDID_READ_RETRY_MS 50L
#define EDID_READ_RETRY_NS (EDID_READ_RETRY_MS * NS_PER_MS)
#define TEST_WIDTH 1920u
#define TEST_HEIGHT 1080u
#define TEST_REFRESH_MHZ 60000u

/*
 * Read the connector EDID from sysfs into buf[EDID_LEN]. The edid property
 * can appear slightly after the card registers, so retry a few times.
 * Returns the byte count read, or -1 on persistent failure.
 */
static int read_sysfs_edid(unsigned minor, const char *connector,
			   unsigned char *buf)
{
	char path[128];
	const struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = EDID_READ_RETRY_NS,
	};

	snprintf(path, sizeof(path), "/sys/class/drm/card%u-%s/edid",
		 minor, connector);

	for (int attempt = 0; attempt < EDID_READ_TRIES; attempt++) {
		int fd = open(path, O_RDONLY);
		if (fd >= 0) {
			ssize_t total = 0;
			while (total < EDID_LEN) {
				ssize_t n = read(fd, buf + total,
						 (size_t)(EDID_LEN - total));
				if (n < 0) {
					if (errno == EINTR)
						continue;
					break;
				}
				if (n == 0)
					break;
				total += n;
			}
			close(fd);
			if (total == EDID_LEN)
				return (int)total;
		}
		(void)nanosleep(&ts, NULL);
	}
	return -1;
}

/* Sum bytes [from, to) modulo 256. */
static unsigned block_sum(const unsigned char *buf, int from, int to)
{
	unsigned s = 0;
	for (int i = from; i < to; i++)
		s += buf[i];
	return s & 0xFFu;
}

/*
 * Force a connector reprobe so the kernel populates the sysfs edid file. A
 * write of "detect" to the status attribute triggers the probe. Errors are
 * ignored — read_sysfs_edid retries.
 */
static void force_detect(unsigned minor, const char *conn)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/class/drm/card%u-%s/status",
		 minor, conn);
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return;
	(void)write(fd, "detect", 6);
	close(fd);
}

/* ADD a monitor on a fresh /dev/lvda fd. Returns the fd (>=0), or:
 *   -1  -> /dev/lvda absent (skip)
 *   -2  -> ADD failed (test failure)
 * On success fills *minor and conn[LVDA_CONNECTOR_NAME_LEN + 1]
 * (NUL-terminated). */
static int create_display(unsigned flags, unsigned *minor, char *conn)
{
	int fd = open("/dev/lvda", O_RDWR);
	if (fd < 0) {
		if (errno == ENOENT || errno == EACCES)
			return -1;
		fprintf(stderr, "open: %s\n", strerror(errno));
		return -2;
	}

	struct lvda_add req;
	memset(&req, 0, sizeof(req));
	req.width = TEST_WIDTH;
	req.height = TEST_HEIGHT;
	req.refresh_mhz = TEST_REFRESH_MHZ;
	req.flags = flags;

	if (ioctl(fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "LVDA_IOC_ADD: %s\n", strerror(errno));
		close(fd);
		return -2;
	}

	*minor = req.drm_card_minor;
	memcpy(conn, req.connector_name, LVDA_CONNECTOR_NAME_LEN);
	conn[LVDA_CONNECTOR_NAME_LEN] = '\0';
	force_detect(*minor, conn);
	return fd;
}

/*
 * Validate one EDID. Returns 0 on success, non-zero on assertion failure.
 * expect_video_in is the byte 0x14 value; want_hdr_block asserts the CTA
 * HDR Static Metadata block (extended tag 0x06) is present iff true.
 */
static int check_edid(const unsigned char *e, unsigned char expect_video_in,
		      int want_hdr_block, const char *tag)
{
	int ok = 1;

	if (block_sum(e, 0, 128) != 0) {
		fprintf(stderr, "%s: base checksum nonzero\n", tag);
		ok = 0;
	}
	if (block_sum(e, 128, 256) != 0) {
		fprintf(stderr, "%s: CTA checksum nonzero\n", tag);
		ok = 0;
	}

	/* Preferred-mode DTD at base offset 0x36. */
	unsigned hactive = e[0x38] | ((unsigned)(e[0x3A] >> 4) << 8);
	unsigned vactive = e[0x3B] | ((unsigned)(e[0x3D] >> 4) << 8);
	if (hactive != TEST_WIDTH) {
		fprintf(stderr, "%s: DTD hactive=%u, want %u\n", tag,
			hactive, TEST_WIDTH);
		ok = 0;
	}
	if (vactive != TEST_HEIGHT) {
		fprintf(stderr, "%s: DTD vactive=%u, want %u\n", tag,
			vactive, TEST_HEIGHT);
		ok = 0;
	}

	if (e[0x14] != expect_video_in) {
		fprintf(stderr, "%s: video input 0x%02x, want 0x%02x\n",
			tag, e[0x14], expect_video_in);
		ok = 0;
	}

	/*
	 * Walk the CTA-861 data block collection (extension byte 0x04
	 * onward) looking for an extended-tag block (tag type 0x07) with
	 * extended tag 0x06 (HDR Static Metadata). Stop at the DTD
	 * offset or at the first padding (zero) header.
	 */
	int found_hdr = 0;
	int dtd_off = e[128 + 0x02];
	int end = (dtd_off > 4 && dtd_off < 128) ? 128 + dtd_off : 128 + 127;
	int pos = 128 + 4;
	while (pos < end) {
		unsigned char hdr = e[pos];
		if (hdr == 0x00)
			break;
		int btag = hdr >> 5;
		int blen = hdr & 0x1F;
		if (btag == 0x07 && blen >= 1 && e[pos + 1] == 0x06)
			found_hdr = 1;
		pos += 1 + blen;
	}

	if (want_hdr_block && !found_hdr) {
		fprintf(stderr, "%s: HDR Static Metadata block missing\n", tag);
		ok = 0;
	}
	if (!want_hdr_block && found_hdr) {
		fprintf(stderr, "%s: unexpected HDR Static Metadata block\n",
			tag);
		ok = 0;
	}

	return ok ? 0 : 1;
}

int main(void)
{
	unsigned char edid[EDID_LEN];
	unsigned minor;
	char conn[LVDA_CONNECTOR_NAME_LEN + 1];
	int rc = 0;

	/* Case 1: SDR, 8 bpc -> 0x80 | (0x02 << 4) | 0x05 == 0xA5. */
	int fd = create_display(0, &minor, conn);
	if (fd == -1) {
		printf("SKIP: /dev/lvda absent\n");
		return EXIT_SUCCESS;
	}
	if (fd == -2)
		return EXIT_FAILURE;

	if (read_sysfs_edid(minor, conn, edid) != EDID_LEN) {
		fprintf(stderr, "SDR: could not read 256-byte EDID for "
			"card%u-%s\n", minor, conn);
		close(fd);
		return EXIT_FAILURE;
	}
	if (check_edid(edid, 0xA5, 0, "SDR") == 0)
		printf("PASS: edid_bytes SDR (card%u-%s)\n", minor, conn);
	else {
		printf("FAIL: edid_bytes SDR\n");
		rc = 1;
	}
	close(fd);

	/* Case 2: HDR, 10 bpc -> 0x80 | (0x03 << 4) | 0x05 == 0xB5. */
	fd = create_display(LVDA_F_HDR, &minor, conn);
	if (fd == -2)
		return EXIT_FAILURE;
	/* fd == -1 cannot happen here: the SDR open already succeeded. */

	if (read_sysfs_edid(minor, conn, edid) != EDID_LEN) {
		fprintf(stderr, "HDR: could not read 256-byte EDID for "
			"card%u-%s\n", minor, conn);
		close(fd);
		return EXIT_FAILURE;
	}
	if (check_edid(edid, 0xB5, 1, "HDR") == 0)
		printf("PASS: edid_bytes HDR (card%u-%s)\n", minor, conn);
	else {
		printf("FAIL: edid_bytes HDR\n");
		rc = 1;
	}
	close(fd);

	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}
