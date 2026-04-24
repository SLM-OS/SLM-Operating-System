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
#   On current L4T kernels the clock hold alone is not enough. Linux's
#   kexec path also tears down the xusb stream's arm-smmu state, which
#   makes SLM-OS's first XHCI DMA transaction wedge at USBCMD.RUN=1.
#   If the arm_smmu_noshutdown module is installed, this helper now
#   loads it automatically before kexec so the xusb stream keeps a live
#   translation context plus an identity mapping over SLM-OS's NC memory
#   region.
#
# Usage:
#   sudo ./jetson-kexec-slmos.sh /path/to/slmos.elf
#   sudo ./jetson-kexec-slmos.sh --no-gpu-suspend /path/to/slmos.elf
#   sudo ./jetson-kexec-slmos.sh --no-usb-hold   /path/to/slmos.elf
#   sudo ./jetson-kexec-slmos.sh --no-smmu-fix   /path/to/slmos.elf
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
#   --no-smmu-fix       Skip loading the optional arm_smmu_noshutdown
#                       kernel module. Only use this if you are not
#                       exercising the XHCI host path, or if you are
#                       deliberately reproducing the pre-fix failure.
#
set -euo pipefail

NO_GPU_SUSPEND=0
NO_USB_HOLD=0
NO_SMMU_FIX=0
KERNEL=""
XHCI_SLOT3_HANDOFF_PAYLOAD=""
for arg in "$@"; do
    case "$arg" in
        --no-gpu-suspend) NO_GPU_SUSPEND=1 ;;
        --no-usb-hold)    NO_USB_HOLD=1 ;;
        --no-smmu-fix)    NO_SMMU_FIX=1 ;;
        *) KERNEL="$arg" ;;
    esac
done
KERNEL="${KERNEL:-/root/slmos.elf}"

