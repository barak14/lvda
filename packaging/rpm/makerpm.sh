#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the lvda-dkms RPM from the working tree. Output lands next to this
# script (the packaging/rpm dir).
#
# One-time install of build deps:
#   Fedora/RHEL: sudo dnf install rpm-build gcc make dkms
#   openSUSE:    sudo zypper install rpm-build gcc make dkms
#
# Then:
#   ./makerpm.sh
#   sudo dnf install ./lvda-dkms-<ver>-<rel>.x86_64.rpm   # or zypper / yum
set -eu

ver=0.1.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # packaging/rpm
reporoot=$(CDPATH= cd -- "$here/../.." && pwd)       # repo root

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

d="$stage/lvda-$ver"
mkdir -p "$d"
cp -a "$reporoot/module" "$reporoot/uapi" "$reporoot/tools" "$reporoot/packaging" "$d/"
cp -a "$reporoot/LICENSE" "$d/LICENSE"

# Drop build artifacts and previously generated packaging output.
find "$d" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name 'Module.symvers' -o -name 'modules.order' \
	-o -name 'compile_commands.json' -o -name '*.tar.gz' \
	-o -name '*.deb' -o -name '*.rpm' -o -name '*.pkg.tar.*' \) -delete
find "$d/tools" -type f -name 'lvda-ctl' -delete
find "$d" -depth -type d -name '.cache' -exec rm -rf {} +

tarball="$stage/lvda-$ver.tar.gz"
( cd "$stage" && tar -czf "$tarball" "lvda-$ver" )

# Self-contained rpmbuild tree so we never touch ~/rpmbuild.
rpmtop="$stage/rpmbuild"
mkdir -p "$rpmtop/SOURCES" "$rpmtop/SPECS" "$rpmtop/BUILD" \
         "$rpmtop/BUILDROOT" "$rpmtop/RPMS" "$rpmtop/SRPMS"
cp "$tarball" "$rpmtop/SOURCES/"
cp "$here/lvda-dkms.spec" "$rpmtop/SPECS/"

rpmbuild --define "_topdir $rpmtop" -bb "$rpmtop/SPECS/lvda-dkms.spec"

# Move the built RPM(s) out of $stage before the trap wipes it.
find "$rpmtop/RPMS" -name '*.rpm' -exec mv {} "$here/" \;

rpm=$(ls -1t "$here"/lvda-dkms-*.rpm 2>/dev/null | head -1 || true)
[ -n "$rpm" ] && echo "wrote $rpm"
