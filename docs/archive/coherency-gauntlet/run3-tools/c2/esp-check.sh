#!/bin/bash
# Read-only ESP and boot-entry state check on jetson-nano-2's SD Linux
# (#1187), unchanged from run 2's c2/bin/esp-check.sh except the header.
# Usage: esp-check.sh <ip> [fetch-dest]
#   fetch-dest: if given, copy \EFI\SLMOS\SLMOS.efi back to this host path.
set -u
IP="$1"; DEST="${2:-}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "root@$IP")
deadline=$((SECONDS + 240))
until "${SSH[@]}" true 2>/dev/null; do
    [ "$SECONDS" -ge "$deadline" ] && { echo "ssh not reachable"; exit 1; }
    sleep 3
done
date -u +"check at %Y-%m-%dT%H:%M:%SZ"
"${SSH[@]}" 'bash -s' <<'REMOTE'
set -u
dev=/dev/disk/by-partlabel/SLMOS-ESP
efibootmgr | grep -E '^Boot(Current|Next|Order)' || true
grep -q "$(readlink -f $dev)" /proc/mounts && echo "ESP MOUNTED (unexpected)"
echo "--- fsck.vfat -n"
fsck.vfat -n "$dev"; echo "fsck rc=$?"
mnt=$(mktemp -d)
mount -o ro "$dev" "$mnt" || { echo "ro mount failed"; exit 1; }
sha256sum "$mnt/EFI/SLMOS/SLMOS.efi" "$mnt/EFI/L4T/l4t.efi" 2>&1
cp "$mnt/EFI/SLMOS/SLMOS.efi" /tmp/slmos-esp-copy.efi
umount "$mnt"; rmdir "$mnt"
grep -q "$(readlink -f $dev)" /proc/mounts && echo "ESP STILL MOUNTED" || echo "ESP unmounted"
REMOTE
if [ -n "$DEST" ]; then
    scp -q -o BatchMode=yes "root@$IP:/tmp/slmos-esp-copy.efi" "$DEST" && sha256sum "$DEST"
fi
"${SSH[@]}" 'rm -f /tmp/slmos-esp-copy.efi'
