#!/usr/bin/env bash
# Build one matrix cell (scripts/ci/build_matrix.py) and record the result.
# Report-only: always exits 0; the build's own exit code goes in result.json.
#
# Environment (set by .github/workflows/build-matrix.yml from the matrix):
#   CELL_ID, CELL_PLATFORM, CELL_TARGET, CELL_MAKE_ARGS, CELL_EXTRA_CMAKE
#   CELL_OUT   output directory (default ci-out/$CELL_ID)
#
# CELL_TARGET is a make target (kernel, kernel-test, kernel-kexec,
# kernel-bzimage) or "wide": `make kernel` with the arguments check-braces uses
# for its wide database, read from the Makefile's TIDY_WIDE_* variables.
#
# Outputs in $CELL_OUT: build.log, warnings.txt (unique, normalised compiler /
# rustc warning lines), result.json.
set -uo pipefail

: "${CELL_ID:?}" "${CELL_PLATFORM:?}" "${CELL_TARGET:?}"
CELL_MAKE_ARGS="${CELL_MAKE_ARGS:-}"
CELL_EXTRA_CMAKE="${CELL_EXTRA_CMAKE:-}"
OUT="${CELL_OUT:-ci-out/$CELL_ID}"
mkdir -p "$OUT"
LOG="$OUT/build.log"

target="$CELL_TARGET"
make_args="$CELL_MAKE_ARGS"
extra="$CELL_EXTRA_CMAKE"
cflags=""
if [ "$target" = "wide" ]; then
    # shellcheck disable=SC2016  # $(...) are make variables, expanded by make
    mapfile -t wide < <(make -s --no-print-directory \
        --eval '_ci_wide: ; @printf "%s\n%s\n%s\n" "$(TIDY_WIDE_MAKE_ARGS)" "$(TIDY_WIDE_CMAKE_ARGS)" "$(TIDY_WIDE_CFLAGS)"' \
        _ci_wide PLATFORM="$CELL_PLATFORM")
    make_args="${wide[0]} $make_args"
    extra="${wide[1]} $extra"
    cflags="${wide[2]}"
    target="kernel"
fi

cmd=(make "$target" "PLATFORM=$CELL_PLATFORM")
# shellcheck disable=SC2206  # make args are NAME=value words without spaces
cmd+=($make_args)
read -r -a _extra_words <<< "$extra"
extra="${_extra_words[*]}"
cmd+=("EXTRA_KERNEL_CMAKE_ARGS=$extra")

{
    echo "cell:    $CELL_ID"
    echo "command: CFLAGS='$cflags' ${cmd[*]}"
    echo "------------------------------------------------------------------"
} > "$LOG"

start=$(date +%s)
CFLAGS="$cflags" "${cmd[@]}" >> "$LOG" 2>&1
rc=$?
secs=$(( $(date +%s) - start ))

# Unique warning lines, with the checkout path and build-dir name normalised
# so cells can be compared. Cargo's "generated N warnings" summaries are dropped.
ws="${GITHUB_WORKSPACE:-$PWD}"
grep -E 'warning:' "$LOG" \
    | grep -vE 'generated [0-9]+ warnings?( \(.*\))?$' \
    | sed -e "s#${ws}/##g" -e 's#build/kernel[-a-z]*/#build/<dir>/#g' \
    | sort -u > "$OUT/warnings.txt"
total=$(grep -E 'warning:' "$LOG" | grep -cvE 'generated [0-9]+ warnings?( \(.*\))?$')
uniq=$(wc -l < "$OUT/warnings.txt")
cmake_w=$(grep -c 'CMake Warning' "$LOG")

# Things the CI host cannot supply (recorded, not faked).
notes=()
grep -q 'will be CPU-only' "$LOG" && notes+=("no nvcc: operator-library SASS not built, Jetson GPU paths CPU-only")
grep -q 'ENABLE_GSP_FIRMWARE=ON but' "$LOG" && notes+=("no /lib/firmware GSP blobs: ENABLE_GSP_FIRMWARE auto-disabled")
grep -q 'Rust lib:   (not built yet' "$LOG" && notes+=("Rust runtime library missing at configure: C stubs linked")

errors=$(grep -nE '(error|Error|ERROR)[: ]|\*\*\*|FATAL' "$LOG" | head -20)

R_MAKE_ARGS="$make_args" R_EXTRA="$extra" R_CFLAGS="$cflags" R_RC=$rc R_SECS=$secs \
R_TOTAL=$total R_UNIQ=$uniq R_CMAKE_W=$cmake_w R_ERRORS="$errors" \
python3 - "$OUT/result.json" "${notes[@]}" <<'PY'
import json, os, sys
e = os.environ
rc = int(e["R_RC"])
json.dump(dict(
    id=e["CELL_ID"], platform=e["CELL_PLATFORM"], target=e["CELL_TARGET"],
    make_args=e["R_MAKE_ARGS"].strip(), extra_cmake=" ".join(e["R_EXTRA"].split()),
    cflags=e["R_CFLAGS"], rc=rc, status="ok" if rc == 0 else "fail",
    seconds=int(e["R_SECS"]), warnings_total=int(e["R_TOTAL"]),
    warnings_unique=int(e["R_UNIQ"]), cmake_warnings=int(e["R_CMAKE_W"]),
    notes=sys.argv[2:], first_errors=[l for l in e["R_ERRORS"].splitlines() if l],
), open(sys.argv[1], "w"), indent=1)
PY

echo "cell $CELL_ID: rc=$rc ${secs}s warnings=$total (unique $uniq) cmake_warnings=$cmake_w"
[ "$rc" -eq 0 ] || tail -40 "$LOG"
exit 0
