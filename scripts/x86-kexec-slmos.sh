#!/usr/bin/env bash
# x86-kexec-slmos.sh — run ON test-pc (Linux side).
#
# Loads SLM-OS as a kexec target and triggers the jump while keeping the
# GPU powered and SEC2 in its nouveau-unlocked state.
#
# Two loader paths, selected via env var KEXEC_MODE:
#   mb2       — multiboot2-x86 (default). Reads $SLMOS_ELF
#               (build/kernel-kexec/slmos.elf). Uses the old
#               kexec_load syscall (-c) to bypass the "Invalid memory
#               segment" validator.
#   bzimage   — Linux bzImage wrapper. Reads $SLMOS_BZIMAGE
#               (build/kernel-bzimage/slmos.bzimage). kexec-tools'
#               most thoroughly-tested x86 loader path.
#
# Both paths require --console-serial (added unconditionally below) —
# without it, kexec's purgatory leaves the UART in a state where our
# post-handoff kernel can't emit anything. See
# docs/x86-64-gpu-inference-status.md §4.2.k for the full investigation.
#
# Prerequisites (checked below):
#   - kexec-tools 2.0.28+ (has both multiboot2-x86 and bzImage loaders)
#   - nouveau driver loaded on the GPU
#   - runtime PM disabled on the GPU (prevents autosuspend across kexec)
#   - SEC2 CPUCTL != 0xbadf5620 (lock cleared — the whole point)
#
# Exit codes:
#   0  kexec fired (no return — machine is now running SLM-OS)
#   1  precondition check failed (safe to retry after fixing)
#   2  kexec --load or --exec failed
set -euo pipefail

KEXEC_MODE=${KEXEC_MODE:-mb2}              # mb2 | bzimage
SLMOS_ELF=${SLMOS_ELF:-/root/slmos.elf}
SLMOS_BZIMAGE=${SLMOS_BZIMAGE:-/root/slmos.bzimage}
GPU_PCI=${GPU_PCI:-0000:01:00.0}
FORCE=${FORCE:-0}

# Resolve mode → kexec image path + --type argument.
case "$KEXEC_MODE" in
    mb2)
        KEXEC_IMAGE="$SLMOS_ELF"
        KEXEC_TYPE="multiboot2-x86"
        ;;
    bzimage)
        KEXEC_IMAGE="$SLMOS_BZIMAGE"
        KEXEC_TYPE="bzImage"
        ;;
    *)
        echo "[kexec-slmos] ERROR: unknown KEXEC_MODE='$KEXEC_MODE' (expected mb2 or bzimage)" >&2
        exit 1
        ;;
esac

