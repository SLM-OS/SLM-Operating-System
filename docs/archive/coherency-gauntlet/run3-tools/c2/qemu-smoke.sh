#!/bin/bash
# QEMU smoke test of the c2t v2 output (coherency run 3, C2).
# Builds with WORK_STEALING=ON through the Makefile, then drives
# `make run` (QEMU_GUARD applies systemd-run MemoryMax/CPUQuota) with
# shell commands on stdin.
# Usage: qemu-smoke.sh <worktree> <logfile> [sustain_ms]
set -u
WT="$1"
LOG="$2"
MS="${3:-2500}"
export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH
echo "head $(git -C "$WT" rev-parse --short HEAD)" > "$LOG.build"
make -C "$WT" kernel WORK_STEALING=ON >> "$LOG.build" 2>&1
rc=$?
echo "build rc=$rc"
[ $rc -eq 0 ] || exit $rc
feed() {
    sleep 25
    printf 'cat /sys/version\r'
    sleep 3
    printf 'bench stealing --sustain %s\r' "$MS"
    sleep 45
    printf 'bench stealing\r'
    sleep 15
}
feed | timeout 120 make -C "$WT" run WORK_STEALING=ON > "$LOG" 2>&1
echo "run EXIT $?"
