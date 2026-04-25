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

# Test 6: Script does not call kexec if preconditions fail
# (implicit in tests 4 and 5 — if kexec were called, it would fail
#  differently or actually attempt to run, which would error on a host
#  without the kexec binary; our exit path is before kexec is invoked)

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
