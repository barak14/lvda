# Repository Guidelines

## Project Overview

`lvda` is a Linux DRM kernel module (C) that exposes a **persistent virtual
GPU** (`/dev/dri/cardN`) carrying a pool of virtual monitors. The monitors
start *disconnected*; a control ioctl lights one up at an **exact** resolution,
refresh rate, and HDR mode on demand and hotplug-connects it. A streaming host
(Sunshine / Apollo + Moonlight) then serves a client at its native mode. The
driver does **no per-frame work**: the compositor scans out to the virtual
monitor and `kmsgrab` captures its framebuffer (LINEAR, zero-copy) via
`drmModeGetFB2()` → DMA-BUF → encoder.

A monitor is owned by the `/dev/lvda` file that added it and is reaped when
that file closes — the open fd is the liveness signal (no heartbeat, no
watchdog).

User-facing docs: `README.md` (overview + `lvda-ctl` usage) and `BUILD.md`
(kernel requirements, install, load, permissions, uninstall).

## Layout

| Path | Purpose |
|---|---|
| `module/` | Kernel module, built by **kbuild** (not a root build). |
| `module/lvda_main.c` | `/dev/lvda` miscdevice, ioctl dispatch, per-fd monitor ownership, `lvda_max_monitors` module param, platform-device parent + module init/exit. |
| `module/lvda_kms.c` | The single persistent DRM card and its on-demand virtual monitors (connector / CRTC / encoder / plane setup, EDID + hotplug). |
| `module/lvda_edid.c` / `.h` | Deterministic EDID synthesizer. Kernel-agnostic — compiles into both the module and the host test binary. |
| `module/lvda.h` | Internal `lvda_main.c` ↔ `lvda_kms.c` contract. |
| `module/Kbuild` / `Makefile` | `obj-m := lvda.o`; OOT wrapper (`KDIR`, `LLVM`, `vng-test`). |
| `uapi/lvda.h` | Userspace ABI — **single source of truth**. The module `#include`s it via `../uapi/lvda.h`. |
| `tools/lvda-ctl/` | Userspace CLI (`up` / `down` / `status`); holds the `/dev/lvda` fd open as the liveness daemon. |
| `tests/host/` | Host-built EDID conformance (`test_edid` compiles `lvda_edid.c` directly) + viability probes (`gbm_probe`, `kms_scanout_probe`, `prime_import_probe`, `scanout_modifiers_probe`); `vectors/` holds golden EDID bytes. |
| `tests/userspace/` | `/dev/lvda` integration tests; each skips with exit 0 when the device is absent. libdrm-based tests build only when `pkg-config` finds libdrm. |
| `packaging/` | Shared drop-ins (`sysusers.d`, `tmpfiles.d`, `udev`, `modules-load.d`) + canonical `dkms.conf`, plus per-distro packagers under `arch/` (PKGBUILD), `debian/` (debhelper + dh-dkms + `makedeb.sh`), `rpm/` (`lvda-dkms.spec` + `makerpm.sh`), and `nix/` (derivations consumed by the repo-root `flake.nix`). |
| `flake.nix` | Nix flake entry point — exposes `packages.lvda-ctl`, `packages.lvda` (kernel module), and `nixosModules.default`. Derivations live in `packaging/nix/`. |
| `sync-and-probe.sh` | Push the driver to a VM over SSH, build + load it, run a viability probe. |

## Build & Test

Kernel module (run from `module/`):
```sh
cd module && make                       # KDIR defaults to /lib/modules/$(uname -r)/build
cd module && make KDIR=<kernel-tree>    # alternate / uninstalled kernel tree (OOT dev)
cd module && make modules_install
cd module && make vng-test              # virtme-ng: boot VM, insmod, run userspace suite, rmmod
cd module && make clean
```
`LLVM` is auto-detected from `$(KDIR)/.config` (`CONFIG_CC_IS_CLANG=y` → `LLVM=1`).
Override with `make LLVM=1` or `make LLVM=` (gcc).

Userspace CLI: `make -C tools/lvda-ctl`

Host EDID tests: `make -C tests/host && tests/host/test_edid`
(regenerate golden vectors with `make -C tests/host dump`).

Userspace integration: `make -C tests/userspace`
(libdrm tests skipped when libdrm is absent).

DKMS package per distro: Arch (`packaging/arch/makedist.sh && makepkg -si`),
Debian/Ubuntu (`packaging/debian/makedeb.sh`), Fedora/RHEL/openSUSE
(`packaging/rpm/makerpm.sh`), NixOS (`nix build .#lvda` or the
`nixosModules.default` from `flake.nix`). All four ship the same DKMS source
(`packaging/dkms.conf`) + drop-ins from `packaging/{sysusers,tmpfiles,udev,modules-load}.d`.

## Conventions

- **UAPI is the single source of truth** (`uapi/lvda.h`): the module includes
  it, never re-declares it. Bump `LVDA_PROTOCOL_{MAJOR,MINOR,PATCH}` and the
  `_Static_assert` struct sizes together on any ABI change.
- **EDID synthesizer is kernel-agnostic.** `lvda_edid.c` builds in both the
  module (`__KERNEL__`) and the host harness; keep kernel-only APIs behind
  `#ifdef __KERNEL__` (see the typedefs in `lvda_edid.h`).
- **Determinism.** EDID bytes are a pure function of
  `(client_id, w, h, refresh_mhz, hdr)`. `tests/host/vectors/*.bin` are golden
  outputs; changing the synthesizer means regenerating them.
- **fd-ownership model.** A monitor lives exactly as long as the `/dev/lvda`
  file that added it; `close(2)` → `lvda_release_owner()` reaps it.
- **Module param** `lvda_max_monitors` (uint, `0444`): clamped to `1..32`,
  default `1`. Raise only for multiple simultaneous streaming clients.
- **Kernel C style.** SPDX headers; `/* */` comments say **WHAT** the code does;
  rationale belongs in the README / integration docs or the commit message, not
  inline. The module builds `-Wall`; the userspace tests build `-Wall -Wextra`.
- **ABI guards.** Userspace-visible structs carry `_Static_assert(sizeof(...))`
  in `uapi/lvda.h`; the EDID size constants live in `lvda_edid.h`.
- **Comments** Only add comments that assist in understanding the code "what",
  not "why" or "how".
