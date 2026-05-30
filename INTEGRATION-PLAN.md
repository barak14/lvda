# lvda ↔ Sunshine-family host integration — resume plan

Goal: stream to my devices. A Sunshine-family host drives the **lvda** kernel
driver so a Moonlight client gets a virtual display at its **exact** mode, which
the host captures and encodes. Target desktop: **KDE Plasma (Wayland)**.

No shortcuts: we want the host actually talking to lvda (not just an external
`prep-cmd` wrapper), so it "just works" per session.

---

## What's already done & verified

- **Driver**: persistent DRM card + on-demand connectors. ioctls
  `LVDA_IOC_ADD` / `REMOVE` / `VERSION`; a monitor is owned by the `/dev/lvda`
  fd that added it and is reaped on close (fd = liveness). EDID synthesized
  in-kernel from `(client_id, w, h, refresh_mhz, hdr)`.
- **Builds & tests**: compiles for both installed kernels (gcc `6.18.x-lts`,
  clang `7.0.10`); all 8 vng integration tests pass; DKMS package builds and
  installs (`packaging/arch`, `./makedist.sh && makepkg -si`).
- **Pool default = 1** (was 8) — one `Virtual-1` connector, `disconnected`
  until a session connects it.
- **★ Core hypothesis confirmed**: on `lvda-ctl up`, **KWin dynamically
  detects the lvda connector** (KDE "new display" notification). The compositor
  adopts the persistent card at runtime — the whole approach is viable.

## Host landscape (facts established this session)

- **No Sunshine-family host implements Linux virtual displays today** — all
  virtual-display code is `#ifdef _WIN32`.
- The Linux seam already exists: `display_device::configure_display(video,
  session)` is called **per session** (Sunshine `nvhttp.cpp:882/987`) but is a
  **no-op on Linux** because `make_settings_manager()` returns `nullptr`
  (no `libdisplaydevice` Linux backend; e.g. Apollo `display_device.cpp:628`).
- **Apollo**: stale Linux tree (no portal/kwin/pipewire/vulkan), Windows-only
  VDD. Not recommended as base.
- **Sunshine (upstream)**: modern Linux capture; `wlgrab` selects a wl_output
  **by name** (`wlgrab.cpp:66-95`); portal + kwin backends; clean per-session
  hook. Solid base.
- **vibeshine / Vibepollo**: most modern (tracks recent Sunshine) + WebRTC +
  the best-factored virtual-display design — a clean-room `libvirtualdisplay`
  whose `ControlClient`/`ControlTransport` are **platform-neutral** (transport
  is a single abstract `ioctl()`), but every consumer + driver is still Windows.
  Lead candidate for base (modern + future-proof), though the Linux work below
  is essentially identical on plain Sunshine.

## Approach: implement the Linux display backend inside the host

Three things must happen per session. Map each to concrete work:

### 1. Lifecycle — create/destroy the lvda monitor at the client's mode
- Implement a Linux `display_device::SettingsManagerInterface`:
  - `applySettings(SingleDisplayConfiguration)` → open `/dev/lvda`, `ADD` a
    monitor at the requested resolution/refresh/HDR; **hold the fd open** for
    the session.
  - `revertSettings()` → close fd / `REMOVE` (monitor reaped).
  - `enumAvailableDevices()` → report the lvda connector.
- Wire it in by making `make_settings_manager()` return this backend on Linux
  (currently `#ifdef _WIN32 … #else return nullptr`).
- Plugs into the **existing** per-session hook — no `process.cpp` surgery — and
  inherits the host's `dd_*` config parsing (resolution/refresh/HDR/device-prep)
  for free. Map HDR state → `LVDA_F_HDR`.

### 2. Capture targeting — point the encoder at the lvda output
- Set `config::video.output_name` to the lvda connector (mirror Windows
  `process.cpp:327` `output_name = map_display_name(...)`).
- Confirm which capture backend KDE-Wayland uses here (kms / kwin / portal) and
  how it selects the output, then make `output_name` resolve to `Virtual-1`.
  (`wlgrab` matches by name; `kmsgrab` by numeric index — name is more stable.)

### 3. Compositor layout — make the lvda output the streamed surface
- The one piece no library does portably. On KDE: `kscreen-doctor` to enable /
  set-primary the lvda output (optionally disable the physical one); on wlroots:
  `wlr-randr`. Do it inside `applySettings`, revert in `revertSettings`.
- Fallback: stream in "extend" mode and rely on `output_name` — but primary/only
  is the clean experience.

## VM test rig (stand up BEFORE host code)

lvda uses no GPU, so the whole chain runs GPU-free in VMs: rendering falls back
to software (Mesa **llvmpipe** / `kms_swrast`, or wlroots **pixman**), encoding
to Sunshine's **software** (x264) encoder. This is the preferred rig —
reproducible, isolated, one VM per compositor, and it never disturbs the daily
desktop (no more phantom-screen reboots).

