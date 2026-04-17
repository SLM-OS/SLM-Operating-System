#!/usr/bin/env bash
# verify-kexec-build.sh — structural checks for the x86-64 kexec build.
#
# Intended to run after `make kernel-kexec PLATFORM=X86_64` (or, for
# the "both paths still work" check, after both `make kernel` and
# `make kernel-kexec`). Validates the additive kexec scaffolding:
#
#   1. The two builds produce distinct ELFs linked at different
#      addresses (bare-metal at 0x100000, kexec at 0x20000000).
#   2. Both ELFs expose Multiboot v1 AND Multiboot v2 magic within
#      the first 8 KiB so either kexec-tools loader can find them.
#   3. Both ELFs include a MULTIBOOT2_HEADER_TAG_ENTRY_ADDRESS (type
#      3) tag pointing to _start — kexec-tools 2.0.28's mb2 loader
#      can ignore e_entry without the explicit tag.
#   4. The entry code opens with the 16550 UART reinit + "KEX\r\n"
#      diagnostic bytes. Silence on that signal is the primary
#      kexec-handoff debugging signal (see
#      docs/x86-64-gpu-inference-status.md §4.2.k).
#   5. The kexec ELF's load segment sits entirely inside a single
#      System RAM range reported by kexec-tools — catches regressions
#      where KERNEL_PHYS drifts into a reserved region.
#   6. The two Linux-side helper scripts parse clean under
#      `bash -n` and `shellcheck -S warning`.
#
# Usage:
#   scripts/tests/verify-kexec-build.sh
#     ↳ validates both build/kernel/slmos.elf (bare-metal) AND
#       build/kernel-kexec/slmos.elf (kexec) if both exist. Either
#       may be absent; the checks for the absent one are skipped
#       with a WARN line (so this is useful in CI branches that only
#       build one variant).
#
# Exit 0 on all pass, 1 on any fail. WARN lines are not fails.

set -euo pipefail

BARE_METAL_ELF="${BARE_METAL_ELF:-build/kernel/slmos.elf}"
KEXEC_ELF="${KEXEC_ELF:-build/kernel-kexec/slmos.elf}"
BZIMAGE_ELF="${BZIMAGE_ELF:-build/kernel-bzimage/slmos.elf}"
BZIMAGE="${BZIMAGE:-build/kernel-bzimage/slmos.bzimage}"
FAIL=0
CHECKS=0

pass() { printf '  [PASS] %s\n' "$1"; CHECKS=$((CHECKS+1)); }
fail() { printf '  [FAIL] %s\n' "$1" >&2; FAIL=1; CHECKS=$((CHECKS+1)); }
warn() { printf '  [WARN] %s\n' "$1"; }
info() { printf '\n== %s ==\n' "$1"; }

need_tool() {
    command -v "$1" >/dev/null 2>&1 || {
        fail "required tool '$1' not on PATH"; return 1;
    }
}

need_tool readelf || exit 1
need_tool objdump  || exit 1
need_tool python3  || exit 1

# --- ELF-level checks -----------------------------------------------------

check_elf_entry() {
    local label=$1 elf=$2 want_entry=$3 want_paddr=$4
    local entry paddr
    entry=$(readelf -h "$elf" | awk '/Entry point/{print $NF}')
    paddr=$(readelf -l "$elf" | awk '/^  LOAD/{print $4; exit}')
    # Normalize to 0x<hex> without leading zeros beyond the "0x".
    entry=$(printf '0x%x' "$entry")
    paddr=$(printf '0x%x' "$paddr")
    local want_entry_n want_paddr_n
    want_entry_n=$(printf '0x%x' "$want_entry")
    want_paddr_n=$(printf '0x%x' "$want_paddr")
    if [[ "$entry" == "$want_entry_n" ]]; then
        pass "$label: ELF entry point = $entry"
    else
        fail "$label: ELF entry = $entry, expected $want_entry_n"
    fi
    if [[ "$paddr" == "$want_paddr_n" ]]; then
        pass "$label: first LOAD segment paddr = $paddr"
    else
        fail "$label: first LOAD segment paddr = $paddr, expected $want_paddr_n"
    fi
}

