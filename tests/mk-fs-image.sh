#!/usr/bin/env bash
# mk-fs-image.sh <ext2|ext4|ntfs> <out.img>: a small volume with the layout `systest fs` checks:
# pattern files of several sizes (byte i = (i * 7 + size) % 251), a nested directory, a
# 300-entry directory, and on ext4 a short and a long symbolic link.
set -euo pipefail
kind=$1; out=$2
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
python3 -I - "$work/root" <<'PY'
import os, sys
root = sys.argv[1]
os.makedirs(root + "/dir/sub")
def pattern(path, size):
    with open(path, "wb") as f:
        f.write(bytes((i * 7 + size) % 251 for i in range(size)))
for size in (0, 1, 4095, 4096, 70000, 5 * 1024 * 1024 + 3):
    pattern(f"{root}/p{size}", size)
pattern(root + "/dir/sub/deep.bin", 12345)
os.makedirs(root + "/many")
for i in range(300):
    open(f"{root}/many/file{i:03d}", "w").write(str(i))
PY
case $kind in
ext2|ext4)
    ln -s p4096 "$work/root/short-link"
    ln -s "$(printf 'dir/%.0s' $(seq 1 1))sub/../sub/deep.bin-that-is-long-enough-to-need-a-block-of-its-own-xxxxxxxxxxxx" "$work/root/long-link"
    truncate -s 64M "$out"
    mke2fs -q -F -t "$kind" -L influxtest -d "$work/root" "$out"
    ;;
ntfs)
    truncate -s 64M "$out"
    mkntfs -q -F -f -L influxtest "$out" >/dev/null
    for f in $(cd "$work/root" && find . -type f | sed 's|^\./||'); do
        ntfscp -q "$out" "$work/root/$f" "/$f" 2>/dev/null || { d=$(dirname "$f"); echo "ntfscp needs $d"; exit 1; }
    done
    ;;
esac
echo "created $out ($kind)"
