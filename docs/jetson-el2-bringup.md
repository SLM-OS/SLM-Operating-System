# Jetson Orin Nano: EL2 Bare-Metal Bringup

This document records the successful bypass of the Tegra234 CBB firewall by running SLM-OS at EL2 with VHE (Virtual Host Extensions). This unblocked serial output, GIC, timer, and scheduler — enough to boot to an interactive shell.

**Date:** April 2026
**Status:** ✅ Fully working — 6-core SMP with cross-CPU task dispatch, interactive shell with serial I/O

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
| GIC Redistributor | 0x0F440000+ | ✅ Works | 6 CPUs, dual-cluster layout (gap at 0x0F500000) |
| ARM Generic Timer | System regs | ✅ Works | 100 Hz tick confirmed |
| OP-TEE carveout | 0xC0000000+ | ❌ Blocked | Secure memory, kills core |
| Watchdog | 0x02190000 | ✅ Works | Disabled in kernel_main() |
| GPU (PMC regs) | 0x17000000 | ✅ Works | GA10B identified: BOOT_0=0xB7B000A1 |

---

## Memory Map

### Usable DRAM

```
0x80000000 ─────────── RAM base (kernel loaded here by kexec)
    │  .text, .data, .bss, stack
0x80437000 ─────────── Heap region 1 start
    │  Buddy allocator (~988 MB)
0xBDE00000 ─────────── NC shared memory (2 MB, Non-Cacheable)
    │  Cross-CPU boot flags, scheduler run queues
0xBE000000 ─────────── OP-TEE secure carveout (64 MB)
    │  OP-TEE binary at 0xC1D35000
0xC2000000 ─────────── Heap region 2 start
    │  Buddy allocator (~958 MB)
0xFFFE0000 ─────────── Small reserved gap
0x100000000 ────────── Heap region 3 start
    │  Buddy allocator (~5 GB)
0x240000000 ────────── Conservative end (reserved sub-regions above)
0x280000000 ────────── RAM end (8 GB total)
```

Total usable: ~6.9 GB across three regions plus 2 MB NC. Verified: `mem` command shows 6.7 GB free.

### OP-TEE Carveout

The OP-TEE (Trusted OS) binary is loaded at 0xC1D35000 during boot. The memory controller protects the region 0xBE000000-0xC1FFFFFF (64 MB). Writing to this region triggers a RAS error and kills the CPU core.

The PMM uses three non-contiguous regions around the carveout via the `pmm_add_region()` helper.

---

## Code Changes

### boot.S (Jetson path)
- Enable VHE: `msr hcr_el2, x10` with E2H=1, TGE=1, RW=1
- Disable stale Linux timers: clear CNTP_CTL, CNTV_CTL, CNTHP_CTL
- UARTC probe: write "EL2\r\n" as boot confirmation
- EFI boot detection: check x1 for EFI_SYSTEM_TABLE signature, call `efi_stub_entry()`
- Self-relocating trampoline: after ExitBootServices, copies image from UEFI load address to 0x80000000 (link address), flushes I-cache, jumps to copy. Skipped when already at link address.

### efi_stub.c
- `efi_stub_entry()`: finds DTB in EFI config table, calls ExitBootServices with retry
- `efi_disable_mmu()`: VHE-compatible — uses `sctlr_el1` (aliased to SCTLR_EL2 under VHE) and `tlbi vmalle1` instead of direct `sctlr_el2`/`tlbi alle2`

### kernel-jetson.ld
- `.data` section aligned to PE SectionAlignment (64KB) for UEFI compliance
- `__kernel_end` aligned to 64KB for PE SizeOfImage
- `.data` padded to PE FileAlignment (512 bytes)
- Build-time PE alignment assertions

### platform.h
- `UART_BASE` changed to `0x0C280000` (UARTC)
- `RAM_SIZE` = 8 GB (full)
- `UART_IRQ` updated for UARTC (SPI 114)

### uart_tegra.c
- Added `UART_INIT_MODE 3` (raw mode) — preserves firmware baud rate config

### pmm.c
- Refactored into `pmm_add_region()` helper for non-contiguous memory
- Three regions on Jetson: 0x80-0xBE (990 MB), 0xC2-0xFF (958 MB), 0x100-0x240 (5 GB)

