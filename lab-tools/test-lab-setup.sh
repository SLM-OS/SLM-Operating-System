#!/bin/bash
#
# Functional tests for lab-tools setup
#
# This script verifies that all lab components are working:
# - Smart plug control (jetson-power.py)
# - Serial communication (UART)
# - SSH connectivity
#
# Usage:
#     ./test-lab-setup.sh           # Run all tests
#     ./test-lab-setup.sh --quick   # Skip power cycle test
#

# Note: Don't use set -e as it causes issues with test functions that can fail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="$SCRIPT_DIR/lab-settings.cfg"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test counters
TESTS_PASSED=0
TESTS_FAILED=0
TESTS_SKIPPED=0

# Load config
if [ -f "$CONFIG_FILE" ]; then
    source "$CONFIG_FILE"
fi

UART_PORT="${JETSON_UART_PORT:-/dev/ttyUSB0}"
BAUD="${SERIAL_BAUD:-115200}"
SSH_HOST="root@gradient-nano.onthewifi.com"
SSH_PORT="4243"
QUICK_MODE=false

# Parse arguments
if [ "$1" = "--quick" ]; then
    QUICK_MODE=true
fi

#------------------------------------------------------------------------------
# Helper functions
#------------------------------------------------------------------------------

pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    ((TESTS_PASSED++))
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    ((TESTS_FAILED++))
}

skip() {
    echo -e "${YELLOW}[SKIP]${NC} $1"
    ((TESTS_SKIPPED++))
}

info() {
    echo -e "       $1"
}

section() {
    echo ""
    echo "=============================================="
    echo " $1"
    echo "=============================================="
}

ssh_cmd() {
    ssh -o ConnectTimeout=5 -o BatchMode=yes -p "$SSH_PORT" "$SSH_HOST" "$@" 2>/dev/null
}

#------------------------------------------------------------------------------
# Tests
#------------------------------------------------------------------------------

test_config_file() {
    section "Configuration File"

    if [ -f "$CONFIG_FILE" ]; then
        pass "Config file exists: $CONFIG_FILE"

        if [ -n "$KASA_PLUG_IP" ]; then
            pass "KASA_PLUG_IP is set: $KASA_PLUG_IP"
        else
            fail "KASA_PLUG_IP is not set"
        fi

        if [ -n "$JETSON_UART_PORT" ]; then
            pass "JETSON_UART_PORT is set: $JETSON_UART_PORT"
        else
            fail "JETSON_UART_PORT is not set"
        fi
    else
        fail "Config file not found: $CONFIG_FILE"
    fi
}

test_smart_plug_status() {
    section "Smart Plug (jetson-power.py)"

    if ! command -v python3 &> /dev/null; then
        skip "python3 not found"
        return
    fi

    echo "Testing: status command..."
    if OUTPUT=$("$SCRIPT_DIR/jetson-power.py" status 2>&1); then
        pass "jetson-power.py status succeeded"
        if echo "$OUTPUT" | grep -q "Jetson Power"; then
            pass "Plug alias 'Jetson Power' detected"
        else
            info "Note: Plug alias not detected (may be unnamed)"
        fi
        if echo "$OUTPUT" | grep -q "on\|off"; then
            STATE=$(echo "$OUTPUT" | grep -oE "is (on|off)" | head -1)
            pass "Power state: $STATE"
        fi
    else
        fail "jetson-power.py status failed"
        info "$OUTPUT"
    fi
}

test_ssh_connectivity() {
    section "SSH Connectivity"

    echo "Testing: SSH connection to Jetson..."
    if OUTPUT=$(ssh_cmd "echo 'SSH_OK'; uname -n" 2>&1); then
        if echo "$OUTPUT" | grep -q "SSH_OK"; then
            pass "SSH connection successful"
            HOSTNAME=$(echo "$OUTPUT" | tail -1)
            info "Hostname: $HOSTNAME"
        else
            fail "SSH connected but unexpected output"
        fi
    else
        fail "SSH connection failed"
        info "Check: Is Jetson running Linux? Is SSH key configured?"
        return 1
    fi

    # Test Jetson is running Linux (not SLM-OS)
    echo "Testing: Jetson is running Linux..."
    if OUTPUT=$(ssh_cmd "cat /etc/os-release | head -1" 2>&1); then
        pass "Jetson is running Linux"
        info "$OUTPUT"
    else
        fail "Could not verify Linux is running"
    fi
}

test_serial_port() {
    section "Serial Port"

    echo "Testing: Serial port exists..."
    if [ -e "$UART_PORT" ]; then
        pass "Serial port exists: $UART_PORT"
    else
        fail "Serial port not found: $UART_PORT"
        info "Available ports:"
        ls -la /dev/ttyUSB* /dev/ttyACM* 2>/dev/null || info "  (none)"
        return 1
    fi

    echo "Testing: Serial port permissions..."
    if [ -r "$UART_PORT" ] && [ -w "$UART_PORT" ]; then
        pass "Serial port is readable/writable"
    else
        info "Serial port requires sudo (normal if not in dialout group)"
    fi
}

