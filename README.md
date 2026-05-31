# lvda — virtual display driver for game streaming

`lvda` is a Linux DRM kernel module that exposes a **persistent virtual GPU**
(`/dev/dri/cardN`) carrying a pool of virtual monitors. The monitors start
*disconnected*; a control ioctl lights one up at an **exact** resolution,
refresh rate, and HDR mode on demand and hotplug-connects it. A streaming host
(Sunshine / Apollo + Moonlight) can then serve a client at its native mode. The
driver does no per-frame work — the compositor scans out to the virtual monitor
and `kmsgrab` captures its framebuffer (LINEAR, zero-copy).

---

## Build & install

See **[BUILD.md](BUILD.md)** — kernel requirements, the DKMS package
(Arch/CachyOS), the manual build path, loading, permissions, verification, and
uninstall.

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
lvda-ctl down --pidfile /run/lvda/s.pid  # closes the fd -> monitor removed
```

Flags: `--width N --height N --fps N --hdr --client-id <32hex> --pidfile P
--card-out P`. With no size flags it falls back to the `SUNSHINE_CLIENT_WIDTH`
/ `SUNSHINE_CLIENT_HEIGHT` / `SUNSHINE_CLIENT_FPS` / `SUNSHINE_CLIENT_HDR`
environment variables, then to 1920x1080@60.

The card exposes one virtual connector by default (`cardN-Virtual-1`), normally
`disconnected` like an unplugged HDMI port on a real GPU. While the daemon
runs, a connector (`Virtual-1`, `Virtual-2`, …) is connected on the card; it
returns to disconnected on `down`:

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