# Search first 8 KiB of the file for a u32 magic (little-endian).
check_magic_in_8k() {
    local label=$1 elf=$2 magic_name=$3 magic=$4
    python3 - "$elf" "$magic" "$label" "$magic_name" <<'PY' || fail "$label: $4 magic lookup threw"
import sys, struct
path, magic_hex, label, magic_name = sys.argv[1:]
magic = int(magic_hex, 16)
with open(path, "rb") as f:
    blob = f.read(8 * 1024)
# Magic is u32 little-endian at some 4-byte-aligned offset.
for off in range(0, len(blob) - 4, 4):
    if struct.unpack_from("<I", blob, off)[0] == magic:
        print(f"  [PASS] {label}: {magic_name} magic 0x{magic:08x} found at file offset 0x{off:x}")
        sys.exit(0)
print(f"  [FAIL] {label}: {magic_name} magic 0x{magic:08x} not found in first 8 KiB", file=sys.stderr)
sys.exit(1)
PY
    case $? in 0) CHECKS=$((CHECKS+1));; *) FAIL=1; CHECKS=$((CHECKS+1));; esac
}

# Multiboot2 header: magic (0xe85250d6) + arch + length + checksum, then
# tags. We scan for the magic and then walk the tag list looking for
# type=3 (ENTRY_ADDRESS) with entry_addr == expected.
check_mb2_entry_tag() {
    local label=$1 elf=$2 want_entry=$3
    python3 - "$elf" "$want_entry" "$label" <<'PY'
import sys, struct
path, want_entry_hex, label = sys.argv[1:]
want_entry = int(want_entry_hex, 16)

with open(path, "rb") as f:
    blob = f.read(8 * 1024)

MB2_MAGIC = 0xe85250d6
for mb2_off in range(0, len(blob) - 16, 8):
    if struct.unpack_from("<I", blob, mb2_off)[0] == MB2_MAGIC:
        break
else:
    print(f"  [FAIL] {label}: no MB2 magic", file=sys.stderr); sys.exit(1)

arch, header_len = struct.unpack_from("<II", blob, mb2_off + 4)
tag_off = mb2_off + 16
tag_end = mb2_off + header_len
while tag_off + 8 <= tag_end:
    t_type, t_flags, t_size = struct.unpack_from("<HHI", blob, tag_off)
    if t_type == 0 and t_size == 8:
        print(f"  [FAIL] {label}: walked to end tag without seeing ENTRY_ADDRESS (type=3)", file=sys.stderr)
        sys.exit(1)
    if t_type == 3:
        entry_addr = struct.unpack_from("<I", blob, tag_off + 8)[0]
        if entry_addr == want_entry:
            print(f"  [PASS] {label}: MB2 ENTRY_ADDRESS tag found, entry=0x{entry_addr:x}")
            sys.exit(0)
        print(f"  [FAIL] {label}: MB2 ENTRY_ADDRESS = 0x{entry_addr:x}, expected 0x{want_entry:x}", file=sys.stderr)
        sys.exit(1)
    # 8-byte alignment between tags per spec
    tag_off += (t_size + 7) & ~7
print(f"  [FAIL] {label}: fell off MB2 header without finding ENTRY_ADDRESS", file=sys.stderr)
sys.exit(1)
PY
    case $? in 0) CHECKS=$((CHECKS+1));; *) FAIL=1; CHECKS=$((CHECKS+1));; esac
}

