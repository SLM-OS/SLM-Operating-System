#!/bin/bash
# C0 item 4: /ci steps plus 3x make test WORK_STEALING=ON on main. One step at a time.
W=$1; O=$2; cd "$W" || exit 1
export PATH="$HOME/.cargo/bin:$PATH"
step() { n=$1; shift; s=$(date -u +%H:%M:%S); { echo "head $(git rev-parse HEAD) step $n start $s load $(cut -d" " -f1-3 /proc/loadavg)"; bash -c "$*"; } > "$O/$n.log" 2>&1; rc=$?; echo "$n rc=$rc start=$s end=$(date -u +%H:%M:%S) load=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$O/summary.txt"; }
echo "head $(git rev-parse HEAD)" > "$O/summary.txt"
step 01-arm64-build   "make kernel-clean && make kernel PLATFORM=QEMU_VIRT"
step 02-x86-build     "make kernel-clean && make kernel PLATFORM=X86_64"
step 03-jetson-real   "make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO AI_WEIGHTS=real && make kernel-test-clean && make kernel-test PLATFORM=JETSON_ORIN_NANO AI_WEIGHTS=real"
step 04-test          "make test"
step 04b-test-x86     "make test PLATFORM=X86_64"
step 05-test-aisched  "make test AI_SCHED=ON"
step 06-runtime-test  "make runtime-test"
step 07-braces        "make test-check-braces && make check-braces"
step 08-build-order   "make test-build-order"
step 09-jetson-layout "make test-jetson-uefi-layout"
for i in $(seq -w 1 20); do step 10-ws-on-$i "make kernel-test-clean && make test WORK_STEALING=ON"; done
git diff origin/main...HEAD --stat > "$O/11-diff.log" 2>&1; git status --porcelain >> "$O/11-diff.log"
echo DONE >> "$O/summary.txt"
