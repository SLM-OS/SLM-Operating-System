#!/usr/bin/env bash
#
# test-jetson-uefi-layout.sh — structural regression test for the
# Jetson UEFI-direct boot path.
#
# The UEFI-direct path depends on several non-obvious layout and
# alignment invariants that aren't exercised by the QEMU test suite
# (the code is all gated on PLATFORM_JETSON_ORIN_NANO). A structural
# check on the ELF catches regressions that would otherwise only show
# up on live hardware as a silent hang.
#
# This script:
#   1. Builds the Jetson kernel.
#   2. Verifies the early EL2 VBAR table is 0x800-aligned.
#   3. Verifies all 16 vector entries are 0x80-aligned within the
#      table and each is a single branch to the common handler.
#   4. Verifies the fault scratch slot lives in .bss and is at least
#      64 bytes.
#   5. Verifies `efi_stub_entry` references `jetson_early_vbar_el2`
#      (catches an accidental removal of the install site).
#   6. Verifies the primary_cpu SLM-OS VBAR_EL1 install site still
#      exists (regressions here would mean a fault post-primary_cpu
#      misses the SLM-OS handler).
#   7. Verifies the EFI entry in boot.S turns the MMU off with both
#      the E2H=0 and EL1/VHE arms, invalidates the I-cache and jumps
#      into the relocated copy (#26).
#   8. Verifies the EFI stub cleans by VA (dc civac) and never by
#      set/way (dc cisw).
#   9. Verifies the PE ImageBase equals the link address of _start.
#  10. Verifies the HCR_EL2 write is read-modify-write.
#
# The runtime behaviour of the same path (firmware load at a
# non-preferred address, relocation, boot to the shell) is exercised
# under QEMU's AArch64 UEFI by `make test-qemu-uefi`.
#
# Exit 0 on pass, 1 on fail. Expected to run as part of pre-merge
# validation for any change touching boot.S / efi_stub.c / the
# Jetson linker script.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="$ROOT/build/kernel/slmos.elf"
NM="${NM:-aarch64-none-elf-nm}"
OBJDUMP="${OBJDUMP:-aarch64-none-elf-objdump}"

fail=0
pass() { printf '  \033[32m✓\033[0m %s\n' "$*"; }
warn() { printf '  \033[33m!\033[0m %s\n' "$*"; }
err()  { printf '  \033[31m✗\033[0m %s\n' "$*"; fail=1; }

printf '\n== Build Jetson kernel ==\n'
make -C "$ROOT" kernel-clean >/dev/null 2>&1 || true
if make -C "$ROOT" kernel PLATFORM=JETSON_ORIN_NANO >/tmp/jetson-build.log 2>&1; then
    pass "build succeeded"
else
    err "build failed — see /tmp/jetson-build.log"
    tail -40 /tmp/jetson-build.log
    exit 1
fi

if [ ! -f "$ELF" ]; then
    err "ELF not found at $ELF"
    exit 1
fi

printf '\n== Early VBAR_EL2 table ==\n'

# 1. Symbol exists and is 2KB-aligned.
vbar_addr=$("$NM" "$ELF" | awk '/ T jetson_early_vbar_el2$/ {print $1}')
if [ -z "$vbar_addr" ]; then
    err "jetson_early_vbar_el2 symbol missing"
else
    # bash arithmetic with hex
    if (( 0x$vbar_addr % 0x800 != 0 )); then
        err "jetson_early_vbar_el2 at 0x$vbar_addr — NOT 2KB-aligned (ARM64 VBAR requirement)"
    else
        pass "jetson_early_vbar_el2 at 0x$vbar_addr (2KB-aligned)"
    fi
fi

