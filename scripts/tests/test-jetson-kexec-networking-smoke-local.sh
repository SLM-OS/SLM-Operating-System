#!/bin/bash
#
# test-jetson-kexec-networking-smoke-local.sh - Local functional tests for
# the Jetson USB networking smoke harness option handling.
#
# These tests do not require a Jetson. They only validate the host-side
# argument/preflight behavior of test-jetson-kexec-networking-smoke.sh.
#
set -euo pipefail

SCRIPT="$(cd "$(dirname "$0")/.." && pwd)/tests/test-jetson-kexec-networking-smoke.sh"
PASS=0
FAIL=0
TMP_ROOT="$(mktemp -d)"
FAKEBIN="$TMP_ROOT/fakebin"
mkdir -p "$FAKEBIN"

pass() { echo "  [PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "  [FAIL] $1"; FAIL=$((FAIL + 1)); }

cleanup() {
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

cat >"$FAKEBIN/ssh" <<'EOF'
#!/bin/bash
echo "fake ssh invoked" >&2
exit 1
EOF

cat >"$FAKEBIN/nc" <<'EOF'
#!/bin/bash
sleep 5
EOF

cat >"$FAKEBIN/timeout" <<'EOF'
#!/bin/bash
shift
exec "$@"
EOF

chmod +x "$FAKEBIN/ssh" "$FAKEBIN/nc" "$FAKEBIN/timeout"

echo "=== test-jetson-kexec-networking-smoke.sh local functional tests ==="
echo "Script: $SCRIPT"
echo

if [[ -x "$SCRIPT" ]]; then
    pass "Script exists and is executable"
else
    fail "Script missing or not executable: $SCRIPT"
    exit 1
fi

if bash -n "$SCRIPT"; then
    pass "Script has valid bash syntax"
else
    fail "Script has syntax errors"
fi

output="$(
    PATH="$FAKEBIN:$PATH" \
    "$SCRIPT" \
        --skip-copy \
        --kernel /does/not/exist.elf \
        --helper /does/not/exist-helper.sh \
        --ssh-target root@example.invalid \
        --console-host 127.0.0.1 \
        --console-port 65535 \
        --boot-timeout 0 \
        --kexec-trigger-timeout 1 \
        --command-timeout 1 \
        --dhcp-timeout 1 \
        --post-boot-settle 0 \
        2>&1
)" || rc=$?

rc="${rc:-0}"
if [[ "$rc" -eq 0 ]]; then
    fail "--skip-copy unexpectedly succeeded"
elif grep -q "helper not found" <<<"$output"; then
    fail "--skip-copy still validates the local helper path"
elif grep -q "kernel not found" <<<"$output"; then
    fail "--skip-copy still validates the local kernel path"
elif grep -q "missing required command: scp" <<<"$output"; then
    fail "--skip-copy still requires scp during preflight"
else
    pass "--skip-copy bypasses local kernel/helper validation and scp preflight"
fi

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
