#!/bin/bash
#
# jetson-deploy-retry.sh — Deploy SLM-OS to Jetson via kexec with retry on
# GPU bringup failure.
#
# Problem this works around: #788 / #819. Channel inheritance across the
# Linux→SLM-OS kexec hop is only ~60% reliable per attempt — a fraction
# of nvgpu's GR context buffers and their phys-adjacent allocations get
# clobbered by SLM-OS's PMM during boot, and the FECS ucode then trips
# during the first dispatch (Mode A `badf` reads, Mode B SM warp_esr,
# Mode D ctxsw_checksum, etc.).
#
# Until #819 lands a deterministic handoff, this script re-attempts the
# whole deploy cycle (power_cycle + Linux boot + kexec + bringup test)
# up to N times until the launch-kernel smoke test passes.
#
# Each attempt costs ~2:20 wall clock (Linux boot dominates at ~55s).
# At default --retries=4 (5 total attempts) and p=0.6 per-attempt PASS:
#   - Mean time-to-success: ~3:08
#   - P(success): 98.96%
#   - Worst case wall clock: ~11:40
#
# Usage:
#   scripts/jetson-deploy-retry.sh [options]
#
# Options:
#   -k, --kernel PATH       SLM-OS ELF to deploy (default: build/kernel/slmos.elf)
#   -t, --target NAME       labctl target name (default: jetson-nano-2)
#   -i, --ip ADDR           Jetson IP for ssh/scp (default: 192.168.4.5)
#   -u, --user USER         ssh user (default: root)
#   -r, --retries N         Max retry count after first attempt (default: 4,
#                           so 5 attempts total)
#   --skip-test             Just deploy and confirm shell — skip the GPU
#                           launch-kernel validation step. Use this if the
#                           build under test doesn't depend on GPU init.
#   --log-dir DIR           Per-attempt log directory (default: a fresh
#                           /tmp/jetson-deploy-YYYYMMDD-HHMMSS)
#   -h, --help              Show this message
#
# Environment overrides:
#   SLMOS_HELPER_DIR        Forwarded to slmos-kexec (default /root/gpu-mnist)
#
# Exit codes:
#   0   — deploy succeeded (within retry budget)
#   1   — exhausted retries without a passing attempt
#   2   — fatal setup error (e.g. ELF missing, labctl unreachable)
#
set -u

KERNEL="build/kernel/slmos.elf"
TARGET="jetson-nano-2"
JET_IP="192.168.4.5"
SSH_USER="root"
RETRIES=4
SKIP_TEST=0
LOG_DIR=""

# Per-stage timeouts (seconds). Hoisted so future board / firmware changes
# only touch one place.
LINUX_LOGIN_TIMEOUT=90
SSH_READY_TIMEOUT=90
SLMOS_SHELL_TIMEOUT=90

usage() {
    cat <<'EOF'
Usage: jetson-deploy-retry.sh [options]

Deploy SLM-OS to Jetson via kexec, retrying on GPU bringup failure
(workaround for #788; long-term fix tracked in #819).

Options:
  -k, --kernel PATH    SLM-OS ELF to deploy (default: build/kernel/slmos.elf)
  -t, --target NAME    labctl target name (default: jetson-nano-2)
  -i, --ip ADDR        Jetson IP for ssh/scp (default: 192.168.4.5)
  -u, --user USER      ssh user (default: root)
  -r, --retries N      Max retries after first attempt (default: 4)
  --skip-test          Skip GPU launch-kernel validation (shell-only deploy)
  --log-dir DIR        Per-attempt log directory (default: /tmp/jetson-deploy-<TS>)
  -h, --help           Show this message

Exit codes:
  0   Deploy succeeded within the retry budget
  1   Retry budget exhausted without a passing attempt
  2   Fatal setup error (ELF missing, labctl unreachable, etc.)
EOF
    exit "${1:-0}"
}

while (( $# > 0 )); do
    case "$1" in
        -k|--kernel)   KERNEL="$2"; shift 2 ;;
        -t|--target)   TARGET="$2"; shift 2 ;;
        -i|--ip)       JET_IP="$2"; shift 2 ;;
        -u|--user)     SSH_USER="$2"; shift 2 ;;
        -r|--retries)
            [[ "${2:-}" =~ ^[0-9]+$ ]] || {
                echo "Error: --retries requires non-negative integer, got: ${2:-<empty>}" >&2
                exit 2
            }
            RETRIES="$2"; shift 2 ;;
        --skip-test)   SKIP_TEST=1; shift ;;
        --log-dir)     LOG_DIR="$2"; shift 2 ;;
        -h|--help)     usage 0 ;;
        *) echo "Unknown argument: $1" >&2; usage 2 ;;
    esac