test_serial_communication() {
    section "Serial Communication"

    # Check if SSH is working first
    if ! ssh_cmd "true" 2>/dev/null; then
        skip "SSH not available - cannot test serial from Jetson side"
        return
    fi

    if [ ! -e "$UART_PORT" ]; then
        skip "Serial port not available"
        return
    fi

    echo "Testing: Jetson -> Local serial..."

    # Generate unique test string
    TEST_STRING="SERIAL_TEST_$(date +%s)"

    # Set up local receiver with timeout, send from Jetson
    sudo stty -F "$UART_PORT" "$BAUD" raw -echo 2>/dev/null

    # Use a subshell to handle the background process properly
    rm -f /tmp/serial_test_output
    {
        sudo timeout 3 cat "$UART_PORT" > /tmp/serial_test_output 2>/dev/null &
        RECEIVER_PID=$!
        sleep 0.5
        ssh_cmd "stty -F /dev/ttyTHS1 $BAUD raw -echo; echo '$TEST_STRING' > /dev/ttyTHS1" 2>/dev/null
        wait $RECEIVER_PID 2>/dev/null || true
    }

    # Check result
    RECEIVED=""
    if [ -f /tmp/serial_test_output ]; then
        RECEIVED=$(cat /tmp/serial_test_output)
        rm -f /tmp/serial_test_output
    fi

    if echo "$RECEIVED" | grep -q "$TEST_STRING"; then
        pass "Jetson -> Local serial working"
    else
        fail "Jetson -> Local serial failed"
        info "Expected: $TEST_STRING"
        info "Received: $(echo "$RECEIVED" | head -c 50)"
    fi

    echo "Testing: Local -> Jetson serial..."

    # Generate unique test string
    TEST_STRING="LOCAL_TEST_$(date +%s)"

    # Start receiver on Jetson, send from local, capture result
    RECEIVED=$(ssh_cmd "stty -F /dev/ttyTHS1 $BAUD raw -echo; timeout 3 cat /dev/ttyTHS1 | xxd" 2>/dev/null &
        JETSON_PID=$!
        sleep 0.5
        echo "$TEST_STRING" | sudo tee "$UART_PORT" > /dev/null
        wait $JETSON_PID 2>/dev/null || true
    )

    if echo "$RECEIVED" | grep -q "LOCAL_TEST"; then
        pass "Local -> Jetson serial working"
    else
        fail "Local -> Jetson serial failed or inconclusive"
        info "Check: Is Jetson serial port in use by another process?"
    fi
}

test_power_cycle() {
    section "Power Cycle (Destructive Test)"

    if [ "$QUICK_MODE" = true ]; then
        skip "Power cycle test (use without --quick to run)"
        return
    fi

    echo "WARNING: This will power cycle the Jetson!"
    echo "Press Ctrl-C within 5 seconds to cancel..."
    sleep 5

    echo "Testing: Power off..."
    if "$SCRIPT_DIR/jetson-power.py" off 2>&1; then
        pass "Power off command succeeded"
        sleep 2

        # Verify SSH is down
        if ssh_cmd "true" 2>/dev/null; then
            fail "Jetson still responding after power off"
        else
            pass "Jetson not responding (power is off)"
        fi
    else
        fail "Power off command failed"
        return
    fi

    echo "Testing: Power on..."
    if "$SCRIPT_DIR/jetson-power.py" on 2>&1; then
        pass "Power on command succeeded"

        echo "Waiting for Jetson to boot (30 seconds)..."
        sleep 30

        # Verify SSH is up
        if ssh_cmd "true" 2>/dev/null; then
            pass "Jetson responding after power on"
        else
            fail "Jetson not responding after power on (may need more time)"
        fi
    else
        fail "Power on command failed"
    fi
}

#------------------------------------------------------------------------------
# Main
#------------------------------------------------------------------------------

echo ""
echo "Lab Setup Functional Tests"
echo "=========================="
echo "Config: $CONFIG_FILE"
echo "UART:   $UART_PORT @ ${BAUD}bps"
echo "SSH:    $SSH_HOST:$SSH_PORT"
if [ "$QUICK_MODE" = true ]; then
    echo "Mode:   Quick (skipping power cycle)"
fi

test_config_file
test_smart_plug_status
test_ssh_connectivity
test_serial_port
test_serial_communication

if [ "$QUICK_MODE" = false ]; then
    test_power_cycle
fi

# Summary
section "Test Summary"
echo -e "Passed:  ${GREEN}$TESTS_PASSED${NC}"
echo -e "Failed:  ${RED}$TESTS_FAILED${NC}"
echo -e "Skipped: ${YELLOW}$TESTS_SKIPPED${NC}"
echo ""

if [ "$TESTS_FAILED" -gt 0 ]; then
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
else
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
fi
