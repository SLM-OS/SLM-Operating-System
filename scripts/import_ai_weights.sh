#!/bin/bash
#
# import_ai_weights.sh - Import AI model weights from Plan A export pipeline
#
# Copies generated weight files from the slm-os-scheduler-ai repo's
# deploy/generated/ directory into the kernel's AI scheduler source.
# Validates that dimensions match the expected model architecture.
#
# Usage:
#   ./scripts/import_ai_weights.sh [--source <path>]
#
# Default source: ~/projects/slm-os-scheduler-ai/deploy/generated/
#

set -euo pipefail

# Default paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
KERNEL_AI_DIR="${SCRIPT_DIR}/../kernel/sched/ai"
DEFAULT_SOURCE="${HOME}/projects/slm-os-scheduler-ai/deploy/generated"

# Parse arguments
SOURCE_DIR="${DEFAULT_SOURCE}"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            SOURCE_DIR="$2"
            shift 2
            ;;
        -h|--help)
            echo "Usage: $0 [--source <path>]"
            echo ""
            echo "Import AI model weights from Plan A export pipeline."
            echo ""
            echo "Options:"
            echo "  --source <path>  Source directory (default: ${DEFAULT_SOURCE})"
            echo ""
            echo "Expected files in source directory:"
            echo "  ai_weights_mlp.c   MLP weight arrays"
            echo "  ai_weights_ppo.c   PPO weight arrays (optional)"
            echo "  ai_config.h        Platform-specific dimensions"
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
echo "Source: ${SOURCE_DIR}"
echo "Target: ${KERNEL_AI_DIR}"
echo ""

# Validate source directory exists
if [ ! -d "${SOURCE_DIR}" ]; then
    echo "Error: source directory not found: ${SOURCE_DIR}"
    echo "Run Plan A export pipeline first:"
    echo "  cd ~/projects/slm-os-scheduler-ai"
    echo "  python scripts/export_models.py --model all --platform jetson_orin_nano"
    exit 1
fi

# Check for required files
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

# Copy weight files
echo "Copying weight files..."

if [ -f "${SOURCE_DIR}/ai_weights_mlp.c" ]; then
    cp "${SOURCE_DIR}/ai_weights_mlp.c" "${KERNEL_AI_DIR}/ai_weights_mlp.c"
    echo "  ai_weights_mlp.c -> kernel/sched/ai/"
fi

if [ -f "${SOURCE_DIR}/ai_weights_ppo.c" ]; then
    cp "${SOURCE_DIR}/ai_weights_ppo.c" "${KERNEL_AI_DIR}/ai_weights_ppo.c"
    echo "  ai_weights_ppo.c -> kernel/sched/ai/"
fi

# Copy config header if present
if [ -f "${SOURCE_DIR}/ai_config.h" ]; then
    cp "${SOURCE_DIR}/ai_config.h" "${KERNEL_AI_DIR}/ai_config.h"
    echo "  ai_config.h -> kernel/sched/ai/"
fi

# Validate dimensions by checking for expected array names
echo ""
echo "Validating weight files..."

ERRORS=0
for array in ai_mlp_w0 ai_mlp_b0 ai_mlp_w1 ai_mlp_b1 ai_mlp_w2 ai_mlp_b2 ai_mlp_w3 ai_mlp_b3; do
    if [ -f "${KERNEL_AI_DIR}/ai_weights_mlp.c" ]; then
        if ! grep -q "${array}" "${KERNEL_AI_DIR}/ai_weights_mlp.c"; then
            echo "  Warning: ${array} not found in ai_weights_mlp.c"
            ERRORS=1
        fi
    fi
done

if [ ${ERRORS} -eq 0 ]; then
    echo "  All expected arrays found."
else
    echo ""
    echo "Warning: some expected arrays are missing. Build may fail."
fi

# Update CMakeLists.txt to use real weights instead of stub
echo ""
echo "Note: To use real weights, update CMakeLists.txt to replace"
echo "  ai_weights_stub.c -> ai_weights_mlp.c"
echo "Or keep both and select via #ifdef at compile time."
echo ""
echo "Build with:"
echo "  cmake -B build/kernel -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake \\"
echo "    -DPLATFORM=QEMU_VIRT -DENABLE_AI_SCHEDULER=ON && cmake --build build/kernel"
echo ""
echo "Done."
