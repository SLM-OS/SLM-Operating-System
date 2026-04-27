#!/usr/bin/env bash
# test-jetson-build-defaults.sh
#
# Functional test for the Jetson default-build-flags policy:
#   1. NET_TELNETD_AUTOSTART defaults ON for JETSON_ORIN_NANO and OFF
#      for QEMU_VIRT (and X86_64 if available).
#   2. GA10B_FIRMWARE_DIR auto-detects $HOME/jetson-ga10b-firmware on a
#      first-time configure when the directory exists, and stays empty
#      when the user explicitly passes -DGA10B_FIRMWARE_DIR= (empty).
#   3. ENABLE_GA10B_FIRMWARE compile-definition is present iff
#      GA10B_FIRMWARE_DIR resolves to a directory with the required
#      firmware blobs.
#
# Why this exists: these are pure CMake-config defaults, but the demo
# runbook (`docs/demo-admin.md`) depends on them. A regression that
# silently flips the Jetson default OFF would only surface as "telnet
# stopped working" after a deploy, with no test signal.
#
# Run after any change to:
#   - CMakeLists.txt NET_TELNETD_AUTOSTART or GA10B_FIRMWARE_DIR logic
#   - Makefile EXTRA_KERNEL_CMAKE_ARGS forwarding
#   - scripts/tools/fetch-ga10b-firmware.sh default destination
#
# Configure-only — no link step. Expected runtime: ~30 s total.

set -euo pipefail

cd "$(dirname "$0")/../.."

REPO_ROOT="$(pwd)"
SCRATCH="$(mktemp -d -t slmos-build-defaults.XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

PASSES=0
FAILS=0

# Run cmake configure into a fresh build dir for the given platform +
# extra args, then echo the cache state.
#
# $1 = scenario label
# $2 = platform (JETSON_ORIN_NANO, QEMU_VIRT, …)
# remaining args = extra -D flags for cmake
#
# Sets the global vars CACHE_NET_TELNETD_AUTOSTART, CACHE_GA10B_FIRMWARE_DIR
# and COMPILE_DEFS_HAS_GA10B (yes/no), COMPILE_DEFS_HAS_TELNETD (yes/no).
configure_case() {
    local label="$1"; shift
    local platform="$1"; shift
    local build_dir="$SCRATCH/case-$(echo "$label" | tr ' /' '__')"

    mkdir -p "$build_dir"
    pushd "$build_dir" >/dev/null

    # Mirror the relevant invocation pattern the top-level Makefile uses
    # (PLATFORM as a -D, optional caller flags). CMAKE_EXPORT_COMPILE_COMMANDS
    # gives us a JSON file we can grep for compile-definitions reliably
    # without parsing the generator's output.
    if ! cmake "$REPO_ROOT" \
            -DPLATFORM="$platform" \
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
            -DCMAKE_TOOLCHAIN_FILE="$REPO_ROOT/cmake/toolchain-aarch64-none-elf.cmake" \
            "$@" >/dev/null 2>"$build_dir/configure.err"; then
        echo "FAIL [$label]: cmake configure failed"
        echo "----- configure stderr -----"
        cat "$build_dir/configure.err" || true
        echo "----------------------------"
        popd >/dev/null
        return 1
    fi

    CACHE_NET_TELNETD_AUTOSTART="$(awk -F'[:=]' \
        '$1 == "NET_TELNETD_AUTOSTART" { print $3 }' CMakeCache.txt | head -1)"
    CACHE_GA10B_FIRMWARE_DIR="$(awk -F'[:=]' \
        '$1 == "GA10B_FIRMWARE_DIR" { print $3 }' CMakeCache.txt | head -1)"

    # Compile defs are emitted as -DENABLE_GA10B_FIRMWARE=1 /
    # -DNET_TELNETD_AUTOSTART=1 in compile_commands.json command lines.
    if grep -q -- '-DENABLE_GA10B_FIRMWARE=1' compile_commands.json 2>/dev/null; then
        COMPILE_DEFS_HAS_GA10B=yes
    else
        COMPILE_DEFS_HAS_GA10B=no
    fi
    if grep -q -- '-DNET_TELNETD_AUTOSTART=1' compile_commands.json 2>/dev/null; then
        COMPILE_DEFS_HAS_TELNETD=yes
    else
        COMPILE_DEFS_HAS_TELNETD=no
    fi

    popd >/dev/null
}

# $1 = label
# $2 = expected ("ON" or "OFF" — accepted forms in CMakeCache.txt)
# $3 = actual cache value
expect_cache_bool() {
    local label="$1" expected="$2" actual="$3"
    case "$actual" in
        ON|TRUE|1)  actual=ON ;;
        OFF|FALSE|0|"") actual=OFF ;;
    esac
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS [$label]: $expected"
        PASSES=$((PASSES + 1))
    else
        echo "  FAIL [$label]: expected $expected, got '$actual'"
        FAILS=$((FAILS + 1))
    fi
}

