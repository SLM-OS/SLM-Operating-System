# Deploying SLM-OS to Jetson Orin Nano — kexec from Linux

This guide covers the **primary deploy path** for Jetson Orin Nano boards: boot Linux (L4T / JetPack) from the normal rootfs, then `kexec` into SLM-OS. This inherits the NVIDIA boot chain's exception-level configuration (NS EL2 + VHE) and PCIe initialization, which SLM-OS needs — the UEFI-direct cold-boot path is still WIP (see `docs/archive/investigations/jetson-uefi-direct-result.md` for blockers).

For Jetson hardware, this guide applies regardless of whether Linux is installed on the microSD card (`jetson-nano-2` layout) or an NVMe SSD (`jetson-nano-1` layout) — the kexec-from-Linux flow is identical.

---

## When to use this guide

| Scenario | Guide |
|---|---|
| Jetson with L4T already installed | This guide |
| Fresh Jetson, no OS installed | Flash L4T via NVIDIA SDK Manager first (out of scope here), then this |
| Jetson cold-boot directly to SLM-OS (no Linux in the chain) | **Not yet working** — `docs/archive/investigations/jetson-uefi-direct-result.md` |

---

## Prerequisites

| Item | Source / Version |
|---|---|
| Jetson Orin Nano Developer Kit with L4T installed | L4T 36.4.7 / JetPack R36 known to work. See `docs/lab-operations.md` for lab board state. |
| Jetson on the network (SSH reachable) | `ssh root@<JETSON_IP>` — password `slmos` in the capstone lab, or pre-shared key |
| SLM-OS source checkout | `make kernel PLATFORM=JETSON_ORIN_NANO` succeeds |
| ARM GNU Toolchain | `aarch64-none-elf-gcc` |
| Serial console set up in labctl | See `docs/lab-operations.md` — CP2102 adapter required for bidirectional serial on the debug header (CH340 is send-only on Jetson) |
| `slmos-kexec` helper installed on the Jetson | One-time setup, below |

---

## One-time setup: install the `slmos-kexec` helper

The `scripts/jetson-kexec-slmos.sh` helper does three things the raw `kexec` call cannot:

