#!/bin/bash
# Fire a detached slmos-kexec after a delay, so a serial capture can start first.
# Usage: kexec-fire.sh <ip> <cell> <delay-seconds>
set -u
IP="$1"; CELL="$2"; DELAY="$3"
sleep "$DELAY"
date -u +"kexec trigger at %Y-%m-%dT%H:%M:%SZ"
timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=2 \
    -o ServerAliveCountMax=2 "root@$IP" \
    "setsid nohup slmos-kexec /root/slmos-c2-$CELL.elf > /tmp/slmos-kexec.log 2>&1 < /dev/null &"
echo "ssh rc=$?"
