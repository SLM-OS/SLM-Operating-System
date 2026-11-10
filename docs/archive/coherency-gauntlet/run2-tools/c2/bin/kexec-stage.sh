#!/bin/bash
# Stage an ELF on jetson-nano-2 for kexec, per the deploy skill and run2 rules:
# /root/slmos-c2-<cell>.elf, sync, sha check. Never touches /root/slmos.elf.
# Usage: kexec-stage.sh <ip> <local-elf> <cell>
set -u
IP="$1"; ELF="$2"; CELL="$3"
REMOTE="/root/slmos-c2-$CELL.elf"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "root@$IP")
deadline=$((SECONDS + 240))
until "${SSH[@]}" true 2>/dev/null; do
    [ "$SECONDS" -ge "$deadline" ] && { echo "ssh not reachable"; exit 1; }
    sleep 3
done
want=$(sha256sum "$ELF" | cut -d' ' -f1)
scp -q -o BatchMode=yes "$ELF" "root@$IP:$REMOTE" || exit 1
got=$("${SSH[@]}" "sync; sha256sum $REMOTE" | cut -d' ' -f1)
echo "local  $want"
echo "remote $got  $REMOTE"
[ "$want" = "$got" ] || { echo "SHA MISMATCH"; exit 1; }
echo "STAGED OK"
