#!/bin/bash
E=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/critic5c2-evidence
W=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees
r() { $E/run.sh "$@" >> $E/summary.txt 2>&1; }
r $W/critic5c2-fix  $E/regs-ws-fix-c0ea0714-run01.txt "fix head, WS=ON, test_regs x1000" WORK_STEALING=ON TEST_FILTER=test_regs_every_cpu_reports TEST_REPEAT=1000 TEST_TIMEOUT=240
r $W/critic5c2-base $E/regs-ws-base-3b369aef-run01.txt "base, WS=ON, test_regs x1000" WORK_STEALING=ON TEST_FILTER=test_regs_every_cpu_reports TEST_REPEAT=1000 TEST_TIMEOUT=240
for i in 01 02 03 04 05 06; do
r $W/critic5c2-fix  $E/full-ws-fix-c0ea0714-run$i.txt "fix head, full make test WORK_STEALING=ON" WORK_STEALING=ON
done
echo "PLAN2 DONE $(date -u +%T)" >> $E/summary.txt
