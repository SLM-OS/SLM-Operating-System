#!/bin/bash
# Copy staged evidence + tables into the worktree, commit and push to
# gauntlet-3-hw-jetson (pull --rebase first, plain push).
# Usage: push-ev.sh "<commit subject>" "<body line>"
set -u
C2=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/a3f89c3b-b428-422e-83c3-16d5b1f3e49e/scratchpad/c2
WT=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/agent-a79529bf39e71d97a
DST="$WT/docs/archive/coherency-gauntlet/run3-hw/c2"
mkdir -p "$DST/analysis"
cp -n "$C2"/ev/run3-hw-c2/* "$DST/" 2>/dev/null
cp "$C2"/ev/tables/* "$DST/analysis/"
for f in stage-log.txt trigger-log.txt uefi-deploy-log.txt; do
    [ -f "$C2/ev/$f" ] && cp "$C2/ev/$f" "$DST/$f"
done
git -C "$WT" add docs/archive/coherency-gauntlet/run3-hw/c2
git -C "$WT" commit -q -m "$1

$2

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" || { echo "nothing to commit"; exit 0; }
git -C "$WT" pull -q --rebase origin gauntlet-3-hw-jetson && git -C "$WT" push -q origin HEAD:gauntlet-3-hw-jetson
git -C "$WT" log --oneline -1