done

if [[ ! -f "$KERNEL" ]]; then
    echo "Error: kernel ELF not found: $KERNEL" >&2
    exit 2
fi

if ! command -v labctl >/dev/null 2>&1; then
    echo "Error: labctl not on PATH — install Embedded-Lab-Control" >&2
    exit 2
fi

if [[ -z "$LOG_DIR" ]]; then
    LOG_DIR="/tmp/jetson-deploy-$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$LOG_DIR"

SUMMARY="$LOG_DIR/summary.txt"

# On Ctrl-C / SIGTERM, leave a breadcrumb in the summary so post-mortem
# `cat summary.txt` shows the run was interrupted (and at which attempt).
# Intentionally do NOT auto-release the labctl claim — the operator may
# want to inspect Jetson state before releasing.
trap 'printf "\nINTERRUPTED at %s\n" "$(date -Is)" | tee -a "$SUMMARY" >&2; exit 130' INT TERM

ATTEMPTS=$((RETRIES + 1))
SSH_OPTS=(
    -o ConnectTimeout=5
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o BatchMode=yes
)

log() {
    printf '%s\n' "$*" | tee -a "$SUMMARY"
}

log "jetson-deploy-retry"
log "  kernel:   $KERNEL"
log "  target:   $TARGET ($JET_IP)"
log "  attempts: up to $ATTEMPTS (1 + $RETRIES retries)"
log "  logs:     $LOG_DIR"
log "  test:     $([[ "$SKIP_TEST" == "1" ]] && echo 'skip (shell-only)' || echo 'nvgpu launch-kernel')"
log "  started:  $(date -Is)"
log ""

# Capture serial output and verify a pattern was actually matched (#935).
#
# `labctl serial capture --until PATTERN` exits 0 even when the pattern
# never matched and the capture timed out. Relying on its exit code alone
# turned "kexec went silent, BL31 reset, no SLM-OS output" into a false
# PASS during PMU #60 hardware validation. This wrapper:
#
#   1. Captures stdout+stderr into a variable so the actual lines are
#      available for inspection.
#   2. Tees them into the per-attempt log_file (same as the original).
#   3. Fails the step if the trailing `[N lines, T, ...]` status line
#      reports `timeout` instead of `... matched]`.
#
# Args: <port-name> <timeout-sec> <pattern> <tail-n> <log-file>
# Returns: 0 if labctl exited 0 AND the status line reports a match,
#          1 otherwise.
capture_until_match() {
    local port="$1" timeout="$2" pattern="$3" tail_n="$4" log_file="$5"
    local out rc
    out=$(labctl serial capture "$port" -t "$timeout" -u "$pattern" -n "$tail_n" 2>&1)
    rc=$?
    printf '%s\n' "$out" >> "$log_file"
    if (( rc != 0 )); then
        return 1
    fi
    # labctl prints a status line like `[Captured N lines in T, pattern '...' matched]`
    # on success or `[Captured 0 lines in T, timeout]` on timeout. We match
    # the explicit "matched]" suffix on any line of the capture output —
    # fragile to format changes but lets us distinguish "saw the pattern"
    # from "ran the clock out." Status line need not be the last line of
    # output (labctl may emit benign trailers after it).
    if printf '%s\n' "$out" | grep -qE 'matched\]\s*$'; then
        return 0
    fi
    return 1
}

