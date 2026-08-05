# SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note
#
# Userspace integration tests for /dev/lvda. The dependency-free
# binaries (POSIX + libc, no libdrm) build always; each skips with exit 0
# when the device is absent. The libdrm-based tests (modifier_negotiation,
# kmsgrab_roundtrip) build only when libdrm is available.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra

BINARIES := add_remove pool_cap fd_reap version edid_bytes debugfs_monitors

DRM_BINARIES := modifier_negotiation kmsgrab_roundtrip displayid_mode \
		connector_caps cursor_plane prime_import scanout_modifiers

HAVE_LIBDRM := $(shell pkg-config --exists libdrm 2>/dev/null && echo 1)

ifeq ($(HAVE_LIBDRM),1)
DRM_CFLAGS := $(shell pkg-config --cflags libdrm)
DRM_LIBS   := $(shell pkg-config --libs libdrm)
BINARIES   += $(DRM_BINARIES)
endif

all: $(BINARIES)
ifneq ($(HAVE_LIBDRM),1)
	@echo "notice: libdrm absent — skipping $(DRM_BINARIES)"
endif

%: %.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $< -o $@

modifier_negotiation: modifier_negotiation.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

kmsgrab_roundtrip: kmsgrab_roundtrip.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

displayid_mode: displayid_mode.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

connector_caps: connector_caps.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

cursor_plane: cursor_plane.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

prime_import: prime_import.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

scanout_modifiers: scanout_modifiers.c ../../uapi/lvda.h
	$(CC) $(CFLAGS) $(DRM_CFLAGS) $< -o $@ $(DRM_LIBS)

run: $(BINARIES)
	@for t in $(BINARIES); do ./$$t || exit $$?; done

clean:
	rm -f add_remove pool_cap fd_reap version edid_bytes debugfs_monitors \
		$(DRM_BINARIES)

.PHONY: all run clean
