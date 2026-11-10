#!/bin/bash
# Clean-build one Jetson cell and stash its images.
# Usage: build-cell.sh <worktree> <cell-name> [extra make args...]
set -u
WT="$1"; CELL="$2"; shift 2
OUT=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/10c489d6-554a-53cc-8bab-69cf599b9e5c/scratchpad/cells
mkdir -p "$OUT"
export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH
echo "head $(git -C "$WT" rev-parse HEAD)"
echo "status: $(git -C "$WT" status --short | wc -l) changed paths"
make -C "$WT" kernel-clean > /dev/null 2>&1
make -C "$WT" kernel PLATFORM=JETSON_ORIN_NANO "$@" > "$OUT/build-$CELL.log" 2>&1
rc=$?
echo "make rc=$rc"
if [ $rc -eq 0 ]; then
    cp "$WT/build/kernel/slmos.elf" "$OUT/slmos-c2-$CELL.elf"
    cp "$WT/build/kernel/slmos.bin" "$OUT/slmos-c2-$CELL.bin"
    sha256sum "$OUT/slmos-c2-$CELL.elf" "$OUT/slmos-c2-$CELL.bin"
fi
grep -E 'warning|error' "$OUT/build-$CELL.log" | grep -o 'kernel/[a-z_/0-9]*\.[ch]:[0-9]*' | grep -v lua | sort | uniq -c
echo "EXIT $rc"