expect_eq() {
    local label="$1" expected="$2" actual="$3"
    if [[ "$expected" == "$actual" ]]; then
        echo "  PASS [$label]: '$actual'"
        PASSES=$((PASSES + 1))
    else
        echo "  FAIL [$label]: expected '$expected', got '$actual'"
        FAILS=$((FAILS + 1))
    fi
}

# -----------------------------------------------------------------------
# Scenario 1: Jetson default — NET_TELNETD_AUTOSTART=ON expected.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 1: Jetson default flags ==="
configure_case "jetson-default" "JETSON_ORIN_NANO"
expect_cache_bool "jetson default NET_TELNETD_AUTOSTART" \
    "ON" "$CACHE_NET_TELNETD_AUTOSTART"
expect_eq "jetson default NET_TELNETD_AUTOSTART compile-def present" \
    "yes" "$COMPILE_DEFS_HAS_TELNETD"

# -----------------------------------------------------------------------
# Scenario 2: Jetson with explicit override — telnetd OFF must win.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 2: Jetson with -DNET_TELNETD_AUTOSTART=OFF override ==="
configure_case "jetson-override-off" "JETSON_ORIN_NANO" \
    "-DNET_TELNETD_AUTOSTART=OFF"
expect_cache_bool "jetson explicit-OFF NET_TELNETD_AUTOSTART" \
    "OFF" "$CACHE_NET_TELNETD_AUTOSTART"
expect_eq "jetson explicit-OFF NET_TELNETD_AUTOSTART compile-def absent" \
    "no" "$COMPILE_DEFS_HAS_TELNETD"

# -----------------------------------------------------------------------
# Scenario 3: QEMU default — NET_TELNETD_AUTOSTART=OFF expected.
# (Sanity check the Pi 5 / Jetson special-case isn't accidentally
# defaulting the rest of the world ON.)
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 3: QEMU default flags ==="
configure_case "qemu-default" "QEMU_VIRT"
expect_cache_bool "qemu default NET_TELNETD_AUTOSTART" \
    "OFF" "$CACHE_NET_TELNETD_AUTOSTART"
expect_eq "qemu default NET_TELNETD_AUTOSTART compile-def absent" \
    "no" "$COMPILE_DEFS_HAS_TELNETD"

# -----------------------------------------------------------------------
# Scenario 4: GA10B firmware auto-detect.
#
# Two sub-cases driven by whether $HOME/jetson-ga10b-firmware exists on
# the build host. The script doesn't synthesize a firmware dir — both
# branches are real CMake behaviour we want to lock in.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 4: Jetson GA10B firmware auto-detect ==="
DEFAULT_FW_DIR="$HOME/jetson-ga10b-firmware"
configure_case "jetson-ga10b-autodetect" "JETSON_ORIN_NANO"
if [[ -d "$DEFAULT_FW_DIR" ]]; then
    expect_eq "auto-detect picks $DEFAULT_FW_DIR" \
        "$DEFAULT_FW_DIR" "$CACHE_GA10B_FIRMWARE_DIR"
    expect_eq "ENABLE_GA10B_FIRMWARE compile-def present" \
        "yes" "$COMPILE_DEFS_HAS_GA10B"
else
    echo "  NOTE: $DEFAULT_FW_DIR is absent; expecting empty + no compile-def"
    expect_eq "no firmware dir → cache empty" \
        "" "$CACHE_GA10B_FIRMWARE_DIR"
    expect_eq "no firmware dir → ENABLE_GA10B_FIRMWARE absent" \
        "no" "$COMPILE_DEFS_HAS_GA10B"
fi

# -----------------------------------------------------------------------
# Scenario 5: explicit empty -DGA10B_FIRMWARE_DIR= must defeat the
# auto-detect even when the default dir exists. Pins the
# `DEFINED CACHE{GA10B_FIRMWARE_DIR}` guard added in fix/jetson-default-build-flags.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 5: Jetson with -DGA10B_FIRMWARE_DIR= explicit empty ==="
configure_case "jetson-ga10b-empty" "JETSON_ORIN_NANO" \
    "-DGA10B_FIRMWARE_DIR="
expect_eq "explicit empty → cache empty" \
    "" "$CACHE_GA10B_FIRMWARE_DIR"
expect_eq "explicit empty → ENABLE_GA10B_FIRMWARE absent" \
    "no" "$COMPILE_DEFS_HAS_GA10B"

# -----------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------
echo ""
echo "================================================================"
echo "test-jetson-build-defaults.sh: $PASSES passed, $FAILS failed"
echo "================================================================"

if (( FAILS > 0 )); then
    exit 1
fi
exit 0
