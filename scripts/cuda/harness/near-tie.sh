#!/bin/bash
# near-tie.sh - where two greedy runs part, how close was the exact reference? (#1167, G4)
#
# Usage: near-tie.sh <model.gguf> <prompt.txt> <reply-a.txt> <reply-b.txt>
#   prompt.txt  the raw prompt, as `slm prompt --raw` receives it (bench/475/prompts/pN.txt)
#   reply-*.txt the two runs' replies exactly as printed (no trailing newline added)
#
# 1. The runtime's tokenizer (host test near_tie_token_ids, --ignored) turns the
#    prompt and replies into ids, checking that the replies round-trip.
# 2. near_tie.cpp teacher-forces the prompt and the shared reply tokens through
#    the exact CPU reference (ADR 0004) and prints the top two tokens and their
#    gap at the step where the runs part.
# No GPU needed. About 1 to 3 minutes on 8 cores.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
[ $# -eq 4 ] || { sed -n 2,13p "$0"; exit 2; }
export PATH="$HOME/.cargo/bin:$PATH"
WORK="$(mktemp -d -t near-tie-XXXXXX)"
trap 'rm -rf "$WORK"' EXIT
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
export SLM_GOLDEN_MODEL="$(abs "$1")" SLM_NEAR_TIE_PROMPT="$(abs "$2")" \
    SLM_NEAR_TIE_A="$(abs "$3")" SLM_NEAR_TIE_B="$(abs "$4")" SLM_NEAR_TIE_OUT="$WORK/ids.txt"
(cd "$HERE/../../../runtime" &&
    cargo test --release --target x86_64-unknown-linux-gnu --features slm near_tie_token_ids \
        -- --ignored --nocapture --test-threads=1 2>&1 | grep -E "^prompt |test result|panicked")
g++ -O2 -std=c++17 -fopenmp -I"$HERE" "$HERE/near_tie.cpp" -o "$WORK/near_tie"
"$WORK/near_tie" "$SLM_GOLDEN_MODEL" "$WORK/ids.txt" 2>/dev/null
