#!/usr/bin/env bash
#
# qemu-uefi-boot.sh — boot an ARM64 SLM-OS PE image under QEMU's
# AArch64 UEFI firmware (EDK2 ArmVirtQemu / AAVMF) instead of the
# usual `-kernel` ELF load.
#
# Exercises the UEFI-direct path end to end on the host: PE/COFF load
# at a firmware-chosen address, efi_stub_entry (memory-map check,
# ExitBootServices, MMU off), the boot.S copy to the link address, and
# kernel_main. The same code runs on Jetson UEFI-direct boot (#26).
#
# Usage: scripts/qemu-uefi-boot.sh [slmos.bin] [timeout-seconds] [expect-regex]
#   slmos.bin      PE image (default: build/kernel/slmos.bin, QEMU_VIRT build)
#   timeout        wall-clock cap in seconds (default 120)
#   expect-regex   success pattern searched in the console log
#                  (default: the shell prompt "slmos>")
#
# Environment:
#   FORCE_IMAGE_BASE=0x...  rewrite the PE ImageBase in a copy of the
#                           image so the firmware cannot honour it and
#                           loads the image elsewhere — exercises the
#                           copy-to-link-address path the way Jetson
#                           firmware does (it never grants 0x80000000).
#                           A non-RAM value such as 0x1000 works.
#   QEMU_ACPI=on|off        firmware hardware description (default off:
#                           the firmware then publishes the DTB in the
#                           EFI configuration table, as Jetson's does).
#   QEMU_MEM=512M           guest RAM (default 1G).
#   MODEL_FILES="a b"       files to put in \\slmos\\models on the ESP.
#   ESP_HOOK=path           run with the ESP image as $1 once it's built.
#   BLKDEBUG_CONF=path      blkdebug rules for the disk, for injected I/O
#                           errors. The hook can write this file.
#   SHELL_CMDS="a|b"        shell commands, |-separated, typed one by one
#                           after the first prompt.
#
# Exit status: 0 if the pattern appeared, 1 otherwise. The console log
# is left in $OUT_DIR (default: build/qemu-uefi) for inspection.
#
# QEMU runs under the same systemd-run memory/CPU guard `make test`
# uses (see CLAUDE.md "QEMU safeguards").

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$ROOT/build/kernel/slmos.bin}"
TIMEOUT="${2:-120}"
EXPECT="${3:-slmos>}"
OUT_DIR="${OUT_DIR:-$ROOT/build/qemu-uefi}"

FW_CODE="${FW_CODE:-/usr/share/AAVMF/AAVMF_CODE.fd}"
FW_VARS_TEMPLATE="${FW_VARS_TEMPLATE:-/usr/share/AAVMF/AAVMF_VARS.fd}"

for f in "$IMG" "$FW_CODE" "$FW_VARS_TEMPLATE"; do
    if [ ! -f "$f" ]; then
        echo "qemu-uefi-boot: missing $f" >&2
        [ "$f" = "$IMG" ] || echo "  (install the qemu-efi-aarch64 package)" >&2
        exit 2
    fi
done
for tool in mformat mmd mcopy qemu-system-aarch64; do
    command -v "$tool" >/dev/null || { echo "qemu-uefi-boot: $tool not found" >&2; exit 2; }
done

mkdir -p "$OUT_DIR"
ESP="$OUT_DIR/esp.img"
VARS="$OUT_DIR/vars.fd"
LOG="$OUT_DIR/console.log"

# FAT32 ESP with the image at the removable-media default path, so the
# firmware boots it without any NVRAM boot entry.
rm -f "$ESP"
mformat -C -i "$ESP" -T 131072 -h 64 -s 32 -F ::
mmd -i "$ESP" ::/EFI ::/EFI/BOOT
BOOT_IMG="$IMG"
if [ -n "${FORCE_IMAGE_BASE:-}" ]; then
    BOOT_IMG="$OUT_DIR/slmos-rebased.efi"
    cp "$IMG" "$BOOT_IMG"
    # PE32+ ImageBase: PE header offset at 0x3C, then 4-byte signature,
    # 20-byte COFF header, ImageBase at optional-header offset 24.
    python3 -I - "$BOOT_IMG" "$FORCE_IMAGE_BASE" <<'PY'
