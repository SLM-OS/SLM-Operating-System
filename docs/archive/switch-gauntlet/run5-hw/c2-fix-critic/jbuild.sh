#!/bin/bash
# Build Jetson test kernels for one head. Usage: jbuild.sh <worktree> <tag> <name=filter>...
set -u
wt=$1; tag=$2; shift 2
E=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/critic5c2-evidence
export PATH="$HOME/.cargo/bin:$PATH"
for nf in "$@"; do
  name=${nf%%=*}; filt=${nf#*=}
  make -C "$wt" kernel-test-clean > $E/jetson/build-$tag-$name.log 2>&1
  make -C "$wt" kernel-test PLATFORM=JETSON_ORIN_NANO TEST_FILTER=$filt TEST_REPEAT=200 >> $E/jetson/build-$tag-$name.log 2>&1; rc=$?
  if [ $rc -eq 0 ]; then cp "$wt/build/kernel-test/slmos.elf" $E/jetson/slmos-s5cc2-$name-$tag.elf; fi
  echo "jbuild $tag $name rc=$rc $(date -u +%T) head=$(git -C $wt rev-parse --short=8 HEAD) sha256=$(sha256sum $E/jetson/slmos-s5cc2-$name-$tag.elf 2>/dev/null | cut -c1-64)" >> $E/summary.txt
done
