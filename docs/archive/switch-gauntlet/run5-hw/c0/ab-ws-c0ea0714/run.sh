#!/bin/bash
# Interleaved A/B: make test WORK_STEALING=ON on base 8ec0e330 (s5-base) and fix head c0ea0714 (s5-ci-c2), one at a time.
O=$1; R=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees
export PATH="$HOME/.cargo/bin:$PATH"
run() { tag=$1; w=$2; cmd=$3; st=$(date -u +%T); ( cd $w && { echo "head $(git rev-parse HEAD) run $tag start $st load $(cut -d' ' -f1-3 /proc/loadavg)"; bash -c "$cmd"; } ) > $O/$tag.log 2>&1; rc=$?; echo "$tag rc=$rc start=$st end=$(date -u +%T) $(grep -E 'PASSED|FAILED' $O/$tag.log | tail -1 | cut -c1-6) fails=$(grep -E '^\s*\[FAIL\] test' $O/$tag.log | tr -d ' ' | tr '\n' ',')" >> $O/summary.txt; }
for i in 1 2 3 4 5 6; do
  run base-ws-$i $R/s5-base "make kernel-test-clean && make test WORK_STEALING=ON"
  run fix-ws-$i $R/s5-ci-c2 "make kernel-test-clean && make test WORK_STEALING=ON"
done
run fix-04r $R/s5-ci-c2 "make kernel-test-clean && make test"
run fix-05r $R/s5-ci-c2 "make kernel-test-clean && make test AI_SCHED=ON"
echo DONE >> $O/summary.txt