Topologies:
- **A — lvda is the only DRM card** (≈ a real headless host): no virtual GPU in
  the VM; the compositor renders in software and scans out straight to the lvda
  connector. Cleanest; exercises compositor → lvda scanout → `kmsgrab` capture.
- **B — render card + lvda scanout** (multi-GPU, mirrors real hardware): add a
  `virtio-gpu` (or `vkms`) render GPU and keep lvda as the scanout sink (PRIME).
  Fallback when a compositor refuses to run render-less on lvda.

**Validate first (the only real risk):** software GBM/EGL on lvda, which exposes
dumb buffers + PRIME but **no render node** (not `DRIVER_RENDER`). GL compositors
need Mesa `kms_swrast` to provide software GBM on lvda's dumb buffers — standard
for llvmpipe-on-KMS, but verify per compositor; if one balks, use topology B.

Tooling: use a full **QEMU/libvirt VM** (distro userspace + Mesa + compositor +
Sunshine + networking) for the compositor/Sunshine E2E. `vng` stays for
driver-only checks (its minimal env lacks logind/seat/D-Bus for a real session).
Moonlight connects from the host over the VM network. Later: wrap as CI (boot →
load lvda → compositor + Sunshine → Moonlight test client → assert stream at the
requested mode).

### Per-compositor matrix (fill in from the VMs)

| Compositor | Adopts lvda card? | Capture backend | Layout / primary tool |
|---|---|---|---|
| KWin (KDE) | yes — verified on host | `kwin` or `kms` | `kscreen-doctor` |
| wlroots (Sway/Hyprland) | TBD (likely; `WLR_DRM_DEVICES`) | `wlr` (zwlr-screencopy) or `kms` | `wlr-randr` |
| Mutter (GNOME) | TBD (stricter multi-GPU) | `kms` or `portal` — no `wlr`/`kwin` | Mutter `DisplayConfig` D-Bus |

## Decisions to confirm first thing tomorrow

1. **Base**: vibeshine vs Sunshine upstream. (Lead: vibeshine.) Vendor it as a
   **submodule + small patch** (keeps upstream trackable) rather than forking.
2. On the chosen base, the exact Linux per-session call path — confirm
   `configure_display`/`applySettings` runs on Linux **before** capture init.
   Note: vibeshine routes through `display_helper_integration` (Windows-only-real,
   no-op on Linux) — verify whether the Linux hook is `configure_display`
   directly or via that indirection.
3. Capture backend + output selection on this KDE-Wayland box (step 2).
4. Whether to reuse vibeshine's `libvirtualdisplay` `ControlClient` (write a
   `LinuxControlTransport` + align lvda's ABI) or keep lvda's lean ABI and
   write a direct backend. Default: **direct backend, keep lvda ABI** (smaller;
   their lease/manifest machinery is Windows-shaped scope we don't need).

## First execution steps (ordered)
1. **Stand up the VM rig (topology A)** — a KWin VM first, then Mutter and Sway
   VMs. Load lvda, run the compositor headless on it, and verify each adopts the
   connector and which `capture` backend works; fill the per-compositor matrix.
   Fall back to topology B for any compositor that won't run render-less.
2. Pick base; add as a submodule (or use the existing checkout); build it
   **unmodified** inside the VM to establish a clean baseline.
3. Locate + confirm the Linux per-session hook and the `make_settings_manager`
   gate.
4. Implement the minimal Linux `SettingsManagerInterface` backed by lvda
   `ADD`/`REMOVE`, holding the fd for the session.
5. Wire `output_name` → lvda connector; verify capture targets it.
6. Add the enable/primary step per compositor: `kscreen-doctor` (KWin) /
   `wlr-randr` (wlroots) / Mutter `DisplayConfig` D-Bus (GNOME).
7. End-to-end in the VM: Moonlight from the host → exact-mode `Virtual-1` →
   capture → software-encode → stream; disconnect → `REMOVE` + desktop restored.

## Done = 

A Moonlight client on another device streams the lvda virtual display at its
requested resolution/refresh; on disconnect the monitor is removed and the
physical desktop is restored.

## Quick context to reload

- Driver + tests: `module`, `tests`, `tools/lvda-ctl`, `README.md`.
- Reference repos: `~/development/{Sunshine,Apollo,vibeshine}`;
  `~/development/sudovda` (Windows driver model).
- Manual smoke now: `sudo modprobe lvda` →
  `lvda-ctl up --width W --height H --fps N [--hdr]` →
  `cat /sys/class/drm/cardN-Virtual-1/status` → `lvda-ctl down --pidfile …`.
