#!/bin/bash
#
# test-ensure-oplib-shaders.sh - host test for scripts/cuda/ensure-oplib-shaders.sh
# (#475): a Jetson `make kernel` builds missing or stale SASS when nvcc exists,
# and otherwise finishes with a warning that the GPU paths are CPU-only.
# Runs against a fake CUDA tree with a fake nvcc and build script.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ENSURE="$ROOT/scripts/cuda/ensure-oplib-shaders.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

CUDA="$TMP/cuda"
mkdir -p "$CUDA/operator_library" "$CUDA/slm"
echo '{}' > "$CUDA/operator_library/MANIFEST.json"
echo 'k' > "$CUDA/a.cu"
echo 'k' > "$CUDA/slm/b.cu"
printf '#!/bin/sh\necho fake\n' > "$TMP/nvcc"
chmod +x "$TMP/nvcc"
cat > "$TMP/build.sh" <<EOF
echo run >> "$TMP/builds"
[ -e "$TMP/break" ] && exit 2
echo sass > "$CUDA/operator_library/x_shader.sass"
EOF
builds() { [ -e "$TMP/builds" ] && wc -l < "$TMP/builds" || echo 0; }
run() { # <nvcc> -> rc, output in $TMP/out
    local rc=0
    CUDA_DIR="$CUDA" OPLIB_BUILD="$TMP/build.sh" NVCC="$1" bash "$ENSURE" > "$TMP/out" 2>&1 || rc=$?
    echo "$rc"
}

# No SASS, no nvcc: the build goes on, loudly.
[ "$(run /nonexistent/nvcc)" = 0 ] || fail "no nvcc: build must not stop"
grep -q "SLM GPU path will be CPU-only" "$TMP/out" || fail "no nvcc: no CPU-only warning"
[ "$(builds)" = 0 ] || fail "no nvcc: ran a build"
echo "PASS: missing SASS without nvcc warns and continues"

# No SASS, nvcc: builds it and stamps it.
[ "$(run "$TMP/nvcc")" = 0 ] || fail "nvcc: exit"
[ "$(builds)" = 1 ] || fail "nvcc: expected one build"
[ -e "$CUDA/operator_library/.built" ] || fail "nvcc: no stamp"
echo "PASS: missing SASS with nvcc is built"

# Current: nothing to do.
[ "$(run "$TMP/nvcc")" = 0 ] || fail "current: exit"
[ "$(builds)" = 1 ] || fail "current: rebuilt anyway"
echo "PASS: current SASS is left alone"

# A source newer than the stamp: rebuilt.
sleep 1
touch "$CUDA/slm/b.cu"
[ "$(run "$TMP/nvcc")" = 0 ] || fail "stale: exit"
[ "$(builds)" = 2 ] || fail "stale: not rebuilt"
echo "PASS: stale SASS is rebuilt"

# Stale without nvcc: still builds, says the SASS may not match.
sleep 1
touch "$CUDA/a.cu"
[ "$(run /nonexistent/nvcc)" = 0 ] || fail "stale, no nvcc: exit"
grep -q "may not match" "$TMP/out" || fail "stale, no nvcc: no warning"
echo "PASS: stale SASS without nvcc warns"

# SASS copied in by hand (no stamp) counts as stale.
rm -f "$CUDA/operator_library/.built"
[ "$(run "$TMP/nvcc")" = 0 ] || fail "no stamp: exit"
[ "$(builds)" = 3 ] || fail "no stamp: not rebuilt"
echo "PASS: unstamped SASS is rebuilt"

# A failed shader build fails the kernel build.
rm -f "$CUDA/operator_library/.built"
touch "$TMP/break"
[ "$(run "$TMP/nvcc")" = 2 ] || fail "broken build: expected exit 2"
grep -q "shader build failed" "$TMP/out" || fail "broken build: no message"
echo "PASS: a failed shader build stops the kernel build"
echo "All ensure-oplib-shaders tests passed"
