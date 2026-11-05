#!/bin/sh
# Builds the inFlux system image on the Linux host with nothing on PATH but the developer tools
# bundle (build/devtools, from build-devtools.sh): exactly the tools inFlux itself has, with
# BusyBox's sh running every recipe. A GNU-only assumption in the build fails here, on Linux,
# where it is cheap to debug, instead of on inFlux.
#
#   tools/toolchain/hermetic-build.sh [builddir] [make targets...]   (default: build/hermetic system)
set -eu
TOP=$(cd "$(dirname "$0")/../.." && pwd)
DT=$TOP/build/devtools
[ -x "$DT/bin/gcc" ] || { echo "no $DT: run tools/toolchain/build-devtools.sh first" >&2; exit 1; }
OUT=${1:-$TOP/build/hermetic}
[ $# -gt 0 ] && shift
[ $# -gt 0 ] || set -- system
exec env -i HOME=/nonexistent TERM=dumb PATH="$DT/bin" "$DT/bin/make" -C "$TOP" -j"$(nproc)" \
    SHELL="$DT/bin/sh" BUILD="$OUT" HOST_LDFLAGS=-static "$@"
# (the host tools static: the bundle's gcc links dynamically against inFlux's
#  /lib/ld-musl-x86_64.so.1, which this Linux host does not have; on inFlux it is there)
