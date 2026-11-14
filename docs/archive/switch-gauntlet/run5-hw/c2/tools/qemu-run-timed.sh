#!/bin/bash
# qemu-run.sh with a QMP socket; records QEMU elapsed time (socket appears
# to runner exit) and, if QEMU is still running 150 s and 200 s after its
# socket appeared, takes 'info registers -a' (3 samples, 0.5 s apart) each
# time. Kills nothing. Usage: qemu-run-timed.sh <worktree> <outfile> <label> <make args...>
set -u
wt=$1; out=$2; label=$3; shift 3
T=$(dirname "$0")
sock=$(mktemp -u /tmp/claude-1000/c2qmp.XXXXXX)
( "$T/qemu-run.sh" "$wt" "$out" "$label" QEMU_CPU="cortex-a76 -accel tcg,thread=multi -qmp unix:$sock,server=on,wait=off" "$@" ) &
runner=$!
t0=""; s150=0; s200=0
while kill -0 $runner 2>/dev/null; do
  sleep 1
  if [ -z "$t0" ] && [ -S "$sock" ]; then t0=$SECONDS; fi
  if [ -n "$t0" ]; then
    el=$((SECONDS - t0))
    if [ $s150 = 0 ] && [ $el -ge 150 ] && [ -S "$sock" ]; then python3 -I "$T/qmp_regs.py.txt" "$sock" 3 > "$out.regs-150s.txt" 2>&1; s150=1; fi
    if [ $s200 = 0 ] && [ $el -ge 200 ] && [ -S "$sock" ]; then python3 -I "$T/qmp_regs.py.txt" "$sock" 3 > "$out.regs-200s.txt" 2>&1; s200=1; fi
  fi
done
wait $runner
el=$(( ${t0:+SECONDS - t0} + 0 ))
echo "# QEMU elapsed (socket to exit, +-1 s): ${el} s; samples at 150 s: $s150, at 200 s: $s200" >> "$out"
echo "elapsed=${el}s s150=$s150 s200=$s200"
