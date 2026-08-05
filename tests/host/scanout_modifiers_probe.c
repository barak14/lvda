// SPDX-License-Identifier: GPL-2.0
/*
 * scanout_modifiers_probe -- list render-GPU modifiers safe for lvda scanout.
 *
 * lvda can scan out PRIME-imported render-GPU buffers, but generic DRM
 * framebuffer validation rejects modifiers that add metadata planes. Query the
 * render GPU through EGL, keep the non-linear, non-external-only modifiers
 * whose GBM plane count is one, and emit the matching lvda module option.
 *
 * Build: cc scanout_modifiers_probe.c -o scanout_modifiers_probe \
 *            $(pkg-config --cflags --libs libdrm gbm egl)
 * Run:   ./scanout_modifiers_probe [/dev/dri/renderD128]
 *        (auto-finds the first usable render node when no path is given)
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_MODIFIERS 63

static const uint32_t formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR2101010,
};

static int open_render(char *out, size_t out_size)
{
	for (int i = 128; i < 192; i++) {
		char path[32];

		snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);

		if (fd < 0)
			continue;
		snprintf(out, out_size, "%s", path);
		return fd;
	}
	return -1;
}

static int has_extension(const char *extensions, const char *extension)
{
	size_t len = strlen(extension);
	const char *match;

	if (!extensions || strchr(extension, ' '))
		return 0;
	for (match = extensions; (match = strstr(match, extension)); match += len) {
		if ((match == extensions || match[-1] == ' ') &&
		    (match[len] == '\0' || match[len] == ' '))
			return 1;
	}
	return 0;
}

static void fourcc_string(uint32_t format, char out[5])
{
	out[0] = format & 0xff;
	out[1] = (format >> 8) & 0xff;
	out[2] = (format >> 16) & 0xff;
	out[3] = (format >> 24) & 0xff;
	out[4] = '\0';
}

static int contains_modifier(const uint64_t *modifiers, size_t count,
			     uint64_t modifier)
{
	for (size_t i = 0; i < count; i++)
		if (modifiers[i] == modifier)
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	char path[64] = {0};
	int fd;

	if (argc > 1) {
		snprintf(path, sizeof(path), "%s", argv[1]);
		fd = open(path, O_RDWR | O_CLOEXEC);
	} else {
		fd = open_render(path, sizeof(path));
	}
	if (fd < 0) {
		fprintf(stderr, "FAIL: no usable render node%s\n",
			argc > 1 ? " at the requested path" :
				   " in /dev/dri/renderD128..renderD191");
		return 1;
	}

	drmVersionPtr version = drmGetVersion(fd);

	printf("render: %s (driver: %s)\n", path,
	       version && version->name ? version->name : "unknown");
	if (version)
		drmFreeVersion(version);

	struct gbm_device *gbm = gbm_create_device(fd);

	if (!gbm) {
		fprintf(stderr, "FAIL: gbm_create_device on %s\n", path);
		close(fd);
		return 1;
	}
	printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)
		eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_platform_display) {
		fprintf(stderr, "FAIL: eglGetPlatformDisplayEXT is unavailable\n");
		gbm_device_destroy(gbm);
		close(fd);
		return 1;
	}

#if defined(EGL_PLATFORM_GBM_KHR)
	EGLenum platform = EGL_PLATFORM_GBM_KHR;
#else
	EGLenum platform = EGL_PLATFORM_GBM_MESA;
#endif
	EGLDisplay display = get_platform_display(platform, gbm, NULL);
	EGLint major = 0, minor = 0;

	if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor)) {
		fprintf(stderr, "FAIL: eglInitialize on %s (0x%x)\n", path,
			eglGetError());
		gbm_device_destroy(gbm);
		close(fd);
		return 1;
	}
	printf("EGL %d.%d vendor: %s\n", major, minor,
	       eglQueryString(display, EGL_VENDOR));

	const char *extensions = eglQueryString(display, EGL_EXTENSIONS);

	if (!has_extension(extensions,
			   "EGL_EXT_image_dma_buf_import_modifiers")) {
		printf("SKIP: EGL_EXT_image_dma_buf_import_modifiers is unavailable\n");
		eglTerminate(display);
		gbm_device_destroy(gbm);
		close(fd);
		return 0;
	}

	PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers =
		(PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
		eglGetProcAddress("eglQueryDmaBufModifiersEXT");
	if (!query_modifiers) {
		fprintf(stderr, "FAIL: eglQueryDmaBufModifiersEXT is unavailable\n");
		eglTerminate(display);
		gbm_device_destroy(gbm);
		close(fd);
		return 1;
	}

	uint64_t kept[MAX_MODIFIERS];
	size_t kept_count = 0;
	int status = 0;

	printf("\n%-8s %-18s %-7s %s\n",
	       "fourcc", "modifier", "planes", "result");
	printf("%-8s %-18s %-7s %s\n",
	       "------", "------------------", "------", "------");

	for (size_t f = 0; f < ARRAY_SIZE(formats); f++) {
		uint32_t format = formats[f];
		char fourcc[5];
		EGLint count = 0;

		fourcc_string(format, fourcc);
		if (!query_modifiers(display, (EGLint)format, 0, NULL, NULL,
				     &count) || count < 0) {
			fprintf(stderr,
				"FAIL: eglQueryDmaBufModifiersEXT(%s) count (0x%x)\n",
				fourcc, eglGetError());
			status = 1;
			break;
		}
		if (count == 0) {
			printf("%-8s %-18s %-7s %s\n", fourcc, "(none)", "-",
			       "skipped (no modifiers)");
			continue;
		}

		EGLuint64KHR *modifiers =
			calloc((size_t)count, sizeof(*modifiers));
		EGLBoolean *external_only =
			calloc((size_t)count, sizeof(*external_only));
		if (!modifiers || !external_only) {
			fprintf(stderr, "FAIL: allocating modifier list for %s\n",
				fourcc);
			free(external_only);
			free(modifiers);
			status = 1;
			break;
		}

		EGLint returned = 0;
		if (!query_modifiers(display, (EGLint)format, count, modifiers,
				     external_only, &returned) || returned < 0 ||
		    returned > count) {
			fprintf(stderr,
				"FAIL: eglQueryDmaBufModifiersEXT(%s) list (0x%x)\n",
				fourcc, eglGetError());
			free(external_only);
			free(modifiers);
			status = 1;
			break;
		}

		for (EGLint i = 0; i < returned; i++) {
			uint64_t modifier = modifiers[i];
			int planes = gbm_device_get_format_modifier_plane_count(
				gbm, format, modifier);
			const char *result;

			if (modifier == DRM_FORMAT_MOD_LINEAR)
				result = "skipped (linear)";
			else if (modifier == DRM_FORMAT_MOD_INVALID)
				result = "skipped (invalid)";
			else if (external_only[i])
				result = "skipped (external-only)";
			else if (planes != 1)
				result = "skipped (not single-plane)";
			else if (contains_modifier(kept, kept_count, modifier))
				result = "skipped (duplicate)";
			else if (kept_count == MAX_MODIFIERS)
				result = "skipped (63-entry cap)";
			else {
				kept[kept_count++] = modifier;
				result = "kept";
			}

			printf("%-8s 0x%016" PRIx64 " %-7d %s\n",
			       fourcc, modifier, planes, result);
		}
		free(external_only);
		free(modifiers);
	}

	if (!status) {
		if (kept_count == 0) {
			printf("\nNo non-linear single-plane modifiers were found; "
			       "no options line emitted.\n");
		} else {
			printf("\nPASS: kept %zu non-linear single-plane modifier%s\n",
			       kept_count, kept_count == 1 ? "" : "s");
			printf("options lvda scanout_modifiers=");
			for (size_t i = 0; i < kept_count; i++)
				printf("%s0x%" PRIx64, i ? "," : "", kept[i]);
			putchar('\n');
		}
	}

	eglTerminate(display);
	gbm_device_destroy(gbm);
	close(fd);
	return status;
}
