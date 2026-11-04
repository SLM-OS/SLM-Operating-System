#!/bin/bash
#
# test-build-mnist-shaders.sh - Host-only test for the cubin selection
# and section-table parsing in scripts/cuda/build-mnist-shaders.sh.
#
# Two bugs found while provisioning jetson-nano-1 (#1043) with CUDA 12.0:
#   1. cuobjdump names its outputs tmpxft_<id>-N.<arch>.cubin, not
#      <src>.N.<arch>.cubin, and the old fallback glob failed under
#      `set -euo pipefail`.
#   2. readelf prints section numbers below 10 as "[ 9]", which awk
#      splits into two fields, so the old parser read the Addr column
#      (0) as the offset and carved the ELF header instead of SASS.
#
# nvcc, cuobjdump and readelf are replaced by stubs, so no CUDA toolkit
# is needed. Each case checks the exact bytes written to the .sass file.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_SCRIPT="${BUILD_SCRIPT:-$SCRIPT_DIR/cuda/build-mnist-shaders.sh}"
[[ -f "$BUILD_SCRIPT" ]] || { echo "FAIL: $BUILD_SCRIPT not found"; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
STUBS="$TMP/stubs"
mkdir -p "$STUBS"

# nvcc stub: `nvcc -arch=X -o <out> <src.cu>` just creates <out>.
cat > "$STUBS/nvcc" <<'EOF'
#!/bin/bash
while [ $# -gt 0 ]; do
    if [ "$1" = "-o" ]; then : > "$2"; shift; fi
    shift
done
EOF

# cuobjdump stub: `cuobjdump --extract-elf all <binary>` writes two
# cubins into the cwd, named per $STUB_NAMING. The kernel cubin holds
# 0x40 filler bytes, then the 16-byte "SASS-<kernel>" marker at the
# .text offset (0x40), padded to 0x20 bytes.
cat > "$STUBS/cuobjdump" <<'EOF'
#!/bin/bash
src=$(basename "$3")
case "$STUB_NAMING" in
    old)  stub="$src.1.sm_87.cubin"; kern="$src.2.sm_87.cubin" ;;
    cuda12) stub="tmpxft_0001-0.sm_87.cubin"; kern="tmpxft_0001-1.sm_87.cubin" ;;
esac
printf 'STUBCUBIN' > "$stub"
{ head -c 64 /dev/zero | tr '\0' 'H'; printf '%-32s' "SASS-$src" | tr ' ' '.'; } > "$kern"
EOF

# readelf stub: prints a section table. The kernel cubin (the one
# whose content starts with H) gets a .text.<kernel> row at offset 0x40,
# size 0x20, numbered per $STUB_SECNUM ("[ 9]" or "[10]"). The host-stub
# cubin is not an ELF, so the stub fails on it the way real readelf does
# ("Failed to read file header", exit 1). It sorts first in both naming
# schemes, so every case checks that the build script skips a candidate
# readelf rejects instead of aborting under pipefail.
cat > "$STUBS/readelf" <<'EOF'
#!/bin/bash
f="$2"
if [ "$(head -c9 "$f")" = "STUBCUBIN" ]; then
    echo "readelf: Error: $f: Failed to read file header" >&2
    exit 1
fi
echo "  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al"
echo "  [ 1] .shstrtab         STRTAB          0000000000000000 000040 000100 00      0   0  1"
if [ "$(head -c1 "$f")" = "H" ]; then
    echo "  [$STUB_SECNUM] .text._Z6kernelv  PROGBITS        0000000000000000 000040 000020 00  AX  3 671088644 128"
fi
EOF
chmod +x "$STUBS/nvcc" "$STUBS/cuobjdump" "$STUBS/readelf"

# Number of entries in the build script's SHADERS table ("src:out.sass").
EXPECTED_SHADERS=$(grep -cE '^[[:space:]]+"[a-z0-9_]+:[a-z0-9_]+_shader\.sass"' "$BUILD_SCRIPT")
if [ "$EXPECTED_SHADERS" -lt 1 ]; then
    echo "FAIL: could not count the SHADERS table in $BUILD_SCRIPT"
    exit 2
