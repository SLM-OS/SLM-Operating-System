#!/bin/bash
# usage: runset.sh <head-short> <first> <last>
S=$(dirname "$(readlink -f "$0")")
W=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/l4-c2-critic
H=$1
cd $W || exit 1
[ "$(git rev-parse --short=8 HEAD)" = "$H" ] || { echo "head mismatch"; exit 1; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo dirty; exit 1; }
make kernel-test-clean > $S/build-$H.log 2>&1
make kernel-test WORK_STEALING=ON >> $S/build-$H.log 2>&1 || { echo BUILD FAIL; exit 1; }
for i in $(seq $2 $3); do
  n=$(printf %02d $i)
  f=$S/ws-on-$H-run$n.txt
  l0=$(cut -d' ' -f1-3 /proc/loadavg); nq=$(pgrep -c qemu-system)
  t0=$(date -u +%H:%M:%S); s0=$(date +%s)
  make test WORK_STEALING=ON > $S/tmp-make.out 2>&1; rc=$?
  s1=$(date +%s); l1=$(cut -d' ' -f1-3 /proc/loadavg)
  sha=$(sha256sum build/kernel-test/slmos.elf | cut -c1-16)
  out=build/test-output.log
  [ -f $out ] || out=$(grep -l . build/*/test-output.log | head -1)
  {
    echo "# head=$H elf_sha256=$sha start=$t0 dur=$((s1-s0))s load_before=[$l0] load_after=[$l1] other_qemu_at_start=$nq make_rc=$rc RESULT=PENDING"
    echo "==== make output ===="; cat $S/tmp-make.out
    echo "==== test-output.log ($out) ===="; cat $out
  } > $f
done
echo done
