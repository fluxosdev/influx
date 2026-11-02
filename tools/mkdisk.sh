#!/bin/sh
# mkdisk.sh <out.img> <label> <size_mb>
# Builds a raw disk image: GPT with one btrfs partition populated from
# tools/diskroot. Requires sfdisk (util-linux) and mkfs.btrfs (btrfs-progs).
set -e
OUT="$1"; LABEL="$2"; SIZE_MB="${3:-256}"
HERE="$(cd "$(dirname "$0")" && pwd)"
PART="$OUT.part"
PART_MB=$((SIZE_MB - 4))
rm -f "$OUT" "$PART"
truncate -s "${PART_MB}M" "$PART"
mkfs.btrfs -q -L "$LABEL" -m single -d single -n 16384 --rootdir "$HERE/diskroot" "$PART" >/dev/null
truncate -s "${SIZE_MB}M" "$OUT"
printf 'label: gpt\nstart=2048, size=%d, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="%s"\n' $((PART_MB * 2048)) "$LABEL" | sfdisk -q "$OUT" >/dev/null
dd if="$PART" of="$OUT" bs=1M seek=1 conv=notrunc status=none
rm -f "$PART"
echo "created $OUT ($SIZE_MB MiB, btrfs '$LABEL')"
