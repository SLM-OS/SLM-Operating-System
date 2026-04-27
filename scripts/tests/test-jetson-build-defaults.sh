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
# Determinism: the auto-detect cases (Scenario 4a / 4b) run cmake with
# HOME overridden to a temp dir under SCRATCH so they don't depend on
# whether the real build host has firmware staged. 4a synthesises the
# 17 expected blob filenames (zero-byte stand-ins; the existence-check
# at configure time only does `IS_DIRECTORY` + per-file `EXISTS`). 4b
# uses a fresh empty fake HOME.
#
# Run after any change to:
#   - CMakeLists.txt NET_TELNETD_AUTOSTART or GA10B_FIRMWARE_DIR logic
#   - Makefile EXTRA_KERNEL_CMAKE_ARGS forwarding
#   - scripts/tools/fetch-ga10b-firmware.sh default destination
#
# Configure-only — no link step. Expected runtime: ~30 s total.
# Wired into `make test-build-defaults` (separate target — not part of
# `make test`, since build-host concerns shouldn't gate kernel-runtime
# regression runs).

set -euo pipefail

cd "$(dirname "$0")/../.."

REPO_ROOT="$(pwd)"
SCRATCH="$(mktemp -d -t slmos-build-defaults.XXXXXX)"
trap 'rm -rf "$SCRATCH"' EXIT

PASSES=0
FAILS=0

# Synthesize a fake firmware dir holding the 17 blob filenames the
# CMake required-files check looks for (kernel CMakeLists.txt
# `_ga10b_required` list). Files are zero-byte; the build never reads
# them at configure time. Used to exercise the auto-detect *positive*
# branch deterministically regardless of whether the build host has
# the real firmware staged.
make_fake_firmware_dir() {
    local dir="$1"
    mkdir -p "$dir"
    local f
    for f in \
        acr-gsp.text.encrypt.bin.prod \
        acr-gsp.data.encrypt.bin.prod \
        acr-gsp.manifest.encrypt.bin.out.bin.prod \
        fecs_encrypt_prod.bin \
        fecs_pkc_sig_encrypt.bin \
        gpccs_encrypt_prod.bin \
        gpccs_pkc_sig_encrypt.bin \
        gpmu_ucode_next_prod_image.bin \
        gpmu_ucode_next_prod_desc.bin \
        pmu_pkc_prod_sig.bin \
        NETA_img_prod_encrypted.bin \
        NETB_img_prod_encrypted.bin \
        NETC_img_prod_encrypted.bin \
        NETD_img_prod_encrypted.bin \
        safety-scheduler.text.encrypt.bin.prod \
        safety-scheduler.data.encrypt.bin.prod \
        safety-scheduler.manifest.encrypt.bin.out.bin.prod
    do
        : > "$dir/$f"
    done
}

# Read a CMakeCache.txt entry by name, robust to '=' or ':' inside
# the value. Cache lines look like `NAME:TYPE=VALUE`; `sed` strips
# the `NAME:TYPE=` prefix (TYPE is one of BOOL/PATH/STRING/…) and
# emits VALUE. The `awk -F'[:=]' '{print $3}'` form this replaces
# would truncate paths containing '=' or ':', which Linux paths can
# legally hold.
read_cache_entry() {
    local name="$1" file="$2"
    sed -nE "s|^${name}:[A-Z]+=||p" "$file" | head -1
}