1. **GPU runtime-PM suspend + BPMP clock re-enable.** Prevents the TF-A RAS Uncorrectable Error that would otherwise kill the CPU core mid-kexec (issue #9). The suspend drains stale nvgpu DMA; the BPMP re-enable puts the GPU's clocks + powergate back on so SLM-OS sees `NV_PMC_BOOT_0` instead of `0xFFFFFFFF`.
2. **USB xHCI clock + powergate holds** (`--no-usb-hold` to skip). Keeps `xusba` / `xusbc` powergates and the `xusb_*` clock tree alive across the kexec so SLM-OS's XHCI driver finds the controller responsive (issue #266).
3. **Optional xusb arm-smmu preservation.** If `arm_smmu_noshutdown.ko` is installed in `/usr/local/lib/slmos/arm_smmu_noshutdown.ko` (or one of the helper's fallback search paths), the helper loads it before `kexec -e`. This preserves the xusb stream's live SMMU context and adds an identity mapping for SLM-OS's NC memory region, which is required for the current XHCI RUN/NO_OP path.

Install once:

```bash
scp scripts/jetson-kexec-slmos.sh root@<JETSON_IP>:/usr/local/bin/slmos-kexec
ssh root@<JETSON_IP> "chmod +x /usr/local/bin/slmos-kexec"
```

Optional but recommended for USB-A/XHCI work:

```bash
scp -r scripts/arm-smmu-noshutdown root@<JETSON_IP>:/root/
ssh root@<JETSON_IP> 'mkdir -p /usr/local/lib/slmos && \
    cd /root/arm-smmu-noshutdown && make && \
    install -m 0644 arm_smmu_noshutdown.ko /usr/local/lib/slmos/'
```

If the `.ko` is not present, `slmos-kexec` still runs, but the Jetson
USB-A host path may wedge at `USBCMD.RUN=1`.

Current status on `jetson-nano-2`: with the helper's wider XUSB hold
set, the SMMU preservation module installed, and the helper's USB2
root-hub cleanup enabled, SLM-OS now preserves a cleaned addressed
slot-1 handoff across `kexec`, adopts the retained Realtek root hub,
fresh-enumerates the downstream RTL8153, and brings networking up with
no manual unplug/replug. Current validation on the lab path:

- `net init` succeeds
- `ifconfig dhcp` binds `192.168.4.5/24` with gateway `192.168.4.1`
- `ping 192.168.4.1 2` succeeds

The helper script is reasonably well commented. Run it with `--help` on the Jetson for the full flag list, or read the script header for rationale on each step.

---

## Deploy workflow

### Step 1 — Build

```bash
make kernel PLATFORM=JETSON_ORIN_NANO
```

Output: `build/kernel/slmos.elf`. Unlike Pi 5, Jetson takes the ELF form directly — kexec handles loading the segments to the entry address (`0x80000000`) itself.

For one-off Jetson bring-up diagnostics that exist as raw CMake
options rather than first-class Make variables, pass them through with
`EXTRA_KERNEL_CMAKE_ARGS`. Example:

```bash
make kernel-clean
make kernel PLATFORM=JETSON_ORIN_NANO \
    EXTRA_KERNEL_CMAKE_ARGS=-DJETSON_XHCI_REBOOT_ON_NOOP=ON
```

That specific flag builds a one-shot diagnostic image that PSCI-resets
back to Linux immediately after the XHCI driver reaches `NO_OP
round-trip OK`, which is useful when serial capture is unavailable.

### Step 2 — Copy to the Jetson

```bash
scp build/kernel/slmos.elf root@<JETSON_IP>:/root/
```

### Step 3 — kexec

```bash
ssh root@<JETSON_IP> 'slmos-kexec /root/slmos.elf'
```

The helper prints its progress as it runs — GPU suspend, BPMP clock force-on, USB hold, final `kexec -e`. The SSH session terminates at the `kexec -e` step (the kernel is replaced underneath the running userspace). Expected: network goes away within ~2 s of the final line.

On the current `jetson-nano-2` lab setup, this path no longer needs a
manual USB unplug/replug after `kexec`; the helper's root-hub cleanup
and the retained slot-1 handoff are enough to get the Realtek USB
Ethernet chain back in SLM-OS automatically.

### Step 4 — Observe via serial

From the host (before or after issuing `kexec`):

```bash
labctl connect jetson-nano-1-console
```

Expected output on serial:

1. Kernel messages from SLM-OS's `uart_tegra.c` driver — via UARTC at `0x0C280000`, routed through the TCU HSP mailbox to the USB-C debug port.
2. SMP bring-up on 6 cores (Jetson is dual-cluster 2+4; MPIDRs `0x000`, `0x100`, `0x200`, `0x300`, `0x10200`, `0x10300`).
3. `slm>` shell prompt. Input works bidirectionally via the TCU mailbox.

### Step 4a — Run the networking smoke test

For the validated `jetson-nano-2` lab path, the host-side smoke script
automates the current regression check end-to-end:

```bash
scripts/tests/test-jetson-kexec-networking-smoke.sh \
    --ssh-target root@192.168.4.93 \
    --console-port 4004
```

That script:

- copies `build/kernel/slmos.elf` and `scripts/jetson-kexec-slmos.sh`
- triggers `slmos-kexec`
- waits for `slmos>` on serial
- runs `net init`, `ifconfig dhcp`, `ifconfig`, and `ping 192.168.4.1 2`
- verifies DHCP `192.168.4.5/24` with gateway `192.168.4.1`

It is intentionally scoped to the current lab topology rather than a
generic Jetson serial automation framework.

### Step 5 — Recovery back to Linux

Once work in SLM-OS is finished:

```bash
labctl power cycle jetson-nano-1
```

Wait ~40 s for Linux to boot, then SSH is available again. SLM-OS itself has no graceful-shutdown-back-to-Linux path — the kexec is a one-way handoff.

---

## Helper script flags

The `slmos-kexec` helper's default behavior suits most cases. The common non-defaults:

| Flag | When to use |
|---|---|
| `--no-gpu-suspend` | Path 3 / issue #190: preserves the GPU's ACR / Falcon security state across kexec so SLM-OS inherits Linux's already-running FECS / GPCCS / PMU. Risk: stale DMA may still trigger a TF-A RAS error, though in practice it hasn't fired when GPU consumers are stopped first. |
| `--no-usb-hold` | SLM-OS builds that don't drive the XHCI controller. The held clocks are otherwise harmless. |
| `--no-smmu-fix` | Skip loading `arm_smmu_noshutdown.ko`. Only useful if you are not exercising the USB-A/XHCI path or are deliberately reproducing the pre-fix failure. |
| `--no-usb-root-cleanup` | Skip the Linux-side USB2 root-hub `authorized=0` cleanup before `kexec`. Useful only if you are intentionally reproducing the old stale-slot path; the default cleanup is what makes the retained slot-1 handoff reproducible on `jetson-nano-2`. |

Full flag list in the script header.

---

## Troubleshooting

### kexec succeeds but the Jetson reboots / resets instead of running SLM-OS

Almost always the TF-A RAS error (issue #9) — a stale nvgpu DMA operation fired after CPU had already entered SLM-OS's early boot, TF-A caught it at EL3, and powered off the core. Confirm `slmos-kexec` was used (not raw `kexec`), and that `--no-gpu-suspend` was NOT passed. The GPU suspend step is what prevents this.

### kexec runs but no serial output

1. **Wrong serial adapter on the debug header.** Jetson's SPE firmware only handles input from CP2102 (Silicon Labs). CH340-based adapters are send-only on Jetson and input characters never reach the SPE mailbox. CH340 is fine on Pi 5, but not Jetson.
2. **Serial output uses UARTC (`0x0C280000`), not UARTA.** UARTA on the 40-pin header is blocked by the CBB firewall at EL2. If a serial adapter is wired to the 40-pin header expecting UARTA, it will show nothing. The USB-C debug port routes UARTC via the TCU HSP mailbox.
3. **DSB-barrier issue.** Post-kexec, speculative MMIO reads can return stale LSR values. `uart_tegra.c` issues `dsb sy` before every LSR read. If a change to the driver removed that barrier, output is silent. See `docs/uart-hardware.md` §"Post-kexec MMIO Ordering".

### SSH still works right after `slmos-kexec` — SLM-OS didn't take over

Confirm the `kexec -e` step actually fired. Sometimes `kexec_load` succeeds but `kexec -e` fails silently if the kernel refuses the image — check `dmesg` on the Jetson. Also confirm the binary path: `/root/slmos.elf` is the expected location, but `slmos-kexec` takes a path argument so check which one was used.

---

## Related documents

- `docs/lab-operations.md` — labctl setup, network / SSH / serial config for each Jetson board
- `docs/jetson-boot.md` — low-level boot sequence reference
- `docs/jetson-el2-bringup.md` — VHE / EL2 configuration after kexec
- `docs/archive/investigations/jetson-nvidia-support.md` — GPU / BPMP / PCIe state across kexec
- `scripts/jetson-kexec-slmos.sh` — the helper itself, with detailed in-file comments
