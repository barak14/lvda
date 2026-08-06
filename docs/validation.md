# Validation results

Where lvda has actually been run, and what that proved. Two kinds of
evidence: automated tests that run on every change, and hand-driven
end-to-end streaming sessions.

## Environments

| Environment | Kernel | Compositor | Covered |
|---|---|---|---|
| CachyOS VM (no GPU) | 7.0.10-2-cachyos | Cinnamon / Wayland | Full path: compositor adopts the monitor → Sunshine captures → Moonlight plays |
| Fedora 44 VM (virtio + lvda) | 7.0.10-201.fc44 | — | Module build (gcc), software GBM/EGL, atomic modeset + `GetFB2` capture |
| Ubuntu 26.04 VM (no GPU) | 7.0.0-22-generic | — | Same as Fedora |
| Real desktop | mainline | KWin / Wayland (amdgpu) | PRIME import of GPU buffers, injected scanout modifiers, HDR stream |

The VMs are GPU-less on purpose: they prove the hardest assumption, that a
compositor can render *in software* straight onto a card with no render node.
The real desktop proves the opposite end — a hardware-rendered buffer scanning
out on the virtual monitor with no copy.

Distro/kernel detail for the VM probes lives in
[`tests/host/STEP0-RESULTS.md`](../tests/host/STEP0-RESULTS.md).

## Automated tests

| Suite | Runs where | Checks |
|---|---|---|
| `tests/host/test_edid` | Any host | EDID conformance against golden vectors — the synthesizer is deterministic |
| `tests/userspace/` (`make -C module vng-test`) | VM with the module loaded | Add/remove, fd reaping, pool cap, EDID bytes, DisplayID modes, connector caps (VRR, HDR metadata, BT.2020, max bpc), cursor plane, `kmsgrab` round-trip, PRIME import (vgem → lvda), scanout modifiers, debugfs table, ABI version |
| `tests/host/*_probe` | Any machine running lvda; the last two need a real GPU | Software GBM/EGL (`gbm_probe`), atomic modeset + capture (`kms_scanout_probe`), GPU buffer import (`prime_import_probe`), modifier filtering (`scanout_modifiers_probe`) |

`sync-and-probe.sh <ssh-alias> [probe]` pushes the tree to a VM, builds and
loads the module, and runs a probe there.

## End-to-end stream (CachyOS VM)

`card0` is lvda, `card1` is virtio-gpu driving the desktop (llvmpipe). lvda is
added as a second display.

1. `lvda-ctl up --width 2560 --height 1440 --fps 120` → `Virtual-2` connected.
2. The compositor adopts it at exactly that mode —
   `/sys/kernel/debug/dri/<lvda>/state`:

   ```
   crtc[36]: lvda-crtc-0   enable=1 active=1
      mode: "2560x1440": 120 …
   connector[38]: Virtual-2  crtc=lvda-crtc-0
   plane[34]: lvda-primary-0  fb=42
      allocated by = cinnamon
      format=XR24  modifier=0x0  size=2560x1440  pitch=10240
      imported=yes
   ```

3. Stock `sunshine` with `capture = kms` picks it up:

   ```
   Found connector ID [38]   Resolution: 2560x1440  Pitch: 10240
   Found H.264 encoder: libx264 [software]
   ```

   (Software encoder because the VM has no GPU; on real hardware this is
   nvenc/vaapi.)

4. `moonlight-qt` on another VM pairs and plays: RTSP handshake, `CLIENT
   CONNECTED`, then frames at 2560x1440 every ~16 ms for 2500+ frames.

That is the whole point of the driver on the wire: a client's exact requested
mode → compositor scanout → `kmsgrab` → encoder → client.

## Reproducing it

```sh
# server
sudo modprobe lvda
sudo lvda-ctl up --width 2560 --height 1440 --fps 120 --card-out /run/lvda/card
sunshine --creds <user> <pass>                       # one-time
printf 'capture = kms\n' >> ~/.config/sunshine/sunshine.conf
sunshine                                             # inside the graphical session

# client: Moonlight → add host <server-ip> → pair → launch "Desktop"
curl -k -u user:pass -d '{"pin":"NNNN","name":"x"}' \
     https://<server>:47990/api/pin                  # PIN can be answered server-side
```

Two practical notes:

- Use `moonlight-qt` as the test client. `moonlight-embedded` has no Ubuntu
  26.04 package yet.
- `kms_scanout_probe` (and only that probe) needs DRM master, so it needs the
  display manager out of the way: `sync-and-probe.sh --stop-dm` isolates
  `multi-user.target` and terminates seat0. Real Sunshine capture never needs
  this — it reads framebuffers with `CAP_SYS_ADMIN` and leaves the compositor
  as the modesetter.

## Known gap

On KWin ≥ 6.7 the compositor may software-render the virtual output instead of
copying from the real GPU, which caps the effective frame rate. Symptoms and
workarounds: [troubleshooting](troubleshooting.md#stream-feels-laggy-1030-hz-while-the-gpu-sits-idle).
