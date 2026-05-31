# Building & installing lvda

## Requirements

- A kernel with `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`, and
  `CONFIG_DRM_GEM_SHMEM_HELPER` (every modern distro kernel). No special config
  and no kernel rebuild are needed.
- Build tools: `make`, a C compiler (gcc or clang), and the headers for your
  running kernel (e.g. `linux-headers`, `linux-cachyos-headers`, …). `dkms` if
  you want automatic rebuilds across kernel updates.

---

## Install

### Option A — DKMS package (recommended)

Auto-rebuilds on kernel updates and installs everything (module, `lvda-ctl`,
and the udev / sysusers / tmpfiles / modules-load drop-ins).

Arch / CachyOS:

```sh
cd packaging/arch
./makedist.sh     # stage the source tarball from this working tree
makepkg -si       # build the package and install it
```

`makepkg -si` pulls `dkms`, builds and installs `lvda.ko` against your running
kernel, creates the `lvda` group, the `/run/lvda` dir, and the udev rule, and
enables autoload at boot. (Re-run `./makedist.sh && makepkg -fi` after editing
the sources to rebuild and reinstall.)

### Option B — manual build

```sh
cd module
make                      # builds lvda.ko against the running kernel
sudo make modules_install
sudo depmod -a
```

For a manual install, also drop in the group + device-permission + autoload
files (the DKMS package does this for you):

```sh
cd packaging
sudo install -Dm644 sysusers.d/lvda.conf     /usr/lib/sysusers.d/lvda.conf
sudo install -Dm644 tmpfiles.d/lvda.conf      /usr/lib/tmpfiles.d/lvda.conf
sudo install -Dm644 udev/60-lvda.rules        /usr/lib/udev/rules.d/60-lvda.rules
sudo install -Dm644 modules-load.d/lvda.conf  /usr/lib/modules-load.d/lvda.conf
sudo install -Dm755 ../tools/lvda-ctl/lvda-ctl /usr/bin/lvda-ctl
sudo systemd-sysusers && sudo systemd-tmpfiles --create && sudo udevadm control --reload
```

---

## Load

The module autoloads at boot via `/usr/lib/modules-load.d/lvda.conf`. To load
it now:

```sh
sudo modprobe lvda
```

Confirm it came up and find its card minor:

```sh
ls -l /dev/lvda                  # control device (root:lvda, 0660)
dmesg | grep -i lvda             # -> [drm] Initialized lvda ... on minor N
ls /dev/dri/card*                # the persistent card is /dev/dri/cardN
```

The card exposes one virtual monitor by default. Raise the pool only if you
stream to multiple clients at once (1..64):

```sh
sudo modprobe lvda lvda_max_monitors=2
```

---

## Permissions

`/dev/lvda` is owned `root:lvda`, mode `0660`. Add the user that drives the
driver (your login, or the streaming-service account) to the `lvda` group and
re-login:

```sh
sudo gpasswd -a "$USER" lvda
```

Capturing the card's framebuffer with `kmsgrab` additionally needs read access
to `/dev/dri/cardN` (group `video`) and the usual `kmsgrab` capability on the
capturing process (`CAP_SYS_ADMIN`).

---

## Verify

```sh
make -C module vng-test                          # boot a VM, load module, run the suite
make -C tests/host && tests/host/test_edid       # EDID synthesizer unit tests
```

(`vng-test` needs `virtme-ng`; it is a developer convenience, not required to
use the driver.)

---

## Uninstall

```sh
sudo modprobe -r lvda
# DKMS install:  sudo pacman -R lvda-dkms      (or: sudo dkms remove lvda/<ver> --all)
```
