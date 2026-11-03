#!/usr/bin/env bash
#
# jetson-uefi-deploy.sh — stage an SLM-OS PE image for UEFI-direct boot
# on a Jetson Orin Nano and boot it once (#26).
#
# Board layout this expects (set up 2026-11-02 on jetson-nano-2, see
# docs/deploy/jetson-uefi-direct.md):
#   - Linux runs from the SD card and is the default cold-boot path.
#   - NVMe partition 2 is a FAT32 ESP labelled SLMOS-ESP.
#   - A UEFI boot entry labelled "SLM-OS NVMe" points at
#     \EFI\SLMOS\SLMOS.efi on that ESP.
#
# Steps (all over SSH to the board's running Linux):
#   1. copy the image to \EFI\SLMOS\SLMOS.efi on the NVMe ESP, sync
#   2. efibootmgr --bootnext <SLM-OS NVMe entry>   (one-shot)
#   3. reboot
#
# BootNext is consumed by the firmware on the next boot, so a power
# cycle (labctl power_cycle) always returns the board to SD Linux.
# Capture the console with labctl serial_capture after this returns.
#
# Usage: scripts/jetson-uefi-deploy.sh <board-ip> [slmos.bin]

set -euo pipefail

BOARD="${1:?usage: $0 <board-ip> [slmos.bin]}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${2:-$ROOT/build/kernel/slmos.bin}"
ESP_DEV="${ESP_DEV:-/dev/disk/by-partlabel/SLMOS-ESP}"
ENTRY_LABEL="${ENTRY_LABEL:-SLM-OS NVMe}"
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=5 "root@$BOARD")

[ -f "$IMG" ] || { echo "jetson-uefi-deploy: $IMG not found" >&2; exit 2; }

# Refuse anything that is not a PE32+ image: the firmware would just
# skip it and boot Linux, which looks like a silent hang of SLM-OS.
python3 -I - "$IMG" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    hdr = f.read(0x40)
    assert hdr[:2] == b"MZ", "no MZ signature"
    pe = struct.unpack("<I", hdr[0x3C:0x40])[0]
    f.seek(pe)
    assert f.read(4) == b"PE\0\0", "no PE signature"
PY

# The board may still be coming back from a power cycle: wait for its
# SD Linux to answer SSH (bounded by WAIT_SECS, default 240).
deadline=$((SECONDS + ${WAIT_SECS:-240}))
until "${SSH[@]}" true 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "jetson-uefi-deploy: $BOARD not reachable over SSH" >&2
        exit 1
    fi
    sleep 3
done

sum=$(sha256sum "$IMG" | cut -d' ' -f1)
scp -q -o BatchMode=yes "$IMG" "root@$BOARD:/tmp/slmos-uefi.efi"

# ssh joins its arguments into one remote command line, so quote each
# one (the entry label contains a space).
"${SSH[@]}" "bash -s -- $(printf '%q ' "$ESP_DEV" "$ENTRY_LABEL" "$sum")" <<'REMOTE'
set -euo pipefail
esp_dev="$1"; label="$2"; want="$3"
mnt=$(mktemp -d)
mount "$esp_dev" "$mnt"
trap 'umount "$mnt" 2>/dev/null; rmdir "$mnt"' EXIT
mkdir -p "$mnt/EFI/SLMOS"
cp /tmp/slmos-uefi.efi "$mnt/EFI/SLMOS/SLMOS.efi"
sync
got=$(sha256sum "$mnt/EFI/SLMOS/SLMOS.efi" | cut -d' ' -f1)
[ "$got" = "$want" ] || { echo "checksum mismatch on ESP" >&2; exit 1; }
umount "$mnt"; rmdir "$mnt"; trap - EXIT
entry=$(efibootmgr | awk -v l="$label" '
    { name = $0; sub(/^Boot[0-9A-Fa-f]+\*? /, "", name); sub(/\t.*/, "", name) }
    name == l { print substr($1, 5, 4); exit }')
[ -n "$entry" ] || { echo "no UEFI boot entry labelled '$label'" >&2; exit 1; }
efibootmgr --bootnext "$entry" >/dev/null
echo "staged sha256=${want:0:12} on $esp_dev; BootNext=$entry; rebooting"
sync
(sleep 1; systemctl reboot) >/dev/null 2>&1 &
REMOTE
