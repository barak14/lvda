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

struct reap_msg {
	uint32_t minor;
	char connector[32];
};

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
	const struct timespec ts = { .tv_sec = 0, .tv_nsec = 50L * 1000 * 1000 };
	char buf[32];
	int i;

	for (i = 0; i < 40; i++) {
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
	req.width = 1920;
	req.height = 1080;
	req.refresh_mhz = 60000;

	if (ioctl(fd, LVDA_IOC_ADD, &req) < 0)
		_exit(3);

	msg.minor = req.drm_card_minor;
	memcpy(msg.connector, req.connector_name, sizeof(msg.connector));
	if (write(wfd, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
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
	char conn[33];
	ssize_t got;
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
	got = read(pipefd[0], &msg, sizeof(msg));
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "waitpid: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (got != (ssize_t)sizeof(msg) || !WIFEXITED(status) ||
	    WEXITSTATUS(status) != 0) {
		fprintf(stderr, "child failed (got=%zd status=%d)\n", got,
			status);
		return EXIT_FAILURE;
	}

	memcpy(conn, msg.connector, 32);
	conn[32] = '\0';
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
