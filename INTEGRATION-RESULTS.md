# lvda integration results

Companion to `INTEGRATION-PLAN.md`. Records what has been validated end-to-end,
the evidence, and the known follow-ups. Test rig: three QEMU/KVM VMs (CachyOS,
Fedora 44, Ubuntu 26.04) on libvirt NAT `192.168.122.0/24`, all GPU-less
(llvmpipe / virtio-gpu).

## Status summary

| Step | What | Result |
|---|---|---|
| 0 | Software GBM/EGL on render-less lvda | PASS — CachyOS, Fedora, Ubuntu |
| 1 | Capture path: atomic modeset @ exact mode → `drmModeGetFB2` → PRIME export | PASS — CachyOS, Fedora, Ubuntu |
| 2–3 | Real compositor adoption + Sunshine `kms` capture + Moonlight client receives frames | PASS — CachyOS (Cinnamon/Wayland) |

Steps 0–1 are the `tests/host/` probes (`gbm_probe`, `kms_scanout_probe`),
recorded in `tests/host/STEP0-RESULTS.md`. Step 2–3 is the full product path
below.

## Step 2–3 — full streaming path (CachyOS VM)

Topology: `card0 = lvda` (render-less), `card1 = virtio-gpu` (desktop, llvmpipe).
Desktop is **Cinnamon on Wayland** (not KDE). lvda is the 2nd display (extend mode).

1. **lvda up**: `lvda-ctl up --width 2560 --height 1440 --fps 120` →
   connector `Virtual-2` connected.
2. **Compositor adopts it** — `/sys/kernel/debug/dri/<lvda>/state`:
   ```
   crtc[36]: lvda-crtc-0   enable=1 active=1
      mode: "2560x1440": 120 …
   connector[38]: Virtual-2  crtc=lvda-crtc-0
   plane[34]: lvda-primary-0  fb=42
      allocated by = cinnamon          # compositor rendered it
      format=XR24  modifier=0x0  size=2560x1440  pitch=10240
      imported=yes                     # PRIME import: virtio renders → lvda scans out
   ```
3. **Sunshine captures it** (stock `sunshine` pkg, `capture = kms`):
   ```
   Found connector ID [38]   Resolution: 2560x1440  Pitch: 10240
   Found H.264 encoder: libx264 [software]   # nvenc/vulkan/vaapi fail on llvmpipe, as expected
   ```
4. **Moonlight client receives frames** — `moonlight-qt 6.1.0` (snap) on
   ubuntu-vm, paired via `POST /api/pin`; server then lists the client cert.
   Sunshine session log:
   ```
   /launch → RTSP/1.0 200 OK (×8) → CLIENT CONNECTED
   Debug: width and height: w 2560 h 1440   # repeating every ~16ms, 2500+ frames
   ```

That is the whole SPEC capture path on the wire: compositor-rendered frame at
the client's **exact** requested mode → lvda scanout → kmsgrab/`GetFB2` →
encoder → Moonlight client.

## Key findings (load-bearing for the real host)

- **Sunshine KMS capture needs `CAP_SYS_ADMIN`, not DRM master.** The distro
  `sunshine` binary ships `cap_sys_admin,cap_sys_nice=p` file caps. It *reads*
  the compositor's live framebuffers (`drmModeGetFB2`); it never calls
  `drmSetMaster`. So there is **no conflict with the running compositor** and no
  need to stop the display manager for streaming. (The `kms_scanout_probe` step-1
  test needed master only because it *is* the modesetter; real Sunshine is not.)
- **lvda has no Linux virtual-display logic in Sunshine/vibeshine** — that
  subsystem is Windows-only (`#ifdef _WIN32`). On Linux the division of labor is:
  lvda creates the connector → compositor adopts it → Sunshine captures by
  output. Extend vs. headless/exclusive is a *compositor-side* choice
  (`kscreen-doctor` / display settings), not a Sunshine setting on Linux.
- **`output_name` targets by numeric KMS index, not connector name.** Sunshine
  auto-picked lvda here because it was the largest output. On a real
  multi-display host, pin `output_name` to lvda's index (read from the Sunshine
  startup KMS monitor list). Connector-name targeting (`Virtual-N`) would be a
  small Sunshine patch — the "output_name seam".
- **Verification tooling**: `kscreen-doctor` is KWin/KDE-only and **hangs** on
  other desktops (e.g. Cinnamon). The compositor-agnostic check that the
  compositor lit the lvda output at the exact mode is reading
  `/sys/kernel/debug/dri/<minor>/state` as root (crtc `active=` + `mode:` +
  plane `allocated by`). Map minors via `/sys/kernel/debug/dri/*/name`.
- **Multi-GPU teardown (step-1 probe only)**: on a dual-GPU box (Fedora:
  virtio + lvda) the step-1 scanout probe needs DRM master, and stopping just
  the greeter is insufficient — the session respawns and re-grabs master on the
  secondary card. `sync-and-probe.sh --stop-dm` uses
  `systemctl isolate multi-user.target` + `loginctl terminate-seat seat0`. This
  is **not** needed for real Sunshine streaming (CAP_SYS_ADMIN reader).
- **No KMS cursor plane** on lvda → the hardware cursor is not in the stream
  (Sunshine warns "No KMS cursor plane found"). Adding a cursor plane to the
  driver is a possible follow-up.
- **Client install**: `moonlight-embedded` is **not** in the Ubuntu 26.04 repos
  (cloudsmith has no 26.04 component yet, so `apt` reports "Unable to locate
  package"). `moonlight-qt` (snap or repo) works.

## Validated reproduction (CachyOS server side)

```
# server VM
sudo modprobe lvda
sudo lvda-ctl up --width 2560 --height 1440 --fps 120 --card-out /run/lvda/card
sunshine --creds <user> <pass>          # one-time
printf 'capture = kms\n' >> ~/.config/sunshine/sunshine.conf
sunshine                                 # in the graphical session

# client: Moonlight → add host <server-ip> → pair (PIN) → launch "Desktop"
#   PIN can be submitted server-side: curl -k -u u:p -d '{"pin":"NNNN","name":"x"}' \
#     https://<server>:47990/api/pin
```

## Not yet done (deferred)

- Pin `output_name` to lvda's KMS index for deterministic targeting on
  multi-display hosts.
- Replicate the full Sunshine streaming path on Fedora (dual-GPU, closest to the
  real amdgpu+lvda host) and Ubuntu/GNOME.
- Real host PC validation (amdgpu renders → lvda scans out → hardware encoder).
- HDR end-to-end (driver advertises `LVDA_F_HDR`; encoder path not yet exercised).
