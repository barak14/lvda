/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * A child ADDs a monitor and _exit()s without REMOVEing
 * or closing the fd. The kernel must reap the monitor on process teardown, so
 * its connector returns to "disconnected" while the persistent card node
 * stays. Skip with success when /dev/lvda is absent.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

struct reap_msg {
	uint32_t minor;
	char connector[LVDA_CONNECTOR_NAME_LEN];
};

static int write_full(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
	char *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int card_exists(unsigned minor)
{
	char path[64];
	struct stat st;

	snprintf(path, sizeof(path), "/dev/dri/card%u", minor);
	return stat(path, &st) == 0;
}

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

/* Open, ADD, send the card minor + connector name, then _exit WITHOUT close. */
static void child(int wfd)
{
	int fd = open("/dev/lvda", O_RDWR);
	struct lvda_add req;
	struct reap_msg msg;

	if (fd < 0)
		_exit(2);

	memset(&req, 0, sizeof(req));
	req.width = TEST_WIDTH;
	req.height = TEST_HEIGHT;
	req.refresh_mhz = TEST_REFRESH_MHZ;

	if (ioctl(fd, LVDA_IOC_ADD, &req) < 0)
		_exit(3);

	msg.minor = req.drm_card_minor;
	memcpy(msg.connector, req.connector_name, sizeof(msg.connector));
	if (write_full(wfd, &msg, sizeof(msg)) != 0)
		_exit(4);

	/* Deliberately leak fd: rely on the kernel to reap on _exit. */
	_exit(0);
}

int main(void)
{
	int probe = open("/dev/lvda", O_RDWR);
	int pipefd[2];
	pid_t pid;
	struct reap_msg msg;
	char conn[LVDA_CONNECTOR_NAME_LEN + 1];
	int got_msg;
	int status;

	if (probe < 0) {
		if (errno == ENOENT || errno == EACCES) {
			printf("SKIP: /dev/lvda absent\n");
			return EXIT_SUCCESS;
		}
		fprintf(stderr, "open: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	close(probe);

	if (pipe(pipefd) < 0) {
		fprintf(stderr, "pipe: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "fork: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (pid == 0) {
		close(pipefd[0]);
		child(pipefd[1]);
	}

	close(pipefd[1]);
	got_msg = read_full(pipefd[0], &msg, sizeof(msg));
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "waitpid: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (got_msg != 0 || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child failed (got_msg=%d status=%d)\n", got_msg,
			status);
		return EXIT_FAILURE;
	}

	memcpy(conn, msg.connector, LVDA_CONNECTOR_NAME_LEN);
	conn[LVDA_CONNECTOR_NAME_LEN] = '\0';
	printf("fd_reap: child added %s on card%u then exited\n", conn,
	       (unsigned)msg.minor);

	if (wait_status(msg.minor, conn, "disconnected") != 0) {
		fprintf(stderr, "connector %s not reaped after child exit\n",
			conn);
		return EXIT_FAILURE;
	}

	if (!card_exists(msg.minor)) {
		fprintf(stderr, "card%u vanished — should be persistent\n",
			(unsigned)msg.minor);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
