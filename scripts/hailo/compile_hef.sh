#!/bin/bash
#
# compile_hef.sh — wrap Hailo DFC 3.x to compile any .onnx into a .hef
#
# Assumes a Python venv is active that has hailo_dataflow_compiler installed
# (the `hailo` CLI must be on PATH). For Hailo-8/8L use DFC 3.33.1 — the 5.x
# track is for Hailo-10H and produces incompatible artifacts.
#
# See docs/hailo-toolchain.md for install + setup instructions.
#
# Usage:
#   scripts/hailo/compile_hef.sh --onnx <path> --calib <path>
#                                [--name <model-name>] [--arch hailo8|hailo8l]
#                                [--outdir <path>] [--keep-intermediates]
#
# Arguments:
#   --onnx   <path>   Required. Path to the ONNX model to compile.
#   --calib  <path>   Required. Calibration dataset (.npy). Shape must match
#                     the model's input tensor (e.g., [N, 1, 28, 28] for MNIST
#                     NCHW, [N, 108] for the scheduler MLP).
#   --name   <str>    Optional. Network name + HEF filename base. Defaults to
#                     the ONNX filename stem.
#   --arch   <str>    Optional. hailo8 or hailo8l. Defaults to hailo8l (matches
#                     the AI HAT+ hardware).
#   --outdir <path>   Optional. Output directory. Defaults to build/hailo.
#   --keep-intermediates   Keep parser/optimize .har files on disk.
#
# Output: <outdir>/<name>.hef
#
# DFC 3.x compilation is a 3-phase pipeline:
#   hailo parser onnx <model.onnx>    -> <name>.har            (frozen IR)
#   hailo optimize <name>.har         -> <name>_optimized.har  (int8 quantized)
#   hailo compiler <optimized>.har    -> <name>.hef            (target binary)
#
# Examples:
#   # MNIST (28×28 grayscale classifier):
#   scripts/hailo/compile_hef.sh \
#       --onnx models/test/mnist.onnx \
#       --calib build/hailo/mnist_calib.npy
#
#   # Scheduler MLP (108-dim state → 42 actions):
#   scripts/hailo/compile_hef.sh \
#       --onnx build/hailo/scheduler_mlp_pi5.onnx \
#       --calib build/hailo/calibration_states.npy \
#       --name scheduler_mlp_pi5

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

ONNX=""
CALIB=""
NAME=""
ARCH="hailo8l"
OUTDIR="$REPO_ROOT/build/hailo"
KEEP_INTERMEDIATES=0

usage() {
    sed -n '3,38p' "$0"
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --onnx)    ONNX="$2"; shift 2 ;;
        --calib)   CALIB="$2"; shift 2 ;;
        --name)    NAME="$2"; shift 2 ;;
        --arch)    ARCH="$2"; shift 2 ;;
        --outdir)  OUTDIR="$2"; shift 2 ;;
        --keep-intermediates) KEEP_INTERMEDIATES=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown arg: $1" >&2; usage ;;
    esac
done

if [ -z "$ONNX" ]; then
    echo "ERROR: --onnx is required" >&2
    usage
fi
if [ -z "$CALIB" ]; then
    echo "ERROR: --calib is required" >&2
    usage
fi

case "$ARCH" in
    hailo8|hailo8l) ;;
    *) echo "ERROR: --arch must be hailo8 or hailo8l (got '$ARCH')" >&2; exit 2 ;;
esac

# Derive name from ONNX filename stem if not supplied.
if [ -z "$NAME" ]; then
    NAME="$(basename "$ONNX" .onnx)"
fi

# Hailo net-name restriction: `.` inside a net name becomes illegal in the
# generated HAR, so drop any filename-version dots (model.v1.onnx → model_v1).
NAME="${NAME//./_}"

HAR_RAW="$OUTDIR/${NAME}.har"
HAR_OPT="$OUTDIR/${NAME}_optimized.har"
HAR_COMP="$OUTDIR/${NAME}_compiled.har"
HEF="$OUTDIR/${NAME}.hef"

if ! command -v hailo >/dev/null 2>&1; then
    cat >&2 <<'EOF'
ERROR: 'hailo' CLI not on PATH.

Activate the Hailo DFC venv first. For the venv in this repo:
    source venv/bin/activate

See docs/hailo-toolchain.md for install instructions.
EOF
    exit 3
fi

if [ ! -f "$ONNX" ]; then
    echo "ERROR: ONNX not found: $ONNX" >&2
    exit 4
fi

if [ ! -f "$CALIB" ]; then
    echo "ERROR: calibration dataset not found: $CALIB" >&2
    exit 5
fi

# Resolve to absolute paths before `cd` — relative paths wouldn't survive
# the chdir into $OUTDIR. `readlink -f` canonicalises and gives absolute.
ONNX="$(readlink -f "$ONNX")"
CALIB="$(readlink -f "$CALIB")"

mkdir -p "$OUTDIR"
OUTDIR="$(readlink -f "$OUTDIR")"
HAR_RAW="$OUTDIR/${NAME}.har"
HAR_OPT="$OUTDIR/${NAME}_optimized.har"
HAR_COMP="$OUTDIR/${NAME}_compiled.har"
HEF="$OUTDIR/${NAME}.hef"
cd "$OUTDIR"

echo "[compile] name=$NAME arch=$ARCH"
echo "[compile] onnx:  $ONNX"
echo "[compile] calib: $CALIB ($(python3 -c "import numpy as np; d=np.load('$CALIB'); print(d.shape, d.dtype)"))"

echo "[compile] phase 1/3: hailo parser onnx -> $HAR_RAW"
hailo parser onnx "$ONNX" \
    --hw-arch "$ARCH" \
    --har-path "$HAR_RAW" \
    --net-name "$NAME" \
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

# `hailo compiler` writes <net-name>.hef into --output-dir — net-name was set
# during the parser phase, so DFC 3.33.1 writes directly to $HEF. Verified on
# DFC 3.33.1: no rename needed. If a future DFC release changes the output
# filename convention, the failure is clearly visible (missing $HEF after the
# compiler runs) rather than silently producing the wrong file.
if [ ! -f "$HEF" ]; then
    echo "ERROR: expected $HEF but compiler produced something else." >&2
    # Enumerate any .hef files DFC may have written with an unexpected name,
    # without the default-bash "glob didn't match" noise.
    found=$(find "$OUTDIR" -maxdepth 1 -name '*.hef' -print 2>/dev/null)
    if [ -n "$found" ]; then
        echo "  Found .hef files:" >&2
        echo "$found" | sed 's/^/    /' >&2
    else
        echo "  (no .hef files in $OUTDIR)" >&2
    fi
    exit 6
fi

# Compiler also writes <net-name>_compiled.har (the compiled-model HAR)
# alongside the HEF. That's an intermediate too — remove unless asked to keep.
if [ $KEEP_INTERMEDIATES -eq 0 ]; then
    rm -f "$HAR_RAW" "$HAR_OPT" "$HAR_COMP"
fi

echo "[compile] done: $HEF"
ls -la "$HEF"
