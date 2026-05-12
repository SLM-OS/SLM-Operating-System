#!/bin/bash
# Run one HailoRT capture session.
#
# Usage: ./launch-capture.sh <corpus-output.jsonl> [--variant <name>]
#
# Boots a clone of the customized qcow2 (so each run starts from the same cold
# state), runs `hailortcli fw-control identify` inside the guest pinned to CPU
# 0, captures every MMIO trap via QEMU's memory_region_ops_{read,write} trace
# events, and converts the trace into the corpus JSONL format defined in
# docs/hailo-re-corpus-format.md.
#
# The disk is always a fresh clone — capture runs are non-destructive.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <corpus-output.jsonl> [--variant <name>]" >&2
    exit 2
fi

CORPUS_OUT="$1"; shift
VARIANT="default"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --variant) VARIANT="$2"; shift 2;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done

# ---------------------------------------------------------------------------
HAILORT_VERSION="${HAILORT_VERSION:-4.23.0}"
UBUNTU_RELEASE="${UBUNTU_RELEASE:-noble}"
VM_IMG_DIR="${VM_IMG_DIR:-$HOME/slmos-ref/derivatives/hailort-vm-images}"
VM_IMG_NAME="${VM_IMG_NAME:-hailort-vm-${HAILORT_VERSION}-ubuntu-${UBUNTU_RELEASE}.qcow2}"
VM_IMG="${VM_IMG_DIR}/${VM_IMG_NAME}"
QEMU_CUSTOM="${QEMU_CUSTOM:-$HOME/slmos-ref/derivatives/hailort-vm-qemu/qemu-system-x86_64}"
CAPTURE_TIMEOUT="${CAPTURE_TIMEOUT:-180}"

HERE="$(cd "$(dirname "$0")" && pwd)"
TRACE_PARSER="${HERE}/tools/qemu-trace-to-corpus.py"

# Prefer the custom QEMU with the hailo-stub-stub device; fall back to system
# QEMU only with -device edu (placeholder of a placeholder).
QEMU_DATA_ARGS=()
if [[ -x "${QEMU_CUSTOM}" ]] && "${QEMU_CUSTOM}" -device help 2>&1 | grep -q hailo-stub-stub; then
    QEMU_BIN="${QEMU_CUSTOM}"
    STUB_DEVICE="hailo-stub-stub"
    # The custom QEMU is invoked from $QEMU_OUT_DIR; point -L at the
    # pc-bios sibling so BIOS / vgabios / option ROMs are findable.
    QEMU_DATA_ARGS=(-L "$(dirname "${QEMU_CUSTOM}")/pc-bios")
else
    QEMU_BIN="qemu-system-x86_64"
    STUB_DEVICE="edu"
    echo "WARNING: custom QEMU not found; falling back to system QEMU + edu device." >&2
    echo "         hailo_pci will fail to probe (no BAR2/BAR4). Build custom QEMU with" >&2
    echo "         ./build-qemu.sh to fix this." >&2
fi

if [[ ! -f "${VM_IMG}" ]]; then
    echo "VM image missing: ${VM_IMG}" >&2
    echo "Run ./build-vm-image.sh first." >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# Working directory per run — fresh disk clone, fresh trace log.
# ---------------------------------------------------------------------------
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-${VARIANT}"
RUN_DIR="${VM_IMG_DIR}/.runs/${RUN_ID}"
mkdir -p "${RUN_DIR}"

DISK_CLONE="${RUN_DIR}/disk.qcow2"
SERIAL_LOG="${RUN_DIR}/serial.log"
TRACE_LOG="${RUN_DIR}/qemu-trace.log"

# Clone disk as overlay so the base image is never mutated.
qemu-img create -f qcow2 -b "${VM_IMG}" -F qcow2 "${DISK_CLONE}" >/dev/null

# ---------------------------------------------------------------------------
# QEMU invocation.
#
# Key flags for determinism + capture:
#   -cpu host,migratable=no    pin to host CPU features
#   -smp 1                     single vCPU; HailoRT runs under taskset -c 0 anyway
#   -rtc base=2026-05-12T...   fixed wall-clock so timer code paths repeat
#   -device edu                placeholder PCI device (BAR0 returns 0 on
#                              unwritten offsets, matches stub-stub spec)
#   -trace memory_region_ops_* capture every MMIO trap
#   -no-reboot                 if the guest reboots, exit (we always shutdown -h)
# ---------------------------------------------------------------------------
ACCEL_ARGS=(-accel tcg,thread=single)
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    ACCEL_ARGS=(-accel kvm -cpu host,migratable=no)
fi

# QEMU's -trace flag glob captures both _read and _write events in one filter.
# Caveat: this is event-name filtering, not per-region — the trace log will
# include rtc/apic/port92/serial MMIO from boot too (~2.6 MB/sec). The Python
# parser drops anything not in BAR_NAME_MAP, so the corpus stays clean.
# Use an array so the literal '*' is passed to QEMU without bash glob expansion.
TRACE_ARGS=(-trace "memory_region_ops_*,file=${TRACE_LOG}")

set +e
timeout "${CAPTURE_TIMEOUT}" "${QEMU_BIN}" \
    "${QEMU_DATA_ARGS[@]}" \
    "${ACCEL_ARGS[@]}" \
    -smp 1 \
    -m 1G \
    -rtc base=2026-05-12T00:00:00,clock=vm \
    -drive "file=${DISK_CLONE},format=qcow2,if=virtio" \
    -nic user,model=virtio-net-pci \
    -device "${STUB_DEVICE}" \
    -nographic \
    -serial "file:${SERIAL_LOG}" \
    -monitor none \
    -no-reboot \
    "${TRACE_ARGS[@]}"
QEMU_STATUS=$?
set -e

if [[ ${QEMU_STATUS} -ne 0 && ${QEMU_STATUS} -ne 124 ]]; then
    echo "QEMU exit ${QEMU_STATUS}. Tail of serial log:" >&2
    tail -50 "${SERIAL_LOG}" >&2 || true
    exit 5
fi

# ---------------------------------------------------------------------------
# Verify capture markers landed.
# ---------------------------------------------------------------------------
if ! grep -q HAILORT_CAPTURE_BEGIN "${SERIAL_LOG}"; then
    echo "Capture begin marker missing in serial log:" >&2
    tail -80 "${SERIAL_LOG}" >&2
    exit 6
fi
if ! grep -q HAILORT_CAPTURE_DONE "${SERIAL_LOG}"; then
    echo "Capture done marker missing — capture may have hung. Serial tail:" >&2
    tail -80 "${SERIAL_LOG}" >&2
    exit 6
fi

# ---------------------------------------------------------------------------
# Convert trace -> corpus.
# ---------------------------------------------------------------------------
mkdir -p "$(dirname "${CORPUS_OUT}")"
python3 "${TRACE_PARSER}" \
    --trace "${TRACE_LOG}" \
    --serial "${SERIAL_LOG}" \
    --hailort-version "${HAILORT_VERSION}" \
    --capture-host "qemu-x86_64-ubuntu-${UBUNTU_RELEASE}-${STUB_DEVICE}" \
    --slmos-base-sha "$(git -C "${HERE}" rev-parse HEAD 2>/dev/null || echo unknown)" \
    --output "${CORPUS_OUT}"

echo "Capture complete: ${CORPUS_OUT}"
echo "Serial log:       ${SERIAL_LOG}"
echo "Trace log:        ${TRACE_LOG}"
