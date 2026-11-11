#!/bin/bash
# Finish a cell: write its evidence files from the transcript, copy them to
# the run3-hw-c2 staging dir, and run analyze.py + bars.py q1.
# Usage: cell-finish.sh <cell-name> <since-ISO> <head> [--no-assign]
set -u
C2=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/a3f89c3b-b428-422e-83c3-16d5b1f3e49e/scratchpad/c2
CELL="$1"; SINCE="$2"; HEAD="$3"; MODE="${4:-}"
UNTIL=$(date -u +%Y-%m-%dT%H:%M:%S)
mkdir -p "$C2/ev/$CELL" "$C2/ev/tables" "$C2/ev/run3-hw-c2"
if [ "$MODE" != "--no-assign" ]; then
    python3 -I "$C2/bin/assign.py" cell "$CELL" "$SINCE" "$UNTIL" "$C2/ev/$CELL" | tail -4
fi
cp -n "$C2/ev/$CELL"/* "$C2/ev/run3-hw-c2/"
cd "$C2/ev/run3-hw-c2" || exit 1
python3 -I "$C2/bin/analyze.py" --head "$HEAD" "$CELL" "$CELL"-run*.txt > "$C2/ev/tables/$CELL.md"
python3 -I "$C2/bin/bars.py" q1 --head "$HEAD" "$CELL"-run*.txt > "$C2/ev/tables/$CELL-q1.txt"
echo "files: $(ls "$CELL"-* | wc -l)  window $SINCE .. $UNTIL"
cat "$C2/ev/tables/$CELL-q1.txt"
sed -n '8,16p' "$C2/ev/tables/$CELL.md"
echo "- Cell $CELL finished: window $SINCE..$UNTIL head $HEAD" >> "$C2/PROGRESS.md"