fi

fails=0
run_case() {
    local naming="$1" secnum="$2"
    local out="$TMP/out-$naming-${secnum// /_}"
    if ! STUB_NAMING="$naming" STUB_SECNUM="$secnum" \
         NVCC="$STUBS/nvcc" CUOBJDUMP="$STUBS/cuobjdump" READELF="$STUBS/readelf" \
         bash "$BUILD_SCRIPT" "$out" > "$out.log" 2>&1; then
        echo "FAIL: naming=$naming secnum='[$secnum]': script exited non-zero"
        tail -5 "$out.log"
        fails=$((fails + 1))
        return
    fi
    local f got want n
    # Every shader in the build script's SHADERS table must be written;
    # a run that exits 0 with nothing produced is a failure, not a pass.
    n=$(find "$out" -maxdepth 1 -name '*_shader.sass' | wc -l)
    if [ "$n" -ne "$EXPECTED_SHADERS" ]; then
        echo "FAIL: naming=$naming secnum='[$secnum]': $n shader(s) written, want $EXPECTED_SHADERS"
        fails=$((fails + 1))
        return
    fi
    for f in "$out"/*_shader.sass; do
        # e.g. conv2d_fp32_direct_shader.sass came from conv2d_fp32_direct.
        local src
        src=$(basename "$f" _shader.sass)
        got=$(cat "$f")
        want=$(printf '%-32s' "SASS-$src" | tr ' ' '.')
        if [ "$got" != "$want" ]; then
            echo "FAIL: naming=$naming secnum='[$secnum]': $(basename "$f") = '$got', want '$want'"
            fails=$((fails + 1))
            return
        fi
    done
    echo "PASS: naming=$naming secnum='[$secnum]'"
}

run_case old    "10"
run_case old    " 9"
run_case cuda12 "10"
run_case cuda12 " 9"

# A section table that parses to offset 0 must be rejected, not carved.
cat > "$STUBS/readelf" <<'EOF'
#!/bin/bash
echo "  [ 9] .text._Z6kernelv  PROGBITS        0000000000000000 000000 000020 00  AX  3 0 128"
EOF
if STUB_NAMING=cuda12 NVCC="$STUBS/nvcc" CUOBJDUMP="$STUBS/cuobjdump" READELF="$STUBS/readelf" \
   bash "$BUILD_SCRIPT" "$TMP/out-zero" > "$TMP/zero.log" 2>&1; then
    echo "FAIL: offset 0 was accepted"
    fails=$((fails + 1))
elif grep -q 'bad .text offset/size' "$TMP/zero.log"; then
    echo "PASS: offset 0 rejected"
else
    echo "FAIL: offset 0 rejected for the wrong reason:"; tail -3 "$TMP/zero.log"
    fails=$((fails + 1))
fi

# A section row whose Off/Size columns aren't hex (a parse that went
# wrong) must be rejected with the script's own error, not a bash
# arithmetic failure.
cat > "$STUBS/readelf" <<'EOF'
#!/bin/bash
echo "  [ 9] .text._Z6kernelv  PROGBITS        0000000000000000 PROGBITS AX"
EOF
if STUB_NAMING=cuda12 NVCC="$STUBS/nvcc" CUOBJDUMP="$STUBS/cuobjdump" READELF="$STUBS/readelf" \
   bash "$BUILD_SCRIPT" "$TMP/out-nonhex" > "$TMP/nonhex.log" 2>&1; then
    echo "FAIL: non-hex offset was accepted"
    fails=$((fails + 1))
elif grep -q 'bad .text offset/size' "$TMP/nonhex.log"; then
    echo "PASS: non-hex offset rejected"
else
    echo "FAIL: non-hex offset rejected for the wrong reason:"; tail -3 "$TMP/nonhex.log"
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails case(s) failed"
    exit 1
fi
echo "All build-mnist-shaders cases passed"
