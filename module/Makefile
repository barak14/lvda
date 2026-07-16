# SPDX-License-Identifier: GPL-2.0
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Match the *target* kernel's toolchain: clang-built kernels need LLVM=1,
# gcc-built kernels need gcc. Detected from $(KDIR)/.config (not /proc/version)
# so DKMS cross-builds pick the right compiler per kernel. Override with
# `make LLVM=1` or `make LLVM=` (gcc).
LLVM ?= $(shell grep -q '^CONFIG_CC_IS_CLANG=y' $(KDIR)/.config 2>/dev/null && echo 1)
SIGN_HASH ?= sha256
SIGN_FILE ?= $(KDIR)/scripts/sign-file

all:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) LLVM=$(LLVM) modules_install

sign: all
	@test -x "$(SIGN_FILE)" || { echo "ERROR: sign-file not found or not executable: $(SIGN_FILE)"; exit 1; }
	@test -n "$(SIGN_KEY)" || { echo "ERROR: set SIGN_KEY=/path/to/private-key"; exit 1; }
	@test -n "$(SIGN_CERT)" || { echo "ERROR: set SIGN_CERT=/path/to/x509-cert"; exit 1; }
	@test -r "$(SIGN_KEY)" || { echo "ERROR: cannot read SIGN_KEY=$(SIGN_KEY)"; exit 1; }
	@test -r "$(SIGN_CERT)" || { echo "ERROR: cannot read SIGN_CERT=$(SIGN_CERT)"; exit 1; }
	"$(SIGN_FILE)" "$(SIGN_HASH)" "$(SIGN_KEY)" "$(SIGN_CERT)" lvda.ko
	@echo "signed lvda.ko with $(SIGN_CERT)"

# --- virtme-ng smoke test (no host root) ----------------------------------
# Boots the running kernel image in a VM, loads the freshly-built module, and
# runs the userspace suite. Override KIMG/KREL/KDIR to target another kernel.
KREL      ?= $(shell uname -r)
KIMG      ?= $(firstword $(wildcard /usr/lib/modules/$(KREL)/vmlinuz /boot/vmlinuz-$(KREL) /lib/modules/$(KREL)/vmlinuz))
TESTDIR   := $(abspath $(PWD)/../tests/userspace)
VNG_TESTS := add_remove version pool_cap fd_reap edid_bytes debugfs_monitors modifier_negotiation kmsgrab_roundtrip displayid_mode connector_caps cursor_plane prime_import
VNG_SCRIPT = cd $(PWD); uname -r; insmod ./lvda.ko || { echo INSMOD_FAILED; exit 1; }; echo INSMOD_OK; mount -t debugfs none /sys/kernel/debug 2>/dev/null || true; modprobe vgem 2>/dev/null || true; ls -l /dev/lvda; fail=0; for t in $(VNG_TESTS); do echo "=== $$t ==="; if "$(TESTDIR)/$$t"; then echo rc=0; else echo "rc=$$?"; fail=$$((fail+1)); fi; done; echo "=== dmesg ==="; dmesg | grep -iE "lvda|BUG|WARN|oops|call trace" | tail -20; rmmod lvda && echo RMMOD_OK; echo "RESULT: $$fail test(s) failed"; exit $$fail

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

.PHONY: all clean modules_install sign vng-test compile_commands.json
