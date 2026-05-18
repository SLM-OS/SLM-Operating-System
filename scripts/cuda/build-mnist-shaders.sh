#!/usr/bin/env bash
# build-mnist-shaders.sh — compile the five MNIST-pipeline CUDA kernels
# down to raw SASS .text blobs for `gpu-kernel-mnist`.
#
# Output is a flat directory of `*_shader.sass` files matching the
# names the helper loads at runtime (see `scripts/gpu-kernel-mnist.c`):
#
#     conv2d_fp32_direct_shader.sass
#     add_bias_relu_fp32_shader.sass
#     maxpool2d_fp32_shader.sass
#     gemm_fp32_shader.sass                 (SIMT FP32 GEMM)
#     gemm_hmma_fp32a_fp16w_shader.sass     (HMMA tier — FP32 act × FP16 wt)
#
# The .sass files are intentionally gitignored (`.gitignore` line 95);
# this script regenerates them deterministically from the .cu sources
# in this same directory. Two host configurations work:
#
#   1. Dev machine cross-build (preferred). Any x86_64 Linux with
#      the NVIDIA CUDA Toolkit installed. `nvcc -arch=sm_87` produces
#      Orin device code from x86 hosts. Output is then `scp`'d to
#      each Jetson's `$SLMOS_HELPER_DIR` (default `/root/gpu-mnist`).
#
#   2. Native build on a Jetson. `/usr/local/cuda/bin` already in
#      the Jetson's PATH at L4T R36. Output lives next to the helper.
#
# In either case `cuobjdump --extract-elf` and `readelf -SW` carve
# the raw GA10B SASS bytes out of the cubin's `.text.<kernel>`
# section — that's the format `gpu_load_shader_buffer` expects.
#
# Usage:
#   scripts/cuda/build-mnist-shaders.sh [output-dir]
#
# Environment overrides:
#   ARCH        nvcc -arch (default: sm_87 = GA10B = Orin Nano/NX/AGX)
#   NVCC        nvcc path  (default: auto-detect in PATH or
#                           /usr/local/cuda/bin)
#   CUOBJDUMP   cuobjdump path (default: alongside NVCC)
#   READELF     readelf path (default: from PATH)
#
# Exit codes:
#   0  — all shaders built
#   1  — toolchain missing (nvcc / cuobjdump / readelf)
#   2  — a kernel failed to compile or its SASS couldn't be extracted

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

OUT_DIR="${1:-$REPO_ROOT/build/cuda/mnist-shaders}"
ARCH="${ARCH:-sm_87}"

# Toolchain auto-detect.
find_tool() {
    local name="$1"
    local override="${2:-}"
    if [ -n "$override" ] && [ -x "$override" ]; then
        printf '%s\n' "$override"
        return 0
    fi
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
        return 0
    fi
    if [ -x "/usr/local/cuda/bin/$name" ]; then
        printf '/usr/local/cuda/bin/%s\n' "$name"
        return 0
    fi
    return 1
}

NVCC="$(find_tool nvcc "${NVCC:-}" 2>/dev/null || true)"
CUOBJDUMP="$(find_tool cuobjdump "${CUOBJDUMP:-}" 2>/dev/null || true)"
# readelf is part of binutils — lives in /usr/bin on every reasonable
# host, no CUDA-dir fallback needed.
READELF="${READELF:-$(command -v readelf 2>/dev/null || true)}"

if [ -z "$NVCC" ] || [ -z "$CUOBJDUMP" ]; then
    cat >&2 <<EOF
build-mnist-shaders: CUDA toolchain not found.
Looked for nvcc and cuobjdump in PATH and /usr/local/cuda/bin.

Install options:
  Ubuntu/Debian: sudo apt install nvidia-cuda-toolkit
  Official:      https://developer.nvidia.com/cuda-downloads
                 (pick "x86_64 Linux" — cross-builds to sm_87 fine)
  Jetson:        already installed by L4T at /usr/local/cuda/

Or build natively on a Jetson where /usr/local/cuda is provisioned.
EOF
    exit 1
fi

if [ -z "$READELF" ]; then
    echo "build-mnist-shaders: readelf not found in PATH" >&2
    exit 1
fi

echo "build-mnist-shaders:"
echo "  nvcc:      $NVCC"
echo "  cuobjdump: $CUOBJDUMP"
echo "  readelf:   $READELF"
echo "  arch:      $ARCH"
echo "  output:    $OUT_DIR"
echo

