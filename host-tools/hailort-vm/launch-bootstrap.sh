#!/bin/bash
# Phase 1 launcher — boots the HailoRT VM against Task 0.2's corpus-driven
# stub, propagating the stub's stdout (HAILO_RE_CORPUS_* lines) so the
# Task 0.5 driver script can consume them.
#
# Contract (matches hailo_re_driver/qemu_runner.py default_command_factory):
#   launch-bootstrap.sh --corpus <path-to-corpus.jsonl>
#
# Outputs:
#   - stdout: anything QEMU writes to fd 1, which includes the Task 0.2
#     stub's HAILO_RE_CORPUS_EXTEND / HAILO_RE_CORPUS_DIVERGENCE lines.
#   - stderr: QEMU diagnostics + HAILO_RE_MSI_FIRE.
#   - exit code: QEMU's exit code (1 on EXTEND/DIVERGENCE, 0 on a clean
#     `Configure() + Activate()` — the Phase 1 exit gate from KICKOFF).
#
# This is the Phase 1 sibling of launch-capture.sh. That script kept the
# placeholder hailo-stub-stub flow alive for the determinism-check
# artifact; this one wires the real corpus-driven stub into the same VM.

set -euo pipefail

# ---------------------------------------------------------------------------
# Args
# ---------------------------------------------------------------------------
CORPUS=""
VARIANT="bootstrap"
RUN_TIMEOUT="${CAPTURE_TIMEOUT:-180}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --corpus)  CORPUS="$2"; shift 2;;
        --variant) VARIANT="$2"; shift 2;;
        --timeout) RUN_TIMEOUT="$2"; shift 2;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# //; s/^#//'
            exit 0
            ;;
        *) echo "Unknown argument: $1" >&2; exit 2;;
    esac
done

if [[ -z "${CORPUS}" ]]; then
    echo "Usage: $0 --corpus <path-to-corpus.jsonl> [--variant N] [--timeout S]" >&2
    exit 2
fi

# Resolve to absolute — the stub re-opens the corpus from QEMU's cwd.
if [[ ! -f "${CORPUS}" ]]; then
    echo "Corpus not found: ${CORPUS}" >&2
    exit 2
fi
CORPUS="$(readlink -f "${CORPUS}")"

# ---------------------------------------------------------------------------
# Environment
# ---------------------------------------------------------------------------
HAILORT_VERSION="${HAILORT_VERSION:-4.23.0}"
UBUNTU_RELEASE="${UBUNTU_RELEASE:-noble}"
VM_IMG_DIR="${VM_IMG_DIR:-$HOME/slmos-ref/derivatives/hailort-vm-images}"
VM_IMG_NAME="${VM_IMG_NAME:-hailort-vm-${HAILORT_VERSION}-ubuntu-${UBUNTU_RELEASE}.qcow2}"
VM_IMG="${VM_IMG_DIR}/${VM_IMG_NAME}"
QEMU_CUSTOM="${QEMU_CUSTOM:-$HOME/slmos-ref/derivatives/hailort-vm-qemu/qemu-system-x86_64}"
PC_BIOS="${PC_BIOS:-$HOME/slmos-ref/derivatives/hailort-vm-qemu/pc-bios}"

if [[ ! -x "${QEMU_CUSTOM}" ]]; then
    echo "Custom QEMU missing: ${QEMU_CUSTOM}" >&2
    echo "Run ./build-qemu.sh first." >&2
    exit 3
fi
if ! "${QEMU_CUSTOM}" -device help 2>&1 | grep -q '"hailo8"'; then
    echo "${QEMU_CUSTOM} does not register -device hailo8." >&2
    echo "Rebuild via ./build-qemu.sh --force." >&2
    exit 3
fi
if [[ ! -f "${VM_IMG}" ]]; then
    echo "VM image missing: ${VM_IMG}" >&2
    echo "Run ./build-vm-image.sh first." >&2
    exit 3
fi

# ---------------------------------------------------------------------------
# Working directory — fresh disk overlay per run.
# ---------------------------------------------------------------------------
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-${VARIANT}"
RUN_DIR="${VM_IMG_DIR}/.runs/${RUN_ID}"
mkdir -p "${RUN_DIR}"

DISK_CLONE="${RUN_DIR}/disk.qcow2"
SERIAL_LOG="${RUN_DIR}/serial.log"

qemu-img create -f qcow2 -b "${VM_IMG}" -F qcow2 "${DISK_CLONE}" >/dev/null

# ---------------------------------------------------------------------------
# Accelerator selection.
#
# KVM if available; TCG single-thread otherwise. Determinism flags match the
# capture variant (see launch-capture.sh + README §Determinism).
# ---------------------------------------------------------------------------
ACCEL_ARGS=(-accel tcg,thread=single)
if [[ -r /dev/kvm && -w /dev/kvm ]]; then
    ACCEL_ARGS=(-accel kvm -cpu host,migratable=no)
fi

# ---------------------------------------------------------------------------
# QEMU invocation.
#
# - No -nographic (that aliases to -serial mon:stdio, stealing stdout from
#   the stub's fprintf path).
# - -serial file:SERIAL_LOG keeps the guest serial off stdout.
# - -monitor none disables the HMP monitor so stdout stays clean.
# - -display none + no -vnc/-spice means no UI surfaces.
# - The Task 0.2 stub's HAILO_RE_CORPUS_* fprintf goes straight to the QEMU
#   process's stdout, which this script does not redirect — so it appears
#   on this script's stdout in turn, where the driver script captures it.
# ---------------------------------------------------------------------------
set +e
timeout "${RUN_TIMEOUT}" "${QEMU_CUSTOM}" \
    -L "${PC_BIOS}" \
    "${ACCEL_ARGS[@]}" \
    -smp 1 \
    -m 1G \
    -rtc base=2026-05-12T00:00:00,clock=vm \
    -drive "file=${DISK_CLONE},format=qcow2,if=virtio" \
    -nic user,model=virtio-net-pci \
    -device "hailo8,corpus=${CORPUS}" \
    -display none \
    -serial "file:${SERIAL_LOG}" \
    -monitor none \
    -no-reboot
QEMU_STATUS=$?
set -e

# 124 == GNU `timeout` SIGTERM; QEMU itself returns 0 on a clean -no-reboot
# halt and 1 on the stub's exit(1). Anything else is unexpected — propagate
# the status so the driver script can react.
exit "${QEMU_STATUS}"
