#!/usr/bin/env bash
# regen-hef-proto.sh — regenerate hef.pb.{c,h} from hef.proto
#
# Called when kernel/ai_accel/hailo/hef.proto changes. Sets up a
# throwaway venv with nanopb (the stock Ubuntu 24.04 Python is
# PEP 668-protected, so a user-wide `pip install` is blocked). The
# venv lives under build/ and is gitignored.
#
# Prerequisite: `protoc` on PATH. Install with:
#     sudo apt-get install protobuf-compiler
# (nanopb_generator shells out to protoc to parse the .proto).
#
# Usage: ./scripts/tools/regen-hef-proto.sh
#
# Commits the regenerated hef.pb.c and hef.pb.h — those files are
# the source of truth for the kernel build, not the .proto itself.
# Rerun this script only when the schema changes.

set -euo pipefail

cd "$(dirname "$0")/../.."
REPO_ROOT="$(pwd)"

PROTO="kernel/ai_accel/hailo/hef.proto"
OPTIONS="kernel/ai_accel/hailo/hef.options"
OUT_DIR="kernel/ai_accel/hailo"
VENV="build/nanopb-venv"

if [[ ! -f "$PROTO" ]]; then
    echo "error: $PROTO not found (run from repo root?)" >&2
    exit 1
fi

if ! command -v protoc >/dev/null 2>&1; then
    echo "error: protoc not on PATH. Install with:" >&2
    echo "    sudo apt-get install protobuf-compiler" >&2
    exit 1
fi

if [[ ! -d "$VENV" ]]; then
    echo "Creating nanopb venv at $VENV..."
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet --upgrade pip
    "$VENV/bin/pip" install --quiet nanopb
fi

echo "Generating hef.pb.c / hef.pb.h from $PROTO..."
# nanopb_generator uses the input path verbatim in its output name.
# Run it from the proto's directory so the output names are just
# `hef.pb.{c,h}` — otherwise we'd end up with
# `kernel/ai_accel/hailo/kernel/ai_accel/hailo/hef.pb.h`.
VENV_ABS="$REPO_ROOT/$VENV"
(cd "$OUT_DIR" && "$VENV_ABS/bin/nanopb_generator" \
    --options-file "hef.options" \
    "hef.proto")

if [[ ! -f "$OUT_DIR/hef.pb.c" || ! -f "$OUT_DIR/hef.pb.h" ]]; then
    echo "error: nanopb generator did not produce expected outputs" >&2
    exit 1
fi

echo "Done. Review + commit:"
ls -la "$OUT_DIR"/hef.pb.{c,h}
