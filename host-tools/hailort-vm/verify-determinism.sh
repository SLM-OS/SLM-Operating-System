#!/bin/bash
# Run two captures back-to-back and diff them.
#
# The load-bearing deliverable of Task 0.3. If the two runs match byte-for-byte
# (after stripping known-volatile fields), HailoRT's BAR write sequence is
# deterministic and the corpus-based RE plan in #795 is viable. Any divergence
# is reported with first-mismatch context and the verify exits non-zero.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CORPORA_DIR="${CORPORA_DIR:-$HOME/slmos-ref/derivatives/hailo-re-corpora}"
mkdir -p "${CORPORA_DIR}"

STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_A="${CORPORA_DIR}/verify-${STAMP}-A.jsonl"
RUN_B="${CORPORA_DIR}/verify-${STAMP}-B.jsonl"
DIFF_OUT="${CORPORA_DIR}/verify-${STAMP}.diff"

echo "=== Run A (${RUN_A}) ==="
"${HERE}/launch-capture.sh" "${RUN_A}" --variant A
echo
echo "=== Run B (${RUN_B}) ==="
"${HERE}/launch-capture.sh" "${RUN_B}" --variant B

# ---------------------------------------------------------------------------
# Diff strategy:
#  - Strip the header (line 1) — capture_started_at and slmos_base_sha may match
#    but timestamps differ.
#  - Strip wall-clock-derived fields entirely.
#  - Compare ops line-by-line.
#
# A pass means every `op` entry — same seq, bar, offset, size, dir, value —
# matches across both runs. The host_clock/cpu_index trace fields are NOT in
# the corpus so are not compared.
# ---------------------------------------------------------------------------

extract_ops() {
    # Drop header / trailer; keep op lines only. Python's json.dumps inserts
    # spaces after colons by default, so the literal needs to match either form.
    grep -E '^\{"type": *"op"' "$1" || true
}

OPS_A="$(mktemp)"
OPS_B="$(mktemp)"
trap 'rm -f "${OPS_A}" "${OPS_B}"' EXIT

extract_ops "${RUN_A}" > "${OPS_A}"
extract_ops "${RUN_B}" > "${OPS_B}"

A_COUNT=$(wc -l < "${OPS_A}")
B_COUNT=$(wc -l < "${OPS_B}")
echo
echo "Run A: ${A_COUNT} op entries"
echo "Run B: ${B_COUNT} op entries"

if [[ "${A_COUNT}" -eq 0 || "${B_COUNT}" -eq 0 ]]; then
    echo "ERROR: at least one run captured zero MMIO operations." >&2
    echo "Check serial logs under ~/slmos-ref/derivatives/hailort-vm-images/.runs/" >&2
    exit 6
fi

if diff -u "${OPS_A}" "${OPS_B}" > "${DIFF_OUT}"; then
    echo
    echo "RESULT: DETERMINISTIC — runs A and B match byte-for-byte across ${A_COUNT} op entries."
    echo "Corpus A: ${RUN_A}"
    echo "Corpus B: ${RUN_B}"
    rm -f "${DIFF_OUT}"
    exit 0
fi

# ---------------------------------------------------------------------------
# Non-deterministic: emit context.
# ---------------------------------------------------------------------------
echo
echo "RESULT: NON-DETERMINISTIC — runs A and B diverge."
echo "Diff:                ${DIFF_OUT}"
echo "First mismatch (op): "
diff -u "${OPS_A}" "${OPS_B}" | grep -E '^[-+]' | grep -v '^[-+]{3}' | head -20

FIRST_A_DIVERGE="$(diff "${OPS_A}" "${OPS_B}" | grep -m1 -E '^< ' | sed 's/^< //')"
FIRST_B_DIVERGE="$(diff "${OPS_A}" "${OPS_B}" | grep -m1 -E '^> ' | sed 's/^> //')"
echo
echo "Run A first divergent line:"
echo "  ${FIRST_A_DIVERGE}"
echo "Run B first divergent line:"
echo "  ${FIRST_B_DIVERGE}"

echo
echo "Investigate determinism mitigations in host-tools/hailort-vm/README.md § Determinism."
exit 7