mkdir -p "$OUT_DIR"

# Intermediate build dir so cubin / host-stub binaries don't pollute
# scripts/cuda/ or the output directory.
WORK_DIR="$(mktemp -d -t mnist-sass-XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

# Map: .cu basename → output _shader.sass name.
# Both the SIMT and HMMA gemm flavors are listed; gpu-kernel-mnist
# picks which to load via the `--gemm-tier hmma|simt` CLI flag.
SHADERS=(
    "conv2d_fp32_direct:conv2d_fp32_direct_shader.sass"
    "add_bias_relu_fp32:add_bias_relu_fp32_shader.sass"
    "maxpool2d_fp32:maxpool2d_fp32_shader.sass"
    "gemm_fp32:gemm_fp32_shader.sass"
    "gemm_hmma_fp32a_fp16w:gemm_hmma_fp32a_fp16w_shader.sass"
)

for entry in "${SHADERS[@]}"; do
    src="${entry%%:*}"
    out_name="${entry##*:}"
    src_cu="$HERE/${src}.cu"
    out_file="$OUT_DIR/$out_name"

    if [ ! -f "$src_cu" ]; then
        echo "ERROR: $src_cu not found" >&2
        exit 2
    fi

    echo "=== $src ==="

    # 1. Compile to a fat-binary host stub.
    (cd "$WORK_DIR" && "$NVCC" -arch="$ARCH" -o "$src" "$src_cu")

    # 2. Extract cubins. cuobjdump writes one host stub + one device
    # cubin, but the filename pattern differs across toolkit versions:
    #   CUDA 12.6 (Jetson L4T):  "<src>.{1,2}.<arch>.cubin"
    #   CUDA 12.0 (Ubuntu repo): "tmpxft_<pid>_00000000-{0,1}.<arch>.cubin"
    # Rather than enumerate every version's pattern, scan all cubins
    # produced this iteration and pick the one whose ELF carries a
    # `.text.<kernel>` section — that's always the device cubin
    # regardless of filename. Clean the work dir first so we don't
    # match cubins from earlier iterations.
    rm -f "$WORK_DIR"/*.cubin
    (cd "$WORK_DIR" && "$CUOBJDUMP" --extract-elf all "$src" >/dev/null)

    cubin=""
    for candidate in "$WORK_DIR"/*."$ARCH".cubin; do
        [ -f "$candidate" ] || continue
        if "$READELF" -SW "$candidate" 2>/dev/null | \
                grep -q '\.text\.'; then
            cubin="$candidate"
            break
        fi
    done
    if [ -z "$cubin" ]; then
        echo "ERROR: no device cubin (with .text.<kernel> section) " \
             "produced for $src" >&2
        exit 2
    fi

    # 3. readelf -SW dumps the section header table. We want the
    #    `.text.<kernel>` section — cuobjdump emits one per
    #    __global__. Match `.text.` (with trailing dot) to skip the
    #    cubin's combined `.text` slot.
    #
    # Parse Offset/Size from the RIGHT of the row, not the left.
    # Reasons:
    #   - "[Nr]" may be one token "[N]" or two tokens "[" "N]"
    #     depending on Nr width (single vs multi-digit).
    #   - The Address column is 8 hex digits on CUDA 12.6 / Jetson
    #     but 16 hex digits on CUDA 12.0 / Ubuntu. Both shifts
    #     perturb left-anchored positional parsing.
    # The right-side columns are stable across toolkit versions:
    #     ... Off Size ES Flg Lk Inf Al
    # So Offset = $(NF-6), Size = $(NF-5).
    line_fields="$("$READELF" -SW "$cubin" | \
        awk '/\.text\./ { print $(NF-6), $(NF-5); exit }')"
    if [ -z "$line_fields" ]; then
        echo "ERROR: no .text.<kernel> section in $cubin" >&2
        exit 2
    fi
    off_hex="${line_fields% *}"
    size_hex="${line_fields#* }"
    off_dec=$((16#$off_hex))
    size_dec=$((16#$size_hex))
    echo "  .text section: offset=0x$off_hex size=0x$size_hex"

    # 4. Carve the SASS bytes out into the output file.
    dd if="$cubin" of="$out_file" bs=1 skip="$off_dec" count="$size_dec" status=none
    echo "  wrote $out_file ($size_dec bytes)"
done

echo
echo "All MNIST shaders built into $OUT_DIR:"
ls -la "$OUT_DIR"/*_shader.sass
