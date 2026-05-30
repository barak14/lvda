#!/bin/sh
# Stage the lvda-<ver> source tarball that PKGBUILD consumes, straight from
# the working tree. Output lands next to this script (the PKGBUILD dir), so
# `makepkg -si` finds it. Run it whenever the sources change.
set -eu

ver=0.1.0
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # packaging/arch
reporoot=$(CDPATH= cd -- "$here/../.." && pwd)       # repo root (holds module/, uapi/, tools/, packaging/, LICENSE)

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
d="$stage/lvda-$ver"
mkdir -p "$d"

cp -a "$reporoot/module" "$reporoot/uapi" "$reporoot/tools" "$reporoot/packaging" "$d/"
cp -a "$reporoot/LICENSE" "$d/LICENSE"

# Drop build artifacts and any previously generated packaging output so the
# tarball is clean and never includes itself.
find "$d" \( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' \
	-o -name '.*.cmd' -o -name 'Module.symvers' -o -name 'modules.order' \
	-o -name 'compile_commands.json' -o -name '*.tar.gz' \
	-o -name '*.pkg.tar.*' \) -delete
find "$d/tools" -type f -name 'lvda-ctl' -delete
find "$d" -depth -type d -name '.cache' -exec rm -rf {} +

( cd "$stage" && tar -czf "$here/lvda-$ver.tar.gz" "lvda-$ver" )
echo "wrote $here/lvda-$ver.tar.gz"
