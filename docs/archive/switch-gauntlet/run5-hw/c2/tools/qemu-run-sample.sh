#!/bin/bash
# qemu-run.sh plus a QMP socket: when the serial log stops growing for 8 s
# after <marker> appeared, take 3 'info registers -a' samples 0.5 s apart
# (qmp_regs.py), then let the Makefile's timeout end the run. Kills nothing.
# Usage: qemu-run-sample.sh <worktree> <outfile> <label> <marker> <make args...>
set -u
wt=$1; out=$2; label=$3; marker=$4; shift 4
T=$(dirname "$0")
sock=$(mktemp -u /tmp/claude-1000/c2qmp.XXXXXX)
ser="$wt/build/test-output.log"
( "$T/qemu-run.sh" "$wt" "$out" "$label" QEMU_CPU="cortex-a76 -accel tcg,thread=multi -qmp unix:$sock,server=on,wait=off" "$@" ) &
runner=$!
sampled=0
while kill -0 $runner 2>/dev/null; do
  sleep 2
  if [ $sampled = 0 ] && [ -S "$sock" ] && grep -a -q "$marker" "$ser" 2>/dev/null; then
    s1=$(stat -c %s "$ser"); sleep 8; s2=$(stat -c %s "$ser")
    if [ "$s1" = "$s2" ] && [ -S "$sock" ]; then
      python3 -I "$T/qmp_regs.py.txt" "$sock" 3 > "$out.regs-raw.txt" 2>&1; sampled=1
    fi
  fi
done
wait $runner
echo "sampled=$sampled"