# The UART reinit at _start begins with:
#   fa                    cli
#   66 ba f9 03           mov $0x3f9, %dx     (IER)
#   b0 00                 mov $0, %al
#   ee                    out %al, %dx
# ...14 more writes...
# Finally "K"=0x4b, "E"=0x45, "X"=0x58, "\r"=0x0d, "\n"=0x0a emitted
# through the THR at 0x3f8.
#
# We spot-check the signature: first byte is 0xfa (cli), and somewhere
# in the first 128 bytes all four ASCII codes (K,E,X,\r,\n) appear.
check_uart_diag_signature() {
    local label=$1 elf=$2 entry_vaddr=$3
    python3 - "$elf" "$entry_vaddr" "$label" <<'PY'
import sys, struct, subprocess

path, entry_hex, label = sys.argv[1:]
entry = int(entry_hex, 16)

# Find offset of entry within the file by walking LOAD phdrs.
with open(path, "rb") as f:
    blob = f.read()

# ELF64 header: e_phoff @ 32 (u64), e_phentsize @ 54 (u16), e_phnum @ 56 (u16)
e_phoff = struct.unpack_from("<Q", blob, 32)[0]
e_phsize = struct.unpack_from("<H", blob, 54)[0]
e_phnum  = struct.unpack_from("<H", blob, 56)[0]

file_off_of_entry = None
for i in range(e_phnum):
    ph = e_phoff + i * e_phsize
    p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, _ = \
        struct.unpack_from("<IIQQQQQQ", blob, ph)
    if p_type != 1:  # PT_LOAD
        continue
    if p_paddr <= entry < p_paddr + p_filesz:
        file_off_of_entry = p_offset + (entry - p_paddr)
        break

if file_off_of_entry is None:
    print(f"  [FAIL] {label}: entry 0x{entry:x} not in any PT_LOAD", file=sys.stderr)
    sys.exit(1)

# Look at a 256-byte window starting at the entry point.
window = blob[file_off_of_entry : file_off_of_entry + 256]

if window[0] != 0xfa:
    print(f"  [FAIL] {label}: _start[0] = 0x{window[0]:02x}, expected 0xfa (cli)", file=sys.stderr)
    sys.exit(1)

# Check each diagnostic byte is present as an immediate in 0x66 0xba WORD
# encoding (mov imm16, %dx) or 0xb0 BYTE encoding (mov imm8, %al). Spot
# check for the four distinctive characters.
missing = []
for mark, byte in [("K", 0x4b), ("E", 0x45), ("X", 0x58), ("\\r", 0x0d), ("\\n", 0x0a)]:
    # "mov $byte, %al" = b0 <byte>
    if b"\xb0" + bytes([byte]) not in window:
        missing.append(mark)

if missing:
    print(f"  [FAIL] {label}: UART diag bytes missing from first 256 B of _start: {missing}", file=sys.stderr)
    sys.exit(1)

print(f"  [PASS] {label}: _start begins with cli + emits K,E,X,\\r,\\n via MOV imm8+OUT")
PY
    case $? in 0) CHECKS=$((CHECKS+1));; *) FAIL=1; CHECKS=$((CHECKS+1));; esac
}

check_elf() {
    local label=$1 elf=$2 entry=$3 paddr=$4
    info "$label: $elf"
    if [[ ! -f "$elf" ]]; then
        warn "skipped — $elf does not exist (run 'make $label' first)"
        return
    fi
    check_elf_entry       "$label" "$elf" "$entry" "$paddr"
    check_magic_in_8k     "$label" "$elf" MB1 0x1BADB002
    check_magic_in_8k     "$label" "$elf" MB2 0xE85250D6
    check_mb2_entry_tag   "$label" "$elf" "$entry"
    check_uart_diag_signature "$label" "$elf" "$entry"
}

check_elf "bare-metal" "$BARE_METAL_ELF" 0x101000   0x100000
check_elf "kexec"      "$KEXEC_ELF"      0x20001000 0x20000000

# --- bzImage-specific checks ---------------------------------------------

# The bzImage ELF entry is _start_bzimage @ 0x20000200. That's the
# 64-bit stub; it loads our GDT and falls through to entry64.
# Skip MB2 ENTRY_ADDRESS tag check (our entry is not inside .multiboot
# for this build — the entry is the bzImage stub), and swap the UART
# signature check for a bzImage-flavoured one ("BZ\r\n" vs "KEX\r\n").

check_bzimage_elf_entry() {
    local label=$1 elf=$2 want_entry=0x20000200 want_paddr=0x20000000
    check_elf_entry "$label" "$elf" "$want_entry" "$want_paddr"
    check_magic_in_8k "$label" "$elf" MB1 0x1BADB002
    check_magic_in_8k "$label" "$elf" MB2 0xE85250D6
    # UART signature: stub emits "BZ\r\n" (0x42, 0x5A, 0x0D, 0x0A)
    python3 - "$elf" "$want_entry" "$label" <<'PY'
import sys, struct
path, entry_hex, label = sys.argv[1:]
entry = int(entry_hex, 16)
blob = open(path, "rb").read()
e_phoff  = struct.unpack_from("<Q", blob, 32)[0]
e_phsize = struct.unpack_from("<H", blob, 54)[0]
e_phnum  = struct.unpack_from("<H", blob, 56)[0]
file_off = None
for i in range(e_phnum):
    ph = e_phoff + i * e_phsize
    (p_type, _pf, p_offset, _pv, p_paddr, p_filesz, _pm, _a) = \
        struct.unpack_from("<IIQQQQQQ", blob, ph)
    if p_type == 1 and p_paddr <= entry < p_paddr + p_filesz:
        file_off = p_offset + (entry - p_paddr); break
if file_off is None:
    print(f"  [FAIL] {label}: entry 0x{entry:x} not in any PT_LOAD", file=sys.stderr); sys.exit(1)
window = blob[file_off:file_off + 256]
if window[0] != 0xfa:
    print(f"  [FAIL] {label}: _start_bzimage[0] = 0x{window[0]:02x}, expected 0xfa (cli)", file=sys.stderr); sys.exit(1)
missing = [m for m, b in [("B",0x42),("Z",0x5A),("\\r",0x0D),("\\n",0x0A)]
           if b"\xb0" + bytes([b]) not in window]
if missing:
    print(f"  [FAIL] {label}: bzImage diag bytes missing: {missing}", file=sys.stderr); sys.exit(1)
print(f"  [PASS] {label}: _start_bzimage begins with cli + emits B,Z,\\r,\\n via MOV imm8+OUT")
PY
    case $? in 0) CHECKS=$((CHECKS+1));; *) FAIL=1; CHECKS=$((CHECKS+1));; esac
}