# attempt_deploy — one full deploy cycle. Returns 0 on PASS, 1 on FAIL.
#   $1 = attempt index (1-based, for logging)
#   $2 = per-attempt log file
attempt_deploy() {
    local n="$1"
    local log_file="$2"
    local start_ts
    start_ts=$(date +%s)

    echo "=== attempt $n/$ATTEMPTS — $(date -Is) ===" > "$log_file"

    # 1. Power cycle into Linux
    echo "[1/5] power cycle" >> "$log_file"
    if ! labctl power cycle "$TARGET" >> "$log_file" 2>&1; then
        echo "      FAIL detail: stage=power-cycle" >> "$log_file"
        return 1
    fi
    sleep 2

    # 2. Wait for Linux login prompt over serial
    echo "[2/5] waiting for Linux login" >> "$log_file"
    if ! capture_until_match "$TARGET" "$LINUX_LOGIN_TIMEOUT" "login:" 3 "$log_file"; then
        echo "      FAIL detail: stage=linux-login-timeout" >> "$log_file"
        return 1
    fi

    # 3. Wait for ssh
    echo "[3/5] waiting for ssh on $JET_IP" >> "$log_file"
    local waited=0
    until ssh "${SSH_OPTS[@]}" "$SSH_USER@$JET_IP" 'echo up' >/dev/null 2>&1; do
        sleep 5
        waited=$((waited + 5))
        if (( waited > SSH_READY_TIMEOUT )); then
            echo "      FAIL detail: stage=ssh-timeout" >> "$log_file"
            return 1
        fi
    done

    # 4. scp + slmos-kexec
    echo "[4/5] deploying $KERNEL" >> "$log_file"
    if ! scp "${SSH_OPTS[@]}" "$KERNEL" "$SSH_USER@$JET_IP:/root/slmos.elf" \
            >> "$log_file" 2>&1; then
        echo "      FAIL detail: stage=scp" >> "$log_file"
        return 1
    fi
    ssh "${SSH_OPTS[@]}" "$SSH_USER@$JET_IP" \
        "nohup /usr/local/bin/slmos-kexec /root/slmos.elf > /tmp/slmos-kexec.log 2>&1 &" \
        >> "$log_file" 2>&1 || true
    # SSH backgrounded — slmos-kexec executes `exec kexec -e`, so the ssh
    # session is torn down by kexec; we don't wait on it.

    # Wait for SLM-OS shell.
    if ! capture_until_match "$TARGET" "$SLMOS_SHELL_TIMEOUT" "slmos>" 5 "$log_file"; then
        echo "      FAIL detail: stage=slmos-shell-timeout" >> "$log_file"
        return 1
    fi

    # 5. GPU bringup validation (unless --skip-test)
    if [[ "$SKIP_TEST" == "1" ]]; then
        echo "[5/5] SKIPPING test (--skip-test): shell reached, declaring PASS" >> "$log_file"
        return 0
    fi

    echo "[5/5] GPU bringup test" >> "$log_file"
    labctl serial send "$TARGET" "nvgpu inherit"    -c 8  -u "slmos> \$" >> "$log_file" 2>&1
    labctl serial send "$TARGET" "nvgpu channel"    -c 8  -u "slmos> \$" >> "$log_file" 2>&1
    labctl serial send "$TARGET" "nvgpu oplib stage" -c 12 -u "slmos> \$" >> "$log_file" 2>&1

    local out
    out=$(labctl serial send "$TARGET" "nvgpu launch-kernel" \
            -c 25 -u "launch-kernel:.*rc=" 2>&1)
    printf '%s\n' "$out" >> "$log_file"

    local elapsed=$(( $(date +%s) - start_ts ))
    echo "      attempt elapsed: ${elapsed}s" >> "$log_file"

    if printf '%s\n' "$out" | grep -qE "launch-kernel: rc=0, state=7"; then
        return 0
    fi

    # Extract failure detail for the summary
    local rc fecs
    rc=$(printf '%s\n' "$out" | grep -oE "launch-kernel: rc=-?[0-9]+, state=[0-9]+" | head -1)
    fecs=$(printf '%s\n' "$out" | grep -oE "ctxsw_mb6=0x[0-9a-fA-F]+" | head -1)
    echo "      FAIL detail: ${rc:-no-rc} ${fecs:-no-fecs}" >> "$log_file"
    return 1
}

OVERALL_START=$(date +%s)
PASS=0
FAIL_DETAILS=()

for n in $(seq 1 "$ATTEMPTS"); do
    log_file="$LOG_DIR/attempt-$n.log"
    log "attempt $n/$ATTEMPTS … (log: $log_file)"
    if attempt_deploy "$n" "$log_file"; then
        PASS=1
        log "  PASS"
        break
    fi
    # Surface the one-line failure cause into the summary. Strip the
    # "FAIL detail: " prefix once here so both the per-attempt log line
    # and the failure-modes recap below read consistently.
    detail=$(grep -oE 'FAIL detail: .*' "$log_file" | head -1)
    detail="${detail#FAIL detail: }"
    log "  FAIL${detail:+ — $detail}"
    FAIL_DETAILS+=("${detail:-unknown}")
done

OVERALL_ELAPSED=$(( $(date +%s) - OVERALL_START ))
log ""
log "=== summary ==="
log "  result:        $([[ "$PASS" == "1" ]] && echo PASS || echo FAIL)"
log "  total wall:    ${OVERALL_ELAPSED}s"
log "  attempts used: $(( PASS == 1 ? n : ATTEMPTS )) / $ATTEMPTS"
log "  finished:      $(date -Is)"
log "  full logs in:  $LOG_DIR"
if (( PASS == 0 )) && (( ${#FAIL_DETAILS[@]} > 0 )); then
    log "  failure modes:"
    for d in "${FAIL_DETAILS[@]}"; do
        log "    $d"
    done
fi

if [[ "$PASS" == "1" ]]; then
    exit 0
fi
exit 1
