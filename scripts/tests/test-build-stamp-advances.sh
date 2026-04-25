#!/usr/bin/env bash
# test-build-stamp-advances.sh — verify SLMOS_BUILD_STAMP advances every build.
#
# Builds the kernel twice with a 1-second sleep between, then compares the
# SLMOS_BUILD_STAMP value extracted from kernel/include/build_info.h. Issue #360.
#
# Runs against the QEMU_VIRT ARM64 default; the change is platform-independent
# so this is sufficient to exercise the CMake glue.

set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
HEADER="${REPO_ROOT}/kernel/include/build_info.h"

extract_stamp() {
    if [[ ! -f "${HEADER}" ]]; then
        echo "FAIL: ${HEADER} was not produced by the build" >&2
        exit 1
    fi
    local stamp
    stamp=$(grep -E '^#define[[:space:]]+SLMOS_BUILD_STAMP' "${HEADER}" \
            | sed -E 's/.*"([^"]+)".*/\1/')
    if [[ -z "${stamp}" ]]; then
        echo "FAIL: could not parse SLMOS_BUILD_STAMP from ${HEADER}" >&2
        exit 1
    fi
    if [[ ! "${stamp}" =~ ^[0-9]{14}$ ]]; then
        echo "FAIL: stamp '${stamp}' is not 14 digits (YYYYMMDDhhmmss UTC)" >&2
        exit 1
    fi
    printf '%s' "${stamp}"
}

cd "${REPO_ROOT}"

echo "[1/2] Building kernel..."
make kernel >/dev/null
STAMP1=$(extract_stamp)
echo "  stamp1: ${STAMP1}"

echo "Sleeping 1 second so the UTC stamp can advance..."
sleep 1

echo "[2/2] Building kernel again..."
make kernel >/dev/null
STAMP2=$(extract_stamp)
echo "  stamp2: ${STAMP2}"

if [[ "${STAMP1}" == "${STAMP2}" ]]; then
    echo "FAIL: SLMOS_BUILD_STAMP did not advance (${STAMP1} == ${STAMP2})" >&2
    exit 1
fi

if [[ ! "${STAMP2}" > "${STAMP1}" ]]; then
    echo "FAIL: stamp2 (${STAMP2}) is not strictly greater than stamp1 (${STAMP1})" >&2
    exit 1
fi

echo "PASS: SLMOS_BUILD_STAMP advanced ${STAMP1} -> ${STAMP2}"
