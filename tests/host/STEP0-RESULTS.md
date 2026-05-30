# Step 0 — software GBM/EGL on render-less lvda (viability gate)

The one hard unknown for the whole streaming approach: lvda exposes a KMS node
with dumb buffers + PRIME but **no render node** (`driver_features` lacks
`DRIVER_RENDER`). A GL compositor on it depends on Mesa `kms_swrast` handing out
a *software* GBM device over those dumb buffers. `tests/host/gbm_probe.c` proves
the full path: open the lvda card -> `gbm_create_device` -> EGL/GLES2 context ->
render a frame -> lock a SCANOUT-capable front buffer (exactly what a compositor
does, and what `kmsgrab` later captures).

Run per VM: `./sync-and-probe.sh <ssh-alias>` (builds module, loads it, runs the probe).

## Result: PASS on all three distros

| Distro | Kernel | Mesa / LLVM | Module build | GBM device | GL renderer | Scanout BO | Verdict |
|---|---|---|---|---|---|---|---|
| Fedora 44 | 7.0.10-201.fc44 | 26.0.7 / LLVM 22 | gcc | `drm` (kms_swrast) | llvmpipe | 1920x1080 stride=7680 mod=0x0 (LINEAR) | **PASS** |
| Ubuntu 26.04 | 7.0.0-22-generic | 26.0.3 / LLVM 21 | gcc | `drm` (kms_swrast) | llvmpipe | 1920x1080 stride=7680 mod=0x0 (LINEAR) | **PASS** |
| CachyOS | 7.0.10-2-cachyos | 26.1.1 / LLVM 22 | clang (`LLVM=1`) | `drm` (kms_swrast) | llvmpipe | 1920x1080 stride=7680 mod=0x0 (LINEAR) | **PASS** |

All under QEMU/KVM, GPU-free (no virtio-gpu render node present in the headless
case). lvda enumerates as the only card on Ubuntu/CachyOS (`card0`) and as a
second card on Fedora (`card1`); the probe finds it by DRM driver name either
way, so topology A holds in both the headless-host and desktop-with-GPU layouts.

## What this establishes

- `kms_swrast` provides software GBM over lvda's dumb buffers across three
  independent Mesa versions (26.0.3 / 26.0.7 / 26.1.1) and both LLVM 21/22.
- The locked front buffer is **LINEAR XRGB8888** — the format the driver
  advertises (`lvda_primary_modifiers = { DRM_FORMAT_MOD_LINEAR }`) and the
  format `kmsgrab` reads via `drmModeGetFB2()`. No modifier negotiation needed.
- The clang / `LLVM=1` kbuild path (CachyOS) builds the module cleanly, not just
  gcc (Fedora/Ubuntu).

Topology A (compositor renders in software, scans out straight to lvda) is
viable and portable. The fallback topology B (virtio-gpu render + lvda scanout
via PRIME) is therefore not required for v1.

## Notes / non-issues

- `module verification failed: ... tainting kernel` on load is expected for an
  unsigned out-of-tree dev module; the DKMS package path signs it.
- The probe's earlier `eglCreateWindowSurface 0x3009` (EGL_BAD_MATCH) was a probe
  bug — `eglChooseConfig` does not filter on `EGL_NATIVE_VISUAL_ID`, so the
  chosen config's visual must be matched to the gbm fourcc (kmscube pattern).
  Fixed; not a driver issue (the driver had already passed
  `gbm_surface_create(SCANOUT|RENDERING)`).

## Step 1 (next gate) — capture path without GL

`tests/host/kms_scanout_probe.c` is the follow-on: with a monitor connected
(`lvda-ctl up …`) it becomes DRM master, does an **atomic modeset at the exact
requested mode** onto the virtual connector, then `drmModeGetFB2()` + PRIME-export
the live plane FB — `kmsgrab`'s exact capture sequence, no compositor/seat/session
needed (`./sync-and-probe.sh <alias> scanout`).
