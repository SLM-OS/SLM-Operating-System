#!/bin/bash
#
# test-jetson-deploy-retry-matcher.sh - Unit test for the
# `capture_until_match` helper in jetson-deploy-retry.sh (#935).
#
# Before #935, the deploy script declared PASS even when `labctl serial
# capture` timed out with zero matched lines, because the wrapping tool
# exits 0 on timeout. The new helper distinguishes match from timeout
# by parsing labctl's trailing status line. This test stubs labctl with
# a fake that emits known status lines and asserts the helper's exit
# code matches.
#
# Runs on any Linux host. No Jetson required.
set -euo pipefail

# Source the helper. The script defines `capture_until_match` near the
# top of attempt_deploy; we source the whole file but skip the main
# control flow by setting a guard env var that doesn't exist — sourcing
# defines functions but doesn't invoke the bottom-of-file loop because
# this test exits before reaching it.
#
# To make sourcing safe, we override the parser-driven argument loop's
# preconditions: set ATTEMPTS=0 so the for-loop doesn't fire, and stub
# labctl/log so any incidental calls don't escape.
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEPLOY_SCRIPT="$SCRIPT_DIR/jetson-deploy-retry.sh"

if [[ ! -f "$DEPLOY_SCRIPT" ]]; then
    echo "FAIL: $DEPLOY_SCRIPT not found"
    exit 2
fi

# Stub PATH with a fake labctl that emits a configurable status line.
# Each test case writes the desired stub output to $LABCTL_STUB_OUT and
# the desired exit code to $LABCTL_STUB_RC; the stub reads those env
# vars and replays them.
STUB_DIR=$(mktemp -d)
trap 'rm -rf "$STUB_DIR"' EXIT

cat > "$STUB_DIR/labctl" <<'STUB'
#!/bin/bash
# Print the canned output and exit with the canned rc.
printf '%s\n' "${LABCTL_STUB_OUT:-}"
exit "${LABCTL_STUB_RC:-0}"
STUB
chmod +x "$STUB_DIR/labctl"
export PATH="$STUB_DIR:$PATH"

# Extract just the helper function definition from the deploy script to
# avoid running its top-level arg parser. We sed out the function body
# from `capture_until_match() {` through its closing `^}`.
HELPER_FILE=$(mktemp)
trap 'rm -rf "$STUB_DIR" "$HELPER_FILE"' EXIT

# Match from `capture_until_match() {` up to the next `^}` at column 0.
awk '
    /^capture_until_match\(\) \{$/ { in_fn = 1 }
    in_fn { print }
    in_fn && /^}$/ { exit }
' "$DEPLOY_SCRIPT" > "$HELPER_FILE"

# Sanity check: the extraction must have at least 3 lines (signature,
# body, closing brace). Otherwise the regex drifted and the test would
# silently pass without exercising anything.
if (( $(wc -l < "$HELPER_FILE") < 3 )); then
    echo "FAIL: failed to extract capture_until_match from $DEPLOY_SCRIPT"
    echo "      sed-out file has only $(wc -l < "$HELPER_FILE") lines"
    exit 2
fi

# shellcheck disable=SC1090
source "$HELPER_FILE"

PASS=0
FAIL=0
LOG_FILE=$(mktemp)
trap 'rm -rf "$STUB_DIR" "$HELPER_FILE" "$LOG_FILE"' EXIT

run_case() {
    local desc="$1" stub_out="$2" stub_rc="$3" expect_rc="$4"
    # Must `export` — the labctl stub runs as a subprocess and only
    # sees the value through the environment.
    export LABCTL_STUB_OUT="$stub_out"
    export LABCTL_STUB_RC="$stub_rc"
    : > "$LOG_FILE"
    set +e
    capture_until_match port 1 "slmos>" 5 "$LOG_FILE"
    local got_rc=$?
    set -e
    if [[ "$got_rc" == "$expect_rc" ]]; then
        echo "  [PASS] $desc (rc=$got_rc)"
        PASS=$((PASS + 1))
    else
        echo "  [FAIL] $desc (expected rc=$expect_rc, got $got_rc)"
        echo "         stub_out=<<<$stub_out>>>"
        FAIL=$((FAIL + 1))
    fi
}

echo "test capture_until_match — labctl status-line parsing"

# Happy path: labctl exit 0, status line says "matched]". Helper returns 0.
run_case \
    "matched pattern → rc=0" \
    "[Captured 5 lines in 0.1s, pattern 'slmos>' matched]
content line 1
slmos>" \
    0 \
    0

# The bug from #935: labctl exit 0 but status line says "timeout".
# Pre-fix the helper would have returned 0 (false PASS); now it must
# return 1.
run_case \
    "timeout with labctl rc=0 → rc=1 (the #935 false-PASS path)" \
    "[Captured 0 lines in 90.0s, timeout]" \
    0 \
    1

# labctl itself errors out (e.g. port not bound). Helper returns 1.
run_case \
    "labctl non-zero exit → rc=1" \
    "Error: port not bound" \
    2 \
    1

# Edge case: empty output (shouldn't happen in practice, but the helper
# must not return success on it).
run_case \
    "empty labctl output → rc=1" \
    "" \
    0 \
    1

# Edge case: status line has trailing whitespace. The grep uses \s*$.
run_case \
    "trailing whitespace on status line → rc=0" \
    "[Captured 1 lines in 0.1s, pattern 'slmos>' matched]   " \
    0 \
    0

echo
echo "Results: $PASS passed, $FAIL failed"
if (( FAIL > 0 )); then
    exit 1
fi