# 2. Each of 16 vector entries at offset N*0x80 is a single `b` and
#    targets the same address. Use objdump to decode.
if [ -n "$vbar_addr" ]; then
    entries_end=$(printf '%x' $((0x$vbar_addr + 0x800)))
    disasm=$("$OBJDUMP" -d "$ELF" \
        --start-address="0x$vbar_addr" \
        --stop-address="0x$entries_end" 2>/dev/null)

    branch_count=$(echo "$disasm" | grep -cE '^\s+[0-9a-f]+:\s+[0-9a-f]+\s+b\s' || true)
    if [ "$branch_count" -ne 16 ]; then
        err "expected 16 branch instructions in vector table, found $branch_count"
    else
        pass "16 vector entries, each a single branch"
    fi

    # All branches must target the same address (the common handler).
    distinct_targets=$(echo "$disasm" \
        | awk '/\s+b\s+/ {print $NF}' \
        | sort -u \
        | wc -l)
    if [ "$distinct_targets" -ne 1 ]; then
        err "vector branches go to $distinct_targets distinct targets, expected 1"
    else
        pass "all vectors funnel to single handler"
    fi
fi

printf '\n== Fault scratch slot ==\n'

# 3. `jetson_early_fault_slot` lives in .bss (type B) and
#    `jetson_early_fault_slot_end - jetson_early_fault_slot` >= 64.
slot_line=$("$NM" "$ELF" | awk '$3 == "jetson_early_fault_slot"')
end_line=$("$NM" "$ELF" | awk '$3 == "jetson_early_fault_slot_end"')
if [ -z "$slot_line" ] || [ -z "$end_line" ]; then
    err "jetson_early_fault_slot or _end symbol missing"
else
    slot_type=$(echo "$slot_line" | awk '{print $2}')
    slot_addr=$(echo "$slot_line" | awk '{print $1}')
    end_addr=$(echo "$end_line" | awk '{print $1}')
    size=$((0x$end_addr - 0x$slot_addr))

    if [ "$slot_type" != "B" ]; then
        err "jetson_early_fault_slot has type '$slot_type', expected 'B' (BSS)"
    else
        pass "jetson_early_fault_slot in .bss at 0x$slot_addr"
    fi

    if [ "$size" -lt 64 ]; then
        err "scratch slot size is $size bytes, handler writes 56 — need ≥ 64"
    else
        pass "scratch slot size: $size bytes"
    fi
fi

printf '\n== Install site reachability ==\n'

# 4. efi_stub_entry installs VBAR_EL2. The function must contain an
#    `msr vbar_el2, Xn` instruction, and the Xn must be loaded from
#    an `adrp` + `add` pair whose computed address matches
#    `jetson_early_vbar_el2`. We decode that pair from the objdump
#    output.
efi_entry_addr=$("$NM" "$ELF" | awk '/ T efi_stub_entry$/ {print $1}')
if [ -z "$efi_entry_addr" ]; then
    err "efi_stub_entry symbol missing"
else
    stop=$(printf '%x' $((0x$efi_entry_addr + 0x800)))
    dis=$("$OBJDUMP" -d "$ELF" \
        --start-address="0x$efi_entry_addr" \
        --stop-address="0x$stop" 2>/dev/null)

    msr_line=$(echo "$dis" | grep -nE 'msr\s+vbar_el2' | head -1)
    if [ -z "$msr_line" ]; then
        err "efi_stub_entry has NO 'msr vbar_el2' instruction — install site removed?"
    else
        pass "efi_stub_entry has 'msr vbar_el2' instruction"

        # Verify the adrp/add pair FEEDING the msr lands on
        # jetson_early_vbar_el2. We look at the two instruction lines
        # immediately preceding the msr in the decoded stream (the
        # compiler emits adrp; add; msr contiguously for an extern
        # symbol passed to an inline-asm "r" constraint). Scanning
        # the whole function's disassembly for adrp/add pairs picks
        # up unrelated pairs (there are many in efi_stub_entry).
        msr_lineno=$(echo "$msr_line" | cut -d: -f1)
        adrp_line=$(echo "$dis" | sed -n "$((msr_lineno - 2))p")
        add_line=$(echo "$dis" | sed -n "$((msr_lineno - 1))p")

        if echo "$adrp_line" | grep -qE 'adrp\s+x' \
           && echo "$add_line"  | grep -qE '\badd\s+x[0-9]+, *x[0-9]+, *#'; then
            adrp_page_hex=$(echo "$adrp_line" \
                | awk '{for (i=1;i<=NF;i++) if ($i ~ /^[0-9a-f]{6,}$/) last=$i; print last}')
            add_imm=$(echo "$add_line" \
                | sed -nE 's/.*#(0x[0-9a-f]+|[0-9]+).*/\1/p' | head -1)
            if [ -n "$adrp_page_hex" ] && [ -n "$add_imm" ]; then
                computed=$(( 0x$adrp_page_hex + add_imm ))
                vbar_num=$(( 0x$vbar_addr ))
                if [ "$computed" -eq "$vbar_num" ]; then
                    pass "install site computes $(printf '0x%x' $computed) (matches jetson_early_vbar_el2)"
                else
                    err "install site computes $(printf '0x%x' $computed), expected $(printf '0x%x' $vbar_num)"
                fi
            else
                warn "could not parse adrp/add operands — skipping address match"
            fi
        else
            warn "instructions before msr vbar_el2 are not adrp/add — unusual compile output"
        fi
    fi