# Run cmake configure into a fresh build dir for the given platform +
# extra args, then echo the cache state.
#
# $1 = scenario label
# $2 = platform (JETSON_ORIN_NANO, QEMU_VIRT, …)
# $3 = HOME override (use "" to inherit the caller's HOME — only
#      Scenario 4 sets this so the auto-detect logic exercises a
#      synthesised firmware dir)
# remaining args = extra -D flags for cmake
#
# Sets the global vars CACHE_NET_TELNETD_AUTOSTART, CACHE_GA10B_FIRMWARE_DIR
# and COMPILE_DEFS_HAS_GA10B (yes/no), COMPILE_DEFS_HAS_TELNETD (yes/no).
configure_case() {
    local label="$1"; shift
    local platform="$1"; shift
    local home_override="$1"; shift
    local build_dir="$SCRATCH/case-$(echo "$label" | tr ' /' '__')"

    mkdir -p "$build_dir"
    pushd "$build_dir" >/dev/null

    # Mirror the relevant invocation pattern the top-level Makefile uses
    # (PLATFORM as a -D, optional caller flags). CMAKE_EXPORT_COMPILE_COMMANDS
    # gives us a JSON file we can grep for compile-definitions. The grep
    # below is global-scope — it matches if *any* compilation unit in
    # the project carries the define. Today the kernel uses
    # `add_compile_definitions(...)` at project scope so this is
    # correct; if a future refactor scopes a define per-target only,
    # tighten this to filter on a known source file.
    local cmake_env=()
    if [[ -n "$home_override" ]]; then
        cmake_env=(env "HOME=$home_override")
    fi
    if ! "${cmake_env[@]}" cmake "$REPO_ROOT" \
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

    CACHE_NET_TELNETD_AUTOSTART="$(read_cache_entry NET_TELNETD_AUTOSTART CMakeCache.txt)"
    CACHE_GA10B_FIRMWARE_DIR="$(read_cache_entry GA10B_FIRMWARE_DIR CMakeCache.txt)"

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
configure_case "jetson-default" "JETSON_ORIN_NANO" ""
expect_cache_bool "jetson default NET_TELNETD_AUTOSTART" \
    "ON" "$CACHE_NET_TELNETD_AUTOSTART"
expect_eq "jetson default NET_TELNETD_AUTOSTART compile-def present" \
    "yes" "$COMPILE_DEFS_HAS_TELNETD"

# -----------------------------------------------------------------------
# Scenario 2: Jetson with explicit override — telnetd OFF must win.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 2: Jetson with -DNET_TELNETD_AUTOSTART=OFF override ==="
configure_case "jetson-override-off" "JETSON_ORIN_NANO" "" \
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
configure_case "qemu-default" "QEMU_VIRT" ""
expect_cache_bool "qemu default NET_TELNETD_AUTOSTART" \
    "OFF" "$CACHE_NET_TELNETD_AUTOSTART"
expect_eq "qemu default NET_TELNETD_AUTOSTART compile-def absent" \
    "no" "$COMPILE_DEFS_HAS_TELNETD"

# -----------------------------------------------------------------------
# Scenario 4a: GA10B firmware auto-detect — POSITIVE branch.
#
# Always-deterministic. Synthesise a fake $HOME with the 17-file
# firmware dir present, override HOME for the cmake run, and assert
# the auto-detect picks it up. This pins the auto-detect-positive
# behaviour regardless of whether the real build host has firmware
# staged at the canonical location.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 4a: Jetson auto-detect with synthesised firmware ==="
FAKE_HOME_PRESENT="$SCRATCH/fake-home-present"
mkdir -p "$FAKE_HOME_PRESENT"
make_fake_firmware_dir "$FAKE_HOME_PRESENT/jetson-ga10b-firmware"
configure_case "jetson-ga10b-autodetect-yes" "JETSON_ORIN_NANO" "$FAKE_HOME_PRESENT"
expect_eq "auto-detect picks fake \$HOME/jetson-ga10b-firmware" \
    "$FAKE_HOME_PRESENT/jetson-ga10b-firmware" "$CACHE_GA10B_FIRMWARE_DIR"
expect_eq "ENABLE_GA10B_FIRMWARE compile-def present" \
    "yes" "$COMPILE_DEFS_HAS_GA10B"

# -----------------------------------------------------------------------
# Scenario 4b: GA10B firmware auto-detect — NEGATIVE branch.
#
# Override HOME to a fresh empty dir so the canonical firmware path
# does NOT exist. Cache stays empty and the compile-def is absent.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 4b: Jetson auto-detect with no firmware staged ==="
FAKE_HOME_ABSENT="$SCRATCH/fake-home-absent"
mkdir -p "$FAKE_HOME_ABSENT"
configure_case "jetson-ga10b-autodetect-no" "JETSON_ORIN_NANO" "$FAKE_HOME_ABSENT"
expect_eq "no firmware dir → cache empty" \
    "" "$CACHE_GA10B_FIRMWARE_DIR"
expect_eq "no firmware dir → ENABLE_GA10B_FIRMWARE absent" \
    "no" "$COMPILE_DEFS_HAS_GA10B"

# -----------------------------------------------------------------------
# Scenario 5: explicit empty -DGA10B_FIRMWARE_DIR= must defeat the
# auto-detect even when the default dir exists. Pins the
# `DEFINED CACHE{GA10B_FIRMWARE_DIR}` guard added in
# fix/jetson-default-build-flags. Uses the firmware-present fake
# HOME so the test would fail loudly if the override were dropped.
# -----------------------------------------------------------------------
echo ""
echo "=== Scenario 5: Jetson with -DGA10B_FIRMWARE_DIR= explicit empty ==="
configure_case "jetson-ga10b-empty" "JETSON_ORIN_NANO" "$FAKE_HOME_PRESENT" \
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
