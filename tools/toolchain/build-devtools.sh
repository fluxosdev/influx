#!/bin/sh
# Builds the inFlux developer tools bundle: a native, statically linked x86_64-linux-musl
# toolchain (gcc, g++, binutils, musl), GNU make and BusyBox. inFlux implements the Linux
# x86-64 system call ABI, so these run on it unmodified; they also run on the Linux host,
# which is how the bundle is checked (tools/toolchain/hermetic-build.sh).
#
#   tools/toolchain/build-devtools.sh [workdir]      (default: build/devtools-work)
#
# Output: build/devtools/ (bin, include, lib, libexec, ...: the toolchain's root, which is
# also its sysroot) and build/devtools.tar. Needs wget, a host gcc/g++ and about 6 GB of
# disk; m4 is built from source when the host has none. Takes 20-60 minutes.
set -eu

TOP=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mkdir -p "${1:-$TOP/build/devtools-work}" && cd "${1:-$TOP/build/devtools-work}" && pwd)
OUT=$TOP/build/devtools
JOBS=${JOBS:-$(nproc)}
TARGET=x86_64-linux-musl

# pinned versions; musl matches third_party/musl
MCM_COMMIT=5e8042e        # github.com/richfelker/musl-cross-make
GCC_VER=14.3.0
BINUTILS_VER=2.44
MUSL_VER=1.2.5
GMP_VER=6.3.0
MPC_VER=1.3.1
MPFR_VER=4.2.2
MAKE_VER=4.4.1
BUSYBOX_VER=1.36.1
M4_VER=1.4.19
GNU_SITE=https://ftpmirror.gnu.org/gnu
# sha256 of the tarballs fetched outside musl-cross-make (which checks its own)
MAKE_SHA256=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
BUSYBOX_SHA256=b8cc24c9574d809e7279c3be349795c5d5ceb6fdf19ca709f80cde50e47de314
M4_SHA256=63aede5c6d33b6d9b13511cd0be2cac046f2e70fd0a07aa9573a04a82783af96

log() { printf '\n==> %s\n' "$*"; }

fetch() {   # fetch URL SHA256 -> $WORK/sources/<name>
    f=$WORK/sources/$(basename "$1")
    mkdir -p "$WORK/sources"
    if [ ! -f "$f" ]; then wget -q -O "$f.part" "$1"; mv "$f.part" "$f"; fi
    echo "$2  $f" | sha256sum -c --quiet - || { echo "checksum mismatch: $f" >&2; exit 1; }
}

# ---- host m4 (GMP's build needs it) ----
if ! command -v m4 >/dev/null 2>&1; then
    if [ ! -x "$WORK/host/bin/m4" ]; then
        log "building m4 $M4_VER for the host"
        fetch "$GNU_SITE/m4/m4-$M4_VER.tar.xz" $M4_SHA256
        rm -rf "$WORK/m4-$M4_VER" && tar -C "$WORK" -xf "$WORK/sources/m4-$M4_VER.tar.xz"
        (cd "$WORK/m4-$M4_VER" && ./configure -q --prefix="$WORK/host" CFLAGS="-O2 -std=gnu17" >/dev/null && make -j"$JOBS" >/dev/null && make install >/dev/null)
    fi
    PATH=$WORK/host/bin:$PATH
fi

# ---- musl-cross-make ----
if [ ! -d "$WORK/mcm" ]; then
    log "fetching musl-cross-make $MCM_COMMIT"
    git clone -q https://github.com/richfelker/musl-cross-make "$WORK/mcm"
fi
(cd "$WORK/mcm" && git checkout -q "$MCM_COMMIT")

cat > "$WORK/mcm/config.mak" <<EOF
TARGET = $TARGET
GCC_VER = $GCC_VER
BINUTILS_VER = $BINUTILS_VER
MUSL_VER = $MUSL_VER
GMP_VER = $GMP_VER
MPC_VER = $MPC_VER
MPFR_VER = $MPFR_VER
COMMON_CONFIG += --disable-nls
GCC_CONFIG += --disable-libquadmath --disable-libitm --disable-lto --disable-libgomp
EOF

# 1. a cross compiler that runs on the host and emits static musl binaries
if [ ! -x "$WORK/cross/bin/$TARGET-gcc" ]; then
    log "building the $TARGET cross toolchain (gcc $GCC_VER, binutils $BINUTILS_VER)"
    # gnu17: the host gcc may default to C23, which GMP's configure and m4 predate
    make -C "$WORK/mcm" -j"$JOBS" OUTPUT="$WORK/cross" \
        COMMON_CONFIG="--disable-nls CC='gcc -std=gnu17' CXX='g++'" install >"$WORK/cross.log" 2>&1 ||
        { tail -40 "$WORK/cross.log"; exit 1; }
fi
PATH=$WORK/cross/bin:$PATH
XCC="$TARGET-gcc -static --static"
XCXX="$TARGET-g++ -static --static"

