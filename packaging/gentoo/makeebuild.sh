#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Stage the lvda-<ver> source tarball from the working tree and refresh the
# ebuild Manifest so packaging/gentoo is a ready-to-use Portage overlay.
#
# Build + install on Gentoo (the staged tarball makes this offline):
#   ./makeebuild.sh
#   sudo ebuild x11-drivers/lvda-dkms/lvda-dkms-<ver>.ebuild merge
#
# Or register the overlay and emerge:
#   sudo eselect repository add lvda git "$PWD"   # or add to /etc/portage/repos.conf
#   sudo emerge x11-drivers/lvda-dkms
set -eu

ver=0.1.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # packaging/gentoo
reporoot=$(CDPATH= cd -- "$here/../.." && pwd)       # repo root
ebuild_file="$here/x11-drivers/lvda-dkms/lvda-dkms-$ver.ebuild"

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d="$stage/lvda-$ver"
mkdir -p "$d"

cp -a "$reporoot/module" "$reporoot/uapi" "$reporoot/tools" "$reporoot/packaging" "$d/"
cp -a "$reporoot/LICENSE" "$reporoot/README.md" "$reporoot/BUILD.md" "$d/"

# Drop build artifacts so the tarball matches a clean release archive.
find "$d" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name 'Module.symvers' -o -name 'modules.order' \
	-o -name 'compile_commands.json' -o -name '*.tar.gz' \
	-o -name '*.deb' -o -name '*.rpm' -o -name '*.pkg.tar.*' \) -delete
find "$d/tools" -type f -name 'lvda-ctl' -delete
find "$d" -depth -type d -name '.cache' -exec rm -rf {} +

# The ebuild's SRC_URI resolves to lvda-<ver>.tar.gz; drop it where the
# overlay's DISTDIR scan finds it so `ebuild manifest` stays offline.
( cd "$stage" && tar -czf "$here/lvda-$ver.tar.gz" "lvda-$ver" )

DISTDIR="$here" ebuild "$ebuild_file" manifest

echo "wrote $here/lvda-$ver.tar.gz and refreshed the ebuild Manifest"
echo "install with: sudo ebuild $ebuild_file merge"
