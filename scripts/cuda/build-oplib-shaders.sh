#!/usr/bin/env bash
# build-oplib-shaders.sh - build every *_shader.sass the kernel can embed.
#
# Two groups, both into scripts/cuda/operator_library/ (gitignored):
#   - each entry of operator_library/MANIFEST.json, from scripts/cuda/<name>.cu
#   - the #475 SLM kernels, from scripts/cuda/slm/*.cu, plus slm/MANIFEST.json
#     with the launch facts read from each cubin.
#
# Without these files a kernel still builds and every op stays on the CPU, so a
# missing toolkit here is an error with instructions, not a silent stub.
#
# Usage: scripts/cuda/build-oplib-shaders.sh [output-dir]
# Environment: NVCC, ARCH (default sm_87), PYTHON (default python3),
#              MANIFEST_OUT (default scripts/cuda/slm/MANIFEST.json).
# Exit codes: 0 built; 1 toolkit missing; 2 compile or extraction failure.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="${1:-$HERE/operator_library}"
ARCH="${ARCH:-sm_87}"
PYTHON="${PYTHON:-python3}"
MANIFEST_OUT="${MANIFEST_OUT:-$HERE/slm/MANIFEST.json}"

NVCC="${NVCC:-$(command -v nvcc 2>/dev/null || true)}"
if [ -z "$NVCC" ] && [ -x /usr/local/cuda/bin/nvcc ]; then
    NVCC=/usr/local/cuda/bin/nvcc
fi
if [ -z "$NVCC" ] || [ ! -x "$NVCC" ]; then
    cat >&2 <<EOF
oplib-shaders: nvcc not found (looked in PATH and /usr/local/cuda/bin).

The GPU kernels are compiled from scripts/cuda/ with the NVIDIA CUDA Toolkit
and are not checked in. Install it and rerun \`make oplib-shaders\`:
  Ubuntu/Debian: sudo apt install nvidia-cuda-toolkit
  Official:      https://developer.nvidia.com/cuda-downloads
  Jetson:        provided by L4T at /usr/local/cuda/

A kernel built without these shaders has no GPU path: every op stays on the CPU.
EOF
    exit 1
fi

TOOLKIT="$("$NVCC" --version | sed -n 's/.*release \([0-9.]*\).*/\1/p' | head -1)"
echo "oplib-shaders: nvcc $NVCC (CUDA ${TOOLKIT:-unknown}), arch $ARCH, output $OUT_DIR"

mkdir -p "$OUT_DIR"
WORK="$(mktemp -d -t oplib-sass-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

compile() { # <src.cu> <out.cubin>
    "$NVCC" -arch="$ARCH" -O3 --cubin -I"$HERE/slm" "$1" -o "$2" || {
        echo "oplib-shaders: failed to compile $1" >&2
        exit 2
    }
}

# Legacy operator library: one kernel per source, named after the file. A
# manifest entry with no source is a roadmap item and is skipped.
LEGACY="$("$PYTHON" - "$HERE/operator_library/MANIFEST.json" <<'EOF'
import json, sys
m = json.load(open(sys.argv[1]))
for e in m["entries"]:
    print(e["sass"])
EOF
)"
for sass in $LEGACY; do
    name="${sass%_shader.sass}"
    src="$HERE/$name.cu"
    if [ ! -f "$src" ]; then
        echo "  $name: no source, skipped" >&2
        continue
    fi
    compile "$src" "$WORK/$name.cubin"
    "$PYTHON" "$HERE/oplib-extract.py" sass "$WORK/$name.cubin" "$name" "$OUT_DIR/$sass" || exit 2
done

# SLM kernels: several per source, every one carved out by name.
CUBINS=()
for src in "$HERE"/slm/*.cu; do
    base="$(basename "$src" .cu)"
    compile "$src" "$WORK/$base.cubin"
    CUBINS+=("$WORK/$base.cubin")
done
"$PYTHON" "$HERE/oplib-extract.py" manifest "CUDA ${TOOLKIT:-unknown}" \
    "$MANIFEST_OUT" "$OUT_DIR" "${CUBINS[@]}" || exit 2
"$PYTHON" - "$MANIFEST_OUT" "$HERE/oplib-extract.py" "$WORK" "$OUT_DIR" <<'EOF' || exit 2
import json, subprocess, sys
manifest, tool, work, out = sys.argv[1:5]
for e in json.load(open(manifest))["entries"]:
    cubin = f"{work}/{e['source'].rsplit('/', 1)[1][:-3]}.cubin"
    subprocess.run([sys.executable, tool, "sass", cubin, e["name"], f"{out}/{e['sass']}"], check=True)
EOF

echo "oplib-shaders: $(ls "$OUT_DIR"/*_shader.sass | wc -l) shaders in $OUT_DIR"
