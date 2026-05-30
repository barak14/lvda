# lvda — virtual display driver for game streaming

`lvda` is a Linux DRM kernel module that exposes a **persistent virtual GPU**
(`/dev/dri/cardN`) carrying a pool of virtual monitors. The monitors start
*disconnected*; a control ioctl lights one up at an **exact** resolution,
refresh rate, and HDR mode on demand and hotplug-connects it. A streaming host
(Sunshine / Apollo + Moonlight) can then serve a client at its native mode. The
driver does no per-frame work — the compositor scans out to the virtual monitor
and `kmsgrab` captures its framebuffer (LINEAR, zero-copy).

It mirrors the Windows "virtual display adapter" model: the adapter is always
present once loaded; individual screens appear only while a session needs them.

---

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
ls -l /dev/lvda                 # control device (root:lvda, 0660)
dmesg | grep -i lvda            # -> [drm] Initialized lvda ... on minor N
ls /dev/dri/card*                # the persistent card is /dev/dri/cardN
```

The card exposes **one** virtual monitor by default. It always appears as a
connector (`cardN-Virtual-1`, normally `disconnected` — like an unplugged
HDMI port on a real GPU) and connects only while a session is active. Raise
the pool only if you stream to multiple clients at once (1..64):

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

## Use it — `lvda-ctl`

`lvda-ctl` opens `/dev/lvda`, adds a virtual monitor, and holds the fd open in
a tiny daemon. **The open fd is the liveness signal** — when it closes (or the
process dies), the monitor is removed automatically. No watchdog, no heartbeat.

```sh
# Add a 1440p144 monitor; daemon keeps it alive until 'down'.
lvda-ctl up --width 2560 --height 1440 --fps 144 --pidfile /run/lvda/s.pid
#   -> added: /dev/dri/card0 connector=Virtual-1 monitor_id=0 client_id=...

lvda-ctl status                          # protocol version + live monitors
lvda-ctl down --pidfile /run/lvda/s.pid # closes the fd -> monitor removed
```

Flags: `--width N --height N --fps N --hdr --client-id <32hex> --pidfile P
--card-out P`. With no size flags it falls back to the `SUNSHINE_CLIENT_WIDTH`
/ `SUNSHINE_CLIENT_HEIGHT` / `SUNSHINE_CLIENT_FPS` / `SUNSHINE_CLIENT_HDR`
environment variables, then to 1920x1080@60.

While the daemon runs, a new connector (`Virtual-1`, `Virtual-2`, …) is
connected on the card; it returns to disconnected on `down`:

```sh
cat /sys/class/drm/card0-Virtual-1/status   # connected | disconnected
```

The card minor and connector name are also published to `/run/lvda/card`.

---

## How it fits into a streaming setup

The virtual card behaves like a real GPU with hotpluggable outputs. A Wayland
compositor that supports multi-GPU (KWin, wlroots / Hyprland) adopts the card
at startup, and when a monitor is added a new output appears at the client's
exact mode. Point your capture (`kmsgrab`) at that output and encode it.

> **Note:** direct Sunshine/Apollo integration (auto add-on-connect /
> remove-on-disconnect, like the Windows *sudovda* path) is **not wired up
> yet**. For now, drive the driver manually with `lvda-ctl` — e.g. from a
> Sunshine app `prep-cmd` (`do: lvda-ctl up …`, `undo: lvda-ctl down …`).
> Building that integration into Apollo is the planned next step.

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