find_smmu_fix_module() {
    local candidate
    for candidate in \
        "/usr/local/lib/slmos/arm_smmu_noshutdown.ko" \
        "/opt/slmos/arm_smmu_noshutdown.ko" \
        "/root/arm-smmu-noshutdown/arm_smmu_noshutdown.ko" \
        "$(dirname "$0")/arm_smmu_noshutdown.ko"
    do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

prepare_kexec_dtb() {
    local cmdline="$1"
    local live_fdt=/sys/firmware/fdt
    local out=/tmp/slmos-kexec.dtb

    if ! command -v fdtput >/dev/null 2>&1; then
        echo "Warning: fdtput not found; using kernel command line without explicit DTB patch" >&2
        return 1
    fi
    if [[ ! -r "$live_fdt" ]]; then
        echo "Warning: live FDT $live_fdt not readable; using kernel command line without explicit DTB patch" >&2
        return 1
    fi

    cp "$live_fdt" "$out"
    if ! fdtput -t s "$out" /chosen bootargs "$cmdline"; then
        echo "Warning: failed to patch /chosen/bootargs in $out; using kernel command line without explicit DTB patch" >&2
        rm -f "$out"
        return 1
    fi

    if [[ -n "$XHCI_SLOT3_HANDOFF_PAYLOAD" ]]; then
        if ! fdtput -c "$out" /slmos-handoff >/dev/null 2>&1; then
            echo "Warning: failed to create /slmos-handoff in $out" >&2
            rm -f "$out"
            return 1
        fi
        if ! fdtput -t s "$out" /slmos-handoff xhci-slot3-handoff \
              "$XHCI_SLOT3_HANDOFF_PAYLOAD"; then
            echo "Warning: failed to patch /slmos-handoff/xhci-slot3-handoff in $out" >&2
            rm -f "$out"
            return 1
        fi
    fi

    printf '%s\n' "$out"
}

verify_smmu_fix_effect() {
    local identity_line='added identity IOMMU mapping IOVA 0xbde00000..0xbe000000'
    local domain_line='xusb iommu_domain type='
    local smmu_lines

    smmu_lines="$(dmesg | grep 'arm-smmu-noshutdown:' 2>/dev/null || true)"
    if [[ "$smmu_lines" == *"$identity_line"* ]]; then
        echo "       verified: $identity_line"
        return 0
    fi

    echo "Warning: arm_smmu_noshutdown loaded but identity-map confirmation was not seen in dmesg" >&2
    if [[ "$smmu_lines" == *"$domain_line"* ]]; then
        echo "         xusb iommu_domain was found, but iommu_map success was not observed" >&2
    else
        echo "         xusb iommu_domain probe output was not observed either" >&2
    fi
    echo "         recent arm-smmu-noshutdown lines:" >&2
    printf '%s\n' "$smmu_lines" | tail -n 12 >&2 || true
    return 1
}

stash_xhci_slot3_handoff() {
    local dbg=/sys/kernel/debug/usb/xhci/3610000.usb
    local slot_ctx="$dbg/devices/03/slot-context"
    local ep_ctx="$dbg/devices/03/ep-context"
    local ep0_deq="$dbg/devices/03/ep00/dequeue"
    local reg_op="$dbg/reg-op"
    local sysfs=/sys/module/arm_smmu_noshutdown/parameters/xhci_slot3_handoff
    local py_out

    if [[ ! -r "$slot_ctx" || ! -r "$ep_ctx" || ! -r "$ep0_deq" || ! -r "$reg_op" ]]; then
        echo "       slot3 handoff skipped (debugfs state unavailable)"
        return 0
    fi

    py_out="$(python3 - "$slot_ctx" "$ep_ctx" "$ep0_deq" "$reg_op" "$sysfs" <<'PY'
import re, sys

slot_ctx_path, ep_ctx_path, ep0_deq_path, reg_op_path, sysfs_path = sys.argv[1:6]
slot_text = open(slot_ctx_path, "r", encoding="utf-8").read().strip()
ep_text = open(ep_ctx_path, "r", encoding="utf-8").read().strip()
ep0_text = open(ep0_deq_path, "r", encoding="utf-8").read().strip()
reg_text = open(reg_op_path, "r", encoding="utf-8").read()

slot_match = re.search(r'^0x([0-9a-fA-F]+):', slot_text)
route_match = re.search(r'\bRS\s+([0-9a-fA-F]+)\b', slot_text)
speed_match = re.search(r'\b(full-speed|low-speed|high-speed|super-speed)\b', slot_text, re.IGNORECASE)
ctx_match = re.search(r'Ctx Entries\s+(\d+)', slot_text)
port_match = re.search(r'Port#\s+(\d+)', slot_text)
addr_match = re.search(r'Addr\s+(\d+)', slot_text)
slot_state_match = re.search(r'State\s+([A-Za-z-]+)', slot_text)

dcbaap_lo = re.search(r'DCBAAP_LOW = 0x([0-9a-fA-F]+)', reg_text)
dcbaap_hi = re.search(r'DCBAAP_HIGH = 0x([0-9a-fA-F]+)', reg_text)
ep0_match = re.search(r'0x([0-9a-fA-F]+)', ep0_text)

ep_state_match = re.search(r'State\s+([A-Za-z-]+)', ep_text)
cerr_match = re.search(r'CErr\s+(\d+)', ep_text)
ep_type_match = re.search(r'Type\s+([A-Za-z]+)', ep_text)
maxp_match = re.search(r'maxp\s+(\d+)', ep_text)
avg_match = re.search(r'avg trb len\s+(\d+)', ep_text, re.IGNORECASE)

if not all([
    slot_match, route_match, speed_match, ctx_match, port_match, addr_match,
    slot_state_match, dcbaap_lo, dcbaap_hi, ep0_match,
    ep_state_match, cerr_match, ep_type_match, maxp_match, avg_match,
]):
    print("       slot3 handoff skipped (failed to parse debugfs state)")
    sys.exit(0)

speed_id = {
    "full-speed": 1,
    "low-speed": 2,
    "high-speed": 3,
    "super-speed": 4,
}[speed_match.group(1).lower()]

slot_state_id = {
    "disabled": 0,
    "default": 1,
    "addressed": 2,
    "configured": 3,
}[slot_state_match.group(1).lower()]

ep_state_id = {
    "disabled": 0,
    "running": 1,
    "halted": 2,
    "stopped": 3,
    "error": 4,
}[ep_state_match.group(1).lower()]

ep_type_id = {
    "ctrl": 4,
    "control": 4,
}[ep_type_match.group(1).lower()]

devctx_phys = int(slot_match.group(1), 16) & ~0x3F
route = int(route_match.group(1), 16) & 0xFFFFF
ctx_entries = int(ctx_match.group(1), 10) & 0x1F
root_port = int(port_match.group(1), 10)
slot_addr = int(addr_match.group(1), 10) & 0xFF
dcbaap = (int(dcbaap_hi.group(1), 16) << 32) | int(dcbaap_lo.group(1), 16)
ep0_deq = int(ep0_match.group(1), 16) & ~0xF
cerr = int(cerr_match.group(1), 10) & 0x3
maxp = int(maxp_match.group(1), 10) & 0xFFFF
avg = int(avg_match.group(1), 10) & 0xFFFF

slot_dw = [
    route | (speed_id << 20) | (ctx_entries << 27),
    root_port << 16,
    0,
    slot_addr | (slot_state_id << 27),
]
ep_dw = [
    ep_state_id,
    (cerr << 1) | (ep_type_id << 3) | (maxp << 16),
    (ep0_deq & 0xFFFFFFFF) | 0x1,
    (ep0_deq >> 32) & 0xFFFFFFFF,
    avg,
    0,
    0,
    0,
]
payload = ",".join(
    [f"0x{dcbaap:x}", f"0x{devctx_phys:x}", f"0x{ep0_deq:x}", str(root_port)] +
    [f"0x{x:x}" for x in slot_dw] +
    [f"0x{x:x}" for x in ep_dw]
)

if sysfs_path and sysfs_path != "-" and __import__("os").access(sysfs_path, __import__("os").W_OK):
    with open(sysfs_path, "w", encoding="utf-8") as f:
        f.write(payload)
    print("       slot3 raw-page handoff updated")

print("PAYLOAD=" + payload)
print(
    "       slot3 handoff: "
    f"DCBAAP=0x{dcbaap:016x} slot3_devctx=0x{devctx_phys:016x} "
    f"ep0_deq=0x{ep0_deq:016x} root={root_port} "
    f"slot_dw0=0x{slot_dw[0]:08x} slot_dw1=0x{slot_dw[1]:08x} "
    f"slot_dw3=0x{slot_dw[3]:08x} ep_dw0=0x{ep_dw[0]:08x} "
    f"ep_dw1=0x{ep_dw[1]:08x} ep_dw2=0x{ep_dw[2]:08x} "
    f"ep_dw3=0x{ep_dw[3]:08x} ep_dw4=0x{ep_dw[4]:08x}"
)
PY
)"

    XHCI_SLOT3_HANDOFF_PAYLOAD="$(printf '%s\n' "$py_out" | sed -n 's/^PAYLOAD=//p' | head -n1)"
    printf '%s\n' "$py_out" | sed '/^PAYLOAD=/d'
}

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
    echo "[4/8] Holding xusb clocks + powergates on for SLM-OS XHCI..."
    # Keep the full XUSB fabric live across kexec, not just the
    # minimal USB2 host subset. On jetson-nano-2, RUN=1 still wedged
    # the aperture with the narrower hold set; forcing the broader
    # xusb_core_*/xusb_falcon_*/xusb_fs_host/xusb_ss* tree on let the
    # controller reach "NO_OP round-trip OK".
    for clk in \
        xusb_core_dev \
        xusb_core_host \
        xusb_core_mux \
        xusb_core_ss \
        xusb_falcon \
        xusb_falcon_host \
        xusb_falcon_ss \
        xusb_fs \
        xusb_fs_host \
        xusb_hs_hsicp \
        xusb_ss \
        xusb_ss_superspeed \
        pex_usb_pad_pll0_mgmt \
        utmi_pll; do
        if [[ -w "$BPMP/clk/$clk/state" ]]; then
            echo 1 > "$BPMP/clk/$clk/state" 2>/dev/null || \
                echo "       ${clk}: write failed" >&2
        fi
    done
    # Keep the whole XUSB partition set alive. xusbb is nominally the
    # device-mode domain, but on nano-2 the successful RUN=1/NO_OP
    # handoff also had it forced on alongside xusba/xusbc.
    for pg in xusba xusbb xusbc; do
        if [[ -w "$BPMP/powergate/$pg/state" ]]; then
            echo 1 > "$BPMP/powergate/$pg/state" 2>/dev/null || \
                echo "       ${pg}: write failed" >&2
        fi
    done

    # Summarise the state we're handing off so the serial log makes
    # post-kexec debugging easier.
    core_dev="$(cat "$BPMP/clk/xusb_core_dev/state" 2>/dev/null || echo '?')"
    core_host="$(cat "$BPMP/clk/xusb_core_host/state" 2>/dev/null || echo '?')"
    core_ss="$(cat "$BPMP/clk/xusb_core_ss/state" 2>/dev/null || echo '?')"
    falcon="$(cat "$BPMP/clk/xusb_falcon/state" 2>/dev/null || echo '?')"
    fs_host="$(cat "$BPMP/clk/xusb_fs_host/state" 2>/dev/null || echo '?')"
    ss="$(cat "$BPMP/clk/xusb_ss/state" 2>/dev/null || echo '?')"
    pg_a="$(cat "$BPMP/powergate/xusba/state" 2>/dev/null || echo '?')"
    pg_b="$(cat "$BPMP/powergate/xusbb/state" 2>/dev/null || echo '?')"
    pg_c="$(cat "$BPMP/powergate/xusbc/state" 2>/dev/null || echo '?')"
    echo "       after hold: xusb_core_dev=$core_dev xusb_core_host=$core_host xusb_core_ss=$core_ss xusb_falcon=$falcon xusb_fs_host=$fs_host xusb_ss=$ss xusba=$pg_a xusbb=$pg_b xusbc=$pg_c"