### vmm.c
- Added `l2_ram_c0` table for 0xC0-0xFF range (skips OP-TEE carveout entries 0-15)
- Added L1 1GB block descriptors for indices 4-8 (0x100000000-0x23FFFFFFF)
- Capped `l2_kernel` at entry 496 (before OP-TEE carveout at 0xBE000000)

### smp.c
- 6-core SMP via PSCI CPU_ON with dual-cluster MPIDR table (0x000, 0x100, 0x200, 0x300, 0x10200, 0x10300)
- Boot flag PSCI success fallback (cache incoherency workaround)
- NC logical map for cross-CPU `cpu_logical_id()` lookup
- Hardcoded MPIDR table in `cpu_logical_id()` for Jetson (avoids all cache visibility issues)

### smp_boot.S
- VHE enable for secondary CPUs (same as primary: E2H=1, TGE=1, RW=1)
- L1/L2 cache invalidation by set/way after MMU enable (same as Pi 5)
- TLB invalidation before MMU enable

### gic.c
- Per-CPU GIC redistributor discovery with Jetson dual-cluster offset table
- `get_cpu_id()` uses `cpu_logical_id()` for proper MPIDR translation

### main.c
- Updated EL info message: "Running at EL2 (VHE)"

---

## Remaining Work

1. ~~**UART RX**~~ — **FIXED.** RX data arrives via TCU HSP mailbox (0x03C10000), not UARTC's RBR register. SPE firmware routes USB-C input to TOP0_HSP SM0. Reading the mailbox and unpacking 1-3 bytes per message gives clean bidirectional serial.
2. ~~**SMP**~~ — **FIXED.** 6 cores online via PSCI CPU_ON. Root cause was wrong MPIDR encoding — Jetson uses dual-cluster Aff2.Aff1 (0x000, 0x100, 0x200, 0x300, 0x10200, 0x10300), not contiguous Aff0. TF-A state is fine after kexec; the original diagnosis was incorrect. Boot flag visibility uses PSCI success fallback (same cache incoherency as Pi 5).
3. ~~**Memory above 0xC0000000**~~ — **DONE.** Three regions mapped: 0x80-0xBE, 0xC2-0xFF, 0x100-0x240. Total ~6.7 GB free.
4. ~~**GPU access**~~ — **DONE.** GPU at 0x17000000 accessible from EL2. GA10B identified (BOOT_0=0xB7B000A1). GSP firmware loading needed for compute.
5. **Direct UEFI boot** — WIP, deeper blockers than previously documented. Probed 2026-04-16 against jetson-nano-2 via UEFI Shell; full write-up in [docs/jetson-uefi-direct-result.md](jetson-uefi-direct-result.md). Summary: Shell's PE loader is the same as the boot manager's (approach C does not help). UEFI's load address varies boot-to-boot — sometimes `0x80000000`, sometimes high DRAM (e.g. `0x25DCC0000`), so approach A is fragile. Even when the image loads at its preferred address it crashes at offset `0x10070` (reported as `mov x0, x20`, which can't itself fault — likely stale ELR or pending trap). UARTC MMIO at `0x0C280000` faults from the EFI-application context both with UEFI's MMU active and after `efi_disable_mmu` returns, so UART-trace debugging is blocked until either UEFI `con_out` is wired up pre-EBS or the post-EBS UARTC path is diagnosed. Remaining path to D1 sequences as: (1) wire `con_out->output_string` into `efi_stub.c` for debug visibility, (2) diagnose why UARTC is unreachable post-`efi_disable_mmu`, (3) add a real `.reloc` section plus PIC early boot (approach B). Each gates the next; together they exceed the one-week budget, so team decision pending on whether to continue or pivot to Path 3 of #190.
6. ~~**Watchdog**~~ — **DONE.** Tegra WDT at 0x02190000 accessible from EL2 and disabled early in `kernel_main()`.
7. ~~**Cross-CPU task dispatch**~~ — **FIXED.** Root cause was UART lock deadlock: `DEBUG_PRINT` in `task_create()` acquired `uart_lock` while 5 secondary CPUs + CPU 0 all contended for it during boot. Fix: skip `DEBUG_PRINT` on secondary CPUs. `bench smp` dispatches tasks to all 6 CPUs (5/5 COMPLETED on every run, verified after shell_sys.c:816 slot-clear bound was corrected from `i < 4` to `i < cpu_count`; see issue #76). Cache maintenance (DC CVAC/CIVAC) now active on Jetson. Page tables flushed to DRAM before secondary boot.

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