# 2. the same toolchain again, built by the cross compiler: static binaries that run on inFlux
if [ ! -x "$WORK/native/bin/gcc" ]; then
    log "building the native toolchain"
    rm -rf "$WORK/native"
    make -C "$WORK/mcm" -j"$JOBS" NATIVE=1 OUTPUT="$WORK/native" \
        COMMON_CONFIG="--disable-nls CC='$XCC' CXX='$XCXX' CFLAGS='-O2 -g0' CXXFLAGS='-O2 -g0' LDFLAGS='-s -static'" \
        install >"$WORK/native.log" 2>&1 || { tail -40 "$WORK/native.log"; exit 1; }
fi

# 3. GNU make
if [ ! -x "$WORK/make-$MAKE_VER/make" ]; then
    log "building GNU make $MAKE_VER"
    fetch "$GNU_SITE/make/make-$MAKE_VER.tar.gz" $MAKE_SHA256
    rm -rf "$WORK/make-$MAKE_VER" && tar -C "$WORK" -xf "$WORK/sources/make-$MAKE_VER.tar.gz"
    (cd "$WORK/make-$MAKE_VER" &&
     ./configure -q --host=$TARGET --prefix=/ --disable-nls --without-guile \
        CC="$XCC" CFLAGS="-O2 -g0" LDFLAGS="-s -static" >/dev/null &&
     make -j"$JOBS" >/dev/null)
fi

# 4. BusyBox: /bin/sh and the POSIX utilities the build uses
BB=$WORK/busybox-$BUSYBOX_VER
if [ ! -x "$BB/busybox" ]; then
    log "building BusyBox $BUSYBOX_VER"
    fetch "https://busybox.net/downloads/busybox-$BUSYBOX_VER.tar.bz2" $BUSYBOX_SHA256
    rm -rf "$BB" && tar -C "$WORK" -xf "$WORK/sources/busybox-$BUSYBOX_VER.tar.bz2"
    make -C "$BB" defconfig >/dev/null
    # static; leave out what needs Linux-only kernel interfaces or does not build against musl
    for opt in STATIC=y TC=n FEATURE_TC_INGRESS=n SELINUX=n PAM=n FEATURE_SYSLOG=y \
               MOUNT=n UMOUNT=n SWAPON=n SWAPOFF=n INSMOD=n RMMOD=n LSMOD=n MODPROBE=n DEPMOD=n \
               FEATURE_HAVE_RPC=n FEATURE_MOUNT_NFS=n FEATURE_INETD_RPC=n; do
        k=CONFIG_${opt%%=*} v=${opt#*=}
        sed -i "/^$k=/d; /^# $k is not set/d" "$BB/.config"
        if [ "$v" = n ]; then echo "# $k is not set" >> "$BB/.config"; else echo "$k=$v" >> "$BB/.config"; fi
    done
    yes '' | make -C "$BB" oldconfig >/dev/null 2>&1
    make -C "$BB" -j"$JOBS" CROSS_COMPILE=$TARGET- CONFIG_EXTRA_CFLAGS="-O2" >"$WORK/busybox.log" 2>&1 ||
        { tail -40 "$WORK/busybox.log"; exit 1; }
fi

# ---- assemble build/devtools ----
log "assembling $OUT"
rm -rf "$OUT"
cp -a "$WORK/native" "$OUT"
# drop what nothing on inFlux uses: the target-prefixed duplicates and the docs
rm -rf "$OUT/share" "$OUT/$TARGET/bin"
for f in "$OUT"/bin/$TARGET-*; do
    [ -e "$f" ] || continue
    plain=$OUT/bin/${f##*/$TARGET-}
    if [ -e "$plain" ]; then rm "$f"; else mv "$f" "$plain"; fi
done
ln -sf gcc "$OUT/bin/cc"
# the native compiler's sysroot is its install root, and it looks for headers in <root>/usr/include
ln -sfn . "$OUT/usr"
cp "$WORK/make-$MAKE_VER/make" "$OUT/bin/make"
cp "$BB/busybox" "$OUT/bin/busybox"
for a in $("$BB/busybox" --list); do
    [ -e "$OUT/bin/$a" ] || ln -s busybox "$OUT/bin/$a"
done
find "$OUT/bin" "$OUT/libexec" -type f -perm -u+x -exec strip --strip-all {} + 2>/dev/null || true

# ---- check it on the host: the bundle alone compiles, links and runs a program ----
log "checking the bundle"
T=$WORK/check && rm -rf "$T" && mkdir -p "$T"
printf '#include <stdio.h>\nint main(void){puts("devtools ok");return 0;}\n' > "$T/hello.c"
printf '#include <string>\n#include <cstdio>\nint main(){std::string s="c++ ok";std::puts(s.c_str());}\n' > "$T/hello.cc"
# static, to run here (the dynamic linker the default links against is inFlux's /lib/ld-musl-x86_64.so.1)
env -i PATH="$OUT/bin" sh -c "cd '$T' && gcc -O2 -static hello.c -o hello && ./hello && g++ -static hello.cc -o hellocc && ./hellocc &&
    gcc -O2 hello.c -o hello-dyn && readelf -l hello-dyn | grep -q 'ld-musl-x86_64.so.1' && echo 'dynamic link ok' && make --version | head -1"

tar --format=ustar --owner=0 --group=0 -C "$OUT" -cf "$TOP/build/devtools.tar" .
log "done: $OUT ($(du -sh "$OUT" | cut -f1)), $TOP/build/devtools.tar"
