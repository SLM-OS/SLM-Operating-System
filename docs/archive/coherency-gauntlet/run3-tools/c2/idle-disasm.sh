#!/bin/bash
# Disassemble idle_task_func in each cell image and count the wait
# instructions; diff each V1 image against its non-V1 base.
set -u
export PATH=/opt/arm-gnu-toolchain/bin:$PATH
CELLS=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/a3f89c3b-b428-422e-83c3-16d5b1f3e49e/scratchpad/cells
OUT=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/a3f89c3b-b428-422e-83c3-16d5b1f3e49e/scratchpad/c2/disasm
mkdir -p "$OUT"
for t in "$@"; do
    aarch64-none-elf-objdump -d --no-show-raw-insn "$CELLS/slmos-r3-$t.elf" \
        | awk '/<idle_task_func>:/,/^$/' | sed 's/^ *[0-9a-f]*:[[:space:]]*//' > "$OUT/idle-$t.s"
    echo "$t: $(wc -l < "$OUT/idle-$t.s") lines; wfi=$(grep -c '^wfi' "$OUT/idle-$t.s") wfe=$(grep -c '^wfe' "$OUT/idle-$t.s")"
done
echo "--- diff ncoff-instr ncoff-instr-v1"
diff "$OUT/idle-ncoff-instr.s" "$OUT/idle-ncoff-instr-v1.s"
echo "--- diff removal-instr removal-instr-v1"
diff "$OUT/idle-removal-instr.s" "$OUT/idle-removal-instr-v1.s"
echo "--- nc-instr wait context"
grep -n -B4 -A2 '^wf' "$OUT/idle-nc-instr.s"
