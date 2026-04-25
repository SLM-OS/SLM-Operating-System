#!/bin/bash
#
# test-jetson-kexec-networking-smoke.sh - End-to-end Jetson USB networking smoke test
#
# Validates the currently supported lab path:
#   Linux on jetson-nano-2 with the Realtek hub + RTL8153 chain attached
#   -> slmos-kexec
#   -> SLM-OS shell on serial
#   -> net init / ifconfig / ping
#
# The script runs from the lab host. It uses:
#   - SSH/SCP for the Linux side on the Jetson
#   - ser2net TCP serial access for the SLM-OS shell side
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

SSH_TARGET="root@192.168.4.93"
CONSOLE_HOST="127.0.0.1"
CONSOLE_PORT="4004"
KERNEL="$REPO_ROOT/build/kernel/slmos.elf"
HELPER="$REPO_ROOT/scripts/jetson-kexec-slmos.sh"
REMOTE_KERNEL="/root/slmos.elf"
REMOTE_HELPER="/usr/local/bin/slmos-kexec"
EXPECTED_IP="192.168.4.5"
EXPECTED_GATEWAY="192.168.4.1"
PING_TARGET="192.168.4.1"
BOOT_TIMEOUT="120"
COMMAND_TIMEOUT="45"
KEXEC_TRIGGER_TIMEOUT="30"
DHCP_TIMEOUT="45"
POST_BOOT_SETTLE="5"
DHCP_RETRIES="1"
SKIP_COPY=0
SKIP_HELPER_COPY=0
KEEP_LOGS=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Smoke-test the validated Jetson USB networking kexec path.

Options:
  --ssh-target USER@HOST      SSH target for the Jetson (default: $SSH_TARGET)
  --console-host HOST         ser2net host for the Jetson console (default: $CONSOLE_HOST)
  --console-port PORT         ser2net TCP port for the Jetson console (default: $CONSOLE_PORT)
  --kernel PATH               Local SLM-OS ELF to deploy (default: $KERNEL)
  --helper PATH               Local slmos-kexec helper to deploy (default: $HELPER)
  --remote-kernel PATH        Remote kernel path on the Jetson (default: $REMOTE_KERNEL)
  --remote-helper PATH        Remote helper path on the Jetson (default: $REMOTE_HELPER)
  --expected-ip ADDR          Expected DHCP address in SLM-OS (default: $EXPECTED_IP)
  --expected-gateway ADDR     Expected gateway in SLM-OS (default: $EXPECTED_GATEWAY)
  --ping-target ADDR          Ping target for the smoke test (default: $PING_TARGET)
  --boot-timeout SEC          Timeout waiting for the SLM-OS prompt (default: $BOOT_TIMEOUT)
  --command-timeout SEC       Timeout waiting for each shell command (default: $COMMAND_TIMEOUT)
  --kexec-trigger-timeout SEC Timeout for the SSH session to drop after triggering kexec (default: $KEXEC_TRIGGER_TIMEOUT)
  --dhcp-timeout SEC          Timeout waiting for DHCP(bound) after net init (default: $DHCP_TIMEOUT)
  --post-boot-settle SEC      Delay after the first SLM-OS prompt before net init (default: $POST_BOOT_SETTLE)
  --dhcp-retries COUNT        Retry `ifconfig dhcp` after DHCP(failed) this many times (default: $DHCP_RETRIES)
  --skip-copy                 Reuse the already-installed kernel and helper on the Jetson
  --skip-helper-copy          Copy only the kernel, not the helper
  --keep-logs                 Keep the temporary console/SSH logs on success
  --help                      Show this help text
EOF
}

info() {
    printf '[INFO] %s\n' "$*"
}

warn() {
    printf '[WARN] %s\n' "$*" >&2
}

die() {
    printf '[ERROR] %s\n' "$*" >&2
    exit 1
}

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

escape_regex() {
    sed 's/[][\.^$*+?{}|()]/\\&/g' <<<"$1"
}

RUN_DIR=""
CONSOLE_LOG=""
SSH_LOG=""
LOG_MARK=0
CONSOLE_READER_PID=""

