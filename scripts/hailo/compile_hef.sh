#!/bin/bash
#
# compile_hef.sh — wrap Hailo DFC 3.x to compile scheduler_mlp_*.onnx into a .hef
#
# Assumes a Python venv is active that has hailo_dataflow_compiler installed
# (the `hailo` CLI must be on PATH). For Hailo-8/8L use DFC 3.33.1 — the 5.x
# track is for Hailo-10H and produces incompatible artifacts.
#
# Usage:
#   scripts/hailo/compile_hef.sh [--arch hailo8|hailo8l] [--variant pi5|jetson]
#                                [--calib <path>] [--outdir <path>]
#
# Default flow (compiles scheduler_mlp_pi5.onnx for hailo8 into a .hef):
#   1. Run scripts/hailo/export_scheduler_mlp_onnx.py (once)
#   2. Run scripts/hailo/generate_calibration_data.py (once)
#   3. Run this script
#
# Output: build/hailo/scheduler_mlp_<variant>.hef
#
# DFC 3.x compilation is a 3-phase pipeline:
#   hailo parser onnx <model.onnx>    -> <model>.har            (frozen IR)
#   hailo optimize <model>.har        -> <model>_optimized.har  (int8 quantized)
#   hailo compiler <optimized>.har    -> <model>.hef            (target binary)
#
# The single-command `hailo compiler` with --calib-set-path also works on
# newer 3.x releases but invoking the phases explicitly surfaces per-phase
# failures more clearly when anything goes wrong.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ARCH="hailo8"
VARIANT="pi5"
CALIB="$REPO_ROOT/build/hailo/calibration_states.npy"
OUTDIR="$REPO_ROOT/build/hailo"
KEEP_INTERMEDIATES=0

usage() {
    sed -n '3,30p' "$0"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)    ARCH="$2"; shift 2 ;;
        --variant) VARIANT="$2"; shift 2 ;;
        --calib)   CALIB="$2"; shift 2 ;;
        --outdir)  OUTDIR="$2"; shift 2 ;;
        --keep-intermediates) KEEP_INTERMEDIATES=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

case "$ARCH" in
    hailo8|hailo8l) ;;
    *) echo "ERROR: --arch must be hailo8 or hailo8l (got '$ARCH')" >&2; exit 2 ;;
esac

case "$VARIANT" in
    pi5|jetson) ;;
    *) echo "ERROR: --variant must be pi5 or jetson (got '$VARIANT')" >&2; exit 2 ;;
esac

ONNX="$OUTDIR/scheduler_mlp_${VARIANT}.onnx"
HAR_RAW="$OUTDIR/scheduler_mlp_${VARIANT}.har"
HAR_OPT="$OUTDIR/scheduler_mlp_${VARIANT}_optimized.har"
HEF="$OUTDIR/scheduler_mlp_${VARIANT}.hef"

if ! command -v hailo >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: 'hailo' CLI not on PATH.

Activate the Hailo DFC venv first:
    source ~/hailo-venv/bin/activate

Or install DFC into a fresh venv (check the DFC release notes for the
exact Python version pin — DFC 3.33.1 expects Python 3.10):
    python3.10 -m venv ~/hailo-venv
    source ~/hailo-venv/bin/activate
    pip install path/to/hailo_dataflow_compiler-3.33.1-*.whl
EOF
    exit 3
fi

if [ ! -f "$ONNX" ]; then
    echo "ERROR: $ONNX not found. Run scripts/hailo/export_scheduler_mlp_onnx.py first." >&2
    exit 4
fi

if [ ! -f "$CALIB" ]; then
    echo "ERROR: $CALIB not found. Run scripts/hailo/generate_calibration_data.py first." >&2
    exit 5
fi

mkdir -p "$OUTDIR"
cd "$OUTDIR"

echo "[compile] arch=$ARCH variant=$VARIANT onnx=$ONNX"
echo "[compile] calibration: $CALIB ($(python3 -c "import numpy as np; d=np.load('$CALIB'); print(d.shape, d.dtype)"))"

echo "[compile] phase 1/3: hailo parser onnx -> $HAR_RAW"
hailo parser onnx "$ONNX" \
    --hw-arch "$ARCH" \
    --har-path "$HAR_RAW" \
    --net-name "scheduler_mlp_${VARIANT}" \
    -y

echo "[compile] phase 2/3: hailo optimize (int8 quantize) -> $HAR_OPT"
hailo optimize "$HAR_RAW" \
    --hw-arch "$ARCH" \
    --calib-set-path "$CALIB" \
    --output-har-path "$HAR_OPT"

echo "[compile] phase 3/3: hailo compiler -> $HEF"
hailo compiler "$HAR_OPT" \
    --hw-arch "$ARCH" \
    --output-dir "$OUTDIR"

# `hailo compiler` writes <net-name>.hef into --output-dir; rename defensively
# (net-name was set during parser phase). Common generated names include both
# scheduler_mlp_<variant>.hef and <variant>_compiled_model.hef across versions.
for candidate in \
    "$OUTDIR/scheduler_mlp_${VARIANT}.hef" \
    "$OUTDIR/scheduler_mlp_${VARIANT}_compiled_model.hef" \
    "$OUTDIR/$(basename "${HAR_OPT%.har}").hef"; do
    if [ -f "$candidate" ] && [ "$candidate" != "$HEF" ]; then
        mv "$candidate" "$HEF"
        break
    fi
done

if [ $KEEP_INTERMEDIATES -eq 0 ]; then
    rm -f "$HAR_RAW" "$HAR_OPT"
fi

echo "[compile] done: $HEF"
ls -la "$HEF"
