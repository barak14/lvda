# Troubleshooting

Work top-down: each section starts at a symptom and ends at a verified state.

## First: know your card numbers

lvda's card minor varies with probe order. Map minors to drivers once and use
the right `N` everywhere below:

```sh
sudo grep . /sys/kernel/debug/dri/*/name      # e.g. dri/2/name: lvda ...
cat /run/lvda/card                            # minor + connector of the last `up`
```

## `open /dev/lvda: Permission denied`

The device is `root:lvda`, mode `0660`. Join the group and **re-login** (a new
shell is not enough — the session needs the new group):

```sh
sudo gpasswd -a "$USER" lvda
id | grep -o lvda                             # verify after re-login
```

If `/dev/lvda` has wrong ownership, the udev rule is missing — reinstall the
package or copy `packaging/udev/60-lvda.rules` and
`sudo udevadm control --reload && sudo udevadm trigger`.

## `lvda-ctl status` says the module is missing

```sh
sudo modprobe lvda
dmesg | grep -i lvda            # -> [drm] Initialized lvda ... on minor N
ls /dev/dri/card*
```

Boot autoload comes from `/usr/lib/modules-load.d/lvda.conf` (shipped by the
packages). After a kernel update without DKMS, rebuild and reinstall the
module manually ([BUILD.md](../BUILD.md)).

## `up` succeeds but no monitor appears on the desktop

Check what the kernel thinks first:

```sh
cat /sys/class/drm/cardN-Virtual-1/status     # must say: connected
sudo cat /sys/kernel/debug/dri/N/monitors     # slot must be: active
```

- **Kernel says connected, desktop shows nothing** — the compositor didn't
  act on the hotplug event. All mainstream Wayland compositors (KWin,
  Mutter, wlroots, Cinnamon) handle DRM hotplug; a session started *before*
  the module loaded may need a re-login. X11 sessions generally do not
  hotplug secondary cards.
- **`EINVAL` from ADD** — mode out of bounds (1–8192 px per axis, 1–1000 Hz).
- **`EOVERFLOW` from ADD** — pixel clock too high even for DisplayID; lower
  the resolution or refresh.

Confirm the compositor lit the output at the exact mode (compositor-agnostic,
works everywhere debugfs is mounted):

```sh
sudo cat /sys/kernel/debug/dri/N/state | grep -A3 'crtc\['
#   crtc[...]: lvda-crtc-0  enable=1 active=1
#     mode: "2560x1440": 120 ...
```

Tip: `kscreen-doctor` is KDE-only and hangs on other desktops — prefer the
debugfs check.

## Stream is captured from the wrong output

Sunshine's `output_name` targets by numeric KMS index, not connector name.
Read Sunshine's startup log for the output list and pin the index — see
[sunshine.md](sunshine.md#targeting-the-virtual-output).

## Stream feels laggy (~10–30 Hz) while the GPU sits idle

The client decodes at full FPS, host latency graphs look fine, but content
updates crawl. Capture duplicates frames, so the *capture* FPS lies; the real
content rate is the compositor's commit rate on the virtual output.

Known cause on **KWin ≥ 6.7**: KWin gives a render-nodeless KMS device (lvda,
vkms) its own **software (llvmpipe) render device** instead of rendering on
the real GPU and copying. Diagnose:

```sh
ps -L -o tid,pcpu,comm -p $(pgrep -x kwin_wayland) | grep llvmpipe
```

Many `llvmpipe-*` threads burning CPU = KWin is software-rendering the
virtual output. Decisive check: drop the virtual monitor to a tiny mode — if
the update rate jumps inversely with pixel count, it's software-bound.

Until this is fixed upstream in KWin, the practical options are patching
KWin's `RenderDevice::isSoftwareDevice()` to treat render-nodeless EGL
displays as software (restores the pre-6.7 copy path), or using a compositor
that multi-GPU-copies from the render GPU. This is not fixable from the lvda
side: the pairing decision is keyed on kernel bus types, and exposing a fake
render node makes KWin match the device to itself — still software.

While streaming, also set the virtual output's VRR policy to *never* if your
desktop enables adaptive sync by default.

## Verifying the zero-copy path

```sh
sudo cat /sys/kernel/debug/dri/N/state | grep -B7 imported
```

- `imported=yes` — PRIME import active: the render GPU's buffer scans out
  directly and capture re-exports it. Zero-copy end to end.
- `imported=no` — the compositor is filling lvda's own LINEAR shmem buffers
  (CPU copy on the render side). Usually means the compositor couldn't scan
  out its render-GPU buffer: inject that GPU's modifiers so tiled buffers are
  accepted —

  ```sh
  make -C tests/host scanout_modifiers_probe && tests/host/scanout_modifiers_probe
  #   -> options lvda scanout_modifiers=0x...,0x...   (write to /etc/modprobe.d/lvda.conf)
  ```

  then reload the module and re-login.

## Capture fails or produces no frames

KMS capture reads live framebuffers with `drmModeGetFB2()`, which requires
`CAP_SYS_ADMIN` on the capturing process. Distro Sunshine packages ship the
file capability; self-built binaries need
`sudo setcap cap_sys_admin+p <binary>` — and again after every reinstall.

## Monitor stuck after a crashed session

It cannot be: the monitor is owned by the `/dev/lvda` fd that created it, and
the kernel reaps it when that fd closes — including on `SIGKILL` or OOM. If
`lvda-ctl status` shows `alive=stale`, that is only a leftover pidfile;
`lvda-ctl down --pidfile <it>` removes it and succeeds.
