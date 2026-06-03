/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * The lvda card is persistent. LVDA_IOC_ADD a
 * 1920x1080@60 SDR monitor, assert its connector reports "connected" and the
 * card node exists; LVDA_IOC_REMOVE it, assert the connector reports
 * "disconnected" and the card node STILL exists (it is never destroyed).
 * Skip with success when /dev/lvda is absent.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../uapi/lvda.h"

#define NS_PER_MS 1000000L
#define CONNECTOR_STATUS_TRIES 40
#define CONNECTOR_STATUS_POLL_MS 50L
#define CONNECTOR_STATUS_POLL_NS (CONNECTOR_STATUS_POLL_MS * NS_PER_MS)
#define TEST_WIDTH 1920u
#define TEST_HEIGHT 1080u
#define TEST_REFRESH_MHZ 60000u

static int card_exists(unsigned minor)
{
	char path[64];
	struct stat st;

	snprintf(path, sizeof(path), "/dev/dri/card%u", minor);
	return stat(path, &st) == 0;
}

/* Read /sys/class/drm/card<minor>-<conn>/status into buf (NUL-terminated,
 * newline stripped). Returns 0 on success. */
static int read_status(unsigned minor, const char *conn, char *buf, size_t n)
{
	char path[128];
	int fd;
	ssize_t r;
	char *nl;

	snprintf(path, sizeof(path), "/sys/class/drm/card%u-%s/status",
		 minor, conn);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = '\0';
	nl = strchr(buf, '\n');
	if (nl)
		*nl = '\0';
	return 0;
}

/* Poll the connector status until it equals want (~2s). Returns 0 on match. */
static int wait_status(unsigned minor, const char *conn, const char *want)
{
	const struct timespec ts = {
		.tv_sec = 0,
		.tv_nsec = CONNECTOR_STATUS_POLL_NS,
	};
	char buf[32];
	int i;

	for (i = 0; i < CONNECTOR_STATUS_TRIES; i++) {
		if (read_status(minor, conn, buf, sizeof(buf)) == 0 &&
		    strcmp(buf, want) == 0)
			return 0;
		(void)nanosleep(&ts, NULL);
	}
	return -1;
}

int main(void)
{
	int fd = open("/dev/lvda", O_RDWR);
	struct lvda_add req;
	struct lvda_remove rm;
	char conn[LVDA_CONNECTOR_NAME_LEN + 1];

	if (fd < 0) {
		if (errno == ENOENT || errno == EACCES) {
			printf("SKIP: /dev/lvda absent\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, "open: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	memset(&req, 0, sizeof(req));
	req.width = TEST_WIDTH;
	req.height = TEST_HEIGHT;
	req.refresh_mhz = TEST_REFRESH_MHZ;
	req.flags = 0;

	if (ioctl(fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "LVDA_IOC_ADD: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (req.drm_card_minor > 1023) {
		fprintf(stderr, "implausible drm_card_minor=%u\n",
			(unsigned)req.drm_card_minor);
		close(fd);
		return EXIT_FAILURE;
	}
	if (req.connector_name[0] == '\0') {
		fprintf(stderr, "empty connector_name\n");
		close(fd);
		return EXIT_FAILURE;
	}

	memcpy(conn, req.connector_name, LVDA_CONNECTOR_NAME_LEN);
	conn[LVDA_CONNECTOR_NAME_LEN] = '\0';
	printf("add_remove: minor=%u connector=%s monitor_id=%u\n",
	       (unsigned)req.drm_card_minor, conn, (unsigned)req.monitor_id);

	if (!card_exists(req.drm_card_minor)) {
		fprintf(stderr, "card%u missing after ADD\n",
			(unsigned)req.drm_card_minor);
		close(fd);
		return EXIT_FAILURE;
	}

	if (wait_status(req.drm_card_minor, conn, "connected") != 0) {
		fprintf(stderr, "connector %s never reported connected\n", conn);
		close(fd);
		return EXIT_FAILURE;
	}

	memset(&rm, 0, sizeof(rm));
	rm.monitor_id = req.monitor_id;
	if (ioctl(fd, LVDA_IOC_REMOVE, &rm) < 0) {
		fprintf(stderr, "LVDA_IOC_REMOVE: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	if (wait_status(req.drm_card_minor, conn, "disconnected") != 0) {
		fprintf(stderr, "connector %s never reported disconnected\n",
			conn);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);

	/* The card is persistent; closing the controlling fd must NOT remove
	 * it (only its monitors are reaped). */
	if (!card_exists(req.drm_card_minor)) {
		fprintf(stderr, "card%u vanished — should be persistent\n",
			(unsigned)req.drm_card_minor);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
