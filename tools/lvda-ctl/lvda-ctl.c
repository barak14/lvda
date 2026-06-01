/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * lvda-ctl — userspace CLI driving /dev/lvda. The virtual display lives for
 * as long as some process keeps the /dev/lvda fd open. `up` creates the
 * display, forks a daemon that parks on the fd, and returns to the shell;
 * `down` terminates that daemon; `status` lists live displays.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../../uapi/lvda.h"

#define DEV_PATH		"/dev/lvda"
#define RENDEZVOUS_DIR		"/run/lvda"
#define DEFAULT_CARD_OUT	RENDEZVOUS_DIR "/card"

#define DEFAULT_WIDTH		1920
#define DEFAULT_HEIGHT		1080
#define DEFAULT_FPS		60

/* Distinct exit code so callers can tell "module not loaded" from a real
 * failure (e.g. a bad request the kernel rejected). */
#define EXIT_NO_DEVICE		2

static const char *progname = "lvda-ctl";

/* Open /dev/lvda, mapping an absent node to EXIT_NO_DEVICE. On failure
 * prints a message and stores the process exit code in *code. */
static int open_device(int *code)
{
	int fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		if (errno == ENOENT) {
			fprintf(stderr,
				"%s: %s not present — is the lvda module loaded?\n",
				progname, DEV_PATH);
			*code = EXIT_NO_DEVICE;
		} else {
			fprintf(stderr, "%s: open %s: %s\n", progname,
				DEV_PATH, strerror(errno));
			*code = EXIT_FAILURE;
		}
	}
	return fd;
}

/* mkdir -p for every component of dir (modifies dir in place transiently). */
static int mkdirs(char *dir)
{
	for (char *p = dir + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			*p = '/';
			return -1;
		}
		*p = '/';
	}
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static void ensure_parent_dir(const char *path)
{
	char buf[PATH_MAX];
	char *slash;

	if (strlen(path) >= sizeof(buf))
		return;
	strcpy(buf, path);
	slash = strrchr(buf, '/');
	if (!slash || slash == buf)
		return;
	*slash = '\0';
	(void)mkdirs(buf);
}

/* Atomic publish: write to <path>.tmp, fsync, rename over <path>. */
static int write_atomic(const char *path, const char *content)
{
	char tmp[PATH_MAX];
	size_t len = strlen(content);
	size_t off = 0;
	int fd;

	ensure_parent_dir(path);
	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -1;

	while (off < len) {
		ssize_t n = write(fd, content + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmp);
			return -1;
		}
		off += (size_t)n;
	}
	fsync(fd);
	close(fd);

	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/*
 * FNV-1a 128-bit over the given NUL-terminated parts, little-endian output.
 * A 0xff domain separator between parts keeps ("a","bc") distinct from
 * ("ab","c").
 */
static void fnv1a_128(const char *const parts[], size_t nparts, uint8_t out[16])
{
	const __uint128_t prime =
		((__uint128_t)0x0000000001000000ULL << 64) | 0x000000000000013bULL;
	__uint128_t h =
		((__uint128_t)0x6c62272e07bb0142ULL << 64) | 0x62b821756295c58dULL;

	for (size_t i = 0; i < nparts; i++) {
		for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++) {
			h ^= (__uint128_t)*p;
			h *= prime;
		}
		h ^= (__uint128_t)0xff;
		h *= prime;
	}
	for (int i = 0; i < 16; i++) {
		out[i] = (uint8_t)(h & 0xff);
		h >>= 8;
	}
}

static int hex_nibble(char c, uint8_t *out)
{
	if (c >= '0' && c <= '9')
		*out = (uint8_t)(c - '0');
	else if (c >= 'a' && c <= 'f')
		*out = (uint8_t)(c - 'a' + 10);
	else if (c >= 'A' && c <= 'F')
		*out = (uint8_t)(c - 'A' + 10);
	else
		return -1;
	return 0;
}

