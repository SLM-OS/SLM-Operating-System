#!/usr/bin/env bash
# capture-hailo-trace.sh — orchestrate a Hailo boundary-trace capture on
# a Pi 5 lab board.
#
# Builds an SLM-OS kernel with the requested trace masks armed via
# cmdline.txt, deploys to a labctl-managed SBC, captures serial output
# through boot + (optionally) a follow-on shell command, and saves the
# capture to ~/slmos-ref/derivatives/slmos-traces/.
#
# Dependencies (runtime):
#   - PR A (feat/hailo-trace-framework) merged: provides the
#     hailo_trace_cmdline_parse() path and the `hailo trace` shell
#     subcommand this script depends on.
#   - labctl CLI in PATH.
#   - aarch64-none-elf-gcc toolchain.
#
# Usage:
#   scripts/capture-hailo-trace.sh \
#       --phase link,fw_boot,postboot \
#       --mech  pci,rpc,irq \
#       --shell-cmd "hailo probe; hailo boot" \
#       --scenario bootphase \
#       [--board pi-5-1] [--skip-build] [--duration 30] [--keep-cmdline]
#
# Exit codes:
#   0  success — capture saved at the printed path
#   1  argument / preflight error
#   2  build failed
#   3  labctl claim / sdwire / serial step failed
#
# Idempotency:
#   On any failure after the SBC is claimed, the trap restores the SD
#   card to a clean state (removes cmdline.txt) and releases the
#   claim. A claimed-board interrupt (Ctrl-C) is treated the same way.
#   Re-running the script is always safe — it does not assume any
#   prior cmdline.txt state on the card.

set -euo pipefail

# ---- defaults --------------------------------------------------------------

BOARD=pi-5-1
SCENARIO=bootphase
PHASE=""
MECH=""
SHELL_CMD=""
SKIP_BUILD=0
KEEP_CMDLINE=0
DURATION_MIN=30
OUT_DIR="${HOME}/slmos-ref/derivatives/slmos-traces"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KERNEL_BIN="${REPO_ROOT}/build/kernel/slmos.bin"

# ---- arg parse -------------------------------------------------------------

usage() {
    # Print the entire leading comment block (everything from line 2
    # up to the first non-`#` line). Beats a hardcoded line range,
    # which silently truncated the idempotency note when the header
    # grew past its assumed length.
    awk 'NR==1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0" >&2
    exit 1
}

# Helper: error out cleanly if the flag that just matched has no
# following value (e.g. `--phase` as the last token). Without this,
# the `"$2"` reference triggers a generic "unbound variable" abort
# under `set -u` instead of a useful message.
require_val() {
    [[ $# -ge 2 ]] || { echo "error: $1 requires a value" >&2; exit 1; }
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --phase)         require_val "$@"; PHASE="$2"; shift 2 ;;
        --mech)          require_val "$@"; MECH="$2"; shift 2 ;;
        --shell-cmd)     require_val "$@"; SHELL_CMD="$2"; shift 2 ;;
        --scenario)      require_val "$@"; SCENARIO="$2"; shift 2 ;;
        --board)         require_val "$@"; BOARD="$2"; shift 2 ;;
        --skip-build)    SKIP_BUILD=1; shift ;;
        --keep-cmdline)  KEEP_CMDLINE=1; shift ;;
        --duration)      require_val "$@"; DURATION_MIN="$2"; shift 2 ;;
        --out-dir)       require_val "$@"; OUT_DIR="$2"; shift 2 ;;
        -h|--help)       usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

if [[ -z "$PHASE" && -z "$MECH" ]]; then
    echo "error: at least one of --phase / --mech must be set " \
         "(empty masks are silent; use --phase all --mech all for everything)" >&2
    exit 1
fi
# Default empties to 'off' rather than ''
PHASE="${PHASE:-off}"
MECH="${MECH:-off}"

mkdir -p "$OUT_DIR"