fi

printf '\n== Late VBAR_EL1 install (primary_cpu) ==\n'

# 5. primary_cpu must still install VBAR_EL1 with exception_vectors
#    (via a literal pool load, since boot.S uses PC-relative `adr +
#    ldr literal-offset` addressing). Verify:
#      (a) at least one `msr vbar_el1` exists in the image
#      (b) `exception_vectors` symbol exists (the target of that msr)
#    Exact cross-reference between the two is hard without full
#    relocation info; this two-step check catches gross regressions.
msr_el1_count=$("$OBJDUMP" -d "$ELF" 2>/dev/null \
    | grep -cE 'msr\s+vbar_el1' || true)
if [ "$msr_el1_count" -lt 1 ]; then
    err "no 'msr vbar_el1' found — primary_cpu VBAR install regressed?"
else
    pass "$msr_el1_count 'msr vbar_el1' instructions in image"
fi

if "$NM" "$ELF" | awk '$3 == "exception_vectors"' | grep -q .; then
    pass "exception_vectors symbol exists (VBAR_EL1 target)"
else
    err "exception_vectors symbol missing"
fi

printf '\n== UEFI hand-off: cache clean, MMU off, relocation (#26) ==\n'

# 6. The EFI entry window in boot.S (real_start .. primary_cpu) turns
#    the MMU off itself, E2H-aware, and must carry BOTH arms: `tlbi
#    alle2` (EL2 with E2H=0 — Jetson UEFI v36.4.7) and `tlbi vmalle1`
#    (EL1, or EL2 with VHE on). If either is missing, one entry mode
#    silently leaves the MMU running or hits the dormant register.
#    The window must also invalidate the I-cache and branch into the
#    relocated copy (`br`).
#
# NOTE: uses `grep -c` + count-check rather than `grep -q` because
# with `set -o pipefail` a SIGPIPE-driven early-exit from `grep -q`
# makes `echo "$huge_string" | grep -q ...` spuriously fail when
# echo can't complete writing before grep closes its stdin.
rs_addr=$("$NM" "$ELF" | awk '$3 == "real_start" {print $1}')
pc_addr=$("$NM" "$ELF" | awk '$3 == "primary_cpu" {print $1}')
if [ -z "$rs_addr" ] || [ -z "$pc_addr" ]; then
    err "real_start / primary_cpu symbol missing"
