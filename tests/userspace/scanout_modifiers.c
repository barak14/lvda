/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Prove the scanout_modifiers module parameter end to end: ADD a monitor,
 * create a dumb buffer, AddFB2 it with the injected modifier (must succeed
 * and round-trip through GETFB2), then AddFB2 with a modifier that was NOT
 * injected (must fail).
 *
 * The injected modifier is read from LVDA_TEST_SCANOUT_MODIFIER (set by the
 * vng harness, which loads lvda.ko with a matching scanout_modifiers=...).
 * Skips with success when /dev/lvda is absent or the env var is unset, so
 * ad-hoc host runs against an unparameterized module stay green.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

#include "../../uapi/lvda.h"

/* Never part of the injected list; AddFB2 with it must be rejected. */
#define LVDA_TEST_BOGUS_MODIFIER 0x0f00dead00000000ull

int main(void)
{
	const char *env = getenv("LVDA_TEST_SCANOUT_MODIFIER");

	if (!env || !env[0]) {
		printf("SKIP: LVDA_TEST_SCANOUT_MODIFIER not set\n");
		return EXIT_SUCCESS;
	}
	uint64_t test_mod = strtoull(env, NULL, 0);

	int lvda_fd = open("/dev/lvda", O_RDWR);
	if (lvda_fd < 0) {
		printf("SKIP: /dev/lvda absent (%s)\n", strerror(errno));
		return EXIT_SUCCESS;
	}

	struct lvda_add req;
	memset(&req, 0, sizeof(req));
	req.width = 1920;
	req.height = 1080;
	req.refresh_mhz = 60000;
	if (ioctl(lvda_fd, LVDA_IOC_ADD, &req) < 0) {
		fprintf(stderr, "FAIL: LVDA_IOC_ADD: %s\n", strerror(errno));
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	char card[64];
	snprintf(card, sizeof(card), "/dev/dri/card%u",
		 (unsigned)req.drm_card_minor);

	int fd = open(card, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "FAIL: open %s: %s\n", card, strerror(errno));
		close(lvda_fd);
		return EXIT_FAILURE;
	}

	int rc = EXIT_FAILURE;
	uint32_t fb_id = 0;
	struct drm_mode_create_dumb dumb;
	memset(&dumb, 0, sizeof(dumb));
	dumb.width = req.width;
	dumb.height = req.height;
	dumb.bpp = 32;
	if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) != 0) {
		fprintf(stderr, "FAIL: CREATE_DUMB: %s\n", strerror(errno));
		goto out;
	}

	uint32_t handles[4] = { dumb.handle, 0, 0, 0 };
	uint32_t pitches[4] = { dumb.pitch, 0, 0, 0 };
	uint32_t offsets[4] = { 0, 0, 0, 0 };
	uint64_t modifiers[4] = { test_mod, 0, 0, 0 };

	/* 1. AddFB2 with the injected modifier must succeed. */
	if (drmModeAddFB2WithModifiers(fd, req.width, req.height,
				       DRM_FORMAT_XRGB8888, handles, pitches,
				       offsets, modifiers, &fb_id,
				       DRM_MODE_FB_MODIFIERS) != 0) {
		fprintf(stderr,
			"FAIL: AddFB2 with injected modifier 0x%llx: %s\n",
			(unsigned long long)test_mod, strerror(errno));
		goto out_dumb;
	}

	/* 2. GETFB2 must round-trip the modifier. */
	drmModeFB2Ptr fb2 = drmModeGetFB2(fd, fb_id);
	if (!fb2) {
		fprintf(stderr, "FAIL: GetFB2: %s\n", strerror(errno));
		goto out_fb;
	}
	if (!(fb2->flags & DRM_MODE_FB_MODIFIERS) ||
	    fb2->modifier != test_mod) {
		fprintf(stderr,
			"FAIL: GETFB2 modifier 0x%llx flags 0x%x (want 0x%llx "
			"with DRM_MODE_FB_MODIFIERS)\n",
			(unsigned long long)fb2->modifier, fb2->flags,
			(unsigned long long)test_mod);
		drmModeFreeFB2(fb2);
		goto out_fb;
	}
	drmModeFreeFB2(fb2);

	/* 3. AddFB2 with a modifier that was not injected must fail. */
	uint32_t bogus_fb = 0;
	modifiers[0] = LVDA_TEST_BOGUS_MODIFIER;
	if (drmModeAddFB2WithModifiers(fd, req.width, req.height,
				       DRM_FORMAT_XRGB8888, handles, pitches,
				       offsets, modifiers, &bogus_fb,
				       DRM_MODE_FB_MODIFIERS) == 0) {
		fprintf(stderr,
			"FAIL: AddFB2 accepted unadvertised modifier 0x%llx\n",
			(unsigned long long)LVDA_TEST_BOGUS_MODIFIER);
		drmModeRmFB(fd, bogus_fb);
		goto out_fb;
	}

	printf("PASS: scanout_modifiers (0x%llx accepted, round-tripped; "
	       "bogus rejected)\n", (unsigned long long)test_mod);
	rc = EXIT_SUCCESS;

out_fb:
	drmModeRmFB(fd, fb_id);
out_dumb:
	{
		struct drm_mode_destroy_dumb destroy = { .handle = dumb.handle };
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
out:
	close(fd);
	close(lvda_fd);
	return rc;
}
