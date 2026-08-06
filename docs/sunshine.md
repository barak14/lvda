# Sunshine / Apollo integration

The end state: a Moonlight client connects, a virtual monitor appears at the
client's exact resolution, refresh rate, and HDR state, the stream serves
from it, and it vanishes when the session ends. Validated end-to-end — see
[validation results](validation.md).

## Prep commands — the whole setup

Sunshine exports the connecting client's mode as `SUNSHINE_CLIENT_*`
environment variables to prep commands, and `lvda-ctl` reads them as
fallbacks for every flag ([reference](lvda-ctl.md)). So one flagless do/undo
pair serves every client natively:

```conf
# sunshine.conf — or Configuration -> General -> Command Preparations in the web UI
global_prep_cmd = [{"do": "/usr/bin/lvda-ctl up --pidfile /run/lvda/sunshine.pid", "undo": "/usr/bin/lvda-ctl down --pidfile /run/lvda/sunshine.pid"}]
```

The monitor appears when the stream starts and vanishes when it ends. It can
never outlive its owning daemon: kill it by any means and the kernel reaps
the display — no stale state survives a crash.

Per-app prep commands work the same way; use a distinct `--pidfile` per app.
The client id (and with it the monitor's EDID identity) is derived from the
Sunshine app identity automatically, so each app's virtual monitor keeps its
own compositor settings.

## Capture

Set KMS capture:

```conf
capture = kms
```

Two facts worth knowing about how this behaves:

- **KMS capture needs `CAP_SYS_ADMIN`, not DRM master.** Sunshine *reads* the
  compositor's live framebuffers (`drmModeGetFB2`); it never becomes the
  modesetter. There is no conflict with the running compositor and no need to
  stop the display manager. Distro packages ship the file capability; for a
  self-built binary: `sudo setcap cap_sys_admin+p $(which sunshine)`.
- **The captured buffer is the compositor's own scanout buffer** — zero-copy
  from render to encode when the PRIME path is active (see
  [architecture](architecture.md#zero-copy-scanout-and-capture)).

The pointer is included: lvda exposes a KMS cursor plane, so the hardware
cursor is part of the captured output.

## Targeting the virtual output

With one physical desktop plus the lvda monitor, Sunshine tends to pick the
largest output — often the right one by accident. Pin it deterministically on
multi-display hosts:

- `output_name` in Sunshine targets by **numeric KMS output index**, not
  connector name. Read the index from Sunshine's startup log, which lists
  every KMS output it found.
- Find lvda's card and connector for scripting from `/run/lvda/card`
  (`<minor>\n<connector>\n`, written by `lvda-ctl up`).

**Extend vs. exclusive is a compositor decision, not a Sunshine one.** On
Linux, Sunshine has no virtual-display logic (that subsystem is
Windows-only): lvda creates the connector, the compositor adopts it, Sunshine
captures it. To stream a *single* headless display, disable the physical
outputs in your desktop's display settings (KDE: `kscreen-doctor`) rather
than looking for a Sunshine option.

## HDR

`lvda-ctl up` honors `SUNSHINE_CLIENT_HDR` (or `--hdr`): the monitor's EDID
declares HDR10/PQ + BT.2020 + 10-bit, and the compositor sees an HDR-capable
display at the exact mode. Pair it with Sunshine's own HDR encoding settings;
`--10bit` advertises 10-bit color for SDR streams.

## Multiple clients

The card exposes one virtual monitor per pool slot. Raise the pool only if
you stream to several clients simultaneously:

```sh
sudo modprobe lvda lvda_max_monitors=2      # or options lvda ... in modprobe.d
```

Each `lvda-ctl up` takes the next free slot (`Virtual-1`, `Virtual-2`, …) and
each daemon owns exactly its own monitor — sessions start and stop
independently.
