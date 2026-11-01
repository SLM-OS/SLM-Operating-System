#!/usr/bin/env bash
#
# scripts/check-preempt-config.sh — check which preemption flags each
# platform's default build (and the documented overrides) actually
# compiles with (#1050).
#
# The Makefile resolves JETSON_HW_TICK / SECONDARY_PREEMPT, and CMake
# derives COOP_PREEMPT from them. The two layers drifted apart once
# already: the docs said JETSON_HW_TICK=ON implied SECONDARY_PREEMPT=ON
# and COOP_PREEMPT=OFF, but the build kept COOP_PREEMPT=ON with no
# trampoline. For each case this script:
#
#   1. asks the Makefile for the resolved flags (make print-preempt-config);
#   2. configures CMake into a temporary directory with the same -D
#      arguments the Makefile would pass;
#   3. checks the -D defines that reach the compiler.
#
# Run from repo root:
#
#     ./scripts/check-preempt-config.sh
#
# Or via the make target:
#
#     make check-preempt-config
#
# Exits 0 when every case matches, 1 otherwise.

set -euo pipefail

# When run from `make test SECONDARY_PREEMPT=ON` (or any other
# override), the outer make passes its command-line variables down
# through MAKEFLAGS and the environment. Drop them so each case below
# resolves from the Makefile defaults plus only that case's arguments.
unset MAKEFLAGS MFLAGS MAKELEVEL MAKEOVERRIDES
unset PLATFORM SECONDARY_PREEMPT PI5_SECONDARY_PREEMPT JETSON_HW_TICK \
      EXTRA_KERNEL_CMAKE_ARGS

TOOLCHAIN="cmake/toolchain-aarch64-none-elf.cmake"
FLAGS_REL="CMakeFiles/slmos.elf.dir/flags.make"

# The CMake half configures with the aarch64 toolchain. On a host
# without it (e.g. an x86-only setup running `make test
# PLATFORM=X86_64`), check only the Makefile resolution and say so,
# rather than failing the whole test run.
have_cmake_toolchain=1
if ! command -v aarch64-none-elf-gcc > /dev/null 2>&1 || \
   ! command -v cmake > /dev/null 2>&1; then
    have_cmake_toolchain=0
    echo "SKIP CMake half: aarch64-none-elf-gcc or cmake not on PATH;" \
         "checking Makefile resolution only"
fi

if [ -t 1 ]; then
    OK=$'\033[32mOK\033[0m'
    FAIL=$'\033[31mFAIL\033[0m'
else
    OK="OK"
    FAIL="FAIL"
fi

tmp_root="$(mktemp -d)"
trap 'rm -rf "$tmp_root"' EXIT

failures=0
case_no=0

# check_case "<make args>" "<extra cmake args>" "<expected make line>" \
#            "<defines that must be present>" "<defines that must be absent>"
check_case() {
    local make_args="$1" extra="$2" want_make="$3" want_on="$4" want_off="$5"
    case_no=$((case_no + 1))

    # Plain `make`, not $(MAKE): this runs as its own top-level make
    # (MAKEFLAGS is unset above), not as a jobserver child.
    # shellcheck disable=SC2086  # make_args is a word list on purpose
    local got_make
    got_make="$(make --no-print-directory print-preempt-config $make_args)"

    local platform hw sp
    platform="$(sed -n 's/.*PLATFORM=\([^ ]*\).*/\1/p' <<<"$got_make")"
    hw="$(sed -n 's/.*JETSON_HW_TICK=\([^ ]*\).*/\1/p' <<<"$got_make")"
    sp="$(sed -n 's/.*SECONDARY_PREEMPT=\([^ ]*\).*/\1/p' <<<"$got_make")"

    # Mirror the Makefile's cmake invocation for these options.
    local args=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
                -DPLATFORM="$platform")
    [ "$hw" = "ON" ] && args+=(-DJETSON_HW_TICK=ON)
    [ "$sp" = "ON" ] && args+=(-DSECONDARY_PREEMPT=ON)
    # shellcheck disable=SC2206  # extra is a word list on purpose
    [ -n "$extra" ] && args+=($extra)

    local label="${make_args:-<default>}${extra:+ + $extra}"
    if [ "$have_cmake_toolchain" -eq 0 ]; then
        if [ "$got_make" = "$want_make" ]; then
            echo "$OK   $label -> make: $got_make (CMake skipped)"
        else
            echo "$FAIL $label -> make: got '$got_make', want '$want_make'"
            failures=$((failures + 1))
        fi
        return
    fi

    local dir="$tmp_root/case$case_no"
    if ! cmake -G "Unix Makefiles" -S . -B "$dir" "${args[@]}" \
            > "$dir.log" 2>&1; then
        echo "$FAIL [$make_args $extra] cmake configure failed:"
        tail -5 "$dir.log"
        failures=$((failures + 1))
        return
    fi

    local defs
    defs="$(grep -oE -- '-D(COOP_PREEMPT|SECONDARY_PREEMPT|JETSON_HW_TICK)=1' \
            "$dir/$FLAGS_REL" | sort -u | tr '\n' ' ' || true)"

    local bad=""
    [ "$got_make" = "$want_make" ] || bad+=" make: got '$got_make', want '$want_make';"
    local d
    for d in $want_on; do
        grep -q -- "-D$d=1" <<<"$defs" || bad+=" missing -D$d;"
    done
    for d in $want_off; do
        if grep -q -- "-D$d=1" <<<"$defs"; then
            bad+=" unexpected -D$d;"
        fi
    done

    if [ -z "$bad" ]; then
        echo "$OK   $label -> ${defs:-<none>}"
    else
        echo "$FAIL $label ->$bad"
        failures=$((failures + 1))
    fi
}

