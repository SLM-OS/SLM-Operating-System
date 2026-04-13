#!/bin/bash
#
# import_eviction_weights.sh — Import trained eviction models
#
# Copies the generated XGBoost (~1.3 MB) and int8-quantized MLP
# (~5 KB) Rust files from the sibling slm-os-page-sim project into
# runtime/src/mm/eviction/generated/. Rewrites the sibling's
# `use crate::mm::eviction_policy::BlockFeatures;` to this repo's
# `use crate::mm::eviction::policy::BlockFeatures;` and validates the
# expected public symbols are present.
#
# After running this script, build with:
#   make kernel AI_EVICTION_MODELS=ON
#
# Usage:
#   ./scripts/import_eviction_weights.sh [--source <path>]
#
# Default source: ~/projects/slm-os-page-sim/data/export/
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_DIR="${REPO_ROOT}/runtime/src/mm/eviction/generated"
DEFAULT_SOURCE="${HOME}/projects/slm-os-page-sim/data/export"

SOURCE_DIR="${DEFAULT_SOURCE}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            SOURCE_DIR="$2"
            shift 2
            ;;
        -h|--help)
            cat <<EOF
Usage: $0 [--source <path>]

Import trained eviction models from slm-os-page-sim into
runtime/src/mm/eviction/generated/.

Options:
  --source <path>   Source directory containing exported .rs files.
                    Default: ${DEFAULT_SOURCE}

Expected files:
  xgb_policy_generated.rs   XGBoost if-else chain (pub fn xgb_predict)
  mlp_policy_generated.rs   Int8 MLP weights (pub fn mlp_predict)
  mlp_policy_f32.rs         Float32 MLP reference (for cross-check tests)
EOF
            exit 0
            ;;
        *)
            echo "Error: unknown option '$1'" >&2
            exit 1
            ;;
    esac
done

echo "Eviction Weights Import"
echo "======================="
echo "Source: ${SOURCE_DIR}"
echo "Target: ${TARGET_DIR}"
echo

if [ ! -d "${SOURCE_DIR}" ]; then
    echo "Error: source directory not found: ${SOURCE_DIR}" >&2
    echo "Run the sibling project's export pipeline first:" >&2
    echo "  cd ~/projects/slm-os-page-sim" >&2
    echo "  python scripts/export_to_slmos.py --output-dir data/export/" >&2
    exit 1
fi

REQUIRED=(xgb_policy_generated.rs mlp_policy_generated.rs)
OPTIONAL=(mlp_policy_f32.rs)

MISSING=0
for f in "${REQUIRED[@]}"; do
    if [ ! -f "${SOURCE_DIR}/${f}" ]; then
        echo "Error: missing ${f} in ${SOURCE_DIR}" >&2
        MISSING=1
    fi
done
if [ ${MISSING} -ne 0 ]; then
    exit 1
fi

mkdir -p "${TARGET_DIR}"

# Rewrite the sibling's `use` path to match this repo's module layout,
# and replace `(expr).exp()` with `libm::expf(expr)` since the runtime
# is `no_std` (no `std::f32::exp` method available). The sibling's
# code is `std`-friendly; we adapt it at import time rather than
# asking the sibling to track both forms.
import_file() {
    local name="$1"
    local src="${SOURCE_DIR}/${name}"
    local dst="${TARGET_DIR}/${name}"
    if [ ! -f "${src}" ]; then
        return 1
    fi
    sed -E \
        -e 's|use crate::mm::eviction_policy::|use crate::mm::eviction::policy::|g' \
        -e 's|\(-([a-zA-Z_][a-zA-Z0-9_]*)\)\.exp\(\)|libm::expf(-\1)|g' \
        "${src}" > "${dst}"
    # Add a provenance header so it's obvious the file is imported.
    # The sibling's own "Auto-generated from ..." header remains as the first
    # line; we append a second comment noting the import source + timestamp.
    local stamp
    stamp="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    sed -i "1a // Imported by scripts/import_eviction_weights.sh on ${stamp}" "${dst}"
    echo "  ${name} -> runtime/src/mm/eviction/generated/"
}

echo "Copying generated files..."
for f in "${REQUIRED[@]}"; do
    import_file "${f}"
done
for f in "${OPTIONAL[@]}"; do
    if [ -f "${SOURCE_DIR}/${f}" ]; then
        import_file "${f}"
    else
        echo "  (optional) ${f} not found in source — skipping"
    fi
done

echo
echo "Validating symbols..."
ERRORS=0

# XGBoost: pub fn xgb_predict
if ! grep -q '^pub fn xgb_predict' "${TARGET_DIR}/xgb_policy_generated.rs"; then
    echo "  Error: xgb_predict not found in xgb_policy_generated.rs" >&2
    ERRORS=$((ERRORS + 1))
fi

# MLP: pub fn mlp_predict + weight constants
if ! grep -q '^pub fn mlp_predict' "${TARGET_DIR}/mlp_policy_generated.rs"; then
    echo "  Error: mlp_predict not found in mlp_policy_generated.rs" >&2
    ERRORS=$((ERRORS + 1))
fi
for sym in W_L1 W_L2 W_L3 W_OUT; do
    if ! grep -q "pub const ${sym}" "${TARGET_DIR}/mlp_policy_generated.rs"; then
        echo "  Warning: ${sym} not found in mlp_policy_generated.rs" >&2
    fi
done

if [ ${ERRORS} -ne 0 ]; then
    echo
    echo "Validation failed with ${ERRORS} error(s). Files were imported but the" >&2
    echo "build will not succeed until the missing symbols are added." >&2
    exit 1
fi

echo "  All required symbols present."
echo
echo "Done. Build with:"
echo "  make kernel AI_EVICTION_MODELS=ON"
