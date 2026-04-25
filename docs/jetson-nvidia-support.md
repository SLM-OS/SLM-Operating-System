# Jetson Orin Nano: Hardware Blockers and NVIDIA Support Requirements

This document consolidates all findings related to running SLM-OS on the Jetson Orin Nano, including hardware security blockers, attempted solutions, NVIDIA forum research, and potential paths forward.

**Status:** RESOLVED — EL2 + VHE approach implemented April 2026. See `docs/jetson-el2-bringup.md`.
**Last Updated:** April 2026

> **April 2026 Update:** The CBB firewall has been partially bypassed by running SLM-OS
> at EL2 with VHE (Virtual Host Extensions). UARTC, GICv3, timer, GPU, and ~6.7 GB of
> memory are all accessible from EL2. SLM-OS boots to an interactive shell.
> This document is preserved as a historical record of the investigation that led to
> the EL2 solution. For current implementation details, see `docs/jetson-el2-bringup.md`.
>
> **17 April 2026 Update:** When investigating bare-metal PCIe access for the #25
> NIC driver, the "CBB firewall blocks peripheral X" framing was **misapplied** —
> the 0xFFFFFFFF reads on PCIe are actually caused by `pex2_c8_core` clock gating
> during kexec, not by firewall policy. See `docs/jetson-pcie-investigation.md` for
> the full evidence chain. Future investigations should **verify clock state first**
> (disable the clock from Linux via BPMP debugfs; does the access symptom reproduce?)
> before assuming the CBB firewall is the culprit.
>
> **21 April 2026 Update:** GA10B GPU compute is working end-to-end from SLM-OS
> via a Linux-helper-assisted kexec handoff. A pre-kexec Linux helper
> (`scripts/gpu-kernel-launch.c --preserve-for-kexec`) allocates the channel,
> uploads a CUDA-compiled shader and a QMD, and publishes a v3 handoff block
> into DRAM. Post-kexec, SLM-OS inherits the channel via `nvgpu channel` and
> launches the compute kernel via `nvgpu launch-kernel` — the GPU SMs execute
> the shader and write 0xCAFE to a known physical address that SLM-OS reads
> back. Resolves #297 (host-family SEMAPHORE_RELEASE), #291 (COMPUTE_B
> SEMAPHORE_RELEASE — the MME_FE1 blocker was actually a symptom of the
> pre-PR-#295 encoding bug, not a missing MME init), and #356 (QMD-based
> compute kernel launch). Fully SLM-OS-native channel/shader bringup
> (without libcuda pre-kexec) remains future work.
> The "no GPU compute capability on bare metal" bullet under Solution 5
> below is therefore outdated for Jetson — it still applies to Pi 5 whose
> VideoCore GPU has no public bare-metal documentation.
>
> **24 April 2026 Update:** Phase 8 scales up beyond the original
> `write_cafe` scalar store. Three more CUDA-compiled kernels
> (`scripts/cuda/dot4.cu` — 4-elem dot product, `matmul4x4.cu` —
> single-thread 4×4 matmul, `matmul4x4_mt.cu` — 16-thread 4×4 matmul)
> all dispatch SLM-OS-side post-kexec. Handoff v4 adds an
> `expected_payload` field so `nvgpu launch-kernel` polls for
> arbitrary values (300, 30, …), not just 0xCAFE. Per-kernel launchers
> share scaffolding via `scripts/gpu-launch-common.{h,c}`; the
> multi-threaded matmul is the first to override the default 1×1×1
> CTA thread dims. All four kernels validated Linux-side and
> SLM-OS-side on jetson-nano-1.
>
> **25 April 2026 Update:** MNIST inference now runs end-to-end on
> the GA10B GPU from SLM-OS post-kexec. `models/test/mnist.onnx`
> dispatches as an 8-op chain (Conv1 → Add+ReLU → Pool1 → Conv2 →
> Add+ReLU → Pool2 → MatMul → AddBias) producing 10 fp32 logits
> whose argmax matches the existing CPU NEON reference. Eight
> milestones (M0–M8 in `docs/jetson-gpu-mnist-plan.md`) cover
> multi-CTA dispatch, fp32 SASS, parameterized GEMM/Conv2D/Pool/
> Add+ReLU kernels, and the v5 multi-op handoff that lets one
> `nvgpu launch-kernel` invocation chain N kernels. The previously-
> outdated "no GPU compute capability on bare metal" framing was
> already retired for Jetson at Phase 8; this milestone closes the
> "demonstration vs real inference" gap as well. The remaining
> future work is fully SLM-OS-native channel + buffer setup
> (Linux helper still does the `nvgpu` ioctls pre-kexec) and Rust
> runtime FFI integration so `slm.model_infer()` routes to GPU
> automatically.

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [The Core Problem: CBB Firewall](#the-core-problem-cbb-firewall)
3. [Attempted Boot Methods](#attempted-boot-methods)
4. [BPMP Communication Issues](#bpmp-communication-issues)
5. [NVIDIA Forum Research](#nvidia-forum-research)
6. [Potential Solutions](#potential-solutions)
7. [Questions for NVIDIA Support](#questions-for-nvidia-support)
8. [Technical Reference](#technical-reference)
9. [Related Documentation](#related-documentation)

---

## Executive Summary

SLM-OS successfully runs on QEMU and Raspberry Pi 5, but is blocked on Jetson Orin Nano by **hardware-enforced security restrictions**. The Tegra234 SoC includes a Control Backbone (CBB) firewall that blocks all peripheral access from unsigned/unauthenticated code.

### Key Findings

| Finding | Status | Impact |
|---------|--------|--------|
| kexec boot | Not supported | NVIDIA confirmed, CBB errors expected |
| Direct UEFI boot | Blocked | Same CBB firewall restrictions |
| extlinux boot | Blocked | L4T uses kexec internally |
| BPMP communication | Corrupted after kexec | Cannot enable UART clocks |
| TCU (USB-C debug) | Not available | Requires SPE firmware (Linux only) |
| EL2 hypervisor mode | Software-disabled | Hardware supports it, BSP disables it |
| Secure boot signing | Requires full reflash | Cannot sign payloads at runtime |

### What Works

- UART clock can be enabled before kexec by opening `/dev/ttyTHS1`
- Serial output works during early boot (UEFI, L4T bootloader)
- Boot errors can be captured via 40-pin header UART
- SLM-OS code executes (verified via PSCI reboot checkpoint)

### What Doesn't Work

- Any peripheral access from SLM-OS (CBB firewall blocks it)
- kexec (not validated by NVIDIA, triggers security errors)
- Direct UEFI boot (same CBB restrictions)
- ~~BPMP IPC re-initialization after kexec~~ — **WORKS** as of April 2026
  via `kernel/drivers/bpmp/` port of edk2-nvidia's BpmpIpc. See
  `docs/jetson-bpmp-ipc-plan.md` and `docs/jetson-pcie-investigation.md`
  §"Step 2 landing".

---

## The Core Problem: CBB Firewall

### What is the CBB Firewall?

The Control Backbone (CBB) is a hardware security feature on Tegra234 that controls access to all SoC peripherals. The CBB firewall:

- Enforces access control lists for memory-mapped I/O regions
- Blocks unauthorized code from accessing peripherals
- Is configured by the bootloader during secure boot
- Cannot be bypassed through software alone

### Error Manifestation

When SLM-OS attempts to access any peripheral (e.g., UART at 0x03100000), the CBB firewall triggers a RAS (Reliability, Availability, Serviceability) error:

```
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:      Status = 0xec00030d
ERROR:   SERR = Illegal address (software fault): 0xd
ERROR:      IERR = CBB Interface Error: 0x6
ERROR:      ADDR = 0x8000000003100000
ERROR:   Powering off core
```

Key observations:
- Error address `0x8000000003100000` is the UART base address
- `IERR = CBB Interface Error: 0x6` indicates firewall rejection
- `SERR = Illegal address` means the access was blocked, not that the address is wrong
- The error comes from EL3 (TrustZone firmware), not from SLM-OS

### Why This Happens

The Tegra234 secure boot chain establishes which code can access which peripherals:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Tegra234 Secure Boot Chain                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   BootROM ──► MB1 ──► MB2 ──► UEFI ──► L4T ──► Linux                │
│      │         │       │       │                  │                 │
│      │         │       │       │                  │                 │
│      ▼         ▼       ▼       ▼                  ▼                 │
│   Verify    Config   Config  Config           Trusted               │
│   signatures  CBB     CBB    CBB              (has CBB              │
│              rules   rules   rules            permissions)          │
│                                                                     │
│   ════════════════════════════════════════════════════════════════  │
│                                                                     │
│   SLM-OS (via kexec or direct boot)                                 │
│      │                                                              │
│      ▼                                                              │
│   NOT in CBB allowlist ──► BLOCKED                                  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

Linux is trusted because it was loaded through the secure boot chain. SLM-OS, loaded via kexec or direct UEFI boot, is not recognized by the CBB configuration established during boot.

---

## Attempted Boot Methods

### Method 1: Kexec from Linux

**Approach:** Boot Linux normally, then use `kexec` to jump to SLM-OS.

**Results:**
- SLM-OS code executes (verified via PSCI reboot checkpoint)
- CBB firewall blocks all peripheral access
- UART writes produce no output

**NVIDIA Position:** "Didn't validate that before. Better using other kind of method to replace kernel dtb/image." (WayneWWW, NVIDIA Engineer)

**Conclusion:** kexec is explicitly not supported on Jetson.

### Method 2: extlinux.conf Boot Entry

**Approach:** Add SLM-OS to `/boot/extlinux/extlinux.conf` and select at boot menu.

**Results:**
- Same CBB firewall errors as kexec
- Error address confirms UART access blocked

**Root Cause:** L4T bootloader (BOOTAA64.efi) uses kexec internally. It boots a minimal Linux kernel first, then uses kexec to load the selected kernel from extlinux.conf.

**Conclusion:** extlinux approach has identical limitations to direct kexec.

### Method 3: Direct UEFI Boot Entry

**Approach:** Create UEFI boot entry pointing directly to SLM-OS binary, bypassing L4T bootloader entirely.

**Commands used:**
```bash
cp /boot/slmos.bin /boot/efi/EFI/BOOT/SLMOS.efi
efibootmgr -c -d /dev/mmcblk0 -p 10 -L 'SLM-OS Direct' -l '\\EFI\\BOOT\\SLMOS.efi'
efibootmgr -n 0009  # Set as next boot
```

**Results:**
- Same CBB firewall errors
- Crash occurs on first UART access

**Conclusion:** The CBB firewall restriction is NOT kexec-specific. It's a fundamental Tegra234 security feature that blocks all unsigned/unauthenticated code from peripheral access.

### Method 4: UART Silent Mode

**Approach:** Disable UART access entirely in SLM-OS to see if other code executes.

**Results:**
- Different error: UEFI Synchronous Exception
- System enters boot loop

**Conclusion:** Even without UART, other peripheral accesses trigger similar restrictions.

---

## BPMP Communication

### Background

The BPMP (Boot and Power Management Processor) controls clocks and power on Tegra SoCs. It runs on a separate ARM Cortex-R5 core and communicates via IVC (Inter-VM Communication) channels on SYSRAM at `0x40070000` / `0x40071000` + HSP doorbells at `0x03C00000`.

### Status (April 2026): WORKING

SLM-OS has a full BPMP IPC stack at `kernel/drivers/bpmp/` (hsp.c / ivc.c / mrq.c / bpmp.c), ported from edk2-nvidia's `BpmpIpcDxe` and cross-checked against Linux's `tegra-bpmp`. After kexec, the driver runs a fresh Sync → Ack → Established IVC handshake to reset channel state, then round-trips MRQ_PING, MRQ_CLK, MRQ_RESET, MRQ_UPHY, and MRQ_PG with BPMP firmware.

Verified via `hspdiag` + `bpmp` shell commands — see `docs/jetson-pcie-investigation.md` §"BPMP IPC port" for live output and `docs/jetson-bpmp-ipc-plan.md` for the port design.

### Previously attempted (now obsolete)

The first iteration (#190, landed January 2026) had three root-cause bugs that made it look like BPMP rejected CCPLEX:

1. Wrong `ivc_channel_header` struct layout — offset 0x04 is `tx.state`, not `r_count` as the code assumed. Writes to "r_count=0" clobbered `State` and caused TF-A RAS Uncorrectable Errors.
2. Hardcoded HSP doorbell offset `HSP+0x10000+master*0x100` instead of computing from `HSP_DIMENSIONING`. Real offset on Tegra234 is `HSP+0x90000+master*0x100`.
3. No IVC handshake — code trusted Linux's pre-kexec channel state.

All three fixed in the April 2026 rewrite. #190 closed.

### UART Clock Status

Verified via debugfs before kexec:
```bash
cat /sys/kernel/debug/clk/uarta/clk_enable_count
# Output: 0 (disabled)

# After opening /dev/ttyTHS1:
cat /sys/kernel/debug/clk/uarta/clk_enable_count
# Output: 1 (enabled)
```

Opening the UART device enables the clock, but Linux disables it before kexec completes.

---

## NVIDIA Forum Research

### Official Position on Kexec

**Source:** [kexec on the Jetson not working (CBB errors)](https://forums.developer.nvidia.com/t/kexec-on-the-jetson-not-working-cbb-errors/275207)

> **NVIDIA Engineer (WayneWWW):** "Didn't validate that before. Better using other kind of method to replace kernel dtb/image."

kexec is confirmed as **not supported/validated** on Jetson platforms. CBB errors are expected behavior.

### CBB Firewall Configuration

**Source:** [How to disable OEM firewall on Orin?](https://forums.developer.nvidia.com/t/how-to-disable-oem-firewall-on-orin/304878)

Users have attempted configuring firewall permissions via `tegra234-mb2-bct-scr-p3701-0000-override.dts`:

```dts
reg@4673 { /* CBB_CENTRAL_CBB_FIREWALL_NVCSI_ENGINE_BLF, READ_CTL */
    exclusion-info = <0>;
}
reg@4674 { /* CBB_CENTRAL_CBB_FIREWALL_NVCSI_ENGINE_BLF, WRITE_CTL */
    exclusion-info = <0>;
}
reg@4675 { /* CBB_CENTRAL_CBB_FIREWALL_NVCSI_ENGINE_BLF, CTL_SETTING */
    exclusion-info = <0>;
}
```

However, NVIDIA did not provide official documentation on how to use this mechanism, and the thread closed without a solution.

### Direct Register Access at EL2

**Source:** [Direct register access to Timer](https://forums.developer.nvidia.com/t/direct-register-access-to-timer/286298)

A user successfully achieved bare-metal peripheral access at EL2 by configuring MMU translation tables:

> "The main issue was that the memory mapped device registers mentioned were not configured in the address translation tables."

**Their working approach:**
1. Run code at EL2 (hypervisor level)
2. Examine existing translation tables from TTBR0_EL2
3. Add missing entries for device memory in L3 translation tables
4. Use 4KB granularity for MMIO register mapping
5. Avoid conflicts with pre-existing UEFI configurations

This allowed accessing peripherals like UARTC (0xC280000) from bare-metal code.

### EL2/Hypervisor Mode Status

**Source:** [Clarification on Jetson Orin Hypervisor Support](https://forums.developer.nvidia.com/t/clarification-on-jetson-orin-hypervisor-support-hardware-lock-or-only-unsupported/348348)

> **NVIDIA:** "By default it is not supported on Jetpack release. You may see if there is a method to enable it."
>
> "If it is pure software approach, it does not impact warranty."

**Key finding:** EL2 is **software-disabled, not hardware-locked**. The Tegra234 hardware fully supports EL2/hypervisor mode, but NVIDIA's BSP intentionally disables it.

### Secure Boot Signing

**Source:** [Jetson Orin AGX UEFI secureboot payload manual signature](https://forums.developer.nvidia.com/t/jetson-orin-agx-uefi-secureboot-payload-manual-signature/291388)

- Primary signing tool: `l4t_sign_image.sh`
- **Cannot modify signed payloads after Secure Boot is activated**
- Requires full reflash with PKC/SBK keys
- "You cannot simply sign custom payloads with OpenSSL and substitute them"

**Source:** [Secure boot UEFI clarifications](https://forums.developer.nvidia.com/t/secure-boot-uefi-clarifications/302602)

- UEFI secure boot uses PK (Platform Key), KEK (Key Exchange Key), and db keys
- Keys generated via `gen_uefi_default_keys_dts.sh`
- QSPI flash protected by firewalls in production deployments

---

## Potential Solutions

### Solution 1: EL2 Hypervisor Approach — ✅ IMPLEMENTED (April 2026)

This approach was successfully implemented. See `docs/jetson-el2-bringup.md` for full details.

**Requirements:**
- Enable EL2 in UEFI/bootloader configuration
- Boot SLM-OS at EL2 instead of EL1
- Configure TTBR0_EL2 with proper device memory mappings
- Handle EL2-specific exception vectors

**Advantages:**
- Forum evidence suggests this works
- Software-only approach (no warranty impact per NVIDIA)
- Doesn't require secure boot integration

**Challenges:**
- No official documentation for EL2 enablement
- May require custom UEFI modifications
- Exception level changes affect entire kernel architecture

**Investigation needed:**
- How to configure UEFI to boot payload at EL2
- Whether CBB firewall respects EL2 differently than EL1
- Full page table configuration requirements

### Solution 2: Secure Boot Integration

Integrate SLM-OS into NVIDIA's secure boot chain so it receives proper CBB permissions.

**Requirements:**
- Generate signing keys (PKC/SBK)
- Modify flash configuration to include SLM-OS
- Sign SLM-OS with `l4t_sign_image.sh`
- Full device reflash

**Advantages:**
- "Correct" approach per NVIDIA's security model
- Would receive full peripheral access
- Could potentially boot as primary OS

**Challenges:**
- Requires full reflash for any kernel update
- No documentation for bare-metal OS integration
- May require fuse burning for production

### Solution 3: CBB Firewall Configuration

Configure CBB firewall rules to allow SLM-OS peripheral access.

**Requirements:**
- Modify `tegra234-mb2-bct-scr-*-override.dts`
- Identify correct register addresses for UART, GIC, timers
- Reflash bootloader with modified configuration

**Advantages:**
- Could allow peripheral access without full secure boot
- Targeted solution for specific peripherals

**Challenges:**
- No official documentation on syntax/registers
- NVIDIA did not respond to forum requests for this info
- May require iterative trial-and-error

### Solution 4: Linux Hypervisor with Device Passthrough

Run Linux as host, pass through GPU and specific devices to SLM-OS guest.

**Requirements:**
- Enable KVM/Xen on Jetson
- Configure IOMMU for device passthrough
- Run SLM-OS as VM guest

**Advantages:**
- Linux handles CBB/security, SLM-OS gets device access
- Well-documented approach (standard virtualization)

**Challenges:**
- Adds latency and complexity
- May not support GPU passthrough
- Defeats purpose of bare-metal OS

### Solution 5: Focus on Raspberry Pi 5

Given the hardware security restrictions on Jetson Orin, Raspberry Pi 5 may be a more viable bare-metal target.

**Advantages:**
- Well-documented peripherals
- No security firewalls blocking peripheral access
- Active bare-metal development community
- SLM-OS already boots and runs on Pi 5

**Challenges:**
- No GPU compute capability (CPU-only inference)
- Less powerful than Jetson for AI workloads

---

## Questions for NVIDIA Support

### Primary Questions

1. **"Is there documentation for running code at EL2 (hypervisor level) on Jetson Orin?"**
   - Forum evidence suggests this works for direct peripheral access
   - What UEFI/bootloader configuration is required?
   - Are there example projects or reference implementations?

2. **"What is the procedure for configuring CBB firewall permissions via `tegra234-mb2-bct-scr-*-override.dts`?"**
   - What is the syntax for adding peripheral access rules?
   - What are the register addresses for UARTA, GIC, timer?
   - Is there a reference for the `reg@XXXX` numbering scheme?

3. **"Can a custom bare-metal OS be signed and integrated into the Jetson secure boot chain?"**
   - What is the process for signing a non-Linux kernel?
   - Would a signed bare-metal OS receive CBB permissions?
   - Is there documentation for this use case?

4. **"Is there a development/debug mode that relaxes CBB restrictions for prototyping?"**
   - Similar to how some SoCs have "engineering mode" fuses
   - Would allow bare-metal development without full secure boot

### Context for Support Request

**Project:** SLM-OS (Small Language Model Operating System)
- Capstone project for Computer Science at Sonoma State University
- Purpose-built OS for AI inference on edge devices
- Successfully runs on QEMU (ARM64 virt) and Raspberry Pi 5
- Blocked on Jetson by CBB firewall restrictions

**Goal:** Run SLM-OS bare-metal on Jetson Orin Nano to leverage:
- 6-core ARM Cortex-A78AE CPU
- 1024-core Ampere GPU (67 TOPS INT8)
- 8GB unified LPDDR5 memory

**Preferred Solution:** Whichever approach NVIDIA recommends for running a custom bare-metal OS with peripheral access.

---

## Technical Reference

### Relevant Addresses

| Peripheral | Address | Type | Status |
|------------|---------|------|--------|
| UARTA | 0x03100000 | NS16550 | Blocked by CBB |
| UARTC | 0x0C280000 | NS16550 | Forum reports EL2 access works |
| GIC Distributor (GICD) | 0x0F400000 | GICv3 | Unknown |
| GIC Redistributor (GICR) | 0x0F440000 | GICv3 | Unknown |
| Watchdog | 0x02190000 | Timer | Works (disabled successfully) |
| BPMP IVC TX | 0x0C168000 | HSP Mailbox | IVC corrupted after kexec |

### CBB Error Decoding

| Field | Value | Meaning |
|-------|-------|---------|
| SERR | 0xd | Illegal address (software fault) |
| IERR | 0x6 | CBB Interface Error |
| IERR | 0x3 | Carveout Uncorrectable Error |
| Base 0xe010000 | IOB | I/O Bridge fabric |
| Base 0xe011000 | SNOC | System Network-on-Chip |

### Exception Levels

| Level | Name | SLM-OS Usage |
|-------|------|--------------|
| EL0 | User | Future user-space components |
| EL1 | Kernel | Current SLM-OS execution level |
| EL2 | Hypervisor | Potential solution (forum success) |
| EL3 | Secure Monitor | TrustZone, generates CBB errors |

---

## Final State (April 2026) — Capstone Delivery

> This section is authored during Phase G6 of the Jetson capstone
> execution plan to consolidate the final Jetson support status for
> the capstone report.

**Boot & serial:** Resolved by running SLM-OS at **NS EL2 with VHE**
after kexec from Linux. UARTC (0x0C280000) is reachable; TCU
continues routing USB-C debug serial. UARTA on the 40-pin header
remains blocked by the CBB firewall even at EL2 — no known workaround
short of an L4T-side firewall reconfiguration, which is out of scope.
See `docs/jetson-el2-bringup.md` for the full boot chain.

**SMP:** 6 cores on-line via PSCI `CPU_ON` after the MPIDR encoding
bug was fixed (`docs/smp.md`). Cross-CPU dispatch validated via
`bench smp`. Secondary-core timer IRQ delivery is the same GIC
Group-config problem Pi 5 has (see below) — cooperative preemption
(`COOP_PREEMPT`) drives scheduler ticks at yield points on all 6
cores.

**GPU compute:** Blocked on GSP firmware loading. Detection and
BAR / MMIO access through the non-engine register space work from
EL2 (same path demonstrated on a discrete RTX 3050 in
`docs/nvidia-gsp.md`). The Orin Nano uses the same Ampere GSP
sequence; a bare-metal loader is out of capstone scope.

**Thesis framing:** `docs/capstone-thesis-framing.md` explains how
the GPU work splits into "infrastructure delivered" (memory, cache,
detection, enumeration) vs. "compute blocked by proprietary firmware"
for the capstone narrative.

**Open for future work:** See GitHub issue #142 ("Future work: GSP
bare-metal loader for Ampere / Orin").

---

## Related Documentation

### Project Documentation

- `docs/jetson-el2-bringup.md` — Current EL2 implementation (April 2026)
- `docs/jetson-boot.md` — Boot process overview
- `docs/jetson-tcu.md` — TCU/HSP architecture research
- `docs/platform-abstraction.md` — QEMU vs Jetson comparison
- `docs/gpu.md` — GPU integration (blocked by GSP firmware requirement)
- `docs/nvidia-gsp.md` — GSP boot sequence research (Ampere / GA10x / GA10B)
- `docs/capstone-thesis-framing.md` — Thesis narrative (Phase G6 draft)

### NVIDIA Forum Threads

- [kexec on the Jetson not working (CBB errors)](https://forums.developer.nvidia.com/t/kexec-on-the-jetson-not-working-cbb-errors/275207) — Official NVIDIA position on kexec
- [How to disable OEM firewall on Orin?](https://forums.developer.nvidia.com/t/how-to-disable-oem-firewall-on-orin/304878) — CBB firewall configuration attempts
- [Direct register access to Timer](https://forums.developer.nvidia.com/t/direct-register-access-to-timer/286298) — **EL2 success story**
- [Clarification on Jetson Orin Hypervisor Support](https://forums.developer.nvidia.com/t/clarification-on-jetson-orin-hypervisor-support-hardware-lock-or-only-unsupported/348348) — EL2 is software-disabled
- [Jetson Orin AGX UEFI secureboot payload manual signature](https://forums.developer.nvidia.com/t/jetson-orin-agx-uefi-secureboot-payload-manual-signature/291388) — Signing requirements
- [Secure boot UEFI clarifications](https://forums.developer.nvidia.com/t/secure-boot-uefi-clarifications/302602) — Secure boot chain details

### NVIDIA Official Documentation

- [Orin Series SoC Technical Reference Manual (TRM)](https://developer.nvidia.com/orin-series-soc-technical-reference-manual) — Requires developer login
- [Jetson Linux Developer Guide](https://docs.nvidia.com/jetson/archives/r36.4/DeveloperGuide/index.html) — L4T documentation
- [UEFI Adaptation Guide](https://docs.nvidia.com/jetson/archives/r36.2/DeveloperGuide/SD/Bootloader/UEFI.html) — UEFI customization
- [Jetson Security Documentation](https://docs.nvidia.com/jetson/archives/r36.4/DeveloperGuide/SD/Security/index.html) — Secure boot details

---

## Appendix: Chronological Investigation Summary

### Phase 1: Initial Boot Attempts (December 2025)

1. Implemented kexec boot support
2. Added spinlock bypass (exclusive monitor corruption after kexec)
3. Added watchdog disable (Linux starts 120s watchdog)
4. Changed UART to direct mode (assume clock enabled)
5. **Result:** No serial output, suspected clock issue

### Phase 2: Serial Hardware Verification (December 2025)

1. Set up USB-serial adapter on 40-pin header
2. Discovered baud rate mismatch (Jetson defaults to 9600)
3. Verified Linux-to-Linux serial works at 115200
4. **Result:** Serial hardware confirmed working

### Phase 3: UART Clock Investigation (December 2025)

1. Verified UART clock disabled via debugfs (count=0)
2. Opened `/dev/ttyTHS1` to enable clock (count=1)
3. Executed kexec with UART held open
4. **Result:** Got serial output! But it was CBB firewall errors.

### Phase 4: CBB Firewall Discovery (December 2025)

1. Analyzed RAS error output
2. Identified error address as UART (0x03100000)
3. Researched CBB on NVIDIA forums
4. Found NVIDIA stating kexec not supported
5. **Result:** Understood root cause is CBB firewall

### Phase 5: Alternative Boot Methods (December-January 2025-2026)

1. Tried extlinux.conf (same errors - L4T uses kexec internally)
2. Tried direct UEFI boot entry (same errors)
3. Tried UART silent mode (different error, still blocked)
4. **Result:** All boot methods blocked by CBB

### Phase 6: Forum Research (January 2026)

1. Found EL2 success story for direct register access
2. Found EL2 is software-disabled, not hardware-locked
3. Found secure boot signing requires full reflash
4. **Result:** Identified potential paths forward

---

*Document created: January 2026*
*Purpose: Consolidate all Jetson Orin blockers for NVIDIA support request*
