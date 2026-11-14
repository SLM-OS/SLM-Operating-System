#!/bin/bash
# critic-c2 QEMU plan, one QEMU at a time, after main's A/B prints DONE.
E=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/critic5c2-evidence
W=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees
AB=/tmp/claude-1000/-home-john-projects-CS-496-Capstone-SLM-Operating-System--claude-worktrees-bridge-cse-014Q9aryGYwccZqUzMHyre5U/1c852bc6-70f3-472d-9685-39a4b46ec59c/scratchpad/ab-ws/summary.txt
until grep -q DONE $AB; do sleep 5; done
echo "A/B DONE seen $(date -u +%T)" >> $E/summary.txt
$E/jbuild.sh $W/critic5c2-jfix c0ea0714 r2=test_preempt_flag_follows_migrated_task r3=test_migrate_ready_during_switch_out,test_wake_during_switch_out &
$E/jbuild.sh $W/critic5c2-jbase 3b369aef r2=test_preempt_flag_follows_migrated_task r3m=test_migrate_ready_during_switch_out r3w=test_wake_during_switch_out &
wait
echo "JBUILD DONE $(date -u +%T)" >> $E/summary.txt
r() { $E/run.sh "$@" >> $E/summary.txt 2>&1; }
LIT="TEST_FILTER=test_regs_every_cpu_reports,test_litmus_patterns_pass"
# open row and the litmus rate, both heads, interleaved, same host load
r $W/critic5c2-fix  $E/lit-default-fix-c0ea0714-run01.txt  "fix head, default config, regs+litmus x15"  $LIT TEST_REPEAT=15 TEST_TIMEOUT=500
r $W/critic5c2-base $E/lit-default-base-3b369aef-run01.txt "base 3b369aef, default config, regs+litmus x15" $LIT TEST_REPEAT=15 TEST_TIMEOUT=500
r $W/critic5c2-fix  $E/lit-ws-fix-c0ea0714-run01.txt  "fix head, WS=ON, regs+litmus x15"  WORK_STEALING=ON $LIT TEST_REPEAT=15 TEST_TIMEOUT=500
r $W/critic5c2-base $E/lit-ws-base-3b369aef-run01.txt "base 3b369aef, WS=ON, regs+litmus x15" WORK_STEALING=ON $LIT TEST_REPEAT=15 TEST_TIMEOUT=500
ALL="TEST_FILTER=test_switch_window_held,test_switch_window_tick_forced,test_migrate_ready_during_switch_out,test_wake_during_switch_out,test_preempt_flag_follows_migrated_task"
r $W/critic5c2-fix  $E/all-fix-c0ea0714-run01.txt "fix head: R1, forced, R3m, R3w, R2 x10" $ALL TEST_REPEAT=10 SWITCH_CHECK=ON SWITCH_WINDOW_TEST=ON TEST_TIMEOUT=300
for i in 01 02; do
r $W/critic5c2-base $E/r1-base-3b369aef-run$i.txt "base: R1 held x1" TEST_FILTER=test_switch_window_held TEST_REPEAT=1 SWITCH_WINDOW_TEST=ON TEST_TIMEOUT=60
done
for i in 01 02; do
r $W/critic5c2-rev $E/forced-reverted-ef11ee3c-run$i.txt "reverted design: forced tick x1" TEST_FILTER=test_switch_window_tick_forced TEST_REPEAT=1 SWITCH_WINDOW_TEST=ON TEST_TIMEOUT=60
done
for i in 01 02; do
r $W/critic5c2-base $E/r3m-base-3b369aef-run$i.txt "base: R3 migrate x50" TEST_FILTER=test_migrate_ready_during_switch_out TEST_REPEAT=50 TEST_TIMEOUT=60
done
for i in 01 02; do
r $W/critic5c2-base $E/r3w-base-3b369aef-run$i.txt "base: R3 wake x50" TEST_FILTER=test_wake_during_switch_out TEST_REPEAT=50 TEST_TIMEOUT=60
done
r $W/critic5c2-fix  $E/r3-r2-default-fix-c0ea0714-run01.txt "fix head, default options: R3m, R3w, R2 x50" TEST_FILTER=test_migrate_ready_during_switch_out,test_wake_during_switch_out,test_preempt_flag_follows_migrated_task TEST_REPEAT=50 TEST_TIMEOUT=300
echo "PLAN1 DONE $(date -u +%T)" >> $E/summary.txt
