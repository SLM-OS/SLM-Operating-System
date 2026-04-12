#!/bin/bash
#
# jetson-kexec-slmos.sh - Cleanly boot SLM-OS via kexec on Jetson Orin Nano
#
# Stops the GPU cleanly before kexec to prevent stale nvgpu DMA from
# triggering a TF-A RAS error that powers off the CPU core.
#
# See GitHub issue #9 for full background.
#
# Usage:
#   sudo ./jetson-kexec-slmos.sh /path/to/slmos.elf
#
# Or install to Jetson and run:
#   sudo slmos-kexec /root/slmos.elf
#
set -euo pipefail

KERNEL="${1:-/root/slmos.elf}"

if [[ ! -f "$KERNEL" ]]; then
    echo "Error: kernel file not found: $KERNEL" >&2
    echo "Usage: $0 <path/to/slmos.elf>" >&2
    exit 1
fi

if [[ $EUID -ne 0 ]]; then
    echo "Error: must run as root (need access to /sys and kexec)" >&2
    exit 1
fi

GPU_POWER=/sys/devices/platform/bus@0/17000000.gpu/power

if [[ ! -d "$GPU_POWER" ]]; then
    echo "Warning: GPU power path not found — not a Jetson Orin, or driver not loaded" >&2
    echo "Proceeding with kexec anyway..." >&2
else
    echo "[1/4] Stopping GPU consumers..."
    # Display manager holds GPU via DRM
    systemctl stop gdm 2>/dev/null || true
    # NVIDIA camera/multimedia services
    systemctl stop nvargus-daemon 2>/dev/null || true
    systemctl stop nvs-service 2>/dev/null || true
    sleep 2

    # Kill any remaining GPU file descriptor holders
    fuser -k /dev/nvhost-gpu /dev/nvmap 2>/dev/null || true
    sleep 1

    echo "[2/4] Requesting GPU runtime PM suspend..."
    # Set autosuspend delay to 0 and enable auto mode — triggers immediate
    # suspend when refcount reaches 0. This power-gates the GPU via BPMP,
    # stopping the PMU firmware and flushing any stale DMA operations.
    echo 0 > "$GPU_POWER/autosuspend_delay_ms"
    echo auto > "$GPU_POWER/control"
    sleep 3

    # Verify GPU actually suspended
    status="$(cat "$GPU_POWER/runtime_status" 2>/dev/null || echo unknown)"
    pg_state="$(cat /sys/kernel/debug/bpmp/debug/powergate/gpu/state 2>/dev/null || echo unknown)"
    echo "       GPU runtime: $status, powergate: $pg_state"
    if [[ "$status" != "suspended" ]]; then
        echo "Warning: GPU did not suspend cleanly (status=$status)" >&2
        echo "         kexec may still crash with a TF-A RAS error" >&2
    fi
fi

echo "[3/4] Loading kernel: $KERNEL"
kexec -l "$KERNEL" --reuse-cmdline

echo "[4/4] Executing kexec (serial console will take over)"
exec kexec -e
