#!/bin/bash
# critic-c2 QEMU runner. Usage: run.sh <worktree> <outfile> <label> <make args...>
# One `make test <args>` in <worktree>; saves the full serial log behind a header. Kills nothing.
set -u
wt=$1; out=$2; label=$3; shift 3
export PATH="$HOME/.cargo/bin:$PATH"
load0=$(cut -d' ' -f1-3 /proc/loadavg); t0=$(date -u +%FT%TZ)
make -C "$wt" kernel-test-clean > "$out.make" 2>&1; rm -f "$wt/build/test-output.log"; make -C "$wt" kernel-test "$@" >> "$out.make" 2>&1; /usr/bin/time -f "user=%U sys=%S elapsed=%e" -o "$out.time" make -C "$wt" test "$@" >> "$out.make" 2>&1; rc=$?
if ! grep -q 'Running kernel tests\|qemu-system' "$out.make" && grep -q 'Error [0-9]' "$out.make" && [ ! -s "$wt/build/test-output.log" ]; then
  echo "BUILD-FAILED $label (see $out.make)"; exit 2; fi
load1=$(cut -d' ' -f1-3 /proc/loadavg); t1=$(date -u +%FT%TZ)
ser="$wt/build/test-output.log"; elf="$wt/build/kernel-test/slmos.elf"
{
  echo "# critic-c2 QEMU run: $label"
  echo "# head $(git -C "$wt" rev-parse --short=8 HEAD) dirty=$(git -C "$wt" status --porcelain --untracked-files=no | wc -l); elf sha256 $(sha256sum "$elf" | cut -d' ' -f1)"
  echo "# banner: $(grep -a -m1 -o 'build [0-9]* ([0-9a-f]*[^)]*)' "$ser")"
  echo "# make test $*"
  echo "# start $t0 load $load0; end $t1 load $load1; make rc=$rc; make-test process tree (QEMU) cpu: $(cat "$out.time" | tail -1)"
  echo "# ---- full serial log ----"
  cat "$ser"
} > "$out"
np=$(grep -a -c '\[ITER [0-9]*/[0-9]*\] *\[PASS\]' "$out"); nf=$(grep -a -c '\[ITER [0-9]*/[0-9]*\] *\[FAIL\]' "$out")
d=$(grep -a -c 'KERNEL PANIC\|Synchronous Exception' "$out"); s=$(grep -a -c '^\[FILTER\] Summary' "$out"); to=$(grep -a -c 'terminating on signal 15' "$out")
echo "$(basename $out) cpu[$(tail -1 "$out.time")] rc=$rc pass=$np fail=$nf dump=$d summary=$s timeout=$to start=$t0 end=$t1 load=$load0"
grep -q 'Error\|error:' "$out.make" && grep -m3 'error:' "$out.make"
rm -f "$out.make.keep"; [ $rc -eq 0 ] && rm -f "$out.make"
