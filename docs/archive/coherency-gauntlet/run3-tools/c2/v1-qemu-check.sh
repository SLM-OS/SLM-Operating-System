#!/bin/bash
# V1 check in QEMU: build the WORK_STEALING=ON kernel at the checked-out
# head, diff idle_task_func against a saved disassembly, then make test.
# Usage: v1-qemu-check.sh <worktree> <saved-idle.s> <outdir>
set -u
WT="$1"; SAVED="$2"; OUT="$3"
BIN=$(dirname "$0")
export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH
head=$(git -C "$WT" rev-parse --short HEAD)
echo "head $head"
make -C "$WT" kernel WORK_STEALING=ON > "$OUT/v1-kernel-$head.log" 2>&1
echo "kernel rc=$?"
bash "$BIN/fn-disasm.sh" "$WT/build/kernel/slmos.elf" idle_task_func "$OUT/qemu-idle-$head.s"
if diff "$SAVED" "$OUT/qemu-idle-$head.s"; then echo "idle_task_func IDENTICAL"; else echo "idle_task_func DIFFERS"; fi
make -C "$WT" test > "$OUT/v1-make-test-$head.log" 2>&1
echo "make test rc=$?"
grep -a -E 'Tests? (run|passed|failed)|PASSED|FAILED|Summary|passed, ' "$OUT/v1-make-test-$head.log" | tail -8
