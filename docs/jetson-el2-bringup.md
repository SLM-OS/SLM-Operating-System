# Jetson Orin Nano: EL2 Bare-Metal Bringup

This document records the successful bypass of the Tegra234 CBB firewall by running SLM-OS at EL2 with VHE (Virtual Host Extensions). This unblocked serial output, GIC, timer, and scheduler — enough to boot to an interactive shell.

**Date:** April 2026
**Status:** 🟡 Partially working — interactive shell with serial I/O, SMP still needs work

---

## Background

SLM-OS bare-metal execution on Jetson was blocked by the CBB (Control Backbone) firewall since December 2025. All peripheral accesses from EL1 were rejected. See `docs/jetson-nvidia-support.md` for the full investigation history.

The breakthrough came from three key discoveries:

1. **SLM-OS enters at EL2 after kexec** — Linux runs at EL2 with VHE, and kexec preserves EL2 via `HVC_SOFT_RESTART`
2. **CBB firewall has per-peripheral permissions** — UARTC (0x0C280000) is accessible from EL2, while UARTA (0x03100000) is not
3. **VHE enables transparent EL2 operation** — setting `HCR_EL2.E2H=1` redirects EL1 register names to EL2, so all existing kernel code works unmodified

---

## How It Works

### Exception Level

After kexec from Linux:
- Linux runs at EL2 with VHE (`CPU: All CPU(s) started at EL2`, `VHE mode initialized successfully`)
- kexec's `HVC_SOFT_RESTART` jumps to SLM-OS still at EL2
- Confirmed via PSCI probe: `SYSTEM_OFF` (EL2 path) executed, Jetson powered down

### VHE (Virtual Host Extensions)

Setting `HCR_EL2.E2H=1, TGE=1, RW=1` in boot.S enables VHE:

```
HCR_EL2 = (1 << 34) | (1 << 31) | (1 << 27)
         = E2H       | RW        | TGE
```

With VHE, all EL1 system register accesses are transparently redirected to their EL2 equivalents:

| Write to | Actually accesses |
|----------|-------------------|
| SCTLR_EL1 | SCTLR_EL2 |
| VBAR_EL1 | VBAR_EL2 |
| TTBR0_EL1 | TTBR0_EL2 |
| TTBR1_EL1 | TTBR1_EL2 |
| TCR_EL1 | TCR_EL2 |
| MAIR_EL1 | MAIR_EL2 |
| CPACR_EL1 | CPTR_EL2 (CPACR format) |

This means the entire kernel (MMU, exceptions, page tables) works without code changes.

### UARTC Instead of UARTA

The CBB firewall blocks UARTA (0x03100000) from EL2 but allows UARTC (0x0C280000). UARTC output is routed through the TCU (Tegra Combined UART) and appears on the USB-C debug serial console.

UART initialization uses "raw mode" (UART_INIT_MODE 3) — the firmware's baud rate configuration is preserved rather than reconfigured, since the UARTC clock frequency is unknown.

---

## Verification Tests Performed

### EL2 Confirmation (PSCI Probe)

Modified boot.S to read `CurrentEL` and signal via PSCI:
- EL2 → `PSCI_SYSTEM_OFF` (Jetson powers down)
- EL1 → `PSCI_SYSTEM_RESET` (Jetson reboots)

**Result:** Jetson powered down → confirmed EL2.

### UARTC Access Test

Wrote "UARTC-OK" directly to UARTC (0x0C280000) from boot.S before any kernel init.

**Result:** `UARTC-OK` appeared on serial console. No CBB error.

### UARTA Access Test

Wrote 'A' to UARTA (0x03100000) from EL2.

**Result:** CBB RAS error, core killed. UARTA is blocked even from EL2.

### Full Boot Test

Boot sequence: kexec → EL2/VHE → UARTC → DTB → PMM → VMM/MMU → GICv3 → Timer → Scheduler → Shell.

**Result:** Full boot to `slmos>` prompt. All subsystems functional.

### QEMU Regression Test

All changes are `#ifdef PLATFORM_JETSON_ORIN_NANO` guarded.

**Result:** `make test` passes — all existing tests unaffected.

