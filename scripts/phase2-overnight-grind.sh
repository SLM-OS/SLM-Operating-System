#!/bin/bash
# Phase 2 overnight grind wrapper.
#
# Runs hailo-re-bootstrap in batches, snapshots after each batch, and stops on
# real signals (divergence, configure-complete, repeated transient failures).
# Forks a background subshell that periodically renews the pi-5-1 labctl
# claim so the grind can run unattended even when the operator (or any
# external cron) goes quiet for >2 h.
#
# Stop conditions (writes STOP_REASON file with details, then exits non-zero):
#   - HAILO_RE_CORPUS_DIVERGENCE appears in any batch log.
#   - hailo-re-bootstrap exits with status=configure_complete.
#   - 3 consecutive transient batch failures.
#   - Claim expiry detected (labctl reports claim error from the driver).
#
# Operator review surface:
#   - $WRAPPER_LOG: rolling tail of all batches.
#   - $STOP_REASON_FILE (only present after stop): single-line reason.
#   - $CORPUS_DIR/*-overnight-batch-NN.jsonl: per-batch corpus snapshots.

set -uo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
CORPUS="${CORPUS:-/home/john/slmos-ref/derivatives/hailo-re-corpora/4.23.0-4.23.0-qemu-x86_64-2026-05-13-phase2-grind.jsonl}"
BATCH_SIZE="${BATCH_SIZE:-150}"
MAX_TRANSIENT_FAILURES="${MAX_TRANSIENT_FAILURES:-3}"
SBC="${SBC:-pi-5-1}"
# Claim duration each renewal asks for, in minutes. Renewal interval is
# this minus a safety margin so we never let the claim expire even if a
# renewal call hiccups.
CLAIM_DURATION_MIN="${CLAIM_DURATION_MIN:-120}"
CLAIM_RENEW_INTERVAL_S="${CLAIM_RENEW_INTERVAL_S:-1800}"  # 30 min

CORPUS_DIR="$(dirname "$CORPUS")"
LOG_DIR=/home/john/slmos-ref/derivatives/slmos-traces
WRAPPER_LOG="${LOG_DIR}/phase2-overnight-$(date -u +%Y%m%dT%H%M%SZ).log"
STOP_REASON_FILE="/tmp/phase2-overnight-stop.reason"

KERNEL_PATH=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/pi-5-gpu-board/build/kernel/slmos.bin
LAUNCHER_PATH=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/pi-5-gpu-board/host-tools/hailort-vm/launch-bootstrap.sh
WORKTREE=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/pi-5-gpu-board
BOOTSTRAP=/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/pi-5-gpu-board/host-tools/hailo-re-driver/bin/hailo-re-bootstrap

mkdir -p "$LOG_DIR"
rm -f "$STOP_REASON_FILE"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
log() {
    local ts
    ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '[%s] %s\n' "$ts" "$*" | tee -a "$WRAPPER_LOG"
}

set_stop() {
    local reason="$1"
    echo "$reason" > "$STOP_REASON_FILE"
    log "STOP: $reason"
}

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
log "==== Phase 2 overnight grind starting ===="
log "Corpus: $CORPUS"
log "Batch size: $BATCH_SIZE"
log "Log: $WRAPPER_LOG"
log "Stop-reason file: $STOP_REASON_FILE"

if [[ ! -f "$CORPUS" ]]; then
    set_stop "corpus file missing: $CORPUS"
    exit 2
fi
if [[ ! -x "$BOOTSTRAP" ]]; then
    set_stop "bootstrap binary missing: $BOOTSTRAP"
    exit 2
fi
if [[ ! -x "$LAUNCHER_PATH" ]]; then
    set_stop "QEMU launcher missing: $LAUNCHER_PATH"
    exit 2
fi
if [[ ! -f "$KERNEL_PATH" ]]; then
    set_stop "SLM-OS kernel binary missing: $KERNEL_PATH"
    exit 2
fi

START_CORPUS_LINES=$(wc -l < "$CORPUS")
START_CORPUS_SHA=$(sha256sum "$CORPUS" | cut -c1-12)
log "Starting corpus: $START_CORPUS_LINES lines, sha=$START_CORPUS_SHA"

# ---------------------------------------------------------------------------
# Claim ownership
# ---------------------------------------------------------------------------
# The wrapper must hold the claim *in its own CLI session* — labctl only
# lets the owning session call `labctl renew`. If the claim is owned by
# someone else (an MCP session, a stale CLI session), bail loudly rather
# than silently spinning on failed renewals.
log "Claiming $SBC for ${CLAIM_DURATION_MIN}m..."
if ! labctl claim "$SBC" \
        -d "${CLAIM_DURATION_MIN}m" \
        -r "Hailo RE Phase 2 grind iteration — #795, post-region-injection" \
        -n hailo-re-phase4-grind >> "$WRAPPER_LOG" 2>&1; then
    set_stop "could not claim $SBC at startup — release any existing claim first"
    exit 6
fi
log "Claim acquired on $SBC."