else
    echo "[4/8] SKIPPING xusb clock hold (--no-usb-hold)"
fi

if [[ "$NO_USB_HOLD" == "0" ]]; then
    echo "[5/8] Pinning tegra-xusb runtime PM so Linux doesn't idle-suspend..."
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
    # Net: clock hold alone is insufficient on current L4T. The
    # xusb stream's SMMU state must also survive into kexec; the
    # helper handles that in the next step when the optional
    # arm_smmu_noshutdown module is installed.
fi

if [[ "$NO_SMMU_FIX" == "0" ]]; then
    echo "[6/8] Preparing xusb arm-smmu state for post-kexec XHCI..."
    if [[ -d /sys/module/arm_smmu_noshutdown ]]; then
        echo "       arm_smmu_noshutdown already loaded"
        if ! verify_smmu_fix_effect; then
            echo "Error: arm_smmu_noshutdown is present, but the xusb identity mapping is not confirmed" >&2
            echo "       refusing to kexec into a known-bad XHCI handoff state" >&2
            exit 1
        fi
    else
        if smmu_mod="$(find_smmu_fix_module)"; then
            echo "       loading $smmu_mod"
            if insmod "$smmu_mod"; then
                echo "       arm_smmu_noshutdown loaded"
                if ! verify_smmu_fix_effect; then
                    echo "Error: arm_smmu_noshutdown did not confirm the xusb identity mapping" >&2
                    echo "       refusing to kexec into a known-bad XHCI handoff state" >&2
                    exit 1
                fi
            else
                echo "Warning: failed to load $smmu_mod" >&2
                echo "         USB-A XHCI may wedge at RUN=1 without the SMMU fix" >&2
            fi
        else
            echo "Warning: arm_smmu_noshutdown.ko not found" >&2
            echo "         looked in /usr/local/lib/slmos, /opt/slmos, /root/arm-smmu-noshutdown, and $(dirname "$0")" >&2
            echo "         USB-A XHCI may wedge at RUN=1 without the SMMU fix" >&2
        fi
    fi
    if [[ "$NO_USB_HOLD" == "0" ]]; then
        stash_xhci_slot3_handoff
    fi
else
    echo "[6/8] SKIPPING arm-smmu fix (--no-smmu-fix)"
fi

echo "[7/8] Loading kernel: $KERNEL"
KEXEC_CMDLINE="$(cat /proc/cmdline)"
if [[ -n "$XHCI_SLOT3_HANDOFF_PAYLOAD" ]]; then
    KEXEC_CMDLINE+=" slmos_xhci_slot3_handoff=$XHCI_SLOT3_HANDOFF_PAYLOAD"
fi
KEXEC_DTB="$(prepare_kexec_dtb "$KEXEC_CMDLINE" || true)"
if [[ -n "$KEXEC_DTB" ]]; then
    echo "       patched DTB: $KEXEC_DTB"
    kexec -l "$KERNEL" --command-line="$KEXEC_CMDLINE" --dtb="$KEXEC_DTB"
else
    kexec -l "$KERNEL" --command-line="$KEXEC_CMDLINE"
fi

echo "[8/8] Executing kexec (serial console will take over)"
exec kexec -e
