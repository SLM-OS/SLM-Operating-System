#!/bin/bash
#
# jetson-kexec-slmos.sh - Cleanly boot SLM-OS via kexec on Jetson Orin Nano
#
# Two-stage GPU handoff:
#   1. Runtime PM suspend nvgpu → drains stale DMA operations, preventing
#      the TF-A RAS error that otherwise powers off the CPU core (#9).
#   2. After DMA is drained, force the GPU clocks and powergate domain
#      back ON via BPMP debugfs. SLM-OS then sees the GPU with its
#      MMIO responsive (NV_PMC_BOOT_0 = 0xB7B000A1 on GA10B) instead
#      of 0xFFFFFFFF from a gated-off engine.
#
# The force-on step is critical: SLM-OS's own BPMP clock-enable MRQs
# are rejected by the BPMP firmware on the L4T BSP (#190), so if the
# GPU is left gated we never recover. Writing to /sys/kernel/debug/bpmp/
# from Linux userspace goes through the kernel's BPMP driver, which
# BPMP firmware DOES accept.
#
# USB handoff (#266 Phase 3A):
#   Same pattern as the GPU. Phase 0 confirmed XHCI at 0x03610000 and
#   XUDC at 0x03550000 are not CBB-firewalled — they just read
#   0xFFFFFFFF post-kexec because the runtime-PM sweep tears down the
#   xusb_* clocks. Holding those clocks + the xusba / xusbc powergates
#   on before kexec keeps the controller live for SLM-OS's XHCI driver.
#
# Usage:
#   sudo ./jetson-kexec-slmos.sh /path/to/slmos.elf
#   sudo ./jetson-kexec-slmos.sh --no-gpu-suspend /path/to/slmos.elf
#   sudo ./jetson-kexec-slmos.sh --no-usb-hold   /path/to/slmos.elf
#
# Or install to Jetson and run:
#   sudo slmos-kexec /root/slmos.elf
#
# Options:
#   --no-gpu-suspend    Skip the GPU runtime-PM suspend and BPMP
#                       clock re-enable steps. Preserves the GPU's
#                       ACR/Falcon security state through the kexec
#                       transition (HWCFG2 bit 13 stays clear).
#                       Required for Path 3 (#190) — SLM-OS inherits
#                       Linux's already-running FECS/GPCCS/PMU state.
#                       Risk: stale nvgpu DMA may trigger a TF-A RAS
#                       error. In practice this hasn't fired in testing
#                       when GPU consumers are stopped before kexec.
#
#   --no-usb-hold       Skip the xusb clock + powergate holds. Only
#                       useful for SLM-OS builds that don't drive the
#                       XHCI controller; the held clocks are otherwise
#                       harmless (they just keep the IP block alive).
#
set -euo pipefail

NO_GPU_SUSPEND=0
NO_USB_HOLD=0
KERNEL=""
for arg in "$@"; do
    case "$arg" in
        --no-gpu-suspend) NO_GPU_SUSPEND=1 ;;
        --no-usb-hold)    NO_USB_HOLD=1 ;;
        *) KERNEL="$arg" ;;
    esac
done
KERNEL="${KERNEL:-/root/slmos.elf}"

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
BPMP=/sys/kernel/debug/bpmp/debug

if [[ ! -d "$GPU_POWER" ]]; then
    echo "Warning: GPU power path not found — not a Jetson Orin, or driver not loaded" >&2
    echo "Proceeding with kexec anyway..." >&2
else
    echo "[1/7] Stopping GPU consumers..."
    # Display manager holds GPU via DRM. `systemctl stop gdm` can hang
    # if the compositor is mid-render, so background it with a timeout.
    systemctl stop gdm 2>/dev/null &
    gdm_pid=$!
    for _ in {1..5}; do
        sleep 1
        kill -0 "$gdm_pid" 2>/dev/null || break
    done
    kill -9 "$gdm_pid" 2>/dev/null || true
    # NVIDIA camera/multimedia services
    systemctl stop nvargus-daemon 2>/dev/null || true
    systemctl stop nvs-service 2>/dev/null || true
    sleep 1

    if [[ "$NO_GPU_SUSPEND" == "1" ]]; then
        # In preserve-channel mode, deliberately DO NOT kill GPU fd
        # holders — the channel-helper process must stay alive to keep
        # its nvgpu channel in the runlist across kexec.
        echo "       preserving GPU consumers (--no-gpu-suspend implies"
        echo "       channel-helper must stay alive through kexec)"
    else
        # Kill any remaining GPU file descriptor holders
        fuser -k /dev/nvhost-gpu /dev/nvmap 2>/dev/null || true
        sleep 1
    fi

    if [[ "$NO_GPU_SUSPEND" == "1" ]]; then
        echo "[2/7] SKIPPING runtime-PM suspend (--no-gpu-suspend)"
        echo "       GPU stays powered — preserving Falcon ACR state for Path 3"
        echo "[3/7] SKIPPING BPMP clock re-enable (GPU already running)"
    else
        echo "[2/7] Runtime-PM suspending GPU (drains DMA to avoid RAS)..."
        echo 0 > "$GPU_POWER/autosuspend_delay_ms"
        echo auto > "$GPU_POWER/control"
        sleep 3

        status="$(cat "$GPU_POWER/runtime_status" 2>/dev/null || echo unknown)"
        pg_state="$(cat "$BPMP/powergate/gpu/state" 2>/dev/null || echo unknown)"
        echo "       after suspend: runtime=$status powergate=$pg_state"
        if [[ "$status" != "suspended" ]]; then
            echo "Warning: GPU did not suspend cleanly (status=$status)" >&2
            echo "         kexec may still crash with a TF-A RAS error" >&2
        fi

        echo "[3/7] Re-enabling GPU clocks + powergate for SLM-OS handoff..."
        # Un-powergate the GPU domain (1 = ungated)
        echo 1 > "$BPMP/powergate/gpu/state" 2>/dev/null || echo "       powergate write failed" >&2
        # Enable the primary GPU clocks. These were turned off by nvgpu's
        # runtime PM suspend; we re-enable via the BPMP debugfs interface
        # (which has the required permissions, unlike SLM-OS's own BPMP MRQs).
        for clk in gpu_pwr gpusysclk gpunvdclk nafll_gpusys; do
            if [[ -w "$BPMP/clk/$clk/state" ]]; then
                echo 1 > "$BPMP/clk/$clk/state" 2>/dev/null || true
            fi
        done
        sleep 1

        pg_final="$(cat "$BPMP/powergate/gpu/state" 2>/dev/null || echo '?')"
        gsys="$(cat "$BPMP/clk/gpusysclk/state" 2>/dev/null || echo '?')"
        gpwr="$(cat "$BPMP/clk/gpu_pwr/state" 2>/dev/null || echo '?')"
        echo "       after re-enable: powergate=$pg_final gpusysclk=$gsys gpu_pwr=$gpwr"
    fi
