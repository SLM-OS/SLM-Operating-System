#!/usr/bin/env bash
# verify-x86-disk.sh — structural checks for the x86-64 UEFI disk image.
#
# Intended to run after `make x86-disk PLATFORM=X86_64`. Validates
# that the CMake `slmos-x86-disk` target produced a correctly-formed
# 128 MB GPT disk with a bootable 64 MB FAT32 ESP containing GRUB and
# the kernel ELF. Covers the regressions that #82 identified (FAT32
# BPB with TotSec32=0, missing boot files, wrong partition type) so
# CI can catch them without needing QEMU + OVMF.
#
# Usage:
#   scripts/tests/verify-x86-disk.sh [path-to-disk.img]
# Default path: build/kernel/slmos-x86.img

set -euo pipefail

DISK="${1:-build/kernel/slmos-x86.img}"
FAIL=0

pass() { printf '  [PASS] %s\n' "$1"; }
fail() { printf '  [FAIL] %s\n' "$1" >&2; FAIL=1; }

echo "Verifying x86-64 UEFI disk image: $DISK"

# --- existence + size -----------------------------------------------------
if [[ ! -f "$DISK" ]]; then
    fail "disk image not found at $DISK"
    exit 1
fi

size=$(stat -c '%s' "$DISK")
expected_size=$((128 * 1024 * 1024))
if [[ "$size" -eq "$expected_size" ]]; then
    pass "disk size = 128 MB ($size bytes)"
else
    fail "disk size = $size bytes, expected $expected_size"
fi

# --- GPT ------------------------------------------------------------------
if ! sgdisk -p "$DISK" > /tmp/verify-x86-disk.sgdisk.txt 2>&1; then
    fail "sgdisk could not read partition table"
    cat /tmp/verify-x86-disk.sgdisk.txt
    exit 1
fi
pass "GPT partition table readable"

if grep -q 'EF00 *EFI' /tmp/verify-x86-disk.sgdisk.txt; then
    pass "partition 1 is EFI System Partition (EF00)"
else
    fail "partition 1 is not EF00"
    grep -E 'Number|^[[:space:]]+[0-9]' /tmp/verify-x86-disk.sgdisk.txt >&2
fi

if grep -qE '^[[:space:]]+1[[:space:]]+2048[[:space:]]+18431' /tmp/verify-x86-disk.sgdisk.txt \
   || grep -qE '^[[:space:]]+1[[:space:]]+2048[[:space:]]+133119' /tmp/verify-x86-disk.sgdisk.txt; then
    pass "ESP spans expected sector range"
else
    fail "ESP sector range unexpected"
fi

# --- FAT32 BPB ------------------------------------------------------------
# Parse the BPB at byte offset 1 MB (sector 2048). FAT32 requires
# TotSec16 == 0 and TotSec32 > 0. Issue #82 surfaced the mformat bug
# where TotSec16 held the count instead, making some UEFI firmware
# reject the filesystem.
python3 - "$DISK" <<'PY' || FAIL=1
import struct, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    f.seek(1024 * 1024)
    bpb = f.read(512)

hidden = struct.unpack_from('<I', bpb, 28)[0]
tot16  = struct.unpack_from('<H', bpb, 19)[0]
tot32  = struct.unpack_from('<I', bpb, 32)[0]
sig_lo = bpb[510]
sig_hi = bpb[511]

failed = False
def ok(msg):
    print(f"  [PASS] {msg}")
def bad(msg):
    print(f"  [FAIL] {msg}", file=sys.stderr)

if sig_lo == 0x55 and sig_hi == 0xAA:
    ok("FAT BPB has 0x55AA signature")
else:
    bad(f"FAT BPB signature = 0x{sig_hi:02x}{sig_lo:02x}, expected 0xAA55")
    failed = True

if hidden == 2048:
    ok("FAT BPB hidden_sectors = 2048 (matches partition offset)")
else:
    bad(f"FAT BPB hidden_sectors = {hidden}, expected 2048")
    failed = True

if tot16 == 0 and tot32 > 0:
    ok(f"FAT32 BPB TotSec16=0, TotSec32={tot32} (well-formed FAT32)")
else:
    bad(f"FAT32 BPB TotSec16={tot16}, TotSec32={tot32} — malformed")
    failed = True

sys.exit(1 if failed else 0)
PY

# --- FAT contents ---------------------------------------------------------
if ! mdir -i "$DISK@@1M" ::/EFI/BOOT > /tmp/verify-x86-disk.mdir.txt 2>&1; then
    fail "mdir ::/EFI/BOOT failed"
    cat /tmp/verify-x86-disk.mdir.txt
else
    if grep -q 'BOOTX64  EFI' /tmp/verify-x86-disk.mdir.txt; then
        pass "/EFI/BOOT/BOOTX64.EFI present"
    else
        fail "/EFI/BOOT/BOOTX64.EFI missing"
        cat /tmp/verify-x86-disk.mdir.txt >&2
    fi
fi

if ! mdir -i "$DISK@@1M" ::/slmos > /tmp/verify-x86-disk.mdir.txt 2>&1; then
    fail "mdir ::/slmos failed"
    cat /tmp/verify-x86-disk.mdir.txt
else
    if grep -q 'kernel   elf' /tmp/verify-x86-disk.mdir.txt; then
        pass "/slmos/kernel.elf present"
    else
        fail "/slmos/kernel.elf missing"
        cat /tmp/verify-x86-disk.mdir.txt >&2
    fi
fi

if [[ "$FAIL" -ne 0 ]]; then
    echo "DISK VERIFICATION FAILED" >&2
    exit 1
fi
echo "DISK VERIFICATION PASSED"