# Capture file naming: slmos-<scenario>-<board>-<YYYY-MM-DD>.txt with
# a -<N> suffix if collision. Keeps the existing capture naming
# convention from the PR A seed artifacts.
DATE_TAG="$(date +%F)"
OUT_BASE="${OUT_DIR}/slmos-${SCENARIO}-${BOARD}-${DATE_TAG}"
OUT_FILE="${OUT_BASE}.txt"
N=2
while [[ -e "$OUT_FILE" ]]; do
    OUT_FILE="${OUT_BASE}-${N}.txt"
    N=$((N+1))
done

# ---- preflight -------------------------------------------------------------

command -v labctl >/dev/null || { echo "error: labctl not in PATH" >&2; exit 1; }
[[ -d "$REPO_ROOT/kernel" ]] || { echo "error: not inside SLM-OS repo (REPO_ROOT=$REPO_ROOT)" >&2; exit 1; }

echo "capture-hailo-trace: board=$BOARD scenario=$SCENARIO" >&2
echo "  phase=$PHASE mech=$MECH" >&2
echo "  shell-cmd=${SHELL_CMD:-<none>}" >&2
echo "  out=$OUT_FILE" >&2

# ---- build -----------------------------------------------------------------

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    echo "==> building kernel for Pi 5 (masks default 0; cmdline.txt arms at boot)" >&2
    make -C "$REPO_ROOT" kernel-clean >/dev/null 2>&1 || true
    make -C "$REPO_ROOT" kernel PLATFORM=RASPI5 \
        >"${OUT_FILE}.build.log" 2>&1 \
        || { echo "error: build failed; see ${OUT_FILE}.build.log" >&2; exit 2; }
    rm -f "${OUT_FILE}.build.log"
fi

[[ -f "$KERNEL_BIN" ]] || { echo "error: $KERNEL_BIN not found after build" >&2; exit 2; }

# ---- claim + cleanup trap --------------------------------------------------

CLAIM_HELD=0
CMDLINE_PUSHED=0
TMP_CMDLINE=""  # set in the "write cmdline.txt" section; cleanup deletes if non-empty

cleanup() {
    local rc=$?
    # Tempfile may have been created but not yet rm'd on the success
    # path (e.g. if labctl sdwire update failed mid-flight). Idempotent.
    if [[ -n "$TMP_CMDLINE" ]]; then
        rm -f "$TMP_CMDLINE" 2>/dev/null || true
    fi
    if [[ "$CMDLINE_PUSHED" -eq 1 && "$KEEP_CMDLINE" -eq 0 ]]; then
        echo "==> removing cmdline.txt (restoring clean card state)" >&2
        labctl power off "$BOARD" >/dev/null 2>&1 || true
        labctl sdwire update "$BOARD" -p 1 --delete cmdline.txt \
            >/dev/null 2>&1 || true
    fi
    if [[ "$CLAIM_HELD" -eq 1 ]]; then
        echo "==> releasing claim on $BOARD" >&2
        labctl release "$BOARD" >/dev/null 2>&1 || true
    fi
    if [[ $rc -ne 0 && -s "$OUT_FILE" ]]; then
        echo "note: partial capture preserved at $OUT_FILE" >&2
    fi
    exit $rc
}
trap cleanup EXIT INT TERM

echo "==> claiming $BOARD for ${DURATION_MIN}m" >&2
labctl claim "$BOARD" -d "${DURATION_MIN}m" \
    -r "capture-hailo-trace scenario=${SCENARIO} phase=${PHASE} mech=${MECH}" \
    >/dev/null \
    || { echo "error: claim failed" >&2; exit 3; }
CLAIM_HELD=1

# ---- write cmdline.txt + flash kernel --------------------------------------

