#!/usr/bin/env bash
# ensure-oplib-shaders.sh - make sure a Jetson kernel embeds current SASS (#475).
#
# `make kernel PLATFORM=JETSON_ORIN_NANO` runs this before CMake. The SASS in
# scripts/cuda/operator_library/ is gitignored, so a fresh checkout has none,
# and a kernel built without it boots with every GPU consumer on the CPU.
#
#   - SASS present and newer than every CUDA source: nothing to do.
#   - Missing or stale, nvcc available: run build-oplib-shaders.sh. A failed
#     build fails the kernel build.
#   - Missing or stale, no nvcc: the kernel still builds; a warning says the
#     GPU paths will be CPU-only (missing) or may not match the sources (stale).
#
# "Current" is the stamp operator_library/.built, written after a successful
# build, against scripts/cuda/*.cu, slm/*.cu, slm/*.cuh, slm/*.h, the legacy
# manifest and the build script. SASS with no stamp (copied in by hand) counts
# as stale.
#
# Environment: NVCC (as build-oplib-shaders.sh), CUDA_DIR (default: this
# script's directory), OPLIB_BUILD (default: build-oplib-shaders.sh there).
# Exit codes: 0 ready or warned; 2 the shader build failed.
set -uo pipefail

DIR="${CUDA_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}"
LIB="$DIR/operator_library"
STAMP="$LIB/.built"
BUILD="${OPLIB_BUILD:-$DIR/build-oplib-shaders.sh}"

nvcc_path() {
    local n="${NVCC:-$(command -v nvcc 2>/dev/null || true)}"
    if [ -z "$n" ] && [ -x /usr/local/cuda/bin/nvcc ]; then
        n=/usr/local/cuda/bin/nvcc
    fi
    [ -n "$n" ] && [ -x "$n" ] && echo "$n"
}

warn() {
    {
        echo "##########################################################################"
        echo "# WARNING: $1"
        echo "# The SLM GPU path will be CPU-only, as will MNIST, sched and eviction."
        echo "# Install the CUDA Toolkit and rerun, or run \`make oplib-shaders\`."
        echo "##########################################################################"
    } >&2
}

have_sass=0
compgen -G "$LIB/*_shader.sass" > /dev/null && have_sass=1

stale=0
if [ "$have_sass" = 0 ] || [ ! -e "$STAMP" ]; then
    stale=1
else
    for f in "$DIR"/*.cu "$DIR"/slm/*.cu "$DIR"/slm/*.cuh "$DIR"/slm/*.h \
             "$LIB/MANIFEST.json" "$BUILD"; do
        if [ -e "$f" ] && [ "$f" -nt "$STAMP" ]; then
            stale=1
            break
        fi
    done
fi
[ "$stale" = 0 ] && exit 0

if NVCC="$(nvcc_path)"; then
    export NVCC
    echo "oplib-shaders: SASS $( [ "$have_sass" = 1 ] && echo stale || echo missing ), building with $NVCC"
    if ! bash "$BUILD"; then
        echo "oplib-shaders: shader build failed; not building a kernel without GPU kernels" >&2
        exit 2
    fi
    touch "$STAMP"
    exit 0
fi

if [ "$have_sass" = 0 ]; then
    warn "no SASS in $LIB and no nvcc to build it."
else
    {
        echo "WARNING: the SASS in $LIB may not match the CUDA sources (no nvcc to"
        echo "         rebuild it); GPU results may differ from the sources until \`make oplib-shaders\`."
    } >&2
fi
exit 0
