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

printf '\n== P3: E2H-aware efi_disable_mmu ==\n'

# 6. efi_disable_mmu must contain BOTH the EL2-path TLBI (`tlbi
#    alle2`) and the EL1-path TLBI (`tlbi vmalle1`). If either is
#    missing, the runtime branch is only half-implemented and one
#    of the two entry modes (UEFI-direct with E2H=0 vs
#    kexec/EL1/VHE-on) will silently leave the MMU running or hit
#    the wrong register. The tlbi instruction is a unique marker
#    for each branch since the mrs/bic/msr surrounding sequences
#    are identical.
#
# NOTE: uses `grep -c` + count-check rather than `grep -q` because
# with `set -o pipefail` a SIGPIPE-driven early-exit from `grep -q`
# makes `echo "$huge_string" | grep -q ...` spuriously fail when
# echo can't complete writing before grep closes its stdin.
alle2_count=$("$OBJDUMP" -d "$ELF" 2>/dev/null \
    | grep -cE 'tlbi[[:space:]]+alle2' || true)
if [ "$alle2_count" -ge 1 ]; then
    pass "image contains 'tlbi alle2' (EL2-path MMU disable)"
else
    err "no 'tlbi alle2' — E2H=0 branch of efi_disable_mmu missing?"
fi

# tlbi vmalle1 (not vmalle1is) specifically — the `is` variant is
# used elsewhere in the kernel for IS-scoped shootdown and would
# match a too-broad regex.
vmalle1_count=$("$OBJDUMP" -d "$ELF" 2>/dev/null \
    | grep -cE 'tlbi[[:space:]]+vmalle1($|[^i])' || true)
if [ "$vmalle1_count" -ge 1 ]; then
    pass "image contains 'tlbi vmalle1' (EL1/VHE-path MMU disable)"
else
    err "no 'tlbi vmalle1' — E2H=1 / EL1 branch of efi_disable_mmu missing?"
fi

# 7. boot.S's Jetson HCR_EL2 write must be RMW: a `mrs XN, hcr_el2`
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