fi

if [[ "$NO_USB_HOLD" == "0" ]]; then
    echo "[4/7] Holding xusb clocks + powergates on for SLM-OS XHCI..."
    # Clocks the tegra-xusb host controller actually runs on. Verified
    # in the #266 Phase 0 probe (commit fb12937) as the set that
    # corresponds to a live MMIO aperture at 0x03610000. xusb_core_dev
    # and xusb_ss are deliberately NOT held — SLM-OS targets USB 2.0
    # host only for Phase 3A.
    for clk in xusb_core_host xusb_falcon xusb_fs xusb_hs_hsicp pex_usb_pad_pll0_mgmt utmi_pll; do
        if [[ -w "$BPMP/clk/$clk/state" ]]; then
            echo 1 > "$BPMP/clk/$clk/state" 2>/dev/null || \
                echo "       ${clk}: write failed" >&2
        fi
    done
    # Powergates: xusba is the USB 3 / padctl infrastructure (padctl
    # is needed even for USB 2.0 to keep the PHY straps valid);
    # xusbc is the host-controller domain. xusbb (device mode) stays
    # off — SLM-OS is host-only.
    for pg in xusba xusbc; do
        if [[ -w "$BPMP/powergate/$pg/state" ]]; then
            echo 1 > "$BPMP/powergate/$pg/state" 2>/dev/null || \
                echo "       ${pg}: write failed" >&2
        fi
    done

    # Summarise the state we're handing off so the serial log makes
    # post-kexec debugging easier.
    core_host="$(cat "$BPMP/clk/xusb_core_host/state" 2>/dev/null || echo '?')"
    falcon="$(cat "$BPMP/clk/xusb_falcon/state" 2>/dev/null || echo '?')"
    pg_a="$(cat "$BPMP/powergate/xusba/state" 2>/dev/null || echo '?')"
    pg_c="$(cat "$BPMP/powergate/xusbc/state" 2>/dev/null || echo '?')"
    echo "       after hold: xusb_core_host=$core_host xusb_falcon=$falcon xusba=$pg_a xusbc=$pg_c"
else
    echo "[4/7] SKIPPING xusb clock hold (--no-usb-hold)"
fi

if [[ "$NO_USB_HOLD" == "0" ]]; then
    echo "[5/7] Pinning tegra-xusb runtime PM so Linux doesn't idle-suspend..."
    XUSB_DEV=/sys/devices/platform/bus@0/3610000.usb
    if [[ -d "$XUSB_DEV/power" ]]; then
        # 'on' disables runtime PM; low-cost pin that doesn't itself
        # prevent Linux's device_shutdown() from running tegra-xusb's
        # .shutdown() callback at kexec time — see #285.
        echo on > "$XUSB_DEV/power/control" 2>/dev/null || \
            echo "       power/control: write failed" >&2
        status="$(cat "$XUSB_DEV/power/control" 2>/dev/null || echo '?')"
        runtime="$(cat "$XUSB_DEV/power/runtime_status" 2>/dev/null || echo '?')"
        echo "       power/control=$status runtime_status=$runtime"
    else
        echo "       tegra-xusb device path not found"
    fi

    # Experimentation notes from #285 (don't re-try these without a plan):
    #   * Unbinding tegra-xusb pre-kexec calls .remove() which halts
    #     the Falcon immediately — even DCBAAP writes wedge the
    #     aperture from SLM-OS afterwards.
    #   * Panic kexec (`kexec -p` + sysrq-c) skips device_shutdown()
    #     but loads SLM-OS at the crashkernel reserved region
    #     (0xefe00000), which doesn't match SLM-OS's 0x80000000 link
    #     address — SLM-OS mis-identifies free RAM and hangs early
    #     in boot. Incompatible without significant SLM-OS work.
    #
    # Net: Linux's kexec-time shutdown of tegra-xusb is the live
    # blocker. Until that is worked around (kernel module that
    # NULLs the .shutdown pointer, or standalone firmware load per
    # #286), SLM-OS can read the XHCI aperture but cannot run the
    # controller — USBCMD.RUN=1 wedges the Falcon-stopped MMIO.
fi

echo "[6/7] Loading kernel: $KERNEL"
kexec -l "$KERNEL" --reuse-cmdline

echo "[7/7] Executing kexec (serial console will take over)"
exec kexec -e
