#!/bin/bash
# gqa-emu-test.sh - host checks of the HMMA GQA kernels (#1167).
#
#   1. gqa_emu_test on scripts/cuda/slm/slm_gqa_hmma.cu: every case of
#      gqa_check.h, lane by lane, must pass (--quick: KV up to 600 only).
#   2. Three mutants of the source must fail, which shows the checks can see
#      each class of defect:
#        mask     the causal mask lets in one position too many
#        barrier  no barrier before a K/V stage is refilled (seen with cp.async
#                 landing at issue)
#        wait     no cp.async wait before a stage is read (seen with cp.async
#                 landing at the wait)
#   3. With nvcc, the cubin passes gqa-sass-check.py (launchable by SLM-OS).
#
# Usage: gqa-emu-test.sh [--quick]. Exit 0 when all of the above hold.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../slm/slm_gqa_hmma.cu"
WORK="$(mktemp -d -t gqa-emu-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
QUICK=""
[ "${1:-}" = --quick ] && QUICK=--quick
CXX="${CXX:-g++}"
FLAGS=(-O2 -std=c++17 -fopenmp -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-function -I"$HERE")
rc=0

build() { # <kernel source> <binary>
    "$CXX" "${FLAGS[@]}" -I"$(dirname "$1")" -DGQA_KERNEL_SRC="\"$(basename "$1")\"" \
        "$HERE/gqa_emu_test.cpp" -o "$2"
}

echo "== gqa_emu_test (source as is)"
build "$SRC" "$WORK/emu" || exit 2
if "$WORK/emu" $QUICK; then
    echo "gqa-emu-test: source PASS"
else
    echo "gqa-emu-test: source FAIL"
    rc=1
fi

mutant() { # <name> <sed expression> <case filter>
    mkdir -p "$WORK/$1"
    sed "$2" "$SRC" > "$WORK/$1/slm_gqa_hmma.cu"
    if cmp -s "$SRC" "$WORK/$1/slm_gqa_hmma.cu"; then
        echo "gqa-emu-test: mutant $1 did not change the source"
        rc=1
        return
    fi
    build "$WORK/$1/slm_gqa_hmma.cu" "$WORK/$1/emu" || { rc=1; return; }
    if "$WORK/$1/emu" --quick --only "$3" > "$WORK/$1/log" 2>&1; then
        echo "gqa-emu-test: mutant $1 passed the checks (must fail)"
        rc=1
    else
        echo "gqa-emu-test: mutant $1 fails as it must: $(grep -c FAIL "$WORK/$1/log") failing checks; $(grep -m1 -E 'FAIL|gqa_emu:' "$WORK/$1/log" | cut -c1-110)"
    fi
}
echo "== mutants"
mutant mask 's/t < lim\[e >> 1\]/t <= lim[e >> 1]/' "opcheck rows"
mutant barrier '/everyone is done with stage st before it is refilled/d' "kv 512 rows 64"
mutant wait 's/^            gq_cp_wait1();$//' "kv 512 rows 64"

if command -v nvcc > /dev/null; then
    echo "== sm_87 cubin"
    nvcc -arch=sm_87 -O3 --cubin -I"$HERE/../slm" "$SRC" -o "$WORK/gqa.cubin" || exit 2
    python3 "$HERE/gqa-sass-check.py" "$WORK/gqa.cubin" || rc=1
fi
echo "gqa-emu-test: $([ $rc = 0 ] && echo PASS || echo FAIL)"
exit $rc
