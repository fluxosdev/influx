#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d /tmp/influx-fs-tests.XXXXXX)
trap 'rm -rf "$build_dir"' EXIT
cc=${CC:-gcc}
flags=(-std=gnu11 -O2 -g -ffreestanding -ffunction-sections -fdata-sections -iquote kernel/include)
"$cc" "${flags[@]}" -fsanitize=address,undefined tests/vfs_regression.c kernel/src/fs/vfs.c kernel/src/fs/tmpfs.c -Wl,--gc-sections -o "$build_dir/vfs"
ASAN_OPTIONS=detect_leaks=0 "$build_dir/vfs"
"$cc" "${flags[@]}" tests/btrfs_append_regression.c tests/btrfs_host_stubs.c kernel/src/fs/vfs.c -Wl,--gc-sections -pthread -o "$build_dir/btrfs"
"$build_dir/btrfs"
