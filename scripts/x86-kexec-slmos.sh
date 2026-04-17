#!/usr/bin/env bash
# x86-kexec-slmos.sh — run ON test-pc (Linux side).
#
# Loads SLM-OS as a kexec target and triggers the jump while keeping the
# GPU powered and SEC2 in its nouveau-unlocked state.
#
# Prerequisites (checked below):
#   - kexec-tools 2.0.28+ (multiboot2-x86 loader)
#   - nouveau driver loaded on the GPU
#   - runtime PM disabled on the GPU (prevents autosuspend across kexec)
#   - SEC2 CPUCTL != 0xbadf5620 (lock cleared — the whole point)
#
# Exit codes:
#   0  kexec fired (no return — machine is now running SLM-OS)
#   1  precondition check failed (safe to retry after fixing)
#   2  kexec --load or --exec failed
set -euo pipefail

SLMOS_ELF=${SLMOS_ELF:-/root/slmos.elf}
GPU_PCI=${GPU_PCI:-0000:01:00.0}
FORCE=${FORCE:-0}

log() { printf '[kexec-slmos] %s\n' "$*"; }
fail() { log "ERROR: $*"; exit "${2:-1}"; }

# 1. kexec-tools installed?
command -v kexec >/dev/null || fail "kexec not on PATH — apt install kexec-tools"

# 2. ELF present and readable?
[[ -r "$SLMOS_ELF" ]] || fail "$SLMOS_ELF not found or not readable"

# 3. multiboot2-x86 loader supported?
if ! kexec --help 2>&1 | grep -q multiboot2-x86; then
    fail "kexec-tools does not support multiboot2-x86"
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

# 7. kexec --load the SLM-OS ELF as a multiboot2 binary.
#
# Use -c to force the older kexec_load syscall. The default (kexec_file_load,
# Linux 3.17+) validates segments against a memory_ranges list that, in
# kexec-tools 2.0.28 on Ubuntu 24.04, comes back empty for the multiboot2
# loader and rejects every load address with "Invalid memory segment".
# -c uses the classic kexec_load which skips that validator path. Verified
# 2026-04-17 that -c returns rc=0 and /sys/kernel/kexec_loaded = 1 where
# the default returns "Invalid memory segment 0x20000000 - 0x22c01fff".
#
# No initrd, no cmdline tags — SLM-OS's multiboot2 entry doesn't consume
# them today; only the info pointer is used. If a cmdline becomes needed
# later, add "--command-line=..." to this invocation.
log "kexec -c --load --type=multiboot2-x86 $SLMOS_ELF"
kexec -c --load --type=multiboot2-x86 "$SLMOS_ELF" \
    || fail "kexec --load failed" 2

# 8. Fire. If the syscall succeeds the machine is now SLM-OS —
# control never reaches the next line. If it returns, kexec itself
# failed (e.g., loader bug, panic path) and we surface that.
log "kexec --exec (no return expected)"
sync
kexec --exec
fail "kexec --exec returned — machine still on Linux" 2
