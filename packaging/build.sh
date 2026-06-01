#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# One build command for any platform. Detect the distro / packaging toolchain
# and produce the native lvda package by dispatching to the per-format packager
# in this directory. Override detection with the LVDA_PKG environment variable
# or by passing the format as the first argument.
#
#   ./build.sh                 # auto-detect and build
#   ./build.sh deb             # force a specific format
#   ./build.sh -n              # print what would run, build nothing
#   LVDA_PKG=rpm ./build.sh    # override via environment
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # packaging/
reporoot=$(CDPATH= cd -- "$here/.." && pwd)         # repo root

usage() {
	cat <<EOF
usage: ${0##*/} [-n|--dry-run] [arch|deb|rpm|nix]

Builds the native lvda package for the current system. With no format the
format is auto-detected from /etc/os-release, then from the installed build
tools. Override with the first argument or the LVDA_PKG environment variable.
EOF
}

dry=0
fmt=""
while [ $# -gt 0 ]; do
	case "$1" in
		-h|--help) usage; exit 0 ;;
		-n|--dry-run) dry=1 ;;
		arch|deb|rpm|nix) fmt="$1" ;;
		*) echo "error: unknown argument '$1'" >&2; usage >&2; exit 1 ;;
	esac
	shift
done

# Explicit override wins: CLI arg, then LVDA_PKG.
[ -n "$fmt" ] || fmt="${LVDA_PKG:-}"

# Auto-detect from /etc/os-release (ID, then the ID_LIKE fallback chain).
if [ -z "$fmt" ] && [ -r /etc/os-release ]; then
	# shellcheck disable=SC1091
	. /etc/os-release
	for token in ${ID:-} ${ID_LIKE:-}; do
		case "$token" in
			arch|cachyos|manjaro|endeavouros|artix|garuda|arcolinux) fmt=arch ;;
			debian|ubuntu|linuxmint|pop|elementary|raspbian|devuan|kali|neon) fmt=deb ;;
			fedora|rhel|centos|rocky|almalinux|ol|amzn|mageia) fmt=rpm ;;
			opensuse*|suse|sles|sled) fmt=rpm ;;
			nixos) fmt=nix ;;
			*) continue ;;
		esac
		break
	done
fi

# Last resort: pick whatever packaging tool is installed.
if [ -z "$fmt" ]; then
	if   command -v makepkg           >/dev/null 2>&1; then fmt=arch
	elif command -v dpkg-buildpackage >/dev/null 2>&1; then fmt=deb
	elif command -v rpmbuild          >/dev/null 2>&1; then fmt=rpm
	elif command -v nix               >/dev/null 2>&1; then fmt=nix
	fi
fi

[ -n "$fmt" ] || {
	echo "error: could not detect a packaging format; pass one of arch|deb|rpm|nix" >&2
	exit 1
}

case "$fmt" in
	arch)
		tool=makepkg
		build_cmd="cd \"$here/arch\" && ./makedist.sh && makepkg -f"
		install_hint="sudo pacman -U $here/arch/*.pkg.tar.zst"
		;;
	deb)
		tool=dpkg-buildpackage
		build_cmd="\"$here/debian/makedeb.sh\""
		install_hint="sudo apt-get install $here/debian/lvda-dkms_*.deb"
		;;
	rpm)
		tool=rpmbuild
		build_cmd="\"$here/rpm/makerpm.sh\""
		install_hint="sudo dnf install $here/rpm/lvda-dkms-*.rpm"
		;;
	nix)
		tool=nix
		build_cmd="cd \"$reporoot\" && nix build .#lvda"
		install_hint="enable the NixOS module (see BUILD.md); the built module is ./result"
		;;
esac

echo "==> packaging format: $fmt"
if [ "$dry" -eq 1 ]; then
	printf 'would run: %s\n' "$build_cmd"
	exit 0
fi

command -v "$tool" >/dev/null 2>&1 || {
	echo "error: '$tool' not found — install the '$fmt' build deps (see BUILD.md)" >&2
	exit 1
}

sh -c "$build_cmd"
echo "install with: $install_hint"
