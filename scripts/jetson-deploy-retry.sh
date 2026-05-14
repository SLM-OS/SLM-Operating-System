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
        -r|--retries)  RETRIES="$2"; shift 2 ;;
        --skip-test)   SKIP_TEST=1; shift ;;
        --log-dir)     LOG_DIR="$2"; shift 2 ;;
        -h|--help)     usage 0 ;;
        *) echo "Unknown argument: $1" >&2; usage 1 ;;
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
        echo "      power cycle failed" >> "$log_file"
        return 1
    fi
    sleep 2

    # 2. Wait for Linux login prompt over serial
    echo "[2/5] waiting for Linux login" >> "$log_file"
    if ! labctl serial capture "$TARGET" -t 90 -u "login:" -n 3 >> "$log_file" 2>&1; then
        echo "      Linux did not reach login:" >> "$log_file"
        return 1
    fi

    # 3. Wait for ssh
    echo "[3/5] waiting for ssh on $JET_IP" >> "$log_file"
    local waited=0
    until ssh "${SSH_OPTS[@]}" "$SSH_USER@$JET_IP" 'echo up' >/dev/null 2>&1; do
        sleep 5
        waited=$((waited + 5))
        if (( waited > 90 )); then
            echo "      ssh never came up" >> "$log_file"
            return 1
        fi
    done

    # 4. scp + slmos-kexec
    echo "[4/5] deploying $KERNEL" >> "$log_file"
    if ! scp "${SSH_OPTS[@]}" "$KERNEL" "$SSH_USER@$JET_IP:/root/slmos.elf" \
            >> "$log_file" 2>&1; then
        echo "      scp failed" >> "$log_file"
        return 1
    fi
    ssh "${SSH_OPTS[@]}" "$SSH_USER@$JET_IP" \
        "nohup /usr/local/bin/slmos-kexec /root/slmos.elf > /tmp/slmos-kexec.log 2>&1 &" \
        >> "$log_file" 2>&1 || true
    # SSH backgrounded — slmos-kexec executes `exec kexec -e`, so the ssh
    # session is torn down by kexec; we don't wait on it.

    # Wait for SLM-OS shell.
    if ! labctl serial capture "$TARGET" -t 90 -u "slmos>" -n 5 \
            >> "$log_file" 2>&1; then
        echo "      SLM-OS shell did not appear" >> "$log_file"
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
    # Surface the one-line failure cause into the summary
    detail=$(grep -oE 'FAIL detail: .*' "$log_file" | head -1)
    log "  FAIL${detail:+ — ${detail#FAIL detail: }}"
    FAIL_DETAILS+=("$detail")
done

OVERALL_ELAPSED=$(( $(date +%s) - OVERALL_START ))
log ""
log "=== summary ==="
log "  result:        $([[ "$PASS" == "1" ]] && echo PASS || echo FAIL)"
log "  total wall:    ${OVERALL_ELAPSED}s"
log "  attempts used: $(( PASS == 1 ? n : ATTEMPTS )) / $ATTEMPTS"
log "  finished:      $(date -Is)"
log "  full logs in:  $LOG_DIR"

if [[ "$PASS" == "1" ]]; then
    exit 0
fi
exit 1
