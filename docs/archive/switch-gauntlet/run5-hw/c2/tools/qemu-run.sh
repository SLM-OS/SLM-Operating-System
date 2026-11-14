#!/bin/bash
# C2 QEMU runner (switch gauntlet run 5). Usage:
#   qemu-run.sh <worktree> <outfile> <label> <make args...>
# Runs `make test <make args>` once in <worktree> (QEMU under the Makefile's
# systemd-run guard and timeout, MTTCG explicit), copies the full serial log
# into <outfile> behind a header with the head, elf sha256, banner, load
# average at start and end, and a one-line classification.
set -u
wt=$1; out=$2; label=$3; shift 3
export PATH="$HOME/.cargo/bin:$PATH"
load0=$(cut -d' ' -f1-3 /proc/loadavg)
t0=$(date -u +%FT%TZ)
make -C "$wt" test QEMU_CPU='cortex-a76 -accel tcg,thread=multi' "$@" > "$out.make" 2>&1
rc=$?
load1=$(cut -d' ' -f1-3 /proc/loadavg)
t1=$(date -u +%FT%TZ)
ser="$wt/build/test-output.log"
elf="$wt/build/kernel-test/slmos.elf"
head=$(git -C "$wt" rev-parse --short=8 HEAD)
dirty=$(git -C "$wt" status --porcelain --untracked-files=no | wc -l)
banner=$(grep -a -m1 -o 'build [0-9]* ([0-9a-f]*[^)]*)' "$ser")
lastiter=$(grep -a -o '\[ITER [0-9]*/[0-9]*\] *\[[A-Z]*\] [a-z_0-9]*' "$ser" | tail -1)
npass=$(grep -a -c '\[ITER [0-9]*/[0-9]*\] *\[PASS\]' "$ser")
nfail=$(grep -a -c '\[ITER [0-9]*/[0-9]*\] *\[FAIL\]' "$ser")
firstfail=$(grep -a -m1 -o '\[ITER [0-9]*/[0-9]*\] *\[FAIL\]' "$ser")
cls=PASS
grep -a -q 'KERNEL PANIC' "$ser" && cls=FAULT
grep -a -q 'terminating on signal 15 from pid [0-9]* (/usr/bin/timeout)' "$ser" && cls="${cls}+TIMEOUT"
[ "$nfail" -gt 0 ] && cls="${cls}+ASSERT"
[ "$rc" -ne 0 ] && [ "$cls" = PASS ] && cls=FAIL-OTHER
{
  echo "# C2 run5 QEMU run: $label"
  echo "# head $head (uncommitted tracked changes: $dirty); elf sha256 $(sha256sum "$elf" | cut -d' ' -f1)"
  echo "# banner: $banner"
  echo "# make test $* QEMU_CPU='cortex-a76 -accel tcg,thread=multi'  (MemoryMax/CPUQuota/timeout guard from the Makefile)"
  echo "# start $t0 load $load0; end $t1 load $load1; make rc=$rc"
  echo "# RESULT $cls iter_pass=$npass iter_fail=$nfail first_fail=[$firstfail] last_iter=[$lastiter]"
  echo "# ---- full serial log (build/test-output.log) ----"
  cat "$ser"
} > "$out"
rm -f "$out.make"
sed -n 6p "$out"