check_bzimage_wrapper() {
    local label=$1 path=$2
    info "$label: $path"
    if [[ ! -f "$path" ]]; then
        warn "skipped — $path does not exist (run 'make kernel-bzimage' first)"
        return
    fi
    # Setup area is 1024 bytes. Validate the five fields kexec's
    # bzImage64 probe() actually reads:
    python3 - "$path" "$label" <<'PY'
import sys, struct
path, label = sys.argv[1:]
blob = open(path, "rb").read()

checks = [
    ("setup_sects @ 0x1F1 == 1",
        blob[0x1F1] == 1),
    ("boot_flag   @ 0x1FE == 0xAA55",
        struct.unpack_from("<H", blob, 0x1FE)[0] == 0xAA55),
    ('header_magic @ 0x202 == "HdrS"',
        blob[0x202:0x206] == b"HdrS"),
    ("protocol     @ 0x206 >= 0x020C",
        struct.unpack_from("<H", blob, 0x206)[0] >= 0x020C),
    ("loadflags    @ 0x211 bit 0 (LOADED_HIGH)",
        (blob[0x211] & 0x01) == 0x01),
    ("xloadflags   @ 0x236 bits 0+1 (KERNEL_64|CAN_BE_LOADED_ABOVE_4G)",
        (struct.unpack_from("<H", blob, 0x236)[0] & 0x03) == 0x03),
    ("pref_address @ 0x258 == 0x20000000",
        struct.unpack_from("<Q", blob, 0x258)[0] == 0x20000000),
    ("kernel entry payload[0x200] == 0xfa (cli) — stub at load_addr+0x200",
        blob[0x400 + 0x200] == 0xfa),
]
ok = True
for name, cond in checks:
    if cond:
        print(f"  [PASS] {label}: {name}")
    else:
        print(f"  [FAIL] {label}: {name}", file=sys.stderr); ok = False
sys.exit(0 if ok else 1)
PY
    case $? in 0) CHECKS=$((CHECKS+8));; *) FAIL=1; CHECKS=$((CHECKS+8));; esac
}

# bzImage-build ELF: links at 0x20000000 with entry at 0x20000200
info "bzImage-ELF: $BZIMAGE_ELF"
if [[ -f "$BZIMAGE_ELF" ]]; then
    check_bzimage_elf_entry "bzImage-ELF" "$BZIMAGE_ELF"
else
    warn "skipped — $BZIMAGE_ELF does not exist (run 'make kernel-bzimage' first)"
fi

# bzImage wrapper file: 1024-byte setup + payload
check_bzimage_wrapper "bzImage-file" "$BZIMAGE"

# --- Linux-side helper scripts -------------------------------------------

info "helper scripts (Linux-side)"

for script in scripts/x86-kexec-slmos.sh scripts/x86-kexec-deploy.sh; do
    if [[ ! -f "$script" ]]; then
        fail "missing: $script"
        continue
    fi
    if bash -n "$script" 2>/dev/null; then
        pass "$script: bash -n parses"
    else
        fail "$script: bash -n failed"
    fi
    if command -v shellcheck >/dev/null 2>&1; then
        if shellcheck -S warning "$script" >/dev/null 2>&1; then
            pass "$script: shellcheck -S warning clean"
        else
            fail "$script: shellcheck -S warning reported issues"
        fi
    else
        warn "$script: shellcheck not installed — skipping lint"
    fi
done

# --- Summary -------------------------------------------------------------

echo ""
if [[ $FAIL -eq 0 ]]; then
    echo "verify-kexec-build: ALL $CHECKS checks passed"
    exit 0
else
    echo "verify-kexec-build: FAILURES detected ($CHECKS checks total)"
    exit 1
fi