cleanup() {
    local rc=$?

    if [[ -n "${CONSOLE_READER_PID:-}" ]]; then
        kill "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
        wait "$CONSOLE_READER_PID" >/dev/null 2>&1 || true
    fi

    exec 3>&- 4<&- >/dev/null 2>&1 || true

    if [[ -n "${CONSOLE_PID:-}" ]]; then
        kill "$CONSOLE_PID" >/dev/null 2>&1 || true
        wait "$CONSOLE_PID" >/dev/null 2>&1 || true
    fi

    if [[ $rc -eq 0 && "$KEEP_LOGS" == "0" && -n "$RUN_DIR" ]]; then
        rm -rf "$RUN_DIR"
    elif [[ -n "$RUN_DIR" ]]; then
        info "logs preserved in $RUN_DIR"
    fi
}
trap cleanup EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in
        --ssh-target)
            SSH_TARGET="$2"
            shift 2
            ;;
        --console-host)
            CONSOLE_HOST="$2"
            shift 2
            ;;
        --console-port)
            CONSOLE_PORT="$2"
            shift 2
            ;;
        --kernel)
            KERNEL="$2"
            shift 2
            ;;
        --helper)
            HELPER="$2"
            shift 2
            ;;
        --remote-kernel)
            REMOTE_KERNEL="$2"
            shift 2
            ;;
        --remote-helper)
            REMOTE_HELPER="$2"
            shift 2
            ;;
        --expected-ip)
            EXPECTED_IP="$2"
            shift 2
            ;;
        --expected-gateway)
            EXPECTED_GATEWAY="$2"
            shift 2
            ;;
        --ping-target)
            PING_TARGET="$2"
            shift 2
            ;;
        --boot-timeout)
            BOOT_TIMEOUT="$2"
            shift 2
            ;;
        --command-timeout)
            COMMAND_TIMEOUT="$2"
            shift 2
            ;;
        --kexec-trigger-timeout)
            KEXEC_TRIGGER_TIMEOUT="$2"
            shift 2
            ;;
        --dhcp-timeout)
            DHCP_TIMEOUT="$2"
            shift 2
            ;;
        --post-boot-settle)
            POST_BOOT_SETTLE="$2"
            shift 2
            ;;
        --dhcp-retries)
            DHCP_RETRIES="$2"
            shift 2
            ;;
        --skip-copy)
            SKIP_COPY=1
            shift
            ;;
        --skip-helper-copy)
            SKIP_HELPER_COPY=1
            shift
            ;;
        --keep-logs)
            KEEP_LOGS=1
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown option: $1"
            ;;
    esac
done

need_cmd ssh
need_cmd scp
need_cmd nc
need_cmd timeout
need_cmd grep
need_cmd tail
need_cmd wc
need_cmd mktemp

[[ "$SKIP_COPY" == "1" || -f "$KERNEL" ]] || die "kernel not found: $KERNEL"
[[ "$SKIP_HELPER_COPY" == "1" || -f "$HELPER" ]] || die "helper not found: $HELPER"

RUN_DIR="$(mktemp -d /tmp/jetson-kexec-net-smoke.XXXXXX)"
CONSOLE_LOG="$RUN_DIR/console.log"
SSH_LOG="$RUN_DIR/ssh.log"
touch "$CONSOLE_LOG" "$SSH_LOG"

tail_since_mark() {
    local start=$((LOG_MARK + 1))
    tail -c +"$start" "$CONSOLE_LOG" 2>/dev/null || true
}

mark_log() {
    LOG_MARK="$(wc -c <"$CONSOLE_LOG")"
}

wait_for_since() {
    local desc="$1"
    local pattern="$2"
    local timeout_secs="$3"
    local start now

    start="$(date +%s)"
    while true; do
        if tail_since_mark | grep -Eq -- "$pattern"; then
            info "$desc"
            return 0
        fi
        now="$(date +%s)"
        if (( now - start >= timeout_secs )); then
            warn "timed out waiting for: $desc"
            tail_since_mark >&2 || true
            return 1
        fi
        sleep 1
    done
}

send_console() {
    local line="$1"
    info "console << $line"
    printf '%s\r' "$line" >&3
}

assert_since() {
    local desc="$1"
    local pattern="$2"
    if tail_since_mark | grep -Eq -- "$pattern"; then
        info "$desc"
        return 0
    fi
    warn "missing expected output: $desc"
    tail_since_mark >&2 || true
    return 1
}