### Pi 5 Build Verification

**Result:** `make kernel PLATFORM=RASPI5` builds cleanly.

---

## CBB Firewall Peripheral Map (EL2)

| Peripheral | Address | EL2 Access | Notes |
|------------|---------|------------|-------|
| UARTA | 0x03100000 | ❌ Blocked | 40-pin header UART |
| UARTC (TX) | 0x0C280000 | ✅ Works | Via TCU to USB-C debug |
| TCU RX Mailbox | 0x03C10000 | ✅ Works | HSP SM0, SPE routes USB-C input here |
| GIC Distributor | 0x0F400000 | ✅ Works | GICv3, 992 interrupt lines |
| GIC Redistributor | 0x0F440000 | ✅ Works | Per-CPU, CPU 0 awake |
| ARM Generic Timer | System regs | ✅ Works | 100 Hz tick confirmed |
| OP-TEE carveout | 0xC0000000+ | ❌ Blocked | Secure memory, kills core |
| Watchdog | 0x02190000 | Not tested | Disabled via timer clear |
| GPU | 0x17000000 | Not tested | Future work |

---

## Memory Map

### Usable DRAM

```
0x80000000 ─────────── RAM base (kernel loaded here by kexec)
    │  .text, .data, .bss, stack
0x80437000 ─────────── Heap start (__kernel_end, page-aligned)
    │  Buddy allocator free memory (~1 GB)
0xC0000000 ─────────── OP-TEE secure carveout (heap capped here)
    │  OP-TEE binary at 0xC1D35000
    │  ...firmware carveouts...
0x280000000 ────────── RAM end (8 GB total)
```

### OP-TEE Carveout

The OP-TEE (Trusted OS) binary is loaded at 0xC1D35000 during boot. The memory controller protects a region starting at approximately 0xC0000000. Writing to this region triggers a RAS error and kills the CPU core.

The PMM caps the heap at 0xC0000000, providing ~1 GB of usable memory for the buddy allocator.

---

## Code Changes

### boot.S (Jetson path)
- Enable VHE: `msr hcr_el2, x10` with E2H=1, TGE=1, RW=1
- Disable stale Linux timers: clear CNTP_CTL, CNTV_CTL, CNTHP_CTL
- UARTC probe: write "EL2\r\n" as boot confirmation

### platform.h
- `UART_BASE` changed to `0x0C280000` (UARTC)
- `RAM_SIZE` = 8 GB (full)
- `UART_IRQ` updated for UARTC (SPI 114)

### uart_tegra.c
- Added `UART_INIT_MODE 3` (raw mode) — preserves firmware baud rate config

### pmm.c
- OP-TEE carveout cap: `heap_end = min(heap_end, 0xC0000000)`

### smp.c
- Skip secondary CPU boot on Jetson (PSCI CPU_ON after kexec unreliable)

### main.c
- Updated EL info message: "Running at EL2 (VHE)"

---

## Remaining Work

1. ~~**UART RX**~~ — **FIXED.** RX data arrives via TCU HSP mailbox (0x03C10000), not UARTC's RBR register. SPE firmware routes USB-C input to TOP0_HSP SM0. Reading the mailbox and unpacking 1-3 bytes per message gives clean bidirectional serial.
2. **SMP** — Secondary CPUs via PSCI CPU_ON after kexec. May need to investigate CPU state after kexec.
3. **Memory above 0xC0000000** — Could potentially use DRAM above the OP-TEE carveout (0xC2000000+) by parsing actual carveout boundaries.
4. **GPU access** — GPU at 0x17000000 not yet tested from EL2. Would unlock AI inference on Jetson's 1024-core Ampere GPU.
5. **Direct UEFI boot** — Avoiding kexec would give cleaner state (no stale Linux config, no watchdog).

---

## Recovery Procedures

### After CBB crash (core killed)
```bash
labctl power cycle jetson-nano-2 --delay 10
# Wait ~45s for Linux to boot
```

### After PSCI SYSTEM_OFF (full power down)
```bash
labctl power off jetson-nano-2
# Wait 30 seconds for PMIC capacitor drain
labctl power on jetson-nano-2
```

---

*Created: 5 April 2026*