log() { printf '[kexec-slmos] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit "${2:-1}"; }

log "mode=$KEXEC_MODE  image=$KEXEC_IMAGE  kexec type=$KEXEC_TYPE"

# 1. kexec-tools installed?
command -v kexec >/dev/null || fail "kexec not on PATH — apt install kexec-tools"

# 2. Image present and readable?
[[ -r "$KEXEC_IMAGE" ]] || fail "$KEXEC_IMAGE not found or not readable"

# 3. Loader supported?
if ! kexec --help 2>&1 | grep -q -- "$KEXEC_TYPE"; then
    fail "kexec-tools does not support $KEXEC_TYPE"
fi

# 4. nouveau loaded (unless forced)?
# Note: avoid `lsmod | grep -q` under `set -o pipefail` — grep -q closes
# stdin on first match, lsmod takes SIGPIPE, pipefail propagates that
# non-zero exit even when the match succeeded. Read /proc/modules
# directly instead.
if [[ "$FORCE" != "1" ]]; then
    if ! grep -q '^nouveau ' /proc/modules; then
        fail "nouveau not loaded — modprobe nouveau modeset=1 first (or FORCE=1 to skip)"
    fi
fi

# 5. Disable runtime PM on the GPU so the kexec transition doesn't
# autosuspend us and re-power-gate SEC2. Equivalent to Jetson's
# --no-gpu-suspend finding.
GPU_POWER_CTRL="/sys/bus/pci/devices/$GPU_PCI/power/control"
if [[ -w "$GPU_POWER_CTRL" ]]; then
    CUR=$(cat "$GPU_POWER_CTRL")
    if [[ "$CUR" != "on" ]]; then
        log "GPU runtime PM was '$CUR', forcing 'on'"
        echo on > "$GPU_POWER_CTRL"
    fi
fi

# 6. Snapshot SEC2 CPUCTL — confirms pre-kexec state for correlation.
if [[ -x /root/sec2_peek ]]; then
    # BAR0 phys/size from /sys
    read -r BAR0_START BAR0_END _ < "/sys/bus/pci/devices/$GPU_PCI/resource"
    BAR0_PHYS=$(printf '%x' $((BAR0_START)))
    BAR0_SIZE=$(printf '%x' $((BAR0_END - BAR0_START + 1)))

    # Unbind from its current driver so /dev/mem mmap doesn't race.
    # Nouveau will not be rebound — we're about to kexec away.
    # If unbind fails (e.g., driver missing), keep going; the peek
    # may still work, and kexec doesn't need the unbind.
    if [[ -L "/sys/bus/pci/devices/$GPU_PCI/driver" ]]; then
        DRV=$(basename "$(readlink "/sys/bus/pci/devices/$GPU_PCI/driver")")
        log "GPU bound to '$DRV' — probing SEC2 state without unbinding..."
    fi
    # Peek WITHOUT unbinding — safe for a read (nouveau holds the mapping
    # but /dev/mem overlay is shared). If it fails we just skip the snapshot.
    if CPUCTL=$(/root/sec2_peek "$BAR0_PHYS" "$BAR0_SIZE" 840100 2>/dev/null \
                | awk '{print $2}'); then
        log "SEC2 CPUCTL pre-kexec = 0x$CPUCTL"
        if [[ "$FORCE" != "1" && "$CPUCTL" == "badf5620" ]]; then
            fail "SEC2 is priv-locked — nouveau has not (yet) unlocked it. Wait or FORCE=1."
        fi
    else
        log "sec2_peek failed (likely BAR held by nouveau); continuing"
    fi
fi

# 7. kexec --load the SLM-OS image.
#
# --console-serial + --serial=0x3F8 --serial-baud=115200 : THIS IS
# THE CRITICAL FLAG. Without it, kexec-tools' purgatory leaves the
# UART in a state (or takes a code path) where our post-handoff
# kernel can't emit anything — the entire 2026-04-17 "silent
# handoff" investigation was ultimately resolved by adding this
# flag. With it: kexec purgatory prints "I'm in purgatory" over
# COM1, then our kernel's UART reinit + "KEX\r\n" / "BZ\r\n"
# diagnostic immediately follows. See x86-64-gpu-inference-status
# §4.2.k for the full investigation.
#
# Different syscall paths per loader:
#   mb2     — must use -c (old kexec_load). Default kexec_file_load
#             rejects every address with "Invalid memory segment".
#   bzimage — with --console-serial the old syscall is used anyway
#             (kexec-tools disables kexec_file_load when purgatory
#             needs customisation). Leaving -c unset is fine.
#
# No initrd, no cmdline — SLM-OS's entry doesn't consume them.
case "$KEXEC_MODE" in
    mb2)     LOAD_ARGS=(-c) ;;
    bzimage) LOAD_ARGS=() ;;
esac
SERIAL_ARGS=(--console-serial --serial=0x3f8 --serial-baud=115200)
log "kexec ${LOAD_ARGS[*]:-} ${SERIAL_ARGS[*]} --load --type=$KEXEC_TYPE $KEXEC_IMAGE"
kexec "${LOAD_ARGS[@]}" "${SERIAL_ARGS[@]}" \
    --load --type="$KEXEC_TYPE" "$KEXEC_IMAGE" \
    || fail "kexec --load failed" 2

# 8. Fire. If the syscall succeeds the machine is now SLM-OS —
# control never reaches the next line. If it returns, kexec itself
# failed (e.g., loader bug, panic path) and we surface that.
log "kexec --exec (no return expected)"
sync
kexec --exec
fail "kexec --exec returned — machine still on Linux" 2