# Tempfile is tracked via the file-scope $TMP_CMDLINE so the EXIT
# trap's cleanup() can rm it on any failure path between here and the
# explicit rm at the end of this section. (A `trap '...' RETURN` only
# fires inside functions, not at script-global scope.)
TMP_CMDLINE="$(mktemp -t cmdline.XXXXXX.txt)"
echo "hailo_trace.phase=${PHASE} hailo_trace.mech=${MECH}" > "$TMP_CMDLINE"

echo "==> powering off + flashing kernel + cmdline.txt" >&2
labctl power off "$BOARD" >/dev/null
labctl sdwire update "$BOARD" -p 1 \
    -c "${KERNEL_BIN}:kernel_2712.img" \
    -c "${TMP_CMDLINE}:cmdline.txt" \
    --reboot \
    >/dev/null \
    || { echo "error: sdwire update failed" >&2; exit 3; }
CMDLINE_PUSHED=1
rm -f "$TMP_CMDLINE"
TMP_CMDLINE=""  # cleanup() now no-ops on the tempfile

# ---- capture ---------------------------------------------------------------

# Boot capture: read serial until the shell prompt appears, panic
# pattern hits, or timeout. The kernel emits >100 lines of init output
# before the shell; a 90 s window is generous.
#
# labctl's `--until` is interpreted as a POSIX extended regex (verified
# end-to-end against pi-5-1 in PR #781 / #783). `|` is alternation and
# `$` anchors end-of-line; that's why "slmos> $" further down matches
# the bare prompt line after each command rather than a literal `$`.
echo "==> capturing boot serial → $OUT_FILE" >&2
{
    echo "# SLM-OS hailo_trace capture"
    echo "# date:     $(date -Iseconds)"
    echo "# board:    $BOARD"
    echo "# scenario: $SCENARIO"
    echo "# phase:    $PHASE"
    echo "# mech:     $MECH"
    echo "# kernel:   $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "# script:   capture-hailo-trace.sh"
    echo "#"
} > "$OUT_FILE"

labctl serial capture "$BOARD" \
    --timeout 90 \
    --until 'SLM-OS Debug Shell|KERNEL PANIC|PAGE FAULT' \
    >> "$OUT_FILE" \
    || { echo "error: serial capture (boot phase) failed" >&2; exit 3; }

# Follow-on shell command, if any.
if [[ -n "$SHELL_CMD" ]]; then
    echo "==> sending shell command(s): $SHELL_CMD" >&2
    # Multiple commands separated by ; are sent in sequence. labctl
    # serial send handles one command at a time so we split + loop.
    # Known limitation: split is unconditional on `;`, so a quoted
    # semicolon inside the shell command (e.g. --shell-cmd "echo 'a;
    # b'") gets miscounted. Acceptable for a debug tool; if needed,
    # switch to a flag that takes a repeated --shell-cmd instead.
    {
        echo ""
        echo "# --- shell-cmd output ---"
    } >> "$OUT_FILE"
    IFS=';' read -ra CMDS <<< "$SHELL_CMD"
    for cmd in "${CMDS[@]}"; do
        cmd="${cmd#"${cmd%%[![:space:]]*}"}"  # ltrim
        cmd="${cmd%"${cmd##*[![:space:]]}"}"  # rtrim
        [[ -z "$cmd" ]] && continue
        echo "" >> "$OUT_FILE"
        echo "slmos> $cmd" >> "$OUT_FILE"
        # 30 s timeout per command; long enough for hailo boot (~1 s)
        # and runmodel (~5 s including its internal timeouts).
        labctl serial send "$BOARD" "$cmd" \
            --capture 30 \
            --until 'slmos> $|FAIL|TIMEOUT|rc=-' \
            >> "$OUT_FILE" \
            || echo "warn: shell command '$cmd' capture had issues; see $OUT_FILE" >&2
    done
fi

# ---- cleanup runs from trap ------------------------------------------------

echo "" >&2
echo "==> capture complete: $OUT_FILE" >&2
echo "    $(wc -l < "$OUT_FILE") lines, $(wc -c < "$OUT_FILE") bytes" >&2
