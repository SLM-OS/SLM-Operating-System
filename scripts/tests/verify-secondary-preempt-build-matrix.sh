#!/usr/bin/env bash
# verify-secondary-preempt-build-matrix.sh
#
# Regression test for Jetson capstone Prereq #2 — SECONDARY_PREEMPT
# rename. Verifies that the ELR-trampoline symbols
# (resched_trampoline + maybe_arm_resched_trampoline) appear in the
# ELF iff SECONDARY_PREEMPT (or the PI5_SECONDARY_PREEMPT alias) is
# enabled, across all ARM64 platforms.
#
# Run after any change to:
#   - CMakeLists.txt SECONDARY_PREEMPT / PI5_SECONDARY_PREEMPT handling
#   - Makefile SECONDARY_PREEMPT variable forwarding
#   - kernel/sched/preempt.c compilation list
#   - `#if defined(SECONDARY_PREEMPT)` guards in preempt.h / preempt.c /
#     sched.c / vectors.S
#
# Expected runtime: ~1 minute (4 clean builds × ~15 s each).

set -euo pipefail

cd "$(dirname "$0")/../.."

NM=aarch64-none-elf-nm
if ! command -v "$NM" >/dev/null 2>&1; then
    echo "SKIP: $NM not found on PATH"
    exit 0
fi

TRAMPOLINE_SYMBOLS=(resched_trampoline maybe_arm_resched_trampoline)

# $1 = human label
# $2 = "yes" or "no" — expected presence
# remaining args = make variables
check_build() {
    local label="$1"; shift
    local expected="$1"; shift

    echo ""
    echo "=== $label — expected trampoline present: $expected ==="
    make kernel-clean >/dev/null 2>&1
    if ! make kernel "$@" >/dev/null 2>&1; then
        echo "FAIL: build failed for $label"
        exit 1
    fi

    # Snapshot nm output once — avoids a `nm | grep -q` SIGPIPE race when
    # grep short-circuits on match and nm exits 141, which `pipefail`
    # then treats as a pipe failure.
    local nm_out
    nm_out="$("$NM" build/kernel/slmos.elf)"

    local found=""
    for sym in "${TRAMPOLINE_SYMBOLS[@]}"; do
        if grep -q " T $sym$" <<<"$nm_out"; then
            found="$found $sym"
        fi
    done

    if [[ "$expected" == "yes" ]]; then
        for sym in "${TRAMPOLINE_SYMBOLS[@]}"; do
            if ! echo "$found" | grep -q "$sym"; then
                echo "FAIL: $label — expected $sym in ELF; not found"
                exit 1
            fi
        done
        echo "PASS: trampoline symbols present:$found"
    else
        if [[ -n "$found" ]]; then
            echo "FAIL: $label — unexpected trampoline symbols in ELF:$found"
            exit 1
        fi
        echo "PASS: trampoline symbols absent (as expected)"
    fi
}

echo "== Prereq #2 — SECONDARY_PREEMPT build-matrix regression test =="

# Default (OFF) — trampoline should NOT be linked on any ARM64 platform.
check_build "QEMU default (OFF)"                  no  PLATFORM=QEMU_VIRT
check_build "Pi 5 default (OFF)"                  no  PLATFORM=RASPI5
check_build "Jetson default (OFF)"                no  PLATFORM=JETSON_ORIN_NANO

# New name — trampoline SHOULD be linked on NC-memory ARM64 platforms.
check_build "Pi 5 SECONDARY_PREEMPT=ON"           yes PLATFORM=RASPI5 SECONDARY_PREEMPT=ON
check_build "Jetson SECONDARY_PREEMPT=ON"         yes PLATFORM=JETSON_ORIN_NANO SECONDARY_PREEMPT=ON

# QEMU doesn't use the trampoline — it has no NC memory backing and
# QEMU's emulated timer IRQ allows schedule-from-ISR. CMakeLists.txt
# silently ignores SECONDARY_PREEMPT=ON here (with a STATUS message).
check_build "QEMU SECONDARY_PREEMPT=ON (ignored)" no  PLATFORM=QEMU_VIRT SECONDARY_PREEMPT=ON

# Legacy alias — should behave identically to SECONDARY_PREEMPT=ON.
check_build "Pi 5 PI5_SECONDARY_PREEMPT=ON"       yes PLATFORM=RASPI5 PI5_SECONDARY_PREEMPT=ON

echo ""
echo "== All build-matrix checks passed =="
