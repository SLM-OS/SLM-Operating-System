#!/bin/bash
# Stage an ELF on jetson-nano-2 for kexec (coherency run 3, C2), per the
# deploy skill and run3 rules: /root/slmos-r3-<tag>.elf, sync, sha check.
# Never touches /root/slmos.elf.
# Usage: kexec-stage.sh <ip> <local-elf> <tag> [expected-sha256]
set -u
IP="$1"; ELF="$2"; TAG="$3"; EXPECT="${4:-}"
REMOTE="/root/slmos-r3-$TAG.elf"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "root@$IP")
deadline=$((SECONDS + 240))
until "${SSH[@]}" true 2>/dev/null; do
    [ "$SECONDS" -ge "$deadline" ] && { echo "ssh not reachable"; exit 1; }
    sleep 3
done
want=$(sha256sum "$ELF" | cut -d' ' -f1)
if [ -n "$EXPECT" ] && [ "$want" != "$EXPECT" ]; then
    echo "LOCAL SHA $want != MANIFEST $EXPECT"; exit 1
fi
scp -q -o BatchMode=yes "$ELF" "root@$IP:$REMOTE" || exit 1
got=$("${SSH[@]}" "sync; sha256sum $REMOTE" | cut -d' ' -f1)
date -u +"staged at %Y-%m-%dT%H:%M:%SZ"
echo "local  $want"
echo "remote $got  $REMOTE"
[ "$want" = "$got" ] || { echo "SHA MISMATCH"; exit 1; }
echo "STAGED OK"
