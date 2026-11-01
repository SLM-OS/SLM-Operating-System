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
#   1. asks the Makefile for the resolved flags and for the exact
#      preemption CMake arguments its configure rules pass
#      (make print-preempt-config, which prints KERNEL_PREEMPT_CMAKE_ARGS
#      plus EXTRA_KERNEL_CMAKE_ARGS);
#   2. configures CMake into a temporary directory with those arguments;
#   3. checks the -D defines that reach the compiler.
#
# Run from anywhere (the script changes to the repo root):
#
#     ./scripts/check-preempt-config.sh
#
# Or via the make target:
#
#     make check-preempt-config
#
# Exits 0 when every case matches, 1 otherwise.

set -euo pipefail

cd "$(dirname "$0")/.."

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
    local label="${make_args:-<default>}${extra:+ + $extra}"

    # Plain `make`, not $(MAKE): this runs as its own top-level make
    # (MAKEFLAGS is unset above), not as a jobserver child. The case's
    # extra CMake arguments go in the way a user would pass them, via
    # EXTRA_KERNEL_CMAKE_ARGS.
    # stdout only: lines are picked by prefix below, so a make warning
    # on stderr can't be mistaken for the PLATFORM=... line. stderr is
    # kept for the failure message.
    local out err="$tmp_root/make$case_no.err"
    # shellcheck disable=SC2086  # make_args is a word list on purpose
    if ! out="$(make --no-print-directory print-preempt-config $make_args \
                ${extra:+"EXTRA_KERNEL_CMAKE_ARGS=$extra"} 2>"$err")"; then
        echo "$FAIL $label -> make print-preempt-config failed:"
        tail -5 "$err"
        failures=$((failures + 1))
        return
    fi
    local got_make cmake_args platform
    got_make="$(grep -m1 '^PLATFORM=' <<<"$out" || true)"
    cmake_args="$(sed -n 's/^CMAKE_ARGS=//p' <<<"$out")"
    platform="$(sed -n 's/^PLATFORM=\([^ ]*\).*/\1/p' <<<"$got_make")"

    # Check the printed preemption arguments directly, so the
    # Makefile -> CMake plumbing is covered even when the CMake half
    # below is skipped. Only SECONDARY_PREEMPT and JETSON_HW_TICK are
    # Makefile-driven; COOP_PREEMPT is CMake's own default.
    local bad=""
    [ "$got_make" = "$want_make" ] || bad+=" make: got '${got_make:-<none>}', want '$want_make';"
    local d
    for d in $want_on; do
        case "$d" in
            SECONDARY_PREEMPT|JETSON_HW_TICK)
                [[ " $cmake_args " == *" -D$d=ON "* ]] || bad+=" make args missing -D$d=ON;" ;;
        esac
    done
    for d in $want_off; do
        case "$d" in
            SECONDARY_PREEMPT|JETSON_HW_TICK)
                [[ " $cmake_args " != *" -D$d=ON "* ]] || bad+=" make args unexpectedly pass -D$d=ON;" ;;
        esac
    done

    if [ "$have_cmake_toolchain" -eq 0 ]; then
        if [ -z "$bad" ]; then
            echo "$OK   $label -> make args: ${cmake_args:-<none>} (CMake skipped)"
        else
            echo "$FAIL $label ->$bad"
            failures=$((failures + 1))
        fi
        return
    fi

    # The configure rules' fixed arguments, plus the preemption/extra
    # arguments exactly as the Makefile printed them.
    local args=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release
                -DPLATFORM="$platform")
    # shellcheck disable=SC2206  # cmake_args is a word list on purpose
    [ -n "$cmake_args" ] && args+=($cmake_args)

    local dir="$tmp_root/case$case_no"
    if ! cmake -G "Unix Makefiles" -S . -B "$dir" "${args[@]}" \
            > "$dir.log" 2>&1; then
        echo "$FAIL $label -> cmake configure failed:"
        tail -5 "$dir.log"
        failures=$((failures + 1))
        return
    fi

    local defs
    defs="$(grep -oE -- '-D(COOP_PREEMPT|SECONDARY_PREEMPT|JETSON_HW_TICK)=1' \
            "$dir/$FLAGS_REL" | sort -u | tr '\n' ' ' || true)"

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
