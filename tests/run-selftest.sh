#!/usr/bin/env bash
# Boots a self-test ISO headless; /bin/systest runs at startup with its output on the serial
# port and the machine powers off. Exits nonzero unless every check passed.
set -uo pipefail
iso=${1:?usage: run-selftest.sh influx-selftest.iso}
log=$(mktemp "${TMPDIR:-/tmp}/influx-selftest.XXXXXX.log")
accel=(-cpu max)
[ -w /dev/kvm ] && accel=(-enable-kvm -cpu host)
timeout "${SELFTEST_TIMEOUT:-180}" qemu-system-x86_64 "${accel[@]}" -M q35 -m 2G -smp 2 \
    -drive "file=$iso,media=cdrom,if=none,id=cd" -device ide-cd,drive=cd \
    -netdev user,id=n0 -device e1000e,netdev=n0 \
    -display none -serial "file:$log" -no-reboot >/dev/null 2>&1
grep -aE '^(PASS|FAIL|systest:)' "$log"
if grep -aq 'SELFTEST: PASSED' "$log"; then echo "selftest: passed"; rm -f "$log"; exit 0; fi
echo "selftest: FAILED (serial log: $log)"
grep -aE 'PANIC|EXCEPTION|SELFTEST|killed' "$log" | head -20
exit 1
