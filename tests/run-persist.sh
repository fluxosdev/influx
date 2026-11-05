#!/usr/bin/env bash
# make test-persist: install onto a blank disk from the selftest ISO, then boot that disk twice.
# The first disk boot writes files all over the root filesystem and fakes a system update on the
# ESP; the second checks that everything (but /tmp) survived and the update was applied as it
# should be. Each boot powers off by itself; each must print SELFTEST: PASSED.
set -uo pipefail
iso=${1:?usage: run-persist.sh influx-selftest.iso disk.img}
disk=${2:?usage: run-persist.sh influx-selftest.iso disk.img}
accel=(-cpu max)
[ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
boot() {   # boot <name> [cd]
    local log; log=$(mktemp "${TMPDIR:-/tmp}/influx-persist-$1.XXXXXX.log")
    timeout "${SELFTEST_TIMEOUT:-300}" qemu-system-x86_64 "${accel[@]}" -M q35 -m 2G -smp 2 \
        ${2:+-drive file=$iso,media=cdrom,if=none,id=cd -device ide-cd,drive=cd,bootindex=0} \
        -drive "file=$disk,format=raw,if=none,id=d" -device ahci,id=ahci -device ide-hd,drive=d,bus=ahci.0,bootindex=1 \
        -display none -serial "file:$log" -no-reboot >/dev/null 2>&1
    grep -aE '^(PASS|FAIL|systest:)|sysroot:' "$log"
    if grep -aq 'SELFTEST: PASSED' "$log"; then rm -f "$log"; return 0; fi
    echo "persist: boot '$1' FAILED (serial log: $log)"
    grep -aE 'PANIC|EXCEPTION|SELFTEST|install:' "$log" | tail -20
    return 1
}
echo "== install from the CD"; boot install cd || exit 1
echo "== first boot from the disk"; boot write || exit 1
echo "== second boot from the disk"; boot check || exit 1
echo "persist: passed"
