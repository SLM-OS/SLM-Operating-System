#!/usr/bin/env bash
# fetch-ga10b-firmware.sh — pull the 14-file GA10B nvgpu firmware set from
# a Jetson into a build-host directory so CMake can .incbin them.
#
# GA10B (integrated Ampere, Jetson Orin Nano) does NOT ship the discrete-
# Ampere GSP-RM stack (gsp-535.113.01.bin et al.). It ships nvgpu-native
# firmware at /lib/firmware/nvidia/ga10b/ on an L4T BSP. This script
# pulls that directory to the build host, preserving names.
#
# Usage:
#   scripts/tools/fetch-ga10b-firmware.sh [--host <addr>] [--dest <dir>]
#
# Defaults:
#   --host 192.168.4.93    (jetson-nano-2's lab IP)
#   --dest ~/jetson-ga10b-firmware
#
# After running, configure CMake with:
#   -DGA10B_FIRMWARE_DIR=$HOME/jetson-ga10b-firmware
#
# The firmware is NVIDIA-proprietary; do NOT commit it to the repo.

set -euo pipefail

HOST="192.168.4.93"
DEST="${HOME}/jetson-ga10b-firmware"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --dest) DEST="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "$DEST"

REQUIRED_FILES=(
    "acr-gsp.text.encrypt.bin.prod"
    "acr-gsp.data.encrypt.bin.prod"
    "acr-gsp.manifest.encrypt.bin.out.bin.prod"
    "fecs_encrypt_prod.bin"
    "fecs_pkc_sig_encrypt.bin"
    "gpccs_encrypt_prod.bin"
    "gpccs_pkc_sig_encrypt.bin"
    "gpmu_ucode_next_prod_image.bin"
    "gpmu_ucode_next_prod_desc.bin"
    "pmu_pkc_prod_sig.bin"
    "NETA_img_prod_encrypted.bin"
    "NETB_img_prod_encrypted.bin"
    "NETC_img_prod_encrypted.bin"
    "NETD_img_prod_encrypted.bin"
    "safety-scheduler.text.encrypt.bin.prod"
    "safety-scheduler.data.encrypt.bin.prod"
    "safety-scheduler.manifest.encrypt.bin.out.bin.prod"
)

echo "Fetching GA10B firmware from $HOST to $DEST..."
scp -o StrictHostKeyChecking=no -o BatchMode=yes \
    "root@${HOST}:/lib/firmware/nvidia/ga10b/*" "$DEST/"

missing=()
for f in "${REQUIRED_FILES[@]}"; do
    if [[ ! -f "$DEST/$f" ]]; then
        missing+=("$f")
    fi
done

if (( ${#missing[@]} > 0 )); then
    echo "error: required files not fetched:" >&2
    for m in "${missing[@]}"; do echo "  - $m" >&2; done
    exit 2
fi

echo "GA10B firmware extracted → $DEST"
(cd "$DEST" && du -b "${REQUIRED_FILES[@]}" | awk '{printf "  %-50s %s bytes\n", $2, $1}')
echo ""
echo "Configure the build with:"
echo "  cmake -B build ... -DGA10B_FIRMWARE_DIR=$DEST"