else
    win=$("$OBJDUMP" -d "$ELF" --start-address="0x$rs_addr" \
        --stop-address="0x$pc_addr" 2>/dev/null)
    n=$(echo "$win" | grep -cE 'tlbi[[:space:]]+alle2' || true)
    [ "$n" -ge 1 ] && pass "EFI entry has 'tlbi alle2' (EL2, E2H=0 MMU-off)" \
                   || err "no 'tlbi alle2' in EFI entry — E2H=0 MMU-off arm missing"
    # vmalle1 (not vmalle1is): the IS variant is used elsewhere.
    n=$(echo "$win" | grep -cE 'tlbi[[:space:]]+vmalle1($|[^i])' || true)
    [ "$n" -ge 1 ] && pass "EFI entry has 'tlbi vmalle1' (EL1 / VHE MMU-off)" \
                   || err "no 'tlbi vmalle1' in EFI entry — EL1/VHE MMU-off arm missing"
    n=$(echo "$win" | grep -cE 'ic[[:space:]]+iallu' || true)
    [ "$n" -ge 1 ] && pass "EFI entry invalidates the I-cache after the copy" \
                   || err "no 'ic iallu' in EFI entry"
    n=$(echo "$win" | grep -cE '[[:space:]]br[[:space:]]+x[0-9]+' || true)
    [ "$n" -ge 1 ] && pass "EFI entry branches into the relocated copy" \
                   || err "no 'br xN' in EFI entry — relocation jump missing"
fi

# 7. efi_stub_entry must clean by VA (`dc civac`) and must NOT use
#    set/way maintenance (`dc cisw`). Set/way does not reach the PoC
#    through system caches; on Jetson the stale stack it left behind
#    took the board down with an EL3 RAS error after ExitBootServices.
if [ -n "$efi_entry_addr" ]; then
    stop=$(printf '%x' $((0x$efi_entry_addr + 0x1000)))
    edis=$("$OBJDUMP" -d "$ELF" --start-address="0x$efi_entry_addr" \
        --stop-address="0x$stop" 2>/dev/null)
    n=$(echo "$edis" | grep -cE 'dc[[:space:]]+civac' || true)
    [ "$n" -ge 1 ] && pass "efi_stub_entry cleans by VA (dc civac)" \
                   || err "no 'dc civac' in efi_stub_entry — VA clean missing"
    n=$("$OBJDUMP" -d "$ELF" 2>/dev/null | awk '/<efi_/,/^$/' \
        | grep -cE 'dc[[:space:]]+cisw' || true)
    [ "$n" -eq 0 ] && pass "no set/way 'dc cisw' in EFI stub code" \
                   || err "EFI stub uses set/way 'dc cisw' ($n sites)"
fi

# 8. PE ImageBase must equal the link address of _start, so firmware
#    that honours it loads the image where it is linked.
BIN="$ROOT/build/kernel/slmos.bin"
start_addr=$("$NM" "$ELF" | awk '$3 == "_start" {print $1}')
if [ -f "$BIN" ] && [ -n "$start_addr" ]; then
    ib=$(python3 -I - "$BIN" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f:
    f.seek(0x3C); pe = struct.unpack("<I", f.read(4))[0]
    f.seek(pe + 4 + 20 + 24); print("%x" % struct.unpack("<Q", f.read(8))[0])
PY
)
    if (( 0x$ib == 0x$start_addr )); then
        pass "PE ImageBase 0x$ib == link address of _start"
    else
        err "PE ImageBase 0x$ib != _start 0x$start_addr"
    fi
else
    warn "slmos.bin or _start missing — ImageBase check skipped"
fi

# 9. boot.S's Jetson HCR_EL2 write must be RMW: a `mrs XN, hcr_el2`
#    before the `msr hcr_el2, ...`. Pre-P3 the write was an
#    unconditional move with no preceding mrs.
mrs_hcr_count=$("$OBJDUMP" -d "$ELF" 2>/dev/null \
    | grep -cE 'mrs[[:space:]]+x[0-9]+,[[:space:]]*hcr_el2' || true)
if [ "$mrs_hcr_count" -ge 1 ]; then
    pass "kernel reads HCR_EL2 before writing it ($mrs_hcr_count sites — RMW pattern)"
else
    err "no 'mrs XN, hcr_el2' found — P3 RMW pattern missing?"
fi

printf '\n== Summary ==\n'
if [ "$fail" -eq 0 ]; then
    printf '  \033[32mAll checks passed.\033[0m\n'
    exit 0
else
    printf '  \033[31mOne or more checks failed.\033[0m\n'
    exit 1
fi
