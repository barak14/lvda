# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Match the *target* kernel's toolchain: clang-built kernels need LLVM=1,
# gcc-built kernels need gcc. Detected from $(KDIR)/.config (not /proc/version)
# so DKMS cross-builds pick the right compiler per kernel. Override with
# `make LLVM=1` or `make LLVM=` (gcc).
LLVM ?= $(shell grep -q '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config 2>/dev/null && echo 1)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules_install

# --- virtme-ng smoke test (no host root) ----------------------------------
# Boots the running kernel image in a VM, loads the freshly-built module, and
# runs the userspace suite. Override KIMG/KREL/KDIR to target another kernel.
KREL      ?= $(shell uname -r)
KIMG      ?= $(firstword $(wildcard /usr/lib/modules/$(KREL)/vmlinuz /boot/vmlinuz-$(KREL) /lib/modules/$(KREL)/vmlinuz))
TESTDIR   := $(abspath $(PWD)/../tests/userspace)
VNG_TESTS := add_remove version pool_cap fd_reap edid_bytes modifier_negotiation kmsgrab_roundtrip displayid_mode connector_caps
VNG_SCRIPT = cd $(PWD); uname -r; insmod ./lvda.ko || { echo INSMOD_FAILED; exit 1; }; echo INSMOD_OK; ls -l /dev/lvda; fail=0; for t in $(VNG_TESTS); do echo "=== $$t ==="; if "$(TESTDIR)/$$t"; then echo rc=0; else echo "rc=$$?"; fail=$$((fail+1)); fi; done; echo "=== dmesg ==="; dmesg | grep -iE "lvda|BUG|WARN|oops|call trace" | tail -20; rmmod lvda && echo RMMOD_OK; echo "RESULT: $$fail test(s) failed"; exit $$fail

vng-test: all
	@command -v vng >/dev/null 2>&1 || { echo "ERROR: vng (virtme-ng) not found in PATH"; exit 1; }
	@test -n "$(KIMG)" && test -f "$(KIMG)" || { echo "ERROR: no kernel image for '$(KREL)' (looked in /usr/lib/modules, /boot, /lib/modules); set KIMG=/path/to/vmlinuz"; exit 1; }
	$(MAKE) -C $(TESTDIR)
	@echo ">>> vng: booting $(KREL) ($(KIMG)) LLVM=$(LLVM)"
	vng --run "$(KIMG)" --user root --exec '$(VNG_SCRIPT)'

# --- clangd ---------------------------------------------------------------
# Generate compile_commands.json so clangd resolves kernel headers (-nostdinc
# plus the kernel -I tree). Depends on `all` so the .*.o.cmd files exist. The
# output embeds absolute $(KDIR) paths, so it is machine-specific, gitignored,
# and must be regenerated after a kernel bump.
compile_commands.json: all
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) compile_commands.json

.PHONY: all clean modules_install vng-test compile_commands.json
