#!/usr/bin/env bash
#
# fetch-slm.sh — fetch and SHA256-verify a Small Language Model GGUF
# for the SLM-OS Jetson SLM integration (see docs/specs/slm-integration.md).
#
# Default model: Qwen2.5-1.5B-Instruct-Q4_K_M (~1.0 GB), the M5 demo target.
# Fall-back model: Llama-3.2-1B-Instruct-Q4_K_M (~0.8 GB), the spec's
# tokenizer-risk fallback.
#
# Output directory defaults to ./build/slm-models/. The script is idempotent:
# a file already on disk that matches the registered SHA256 is left alone.
#
# Usage:
#   scripts/fetch-slm.sh [--target DIR] [--model NAME] [--verify-only]
#                        [--print-sha] [--list]
#
# Options:
#   --target DIR     Where to put the GGUF (default: ./build/slm-models)
#   --model NAME     Which model to fetch from the registry below
#                    (default: qwen2.5-1.5b-instruct-q4_k_m).
#   --verify-only    Don't download; just verify the file already on disk.
#   --print-sha      Compute the SHA256 of the on-disk file and print it.
#                    Useful when the registered SHA256 is still TBD.
#   --list           List the registered models and exit.

set -euo pipefail

# -----------------------------------------------------------------------------
# Model registry. SHA256 strings marked `TBD-PIN-AFTER-FIRST-DOWNLOAD` are
# placeholder pins; they must be replaced with a real hex digest before the
# script will declare a verified download a success. The recommended workflow:
#
#   1. Run `scripts/fetch-slm.sh --model qwen2.5-1.5b-instruct-q4_k_m`.
#      The script will refuse the verify step but report the *actual* hash
#      of the freshly downloaded file.
#   2. Replace the TBD entry below with that hash and commit.
#
# This gate prevents an attacker swapping the upstream file from being
# silently accepted, while still letting a developer bootstrap the pin from
# their own download.
# -----------------------------------------------------------------------------

declare -A MODEL_URL=(
    [qwen2.5-1.5b-instruct-q4_k_m]="https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf"
    [llama-3.2-1b-instruct-q4_k_m]="https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q4_K_M.gguf"
)

declare -A MODEL_SHA256=(
    [qwen2.5-1.5b-instruct-q4_k_m]="TBD-PIN-AFTER-FIRST-DOWNLOAD"
    [llama-3.2-1b-instruct-q4_k_m]="TBD-PIN-AFTER-FIRST-DOWNLOAD"
)

declare -A MODEL_SIZE_MB=(
    [qwen2.5-1.5b-instruct-q4_k_m]="1014"
    [llama-3.2-1b-instruct-q4_k_m]="808"
)

declare -A MODEL_FILE=(
    [qwen2.5-1.5b-instruct-q4_k_m]="qwen2.5-1.5b-instruct-q4_k_m.gguf"
    [llama-3.2-1b-instruct-q4_k_m]="llama-3.2-1b-instruct-q4_k_m.gguf"
)

# -----------------------------------------------------------------------------
# CLI parsing
# -----------------------------------------------------------------------------
TARGET_DIR="./build/slm-models"
MODEL_NAME="qwen2.5-1.5b-instruct-q4_k_m"
VERIFY_ONLY=0
PRINT_SHA=0
LIST_ONLY=0

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target)       TARGET_DIR="$2"; shift 2 ;;
        --target=*)     TARGET_DIR="${1#*=}"; shift ;;
        --model)        MODEL_NAME="$2"; shift 2 ;;
        --model=*)      MODEL_NAME="${1#*=}"; shift ;;
        --verify-only)  VERIFY_ONLY=1; shift ;;
        --print-sha)    PRINT_SHA=1; shift ;;
        --list)         LIST_ONLY=1; shift ;;
        -h|--help)      usage 0 ;;
        *)              echo "fetch-slm.sh: unknown argument: $1" >&2; usage 2 ;;
    esac
done

if [[ "$LIST_ONLY" -eq 1 ]]; then
    printf '%-40s %8s   %s\n' "model" "size_mb" "url"
    for name in "${!MODEL_URL[@]}"; do
        printf '%-40s %8s   %s\n' "$name" "${MODEL_SIZE_MB[$name]}" "${MODEL_URL[$name]}"
    done
    exit 0
