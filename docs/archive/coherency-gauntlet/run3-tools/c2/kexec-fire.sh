#!/bin/bash
# Fire a detached slmos-kexec of /root/slmos-r3-<tag>.elf after a delay,
# so the serial capture (labctl serial_capture) is already running when
# Linux drops. Checks the staged file's sha256 first when one is given.
# Usage: kexec-fire.sh <ip> <tag> <delay-seconds> [expected-sha256]
set -u
IP="$1"; TAG="$2"; DELAY="$3"; EXPECT="${4:-}"
REMOTE="/root/slmos-r3-$TAG.elf"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "root@$IP")
deadline=$((SECONDS + 240))
until "${SSH[@]}" true 2>/dev/null; do
    [ "$SECONDS" -ge "$deadline" ] && { echo "ssh not reachable"; exit 1; }
    sleep 3
done
if [ -n "$EXPECT" ]; then
    got=$("${SSH[@]}" "sha256sum $REMOTE" | cut -d' ' -f1)
    [ "$got" = "$EXPECT" ] || { echo "REMOTE SHA $got != $EXPECT; not firing"; exit 1; }
    echo "remote sha ok ${got:0:16}"
fi
sleep "$DELAY"
date -u +"kexec trigger at %Y-%m-%dT%H:%M:%SZ"
timeout 60 ssh -o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=2 \
    -o ServerAliveCountMax=2 "root@$IP" \
    "setsid nohup slmos-kexec $REMOTE > /tmp/slmos-kexec.log 2>&1 < /dev/null &"
echo "ssh rc=$?"