import struct, sys
path, base = sys.argv[1], int(sys.argv[2], 0)
with open(path, "r+b") as f:
    f.seek(0x3C)
    pe = struct.unpack("<I", f.read(4))[0]
    f.seek(pe)
    assert f.read(4) == b"PE\0\0", "no PE signature"
    f.seek(pe + 4 + 20)
    assert struct.unpack("<H", f.read(2))[0] == 0x20B, "not PE32+"
    f.seek(pe + 4 + 20 + 24)
    f.write(struct.pack("<Q", base))
PY
fi
mcopy -i "$ESP" "$BOOT_IMG" ::/EFI/BOOT/BOOTAA64.EFI
if [ -n "${MODEL_FILES:-}" ]; then
    mmd -i "$ESP" ::/slmos ::/slmos/models
    for f in $MODEL_FILES; do
        mcopy -i "$ESP" "$f" "::/slmos/models/$(basename "$f")"
    done
fi
if [ -n "${ESP_HOOK:-}" ]; then
    "$ESP_HOOK" "$ESP"
fi
cp "$FW_VARS_TEMPLATE" "$VARS"

if [ -n "${BLKDEBUG_CONF:-}" ]; then
    DISK=(-drive "if=virtio,format=raw,file.driver=blkdebug,file.config=$BLKDEBUG_CONF,file.image.driver=file,file.image.filename=$ESP")
else
    DISK=(-drive "if=virtio,format=raw,file=$ESP")
fi

# With shell commands, QEMU's stdin stays open on a FIFO. The writer runs
# here so the console can be watched for the prompt first.
: >"$LOG"
FIFO="$OUT_DIR/stdin.fifo"
rm -f "$FIFO"
mkfifo "$FIFO"
exec 9<>"$FIFO"

set +e
systemd-run --user --scope -q -p MemoryMax=3G -p CPUQuota=200% \
    timeout "$TIMEOUT" \
    qemu-system-aarch64 -machine "virt,acpi=${QEMU_ACPI:-off}" -cpu cortex-a76 -smp cores=4 -m "${QEMU_MEM:-1G}" \
        -nographic -accel tcg,tb-size=128 -no-reboot \
        -drive if=pflash,format=raw,readonly=on,file="$FW_CODE" \
        -drive if=pflash,format=raw,file="$VARS" \
        "${DISK[@]}" \
    >"$LOG" 2>&1 <&9 &
QEMU_PID=$!
set -e

prompts_seen() { grep -a -c "slmos>" "$LOG" || true; }

# Wait up to `timeout` for at least $1 prompts, or for QEMU to exit.
wait_prompts() {
    local want="$1"
    while kill -0 "$QEMU_PID" 2>/dev/null; do
        if [ "$(prompts_seen)" -ge "$want" ]; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

if wait_prompts 1 && [ -n "${SHELL_CMDS:-}" ]; then
    IFS='|' read -r -a CMDS <<<"$SHELL_CMDS"
    for cmd in "${CMDS[@]}"; do
        before=$(prompts_seen)
        printf '%s\r' "$cmd" >&9
        wait_prompts $((before + 1)) || break
    done
fi
sleep 0.2
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
exec 9>&-
rm -f "$FIFO"

if grep -a -q -E "$EXPECT" "$LOG"; then
    echo "qemu-uefi-boot: PASS ('$EXPECT' seen) — log: $LOG"
    exit 0
fi
echo "qemu-uefi-boot: FAIL ('$EXPECT' not seen) — log: $LOG" >&2
tail -c 3000 "$LOG" >&2
exit 1