# ---------------------------------------------------------------------------
# Claim renewal subshell
# ---------------------------------------------------------------------------
# Without this, an unattended overnight run hits the 2-h claim TTL and
# all subsequent labctl calls fail with "claim_not_found", forcing 3
# transient failures and a STOP. Renew every 30 min so even a missed
# call leaves >1.5 h of headroom.
renew_loop() {
    while true; do
        sleep "$CLAIM_RENEW_INTERVAL_S"
        # Cap the renew call so a hung labctl (stuck TCP, daemon
        # lock contention) can't silently freeze the renew loop and
        # let the claim TTL expire. 30 s is generous for a healthy
        # local-host call (~10 ms) but short enough that we recover
        # within one renew tick.
        if ! timeout 30 labctl renew "$SBC" -d "${CLAIM_DURATION_MIN}m" \
                >/dev/null 2>&1; then
            log "WARN: claim renewal for $SBC failed (will retry next tick)"
        fi
    done
}
renew_loop &
RENEW_PID=$!
log "Claim renewal subshell PID=$RENEW_PID (interval=${CLAIM_RENEW_INTERVAL_S}s, duration=${CLAIM_DURATION_MIN}m)"
# On exit: stop renewing and release the claim so the next claimant
# doesn't have to wait for the TTL.
trap 'kill "$RENEW_PID" 2>/dev/null || true; labctl release "$SBC" >/dev/null 2>&1 || true' EXIT

# ---------------------------------------------------------------------------
# Outer loop
# ---------------------------------------------------------------------------
batch_n=0
transient_failures=0

while true; do
    batch_n=$((batch_n + 1))
    batch_log="${LOG_DIR}/phase2-overnight-batch-$(printf '%02d' $batch_n).log"
    snapshot_path="${CORPUS_DIR}/$(basename "${CORPUS%.jsonl}")-overnight-batch-$(printf '%02d' $batch_n).jsonl"

    log "---- Batch $batch_n starting (max-iter=$BATCH_SIZE) ----"
    pre_lines=$(wc -l < "$CORPUS")

    # Run the batch inside sg kvm subshell. Single-quoted heredoc body to keep
    # variable expansion deterministic.
    sg kvm -c "
        export HAILO_RE_QEMU_LAUNCHER='$LAUNCHER_PATH'
        export HAILO_RE_SBC=pi-5-1
        export HAILO_RE_KERNEL='$KERNEL_PATH'
        export HAILO_RE_PENDING_DIR=/tmp
        cd '$WORKTREE'
        '$BOOTSTRAP' '$CORPUS' --max-iterations $BATCH_SIZE --verbose 2>&1
    " > "$batch_log" 2>&1
    rc=$?

    post_lines=$(wc -l < "$CORPUS")
    new_entries=$((post_lines - pre_lines))
    log "Batch $batch_n exit=$rc, corpus $pre_lines -> $post_lines (+$new_entries entries)"

    # Snapshot after the batch (regardless of rc — even partial batches yield
    # useful corpus state for rollback).
    cp "$CORPUS" "$snapshot_path"
    log "Snapshot: $(basename "$snapshot_path")"

    # ----- Signal detection -----
    if grep -q "HAILO_RE_CORPUS_DIVERGENCE" "$batch_log"; then
        set_stop "divergence detected in batch $batch_n — see $batch_log"
        exit 3
    fi
    # The bootstrap emits `status=complete ... last_seq_appended=None`
    # when the QEMU stub can't surface any new unknown reads — i.e. the
    # corpus has reached the natural fixed point relative to what the
    # HailoRT host driver exercises during the captured control-plane
    # sequence. Without this check the wrapper just spins in tight
    # ~1.5 min no-op batches forever (rc=0, +0 entries). See the
    # 25-May-2026 grind notes — first hit after batch 27 saturated.
    if grep -qE '^status=complete .* last_seq_appended=None' "$batch_log" \
            || grep -q "QEMU stub exited 0 with no unknown reads" "$batch_log"; then
        set_stop "corpus saturated (status=complete, no unknown reads) at batch $batch_n — see $batch_log"
        exit 0
    fi
    if grep -q "status=configure_complete" "$batch_log"; then
        set_stop "HailoRT Configure+Activate completed at batch $batch_n!"
        exit 0
    fi

    # ----- Exit-code analysis -----
    # hailo-re-bootstrap exit codes (observed):
    #   1 = max-iterations hit (NORMAL — keep going)
    #   non-1 with errors in log = transient (retry up to MAX_TRANSIENT_FAILURES)
    if [[ $rc -eq 0 ]]; then
        # Unexpected: bootstrap returns non-zero on normal max-iter exit.
        # If we got here with rc=0, something unusual happened. Treat as
        # success-ish and keep going, but log it.
        log "Batch $batch_n exited rc=0 (unusual but proceeding)"
        transient_failures=0
    elif [[ $rc -eq 1 ]] && grep -q "status=max-iterations" "$batch_log"; then
        log "Batch $batch_n hit max-iterations cleanly; continuing"
        transient_failures=0
    else
        # Transient failure (SQLite lock, sdwire glitch, etc.)
        transient_failures=$((transient_failures + 1))
        log "Batch $batch_n transient failure ($transient_failures/$MAX_TRANSIENT_FAILURES); last error:"
        tail -8 "$batch_log" | sed 's/^/  | /' | tee -a "$WRAPPER_LOG"
        if [[ $transient_failures -ge $MAX_TRANSIENT_FAILURES ]]; then
            set_stop "$MAX_TRANSIENT_FAILURES consecutive transient failures; last batch log $batch_log"
            exit 4
        fi
        # Brief pause before retrying — let labctl recover.
        log "Pausing 60 s before retry..."
        sleep 60
    fi

    # ----- Health check after each batch -----
    # If new_entries == 0 across a whole batch, the loop made no progress.
    # Could indicate a wedge that isn't surfacing as a clean error.
    if [[ $new_entries -eq 0 ]]; then
        transient_failures=$((transient_failures + 1))
        log "Batch $batch_n added zero entries — counting as transient failure ($transient_failures/$MAX_TRANSIENT_FAILURES)"
        if [[ $transient_failures -ge $MAX_TRANSIENT_FAILURES ]]; then
            set_stop "zero-progress batches reached limit"
            exit 5
        fi
        sleep 60
    fi
done