/* Parse exactly 32 hex chars into 16 bytes. Returns 0 or -1. */
static int parse_client_id(const char *s, uint8_t out[16])
{
	if (strlen(s) != 32)
		return -1;
	for (int i = 0; i < 16; i++) {
		uint8_t hi, lo;

		if (hex_nibble(s[2 * i], &hi) < 0 || hex_nibble(s[2 * i + 1], &lo) < 0)
			return -1;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

static void derive_client_id(uint8_t out[16])
{
	const char *app_id = getenv("SUNSHINE_APP_ID");
	const char *app_name = getenv("SUNSHINE_APP_NAME");
	const char *parts[3] = {
		"lvda-ctl/v1",
		app_id ? app_id : "",
		app_name ? app_name : "",
	};

	fnv1a_128(parts, 3, out);
}

static void format_client_id(const uint8_t id[16], char out[33])
{
	for (int i = 0; i < 16; i++)
		sprintf(out + 2 * i, "%02x", id[i]);
	out[32] = '\0';
}

/* Resolve a u32 setting: CLI value (>= 0) wins, else env, else fallback. */
static uint32_t resolve_u32(long cli, const char *env, uint32_t fallback)
{
	const char *s;

	if (cli >= 0)
		return (uint32_t)cli;
	s = getenv(env);
	if (s && *s) {
		char *end;
		unsigned long v = strtoul(s, &end, 10);

		if (*end == '\0')
			return (uint32_t)v;
	}
	return fallback;
}

static int env_bool(const char *env)
{
	const char *s = getenv(env);

	return s && strcmp(s, "true") == 0;
}

/* Copy a possibly-unterminated kernel buffer into a NUL-terminated string. */
static void connector_str(const __u8 in[32], char out[33])
{
	int i;

	for (i = 0; i < 32 && in[i]; i++)
		out[i] = (char)in[i];
	out[i] = '\0';
}

static long need_u32_arg(const char *flag, int *i, int argc, char **argv)
{
	char *end;
	unsigned long v;

	if (++(*i) >= argc) {
		fprintf(stderr, "%s: %s requires a value\n", progname, flag);
		exit(EXIT_FAILURE);
	}
	v = strtoul(argv[*i], &end, 10);
	if (*end != '\0') {
		fprintf(stderr, "%s: %s: invalid integer %s\n", progname, flag,
			argv[*i]);
		exit(EXIT_FAILURE);
	}
	return (long)v;
}

static const char *need_str_arg(const char *flag, int *i, int argc, char **argv)
{
	if (++(*i) >= argc) {
		fprintf(stderr, "%s: %s requires a value\n", progname, flag);
		exit(EXIT_FAILURE);
	}
	return argv[*i];
}

/*
 * The daemon half of `up`: detach, park on the held fd until a termination
 * signal arrives, then close it (reaping the display) and remove the
 * rendezvous files. NEVER returns — exits the child directly.
 */
static void run_daemon(int fd, const sigset_t *set, const char *pidfile,
		       const char *card_out)
{
	int null, sig;

	setsid();
	if (chdir("/") < 0) {
		/* Non-fatal: we just prefer not to pin a cwd. */
	}

	null = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
		if (null > STDERR_FILENO)
			close(null);
	}

	/* No ping loop: the fd itself is the liveness signal. Block until
	 * asked to stop. */
	if (sigwait(set, &sig) != 0)
		_exit(EXIT_FAILURE);

	close(fd);
	unlink(pidfile);
	unlink(card_out);
	_exit(EXIT_SUCCESS);
}

static int cmd_up(int argc, char **argv)
{
	long cli_w = -1, cli_h = -1, cli_fps = -1;
	long cli_pw = -1, cli_ph = -1;
	int cli_hdr = 0, cli_10bpc = 0;
	const char *cli_cid = NULL;
	const char *cli_name = NULL;
	const char *pidfile = NULL;
	const char *card_out = DEFAULT_CARD_OUT;
	uint8_t client_id[16];
	char cid_str[33];
	char connector[33];
	char buf[64];
	struct lvda_add req;
	uint32_t flags = 0;
	int fd, code;
	pid_t pid;

	for (int i = 0; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--width"))
			cli_w = need_u32_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--height"))
			cli_h = need_u32_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--fps"))
			cli_fps = need_u32_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--hdr"))
			cli_hdr = 1;
		else if (!strcmp(a, "--10bit"))
			cli_10bpc = 1;
		else if (!strcmp(a, "--phys-width-mm"))
			cli_pw = need_u32_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--phys-height-mm"))
			cli_ph = need_u32_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--name"))
			cli_name = need_str_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--client-id"))
			cli_cid = need_str_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--pidfile"))
			pidfile = need_str_arg(a, &i, argc, argv);
		else if (!strcmp(a, "--card-out"))
			card_out = need_str_arg(a, &i, argc, argv);
		else {
			fprintf(stderr, "%s: up: unknown argument %s\n", progname, a);
			return EXIT_FAILURE;
		}
	}

	if (cli_cid) {
		if (parse_client_id(cli_cid, client_id) < 0) {
			fprintf(stderr,
				"%s: --client-id must be 32 hex characters (16 bytes)\n",
				progname);
			return EXIT_FAILURE;
		}
	} else {
		derive_client_id(client_id);
	}

	if (cli_hdr || env_bool("SUNSHINE_CLIENT_HDR"))
		flags |= LVDA_F_HDR;

	if (cli_10bpc || env_bool("SUNSHINE_CLIENT_10BPC"))
		flags |= LVDA_F_10BPC;

	memset(&req, 0, sizeof(req));
	memcpy(req.client_id, client_id, sizeof(req.client_id));
	req.width = resolve_u32(cli_w, "SUNSHINE_CLIENT_WIDTH", DEFAULT_WIDTH);
	req.height = resolve_u32(cli_h, "SUNSHINE_CLIENT_HEIGHT", DEFAULT_HEIGHT);
	req.refresh_mhz =
		resolve_u32(cli_fps, "SUNSHINE_CLIENT_FPS", DEFAULT_FPS) * 1000u;
	req.flags = flags;
	req.phys_width_mm =
		resolve_u32(cli_pw, "SUNSHINE_CLIENT_PHYS_WIDTH_MM", 0);
	req.phys_height_mm =
		resolve_u32(cli_ph, "SUNSHINE_CLIENT_PHYS_HEIGHT_MM", 0);
	{
		const char *name = cli_name;

		if (!name)
			name = getenv("SUNSHINE_CLIENT_NAME");
		if (name && name[0])
			snprintf((char *)req.name, sizeof(req.name), "%s", name);
	}

	fd = open_device(&code);
	if (fd < 0)
		return code;

	if (ioctl(fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "%s: LVDA_IOC_ADD failed: %s\n", progname,
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	format_client_id(client_id, cid_str);
	connector_str(req.connector_name, connector);

	/* Block the termination set before forking so the child cannot miss a
	 * signal in the window before it reaches sigwait. */
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "%s: fork: %s\n", progname, strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}
	if (pid == 0) {
		run_daemon(fd, &set, pidfile ? pidfile : "", card_out);
		/* unreachable */
	}

	/* Parent: publish the card minor and the daemon pid, report, exit.
	 * Closing our fd on exit is harmless — the child holds the open file
	 * description that keeps the display alive. */
	snprintf(buf, sizeof(buf), "%u\n%s\n", req.drm_card_minor, connector);
	if (write_atomic(card_out, buf) < 0)
		fprintf(stderr, "%s: write %s: %s\n", progname, card_out,
			strerror(errno));
	if (pidfile) {
		snprintf(buf, sizeof(buf), "%d\n", (int)pid);
		if (write_atomic(pidfile, buf) < 0)
			fprintf(stderr, "%s: write %s: %s\n", progname, pidfile,
				strerror(errno));
	}

	printf("added: /dev/dri/card%u connector=%s monitor_id=%u client_id=%s\n",
	       req.drm_card_minor, connector, (unsigned)req.monitor_id, cid_str);
	return EXIT_SUCCESS;
}

/* Read a pid from a file. Returns 0 on success. */
static int read_pid(const char *path, int *pid)
{
	char buf[32];
	char *end;
	long v;
	ssize_t n;
	int fd = open(path, O_RDONLY | O_CLOEXEC);

	if (fd < 0)
		return -1;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	v = strtol(buf, &end, 10);
	if (end == buf || v <= 0)
		return -1;
	*pid = (int)v;
	return 0;
}

static int cmd_down(int argc, char **argv)
{
	const char *pidfile = NULL;
	int pid;

	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--pidfile"))
			pidfile = need_str_arg(argv[i], &i, argc, argv);
		else {
			fprintf(stderr, "%s: down: unknown argument %s\n", progname,
				argv[i]);
			return EXIT_FAILURE;
		}
	}
	if (!pidfile) {
		fprintf(stderr, "%s: down requires --pidfile <path>\n", progname);
		return EXIT_FAILURE;
	}

	if (read_pid(pidfile, &pid) < 0) {
		fprintf(stderr, "%s: read pid from %s: %s\n", progname, pidfile,
			strerror(errno));
		return EXIT_FAILURE;
	}

	if (kill((pid_t)pid, SIGTERM) == 0)
		return EXIT_SUCCESS;
	if (errno == ESRCH) {
		/* Daemon already gone; the desired state is the current state.
		 * Drop the stale pidfile and report success. */
		unlink(pidfile);
		return EXIT_SUCCESS;
	}
	fprintf(stderr, "%s: kill %d: %s\n", progname, pid, strerror(errno));
	return EXIT_FAILURE;
}

/* List *.pid rendezvous files and whether their daemon is still alive. */
static void list_daemons(void)
{
	DIR *dir = opendir(RENDEZVOUS_DIR);
	struct dirent *ent;
	int found = 0;

	if (!dir) {
		printf("daemons: none\n");
		return;
	}
	while ((ent = readdir(dir))) {
		char path[PATH_MAX];
		size_t len = strlen(ent->d_name);
		int pid, alive;

		if (len < 4 || strcmp(ent->d_name + len - 4, ".pid") != 0)
			continue;
		if ((size_t)snprintf(path, sizeof(path), "%s/%s", RENDEZVOUS_DIR,
				     ent->d_name) >= sizeof(path))
			continue;
		if (read_pid(path, &pid) < 0)
			continue;
		if (!found) {
			printf("daemons:\n");
			found = 1;
		}
		alive = kill((pid_t)pid, 0) == 0 || errno == EPERM;
		printf("  pid=%-6d alive=%s pidfile=%s\n", pid,
		       alive ? "yes" : "stale", path);
	}
	closedir(dir);
	if (!found)
		printf("daemons: none\n");
}

static int cmd_status(void)
{
	struct lvda_version v;
	int fd, code;

	fd = open_device(&code);
	if (fd < 0)
		return code;

	if (ioctl(fd, LVDA_IOC_VERSION, &v) < 0) {
		fprintf(stderr, "%s: LVDA_IOC_VERSION failed: %s\n", progname,
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}
	close(fd);

	printf("module: loaded, protocol %u.%u.%u\n", v.major, v.minor, v.patch);
	list_daemons();
	return EXIT_SUCCESS;
}

static void usage(FILE *f)
{
	fprintf(f,
		"usage: %s <command> [options]\n"
		"  up   [--width N] [--height N] [--fps N] [--hdr] [--10bit]\n"
		"       [--phys-width-mm N] [--phys-height-mm N] [--name S]\n"
		"       [--client-id <32hex>] [--pidfile P] [--card-out P]\n"
		"  down --pidfile P\n"
		"  status\n",
		progname);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(stderr);
		return EXIT_FAILURE;
	}
	if (!strcmp(argv[1], "up"))
		return cmd_up(argc - 2, argv + 2);
	if (!strcmp(argv[1], "down"))
		return cmd_down(argc - 2, argv + 2);
	if (!strcmp(argv[1], "status"))
		return cmd_status();
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(stdout);
		return EXIT_SUCCESS;
	}
	fprintf(stderr, "%s: unknown command %s\n", progname, argv[1]);
	usage(stderr);
	return EXIT_FAILURE;
}
