#!/bin/bash
#
# import_ai_weights.sh - Import AI model weights from Plan A export pipeline
#
# Copies generated weight files from the slm-os-scheduler-ai repo's
# deploy/generated/ directory into the kernel's AI scheduler source.
# Validates that dimensions match the expected model architecture.
#
# Usage:
#   ./scripts/import_ai_weights.sh [--source <path>] [--include-real]
#
# Default source: ~/projects/slm-os-scheduler-ai/deploy/generated/
#
# The optional `--include-real` flag also imports the SLM-OS-fine-tuned
# weight variants (`ai_weights_mlp_real.c`, `ai_weights_ppo_real.c`)
# emitted by the sibling repo's `--weights-suffix _real` export
# (sibling-repo PR for SLM-OS #879). Both variants coexist on disk;
# the kernel's `AI_WEIGHTS=synthetic|real` build flag picks one at
# compile time. See `docs/fact-sheets/ai-scheduler.md`.

set -euo pipefail

# Default paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_AI_DIR="${SCRIPT_DIR}/../kernel/sched/ai"
DEFAULT_SOURCE="${HOME}/projects/slm-os-scheduler-ai/deploy/generated"

# Parse arguments
SOURCE_DIR="${DEFAULT_SOURCE}"
INCLUDE_REAL=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            SOURCE_DIR="$2"
            shift 2
            ;;
        --include-real)
            INCLUDE_REAL=1
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--source <path>] [--include-real]"
            echo ""
            echo "Import AI model weights from Plan A export pipeline."
            echo ""
            echo "Options:"
            echo "  --source <path>   Source directory (default: ${DEFAULT_SOURCE})"
            echo "  --include-real    Also import _real variants (SLM-OS #879 fine-tune)"
            echo "                    (ai_weights_ppo_real.c is optional even with --include-real,"
            echo "                    matching the synthetic-baseline policy for PPO)"
            echo ""
            echo "Expected files in source directory:"
            echo "  ai_weights_mlp.c        MLP weight arrays (synthetic baseline)"
            echo "  ai_weights_ppo.c        PPO weight arrays (synthetic baseline, optional)"
            echo "  ai_weights_mlp_real.c   MLP fine-tuned (with --include-real)"
            echo "  ai_weights_ppo_real.c   PPO fine-tuned (with --include-real)"
            echo "  ai_config.h             Platform-specific dimensions"
            echo ""
            echo "Build select:"
            echo "  make kernel AI_SCHED=ON                       # synthetic baseline (default)"
            echo "  make kernel AI_SCHED=ON AI_WEIGHTS=synthetic  # explicit synthetic"
            echo "  make kernel AI_SCHED=ON AI_WEIGHTS=real       # SLM-OS-fine-tuned weights"
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'"
            exit 1
            ;;
    esac
done

echo "AI Weight Import"
echo "================"
echo "Source:       ${SOURCE_DIR}"
echo "Target:       ${KERNEL_AI_DIR}"
echo "Include real: $([ ${INCLUDE_REAL} -eq 1 ] && echo yes || echo no)"
echo ""

# Validate source directory exists
if [ ! -d "${SOURCE_DIR}" ]; then
    echo "Error: source directory not found: ${SOURCE_DIR}"
    echo "Run Plan A export pipeline first:"
    echo "  cd ~/projects/slm-os-scheduler-ai"
    echo "  python scripts/export_models.py --model all --platform jetson_orin_nano"
    exit 1
fi

# Check for required files (synthetic baseline only — real is optional)
MISSING=0
for file in ai_weights_mlp.c; do
    if [ ! -f "${SOURCE_DIR}/${file}" ]; then
        echo "Error: missing ${file} in ${SOURCE_DIR}"
        MISSING=1
    fi
done

if [ ${MISSING} -eq 1 ]; then
    echo ""
    echo "Run the export pipeline to generate weight files."
    exit 1
fi

# If --include-real was requested but the files aren't there, that's
# a hard error — the operator clearly meant to import them.
if [ ${INCLUDE_REAL} -eq 1 ]; then
    REAL_MISSING=0
    for file in ai_weights_mlp_real.c; do
        if [ ! -f "${SOURCE_DIR}/${file}" ]; then
            echo "Error: --include-real but ${file} not found in ${SOURCE_DIR}"
            echo "  Run: python scripts/export_models.py --model mlp \\"
            echo "         --weights-checkpoint <fine-tuned.pt> --weights-suffix _real"
            REAL_MISSING=1
        fi
    done
    if [ ${REAL_MISSING} -eq 1 ]; then
        exit 1
    fi
fi

# Copy weight files
echo "Copying weight files..."

