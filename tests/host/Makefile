# SPDX-License-Identifier: GPL-2.0
#
# Host build of the EDID synthesizer conformance harness. Compiles the
# synthesizer source directly into a standalone userspace binary; no
# kernel headers, no module build.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -I../../module
SRC     := test_edid.c ../../module/lvda_edid.c

test_edid: $(SRC) ../../module/lvda_edid.h
	$(CC) $(CFLAGS) $(SRC) -o $@

# Step-0 viability probe: software GBM/EGL on the render-less lvda node.
# Needs libdrm + gbm + EGL + GLESv2 dev packages (see gbm_probe.c header).
GBM_PKGS := libdrm gbm egl glesv2
gbm_probe: gbm_probe.c
	$(CC) -std=c11 -Wall -Wextra gbm_probe.c -o $@ \
		$(shell pkg-config --cflags --libs $(GBM_PKGS))

probe: gbm_probe
	./gbm_probe

# Step-1 capture-path probe: atomic modeset onto a connected lvda connector,
# then drmModeGetFB2 + PRIME export (kmsgrab's capture sequence). Needs libdrm
# and a monitor connected first (lvda-ctl up …). See kms_scanout_probe.c.
kms_scanout_probe: kms_scanout_probe.c
	$(CC) -std=c11 -Wall -Wextra kms_scanout_probe.c -o $@ \
		$(shell pkg-config --cflags --libs libdrm)

# Step-2 zero-copy probe: import a render-GPU dma-buf into lvda, scan it out,
# capture + re-export it, and verify it is the same buffer (see
# prime_import_probe.c). Needs libdrm + gbm and a real render GPU.
prime_import_probe: prime_import_probe.c
	$(CC) -std=c11 -Wall -Wextra prime_import_probe.c -o $@ \
		$(shell pkg-config --cflags --libs libdrm gbm)

run: test_edid
	./test_edid

dump: test_edid
	./test_edid --dump vectors

clean:
	rm -f test_edid gbm_probe kms_scanout_probe prime_import_probe

.PHONY: run dump probe clean
