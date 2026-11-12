#!/bin/bash
# Production Jetson build at HEAD (images copied to scratchpad), then the /ci steps of
# .claude/commands/ci.md in order, each logged; records every step's rc (does not stop).
W=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/l4-c3
S=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/eff61e97-fcbb-4f99-885f-81fd717f4885/scratchpad
L=$S/ci2; mkdir -p $L; H=$(git -C $W rev-parse --short=8 HEAD)
cd $W
echo "00 skipped (prod images already built at 8a3406ea)" >> $L/summary.txt
step() { n=$1; shift; echo "== $n: $* ($(date -u +%T))" > $L/$n.log; bash -c "$*" >> $L/$n.log 2>&1; echo "$n rc=$? ($*) $(date -u +%T)" >> $L/summary.txt; }
echo "ci at $(git rev-parse HEAD) start $(date -u +%T)" >> $L/summary.txt
step 01-arm64 "make kernel-clean && make kernel PLATFORM=QEMU_VIRT"
step 02-x86 "make kernel-clean && make kernel PLATFORM=X86_64"
step 03-jetson-real "make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO AI_WEIGHTS=real && make kernel-test-clean && make kernel-test PLATFORM=JETSON_ORIN_NANO AI_WEIGHTS=real"
step 04-test "make test"
step 04b-test-x86 "make test PLATFORM=X86_64"
step 05-test-aisched "make test AI_SCHED=ON"
step 06-runtime "make runtime-test"
step 07-braces "make test-check-braces && make check-braces"
step 08-build-order "make test-build-order"
step 09-layout "make test-jetson-uefi-layout"
step 09b-qemu-uefi "make kernel-clean && make kernel PLATFORM=QEMU_VIRT && make test-qemu-uefi"
step 10-diff "git diff origin/main...HEAD --stat; git status --short"
echo "CI DONE $(date -u +%T)" >> $L/summary.txt
