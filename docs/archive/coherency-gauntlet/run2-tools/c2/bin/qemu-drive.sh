#!/bin/bash
# Drive `make run` (QEMU, WORK_STEALING=ON) with bench stealing commands.
# Usage: qemu-drive.sh <worktree> <logfile>
set -u
WT="$1"
LOG="$2"
export PATH=/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH
feed() {
    sleep 40
    printf 'bench stealing\r'
    sleep 15
    printf 'bench stealing --sustain 2000\r'
    sleep 60
    printf 'help bench\r'
    sleep 5
}
feed | timeout 150 make -C "$WT" run WORK_STEALING=ON > "$LOG" 2>&1
echo "EXIT $?"
