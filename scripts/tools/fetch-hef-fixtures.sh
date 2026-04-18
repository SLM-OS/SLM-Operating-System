#!/usr/bin/env bash
# fetch-hef-fixtures.sh — download Hailo Model Zoo HEF files for tests
#
# The .hef files are 5-20 MB compiled binaries; committing them would
# bloat git clones permanently, so tests that need real-data
# validation pull them on demand via this script. A .gitignore in
# kernel/tests/fixtures/ excludes *.hef, so a fetch + test + clean
# workflow keeps the repo clean.
#
# Usage: ./scripts/tools/fetch-hef-fixtures.sh
#
# Defaults to yolov5s.hef (9 MB). Source: Hailo Model Zoo v2.18.0
# compiled bucket (public S3, no authentication required). License:
# MIT per https://github.com/hailo-ai/hailo_model_zoo/blob/master/LICENSE.

set -euo pipefail

cd "$(dirname "$0")/../.."
FIXTURE_DIR="kernel/tests/fixtures"
MZOO_BASE="https://hailo-model-zoo.s3.eu-west-2.amazonaws.com/ModelZoo/Compiled/v2.18.0/hailo8"

mkdir -p "$FIXTURE_DIR"

# Default model list. Extend as new phases need more fixtures.
MODELS=(
    "yolov5s"
)

for model in "${MODELS[@]}"; do
    out="$FIXTURE_DIR/${model}.hef"
    if [[ -f "$out" ]]; then
        echo "skip (exists): $out"
        continue
    fi
    url="${MZOO_BASE}/${model}.hef"
    echo "fetching $url -> $out"
    curl -sL -o "$out" "$url"
    # Verify magic: first 4 bytes of every HEF are 0x01 'H' 'E' 'F'
    magic=$(head -c 4 "$out" | xxd -p)
    if [[ "$magic" != "01484546" ]]; then
        echo "error: $out does not start with HEF magic (got $magic)" >&2
        rm -f "$out"
        exit 1
    fi
    size=$(stat -c %s "$out")
    echo "  ok: $(( size / 1024 )) KB"
done
