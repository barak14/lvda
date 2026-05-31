#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the lvda-dkms .deb from the working tree. Output lands next to this
# script (the packaging/debian dir).
#
# One-time install of build deps (Debian 12+ / Ubuntu 22.04+):
#   sudo apt-get install build-essential debhelper dh-dkms dkms
#
# Then:
#   ./makedeb.sh
#   sudo apt-get install ./lvda-dkms_<ver>_amd64.deb
set -eu

ver=0.1.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # packaging/debian
reporoot=$(CDPATH= cd -- "$here/../.." && pwd)       # repo root

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

d="$stage/lvda-$ver"
mkdir -p "$d"
cp -a "$reporoot/module" "$reporoot/uapi" "$reporoot/tools" "$reporoot/packaging" "$d/"
cp -a "$reporoot/LICENSE" "$d/LICENSE"

# Drop build artifacts and previously generated packaging output so the
# source tree dpkg-buildpackage sees is clean.
find "$d" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name 'Module.symvers' -o -name 'modules.order' \
	-o -name 'compile_commands.json' -o -name '*.tar.gz' \
	-o -name '*.deb' -o -name '*.dsc' -o -name '*.changes' \
	-o -name '*.buildinfo' -o -name '*.rpm' -o -name '*.pkg.tar.*' \) -delete
find "$d/tools" -type f -name 'lvda-ctl' -delete
find "$d" -depth -type d -name '.cache' -exec rm -rf {} +

# 3.0 (native) format wants debian/ inside the source tree.
cp -a "$here" "$d/debian"

( cd "$d" && dpkg-buildpackage -us -uc -b )

# dpkg-buildpackage drops .deb/.changes/.buildinfo in the parent of the
# source dir; move them out of $stage before the trap wipes it.
for ext in deb changes buildinfo; do
	for f in "$stage"/*."$ext"; do
		[ -e "$f" ] && mv "$f" "$here/"
	done
done

deb=$(ls -1t "$here"/lvda-dkms_*.deb 2>/dev/null | head -1 || true)
[ -n "$deb" ] && echo "wrote $deb"