# QEMU: no trampoline, no cooperative tick (the emulated timer IRQ can
# call schedule() from the ISR).
check_case "" "" \
    "PLATFORM=QEMU_VIRT JETSON_HW_TICK=OFF SECONDARY_PREEMPT=OFF" \
    "" "COOP_PREEMPT SECONDARY_PREEMPT JETSON_HW_TICK"

# Pi 5 default: hardware preemption (#742/#777).
check_case "PLATFORM=RASPI5" "" \
    "PLATFORM=RASPI5 JETSON_HW_TICK=OFF SECONDARY_PREEMPT=ON" \
    "SECONDARY_PREEMPT" "COOP_PREEMPT JETSON_HW_TICK"

# Pi 5 cooperative build, as documented in docs/fact-sheets/preemption.md:
# both CMake spellings are needed.
check_case "PLATFORM=RASPI5 SECONDARY_PREEMPT=OFF" \
    "-DCOOP_PREEMPT=ON -DPI5_COOP_PREEMPT=ON" \
    "PLATFORM=RASPI5 JETSON_HW_TICK=OFF SECONDARY_PREEMPT=OFF" \
    "COOP_PREEMPT" "SECONDARY_PREEMPT JETSON_HW_TICK"

# Jetson default: hardware tick + trampoline, no cooperative tick (#1050).
check_case "PLATFORM=JETSON_ORIN_NANO" "" \
    "PLATFORM=JETSON_ORIN_NANO JETSON_HW_TICK=ON SECONDARY_PREEMPT=ON" \
    "JETSON_HW_TICK SECONDARY_PREEMPT" "COOP_PREEMPT"

# Jetson on stock BL31: cooperative fallback only.
check_case "PLATFORM=JETSON_ORIN_NANO JETSON_HW_TICK=OFF" "" \
    "PLATFORM=JETSON_ORIN_NANO JETSON_HW_TICK=OFF SECONDARY_PREEMPT=OFF" \
    "COOP_PREEMPT" "SECONDARY_PREEMPT JETSON_HW_TICK"

# Jetson hardware tick without the trampoline: secondaries keep their
# cooperative ticks (only CPU 0 unmasks at task entry).
check_case "PLATFORM=JETSON_ORIN_NANO SECONDARY_PREEMPT=OFF" "" \
    "PLATFORM=JETSON_ORIN_NANO JETSON_HW_TICK=ON SECONDARY_PREEMPT=OFF" \
    "JETSON_HW_TICK COOP_PREEMPT" "SECONDARY_PREEMPT"

if [ "$failures" -ne 0 ]; then
    echo "$failures of $case_no preemption-config case(s) failed"
    exit 1
fi
echo "All $case_no preemption-config cases match"
