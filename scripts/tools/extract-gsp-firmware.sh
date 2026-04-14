#!/usr/bin/env bash
# extract-gsp-firmware.sh — locate and decompress NVIDIA GSP firmware at
# build time, without committing proprietary binaries to the repo.
#
# Usage:
#   scripts/tools/extract-gsp-firmware.sh <out-dir> <chip>
# Example:
#   scripts/tools/extract-gsp-firmware.sh build/kernel/gsp_fw ga107
#
# Inputs (found automatically):
#   /lib/firmware/nvidia/<chip>/gsp/gsp-535.113.01.bin.zst
#   /lib/firmware/nvidia/<chip>/gsp/bootloader-535.113.01.bin.zst
#   /lib/firmware/nvidia/<chip>/gsp/booter_load-535.113.01.bin.zst
#   /lib/firmware/nvidia/<chip>/gsp/booter_unload-535.113.01.bin.zst
#
# Outputs (decompressed, ready to .incbin):
#   <out-dir>/gsp.bin
#   <out-dir>/bootloader.bin
#   <out-dir>/booter_load.bin
#   <out-dir>/booter_unload.bin
#
# Exit codes:
#   0   Success, all four files extracted
#   1   Missing tools (zstd) or firmware directory
#   2   One or more firmware files not found
#
# On Jetson, firmware lives under /lib/firmware/nvidia/ga10b/gsp/ and can
# be extracted the same way — just pass `ga10b` as the chip argument.
# The file format is identical across all Ampere GA10x variants.

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 <out-dir> <chip>" >&2
    exit 1
fi

OUT_DIR="$1"
CHIP="$2"
FW_DIR="/lib/firmware/nvidia/${CHIP}/gsp"
# Driver version to extract. Pinned to 535.113.01 — the version nouveau
# targets and the one our docs/nvidia-gsp.md boot sequence is traced
# against. Newer driver packages ship this file alongside their own.
FW_VERSION="535.113.01"

if ! command -v zstd >/dev/null 2>&1; then
    echo "error: zstd not found in PATH" >&2
    exit 1
fi

if [[ ! -d "$FW_DIR" ]]; then
    cat >&2 <<EOF
error: firmware directory not found: $FW_DIR

On Ubuntu/Debian, install the NVIDIA firmware package:
    sudo apt install linux-firmware
    (or: nvidia-driver-535 / libnvidia-compute-535)

On Jetson, firmware ships in the JetPack BSP at
/lib/firmware/nvidia/ga10b/gsp/ — pass 'ga10b' as the chip arg.

For CI without hardware, set ENABLE_GSP_FIRMWARE=OFF in CMake.
EOF
    exit 1
fi

mkdir -p "$OUT_DIR"

declare -a PAIRS=(
    "gsp-${FW_VERSION}.bin.zst:gsp.bin"
    "bootloader-${FW_VERSION}.bin.zst:bootloader.bin"
    "booter_load-${FW_VERSION}.bin.zst:booter_load.bin"
    "booter_unload-${FW_VERSION}.bin.zst:booter_unload.bin"
)

missing=()
for pair in "${PAIRS[@]}"; do
    src="${pair%%:*}"
    dst="${pair#*:}"
    if [[ ! -f "$FW_DIR/$src" ]]; then
        missing+=("$FW_DIR/$src")
    fi
done
if (( ${#missing[@]} > 0 )); then
    echo "error: missing firmware file(s):" >&2
    for m in "${missing[@]}"; do echo "  - $m" >&2; done
    exit 2
fi

for pair in "${PAIRS[@]}"; do
    src="${pair%%:*}"
    dst="${pair#*:}"
    zstd -d -q -f -c "$FW_DIR/$src" > "$OUT_DIR/$dst"
done

# Pretty print what we got, for build-log observability.
echo "GSP firmware extracted from $FW_DIR → $OUT_DIR"
(cd "$OUT_DIR" && ls -la gsp.bin bootloader.bin booter_load.bin booter_unload.bin | awk '{printf "  %-20s %10s bytes\n", $9, $5}')
