// SPDX-License-Identifier: GPL-2.0
/*
 * gbm_probe — step 0: does software GBM/EGL come up on lvda?
 *
 * lvda exposes a KMS node with dumb buffers + PRIME but NO render node
 * (driver_features lacks DRIVER_RENDER). A GL compositor on it needs Mesa
 * kms_swrast to hand out a software GBM device over the dumb buffers. This
 * probe answers, in seconds, the only real viability unknown: can we make a
 * GBM device + EGL/GLES context on the lvda card, render a frame, and lock a
 * SCANOUT-capable front buffer (what a compositor must do).
 *
 * Build: cc gbm_probe.c -o gbm_probe $(pkg-config --cflags --libs libdrm) -lgbm -lEGL -lGLESv2
 * Run:   sudo modprobe lvda && ./gbm_probe        (auto-finds the lvda card)
 *        ./gbm_probe /dev/dri/card1                 (explicit node)
 *
 * Deps:
 *   Fedora:  sudo dnf install gcc pkgconf libdrm-devel mesa-libgbm-devel \
 *                             mesa-libEGL-devel mesa-libGLES-devel
 *   Ubuntu:  sudo apt install gcc pkg-config libdrm-dev libgbm-dev \
 *                             libegl-dev libgles-dev
 *   Arch:    sudo pacman -S --needed gcc pkgconf libdrm mesa
 *
 * If a hardware GL driver gets picked (topology B with virtio-gpu present),
 * force software with: LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe ./gbm_probe
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xf86drm.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#define LVDA_GBM_FORMAT GBM_FORMAT_XRGB8888

static int open_lvda(char *out, size_t n)
{
	for (int i = 0; i < 64; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;

		drmVersionPtr v = drmGetVersion(fd);
		int match = v && v->name && strcmp(v->name, "lvda") == 0;
		if (v)
			drmFreeVersion(v);
		if (match) {
			snprintf(out, n, "%s", path);
			return fd;
		}
		close(fd);
	}
	return -1;
}

int main(int argc, char **argv)
{
	char path[64] = {0};
	int fd;

	if (argc > 1) {
		snprintf(path, sizeof(path), "%s", argv[1]);
		fd = open(path, O_RDWR | O_CLOEXEC);
	} else {
		fd = open_lvda(path, sizeof(path));
	}
	if (fd < 0) {
		fprintf(stderr, "FAIL: no lvda card (sudo modprobe lvda?)\n");
		return 1;
	}
	printf("card: %s\n", path);

	char *rnode = drmGetRenderDeviceNameFromFd(fd);
	printf("render node: %s\n", rnode ? rnode : "(none — expected; lvda is scanout-only)");
	free(rnode);

	struct gbm_device *gbm = gbm_create_device(fd);
	if (!gbm) {
		fprintf(stderr, "FAIL: gbm_create_device (no software GBM for this node)\n");
		return 1;
	}
	printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_disp =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_disp
		? get_disp(EGL_PLATFORM_GBM_KHR, gbm, NULL)
		: eglGetDisplay((EGLNativeDisplayType)gbm);

	EGLint major, minor;
	if (!eglInitialize(dpy, &major, &minor)) {
		fprintf(stderr, "FAIL: eglInitialize (0x%x)\n", eglGetError());
		return 1;
	}
	printf("EGL %d.%d  vendor: %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));

	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
		EGL_NONE,
	};
	EGLint nconf = 0;
	eglGetConfigs(dpy, NULL, 0, &nconf);
	EGLConfig *configs = calloc(nconf > 0 ? (size_t)nconf : 1, sizeof(*configs));
	EGLint matched = 0;
	if (!configs ||
	    !eglChooseConfig(dpy, cfg_attr, configs, nconf, &matched) || matched < 1) {
		fprintf(stderr, "FAIL: eglChooseConfig\n");
		return 1;
	}

	// eglChooseConfig does NOT filter on EGL_NATIVE_VISUAL_ID, so on llvmpipe
	// it can hand back a config whose visual (e.g. XBGR2101010 / an alpha
	// format) mismatches the gbm_surface fourcc — eglCreateWindowSurface then
	// returns EGL_BAD_MATCH (0x3009). Pick the config whose visual id equals
	// the surface format. (kmscube pattern.)
	EGLConfig cfg = configs[0];
	int cfg_found = 0;
	for (EGLint i = 0; i < matched; i++) {
		EGLint vid = 0;
		if (eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &vid) &&
		    vid == (EGLint)LVDA_GBM_FORMAT) {
			cfg = configs[i];
			cfg_found = 1;
			break;
		}
	}
	free(configs);
	if (!cfg_found)
		fprintf(stderr, "warn: no config matched XRGB8888 visual; using configs[0]\n");

	EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
	if (ctx == EGL_NO_CONTEXT) {
		fprintf(stderr, "FAIL: eglCreateContext (0x%x)\n", eglGetError());
		return 1;
	}

	struct gbm_surface *surf = gbm_surface_create(gbm, 1920, 1080,
		LVDA_GBM_FORMAT, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!surf) {
		fprintf(stderr, "FAIL: gbm_surface_create(SCANOUT|RENDERING) — "
				"the lvda scanout path is the blocker\n");
		return 1;
	}

	EGLSurface egls = eglCreateWindowSurface(dpy, cfg,
		(EGLNativeWindowType)surf, NULL);
	if (egls == EGL_NO_SURFACE) {
		fprintf(stderr, "FAIL: eglCreateWindowSurface (0x%x)\n", eglGetError());
		return 1;
	}

	if (!eglMakeCurrent(dpy, egls, egls, ctx)) {
		fprintf(stderr, "FAIL: eglMakeCurrent (0x%x)\n", eglGetError());
		return 1;
	}
	printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
	printf("GL_VERSION : %s\n", glGetString(GL_VERSION));

	glClearColor(0.1f, 0.6f, 0.9f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	eglSwapBuffers(dpy, egls);

	struct gbm_bo *bo = gbm_surface_lock_front_buffer(surf);
	if (!bo) {
		fprintf(stderr, "FAIL: gbm_surface_lock_front_buffer\n");
		return 1;
	}
	printf("scanout bo: %ux%u stride=%u modifier=0x%llx (expect LINEAR=0x0)\n",
	       gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_stride(bo),
	       (unsigned long long)gbm_bo_get_modifier(bo));

	printf("\nPASS: software GBM/EGL renders + locks a scanout buffer on %s\n", path);

	gbm_surface_release_buffer(surf, bo);
	return 0;
}