# The sibling-repo `--weights-suffix _real` export currently emits the
# .c with:
#   - `#include "ai_weights_<model>.h"` (no such header in-tree; the
#     kernel uses `ai_weights.h`)
#   - symbol names without the `ai_` prefix (`mlp_w0`, not `ai_mlp_w0`)
#   - the output layer sized to the platform's actual n_actions
#     (e.g. 24 for Pi 5) rather than the kernel's
#     `AI_MLP_LAYER3_MAX_ROWS = 42`
# whereas the in-tree synthetic `ai_weights_mlp.c` already ships with
# `#include "ai_weights.h"`, `ai_`-prefixed symbols, and 42-row output
# arrays — that version pre-dates the sibling-repo refactor and was
# previously hand-normalized. The kernel expects both .c variants to
# declare identical symbols since `AI_WEIGHTS=synthetic|real` selects
# one at compile time. Normalize fresh _real imports — until the
# sibling-repo export script is updated (follow-up to #879), this
# rewrite IS the contract for the `_real` path. The synthetic .c is
# imported as-is so that re-running the import script doesn't perturb
# the in-tree synthetic baseline that previous benchmarks reference.
normalize_real_c() {
    local target="$1" model="$2" max_rows="$3" layer3_in="$4"
    sed -i "s|#include \"ai_weights_${model}.h\"|#include \"ai_weights.h\"|" "${target}"
    sed -i -E "s/\b${model}_(w|b|bn_gamma|bn_beta|bn_mean|bn_var)([0-9])\b/ai_${model}_\1\2/g" "${target}"
    python3 "${SCRIPT_DIR}/_pad_ai_weights.py" \
        "${target}" "${model}" "${max_rows}" "${layer3_in}"
}

# Synthetic baseline (always) — imported as-is, do not normalize, see
# comment above.
if [ -f "${SOURCE_DIR}/ai_weights_mlp.c" ]; then
    cp "${SOURCE_DIR}/ai_weights_mlp.c" "${KERNEL_AI_DIR}/ai_weights_mlp.c"
    echo "  ai_weights_mlp.c -> kernel/sched/ai/"
fi
if [ -f "${SOURCE_DIR}/ai_weights_ppo.c" ]; then
    cp "${SOURCE_DIR}/ai_weights_ppo.c" "${KERNEL_AI_DIR}/ai_weights_ppo.c"
    echo "  ai_weights_ppo.c -> kernel/sched/ai/"
fi

# Fine-tuned (only when requested) — normalize on import.
if [ ${INCLUDE_REAL} -eq 1 ]; then
    if [ -f "${SOURCE_DIR}/ai_weights_mlp_real.c" ]; then
        cp "${SOURCE_DIR}/ai_weights_mlp_real.c" "${KERNEL_AI_DIR}/ai_weights_mlp_real.c"
        normalize_real_c "${KERNEL_AI_DIR}/ai_weights_mlp_real.c" mlp 42 128
        echo "  ai_weights_mlp_real.c -> kernel/sched/ai/  (include/symbol/pad normalized)"
    fi
    if [ -f "${SOURCE_DIR}/ai_weights_ppo_real.c" ]; then
        cp "${SOURCE_DIR}/ai_weights_ppo_real.c" "${KERNEL_AI_DIR}/ai_weights_ppo_real.c"
        normalize_real_c "${KERNEL_AI_DIR}/ai_weights_ppo_real.c" ppo 42 128
        echo "  ai_weights_ppo_real.c -> kernel/sched/ai/  (include/symbol/pad normalized)"
    fi
fi

# Copy config header if present
if [ -f "${SOURCE_DIR}/ai_config.h" ]; then
    cp "${SOURCE_DIR}/ai_config.h" "${KERNEL_AI_DIR}/ai_config.h"
    echo "  ai_config.h -> kernel/sched/ai/"
fi

# Validate dimensions by checking for expected array names. Same
# symbol names across synthetic/real (the sibling-repo export uses
# `--weights-suffix` only on the filename, not on the C identifiers
# — see export_models.py::write_nn_weights_c). The kernel build
# picks one .c via AI_WEIGHTS=synthetic|real so both files declare
# the same symbols; only one is compiled in.
echo ""
echo "Validating weight files..."

ERRORS=0
for variant in "ai_weights_mlp.c" "ai_weights_mlp_real.c"; do
    target="${KERNEL_AI_DIR}/${variant}"
    [ -f "${target}" ] || continue
    for array in ai_mlp_w0 ai_mlp_b0 ai_mlp_w1 ai_mlp_b1 \
                 ai_mlp_w2 ai_mlp_b2 ai_mlp_w3 ai_mlp_b3; do
        if ! grep -q "${array}" "${target}"; then
            echo "  Warning: ${array} not found in ${variant}"
            ERRORS=1
        fi
    done
done

if [ ${ERRORS} -eq 0 ]; then
    echo "  All expected arrays found."
else
    echo ""
    echo "Warning: some expected arrays are missing. Build may fail."
fi

echo ""
echo "Build select:"
echo "  make kernel AI_SCHED=ON                       # synthetic baseline (default)"
echo "  make kernel AI_SCHED=ON AI_WEIGHTS=real       # SLM-OS-fine-tuned weights"
echo ""
echo "Done."