wait_for_dhcp_bound() {
    local timeout_secs="$1"
    local start now
    local retries_remaining="$DHCP_RETRIES"

    start="$(date +%s)"
    while true; do
        mark_log
        send_console "ifconfig"
        wait_for_since "prompt returned after ifconfig" 'slmos>' "$COMMAND_TIMEOUT"

        if tail_since_mark | grep -Eq 'sl0: flags=.*DHCP\(bound\)'; then
            info "ifconfig reports DHCP(bound)"
            assert_since "ifconfig reports expected IP" "inet[[:space:]]+$(escape_regex "$EXPECTED_IP")"
            assert_since "ifconfig reports expected gateway" "gateway[[:space:]]+$(escape_regex "$EXPECTED_GATEWAY")"
            return 0
        fi

        if tail_since_mark | grep -Eq 'sl0: flags=.*DHCP\(failed\)'; then
            if (( retries_remaining > 0 )); then
                retries_remaining=$((retries_remaining - 1))
                info "DHCP fell back to static config; retrying ifconfig dhcp"
                mark_log
                send_console "ifconfig dhcp"
                wait_for_since "prompt returned after DHCP retry" 'slmos>' "$COMMAND_TIMEOUT"
                sleep 1
                continue
            fi
            warn "DHCP reported failure before reaching the expected lease"
            tail_since_mark >&2 || true
            return 1
        fi

        now="$(date +%s)"
        if (( now - start >= timeout_secs )); then
            warn "timed out waiting for DHCP(bound)"
            tail_since_mark >&2 || true
            return 1
        fi

        info "DHCP still pending; retrying"
        sleep 1
    done
}

info "connecting to console tcp:$CONSOLE_HOST:$CONSOLE_PORT"
coproc CONSOLE { nc "$CONSOLE_HOST" "$CONSOLE_PORT"; }
exec 3>&${CONSOLE[1]} 4<&${CONSOLE[0]}
cat <&4 >>"$CONSOLE_LOG" &
CONSOLE_READER_PID=$!
sleep 1

if [[ "$SKIP_COPY" != "1" ]]; then
    info "copying kernel to $SSH_TARGET:$REMOTE_KERNEL"
    scp -o StrictHostKeyChecking=no "$KERNEL" "$SSH_TARGET:$REMOTE_KERNEL" >>"$SSH_LOG" 2>&1

    if [[ "$SKIP_HELPER_COPY" != "1" ]]; then
        info "copying helper to $SSH_TARGET:$REMOTE_HELPER"
        scp -o StrictHostKeyChecking=no "$HELPER" "$SSH_TARGET:$REMOTE_HELPER" >>"$SSH_LOG" 2>&1
        ssh -o StrictHostKeyChecking=no "$SSH_TARGET" "chmod +x '$REMOTE_HELPER'" >>"$SSH_LOG" 2>&1
    fi
else
    info "skipping remote copy"
fi

mark_log
info "triggering kexec via $SSH_TARGET"
if ! timeout "$KEXEC_TRIGGER_TIMEOUT" \
      ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
      "$SSH_TARGET" "$REMOTE_HELPER '$REMOTE_KERNEL'" >>"$SSH_LOG" 2>&1; then
    info "kexec SSH session exited non-zero or timed out during disconnect window (expected once Linux is replaced)"
fi

wait_for_since "SLM-OS prompt reached after kexec" 'slmos>' "$BOOT_TIMEOUT"

if [[ "$POST_BOOT_SETTLE" != "0" ]]; then
    info "waiting ${POST_BOOT_SETTLE}s for post-boot USB/Ethernet settle"
    sleep "$POST_BOOT_SETTLE"
fi

mark_log
send_console "net init"
wait_for_since "prompt returned after net init" 'slmos>' "$COMMAND_TIMEOUT"

mark_log
send_console "ifconfig dhcp"
wait_for_since "prompt returned after ifconfig dhcp" 'slmos>' "$COMMAND_TIMEOUT"

wait_for_dhcp_bound "$DHCP_TIMEOUT"

mark_log
send_console "ping $PING_TARGET 2"
wait_for_since "prompt returned after ping" 'slmos>' "$COMMAND_TIMEOUT"
assert_since "ping reports no packet loss" '2 packets transmitted, 2 received, 0% packet loss'

info "Jetson kexec USB networking smoke passed"
info "target=$SSH_TARGET console=tcp:$CONSOLE_HOST:$CONSOLE_PORT ip=$EXPECTED_IP gw=$EXPECTED_GATEWAY ping=$PING_TARGET"
