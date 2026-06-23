#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Build the native lvda package for whatever distro this is. Thin entry point:
# distro detection and per-format dispatch live in packaging/build.sh.
#
#   ./build.sh                 # auto-detect the distro and build
#   ./build.sh deb             # force a specific format
#   ./build.sh -n              # print what would run, build nothing
#   LVDA_PKG=rpm ./build.sh    # override detection via environment
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # repo root
exec "$here/packaging/build.sh" "$@"