fi

if [[ -z "${MODEL_URL[$MODEL_NAME]:-}" ]]; then
    echo "fetch-slm.sh: unknown model '$MODEL_NAME'" >&2
    echo "Run with --list to see registered models." >&2
    exit 2
fi

# -----------------------------------------------------------------------------
# Dependency check. We use sha256sum + curl; both are universally available
# on the labctl host and any Linux dev machine. macOS dev users can install
# coreutils to get sha256sum.
# -----------------------------------------------------------------------------
for tool in curl sha256sum; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "fetch-slm.sh: required tool '$tool' is not on PATH." >&2
        exit 3
    fi
done

URL="${MODEL_URL[$MODEL_NAME]}"
EXPECTED_SHA="${MODEL_SHA256[$MODEL_NAME]}"
FILENAME="${MODEL_FILE[$MODEL_NAME]}"
DEST="${TARGET_DIR}/${FILENAME}"

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------
compute_sha() {
    sha256sum "$1" | awk '{print $1}'
}

verify_file() {
    local file="$1" expected="$2"
    if [[ "$expected" == "TBD-PIN-AFTER-FIRST-DOWNLOAD" ]]; then
        local actual; actual=$(compute_sha "$file")
        echo "fetch-slm.sh: SHA256 pin is unset for '$MODEL_NAME'." >&2
        echo "  Actual SHA256 of '$file':" >&2
        echo "    $actual" >&2
        echo "  Update MODEL_SHA256[$MODEL_NAME] in scripts/fetch-slm.sh and re-run." >&2
        return 4
    fi
    local actual; actual=$(compute_sha "$file")
    if [[ "$actual" != "$expected" ]]; then
        echo "fetch-slm.sh: SHA256 mismatch for '$file'." >&2
        echo "  expected: $expected" >&2
        echo "  actual:   $actual" >&2
        return 5
    fi
    return 0
}

mkdir -p "$TARGET_DIR"

# -----------------------------------------------------------------------------
# Print-only path
# -----------------------------------------------------------------------------
if [[ "$PRINT_SHA" -eq 1 ]]; then
    if [[ ! -f "$DEST" ]]; then
        echo "fetch-slm.sh: nothing to hash — '$DEST' does not exist." >&2
        exit 1
    fi
    compute_sha "$DEST"
    exit 0
fi

# -----------------------------------------------------------------------------
# Verify-only path: don't touch the network.
# -----------------------------------------------------------------------------
if [[ "$VERIFY_ONLY" -eq 1 ]]; then
    if [[ ! -f "$DEST" ]]; then
        echo "fetch-slm.sh: file not found at '$DEST' (use without --verify-only to download)." >&2
        exit 1
    fi
    verify_file "$DEST" "$EXPECTED_SHA"
    echo "OK: $DEST  (sha256=$EXPECTED_SHA)"
    exit 0
fi

# -----------------------------------------------------------------------------
# Idempotent download. If the file is already there and matches the pin,
# we're done. Otherwise fetch, then verify.
# -----------------------------------------------------------------------------
if [[ -f "$DEST" ]]; then
    if [[ "$EXPECTED_SHA" != "TBD-PIN-AFTER-FIRST-DOWNLOAD" ]]; then
        actual=$(compute_sha "$DEST")
        if [[ "$actual" == "$EXPECTED_SHA" ]]; then
            echo "OK: $DEST already present and verified (sha256=$EXPECTED_SHA)"
            exit 0
        fi
        echo "fetch-slm.sh: $DEST exists but SHA256 mismatched ($actual); re-downloading." >&2
        rm -f "$DEST"
    fi
fi

echo "Downloading $MODEL_NAME (~${MODEL_SIZE_MB[$MODEL_NAME]} MB) → $DEST"
echo "  URL: $URL"
curl --fail --location --show-error --output "$DEST" "$URL"
echo "Download complete: $(stat --format='%s' "$DEST") bytes"

verify_file "$DEST" "$EXPECTED_SHA"
echo "OK: $DEST  (sha256=$EXPECTED_SHA)"
