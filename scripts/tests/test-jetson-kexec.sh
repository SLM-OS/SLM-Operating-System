#!/bin/bash
#
# test-jetson-kexec.sh - Functional tests for jetson-kexec-slmos.sh
#
# Runs on any Linux host (no Jetson required). Exercises error paths
# and non-destructive behavior. Actual kexec invocation is never
# reached because we don't run as root — the script bails out before
# touching kexec.
#
set -euo pipefail

SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/jetson-kexec-slmos.sh"
PASS=0
FAIL=0

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

# Assert command exits with expected code and stderr contains pattern
assert_fails_with() {
    local desc="$1"
    local expected_code="$2"
    local expected_pattern="$3"
    shift 3

    local output
    local rc=0
    output="$("$@" 2>&1)" || rc=$?

    if [[ $rc -ne $expected_code ]]; then
        fail "$desc: expected exit $expected_code, got $rc"
        return
    fi
    if [[ -n "$expected_pattern" ]] && ! grep -q -- "$expected_pattern" <<<"$output"; then
        fail "$desc: output missing pattern '$expected_pattern'"
        echo "    Got: $output" >&2
        return
    fi
    pass "$desc"
}

echo "=== jetson-kexec-slmos.sh functional tests ==="
echo "Script: $SCRIPT"
echo

# Test 1: Script exists and is executable
if [[ -x "$SCRIPT" ]]; then
    pass "Script exists and is executable"
else
    fail "Script missing or not executable: $SCRIPT"
    exit 1
fi

# Test 2: Syntax check
if bash -n "$SCRIPT"; then
    pass "Script has valid bash syntax"
else
    fail "Script has syntax errors"
fi

# Test 3: shellcheck (only if available)
if command -v shellcheck >/dev/null 2>&1; then
    if shellcheck "$SCRIPT"; then
        pass "shellcheck passes"
    else
        fail "shellcheck reports issues"
    fi
else
    echo "  [SKIP] shellcheck not installed"
fi

# Test 4: Nonexistent kernel file → exit 1 with clear error
assert_fails_with "Nonexistent kernel file is rejected" \
    1 "not found" \
    "$SCRIPT" /does/not/exist.elf

# Test 5: Non-root invocation with valid file → exit 1 with clear error
tmpfile="$(mktemp)"
trap 'rm -f "$tmpfile"' EXIT
echo "stub" > "$tmpfile"

if [[ $EUID -eq 0 ]]; then
    echo "  [SKIP] Non-root test (already running as root)"
else
    assert_fails_with "Non-root invocation is rejected" \
        1 "must run as root" \
        "$SCRIPT" "$tmpfile"

    assert_fails_with "Non-root invocation with --no-gpu-suspend is rejected" \
        1 "must run as root" \
        "$SCRIPT" --no-gpu-suspend "$tmpfile"

    assert_fails_with "Non-root invocation with --gpu-suspend is rejected" \
        1 "must run as root" \
        "$SCRIPT" --gpu-suspend "$tmpfile"

    assert_fails_with "Non-root invocation with --no-usb-hold is rejected" \
        1 "must run as root" \
        "$SCRIPT" --no-usb-hold "$tmpfile"

    assert_fails_with "Non-root invocation with --no-smmu-fix is rejected" \
        1 "must run as root" \
        "$SCRIPT" --no-smmu-fix "$tmpfile"

    assert_fails_with "Non-root invocation with --no-usb-root-cleanup is rejected" \
        1 "must run as root" \
        "$SCRIPT" --no-usb-root-cleanup "$tmpfile"

    assert_fails_with "Non-root invocation with all supported flags is rejected" \
        1 "must run as root" \
        "$SCRIPT" --no-gpu-suspend --no-usb-hold --no-smmu-fix --no-usb-root-cleanup "$tmpfile"
fi

# Test 6: helper selection (#1029). --print-helpers resolves and exits 0
# without root, so these run on any host.
assert_prints() {
    local desc="$1" expected="$2"
    shift 2
    local out rc=0
    out="$("$@" 2>&1)" || rc=$?
    if [[ $rc -eq 0 && "$out" == "$expected" ]]; then
        pass "$desc"
    else
        fail "$desc: expected '$expected' (rc 0), got '$out' (rc $rc)"
    fi
}
assert_prints "Default helper selection is mnist,sched" \
    "mnist,sched" env -u SLMOS_HELPERS "$SCRIPT" --print-helpers
assert_prints "--helpers=eviction selects only the eviction producer" \
    "eviction" "$SCRIPT" --helpers=eviction --print-helpers
assert_prints "--helpers keeps order and drops duplicates" \
    "eviction,mnist" "$SCRIPT" --helpers=eviction,mnist,eviction --print-helpers
assert_prints "SLMOS_HELPERS env selects helpers" \
    "sched" env SLMOS_HELPERS=sched "$SCRIPT" --print-helpers
assert_prints "--helpers overrides SLMOS_HELPERS" \
    "mnist" env SLMOS_HELPERS=sched "$SCRIPT" --helpers=mnist --print-helpers
assert_prints "--no-helper wins over --helpers" \
    "none" "$SCRIPT" --helpers=eviction --no-helper --print-helpers
assert_prints "--helpers=none selects nothing" \
    "none" "$SCRIPT" --helpers=none --print-helpers
assert_fails_with "Unknown helper name is rejected" \
    2 "unknown helper 'mnsit'" \
    "$SCRIPT" --helpers=mnist,mnsit --print-helpers
assert_fails_with "Empty --helpers= is rejected" \
    2 "empty helper list" \
    "$SCRIPT" --helpers= --print-helpers
assert_fails_with "Unknown helper in SLMOS_HELPERS is rejected" \
    2 "unknown helper 'gpu'" \
    env SLMOS_HELPERS=gpu "$SCRIPT" --print-helpers
if [[ $EUID -ne 0 ]]; then
    assert_fails_with "Valid --helpers still reaches the root check" \
        1 "must run as root" \
        "$SCRIPT" --helpers=eviction "$tmpfile"
fi

# Test 7: Script does not call kexec if preconditions fail
# (implicit in tests 4 and 5 — if kexec were called, it would fail
#  differently or actually attempt to run, which would error on a host
#  without the kexec binary; our exit path is before kexec is invoked)

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
