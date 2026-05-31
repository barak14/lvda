# Building & installing lvda

## Requirements

- A kernel with `CONFIG_DRM`, `CONFIG_DRM_KMS_HELPER`, and
  `CONFIG_DRM_GEM_SHMEM_HELPER` (every modern distro kernel). No special config
  and no kernel rebuild are needed.
- Build tools: `make`, a C compiler (gcc or clang), and the headers for your
  running kernel (`linux-headers`, `kernel-devel`, `linux-headers-amd64`,
  `kernel-default-devel`, …). `dkms` if you want automatic rebuilds across
  kernel updates.

---

## Install — pick your distro

Every distro recipe below stages a clean source tarball from the working tree,
produces a DKMS package, and installs the same drop-ins: the `lvda` group
(`/usr/lib/sysusers.d`), the `/run/lvda` rendezvous dir
(`/usr/lib/tmpfiles.d`), `/dev/lvda` ownership/perms
(`/usr/lib/udev/rules.d`), boot autoload (`/usr/lib/modules-load.d`), plus
`/usr/bin/lvda-ctl` and the `/usr/include/lvda/lvda.h` UAPI header.

### Arch / CachyOS — `.pkg.tar.zst`

```sh
cd packaging/arch
./makedist.sh                          # stage the source tarball
makepkg -si                            # build + install (pulls dkms)
```

Re-run `./makedist.sh && makepkg -fi` after editing the sources.

### Debian / Ubuntu — `.deb`

One-time build deps (Debian 12+, Ubuntu 22.04+):

```sh
sudo apt-get install build-essential debhelper dh-dkms dkms linux-headers-$(uname -r)
```

Build + install:

```sh
cd packaging/debian
./makedeb.sh                                       # writes lvda-dkms_<ver>-1_amd64.deb
sudo apt-get install ./lvda-dkms_*.deb
```

`apt` runs DKMS in the postinst, so install builds and loads the module
against the running kernel.

### Fedora / RHEL / openSUSE — `.rpm`

One-time build deps:

```sh
# Fedora / RHEL / CentOS / Rocky:
sudo dnf install rpm-build gcc make dkms kernel-devel
# openSUSE:
sudo zypper install rpm-build gcc make dkms kernel-default-devel
```

Build + install:

```sh
cd packaging/rpm
./makerpm.sh                                       # writes lvda-dkms-<ver>.x86_64.rpm
sudo dnf install ./lvda-dkms-*.rpm                 # or: sudo zypper install ./lvda-dkms-*.rpm
```

### NixOS — flake

Pull the module via the flake and turn it on:

```nix
# flake.nix
{
  inputs.lvda.url = "github:_/lvda";
  outputs = { self, nixpkgs, lvda }: {
    nixosConfigurations.mybox = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        lvda.nixosModules.default
        ({ ... }: {
          services.lvda = {
            enable = true;
            maxMonitors = 1;            # raise only for multi-client streaming
          };
          users.users.alice.extraGroups = [ "lvda" ];
        })
      ];
    };
  };
}
```

`sudo nixos-rebuild switch` rebuilds the module against
`config.boot.kernelPackages` and installs `lvda-ctl` system-wide. No DKMS —
the module is rebuilt automatically every time the kernel input bumps.

Standalone builds (no NixOS): `nix build .#lvda-ctl` or `nix build .#lvda`.

### Manual build (no distro package)

If your distro isn't listed, build the module and CLI by hand:

```sh
cd module
make                                                # builds lvda.ko
sudo make modules_install
sudo depmod -a

cd ../tools/lvda-ctl
make
sudo make install                                   # /usr/bin/lvda-ctl

cd ../../packaging
sudo install -Dm644 sysusers.d/lvda.conf      /usr/lib/sysusers.d/lvda.conf
sudo install -Dm644 tmpfiles.d/lvda.conf      /usr/lib/tmpfiles.d/lvda.conf
sudo install -Dm644 udev/60-lvda.rules        /usr/lib/udev/rules.d/60-lvda.rules
sudo install -Dm644 modules-load.d/lvda.conf  /usr/lib/modules-load.d/lvda.conf
sudo systemd-sysusers && sudo systemd-tmpfiles --create && sudo udevadm control --reload
```

This skips DKMS — rebuild the module manually after each kernel update.

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
stream to multiple clients at once (1..32):

```sh
sudo modprobe lvda lvda_max_monitors=2
```

(NixOS users set `services.lvda.maxMonitors = N` instead.)

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
```

Then remove the package via your distro tool:

| Distro | Command |
|---|---|
| Arch / CachyOS | `sudo pacman -R lvda-dkms` |
| Debian / Ubuntu | `sudo apt-get purge lvda-dkms` |
| Fedora / RHEL | `sudo dnf remove lvda-dkms` |
| openSUSE | `sudo zypper remove lvda-dkms` |
| NixOS | set `services.lvda.enable = false;` and `nixos-rebuild switch` |
| Manual | `sudo dkms remove lvda/<ver> --all` (if you added it to DKMS by hand) plus delete the drop-ins under `/usr/lib/{sysusers,tmpfiles,modules-load}.d/lvda.conf`, `/usr/lib/udev/rules.d/60-lvda.rules`, and `/usr/bin/lvda-ctl` |
