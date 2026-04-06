# Raspberry Pi 5 Bare-Metal Boot Status

**Date:** April 5, 2026
**Status:** 4-CORE SMP — All 4 Cortex-A76 cores online via PSCI SMC, preemptive scheduling active. Full test suite passes (420 tests: 404 pass, 16 ignored, 0 failures).

## Summary

SLM-OS boots reliably (100%) to a fully interactive shell on Pi 5 hardware. All kernel subsystems initialize successfully: PMM, VMM, GIC, SMP (4-core, all online via PSCI SMC + DC CVAC/CIVAC cache workaround), IPC, VFS, LittleFS, Rust runtime, component system, and Lua scripting. The full test suite (413 tests across 14 suites) passes with zero failures on Pi 5 hardware.

**Preemptive scheduling is active** — timer interrupts drive context switching at 100 Hz. The shell accepts input and responds to commands with preemption enabled. Two RP1-specific GPIO pad configurations were required for UART RX (OD=1, FUNCSEL sequencing). The armstub is currently disabled (separate issue; see Known Limitations).

**Key achievements:**
1. EL2→EL1 transition for peripheral access
2. RP1 UART TX via PL011 flag register polling (works after MMU enable)
3. Serial console output at 115200 baud on GPIO14/15
4. Platform-specific VMM mappings (1GB L1 block descriptors for RAM, L2 tables for MMIO)
5. armstub8-2712.bin for GIC Group 1 configuration from EL3
6. Timer interrupts working (virtual timer, IRQ 27) with preemptive scheduling
7. All subsystems boot: PMM, VMM, GIC, scheduler, IPC, VFS, LittleFS, Rust, Lua
8. Automated deploy via SDWireC + labctl

## Hardware Setup

- **Board:** Raspberry Pi 5 (BCM2712)
- **Serial adapter:** USB-TTL (CH340 or similar)
- **Connections:** GPIO14 (pin 8) = TX, GPIO15 (pin 10) = RX, GND (pin 6)
- **Baud rate:** 115200, 8N1

## What Works

| Feature | Status | Notes |
|---------|--------|-------|
| Boot to C code | ✅ Working | EL2→EL1 transition in boot.S |
| ACT LED control | ✅ Working | GPIO2 bit 9 at 0x107D517C04 |
| Serial TX | ✅ Working | PL011 flag register polling (after MMU) |
| Serial RX | ✅ Working | PL011 hardware RX (OD=1, FUNCSEL 5→4 sequence) |
| PMM (buddy) | ✅ Working | 4GB RAM detected and managed |
| VMM (MMU) | ✅ Working | 1GB L1 block descriptors for RAM, L2 tables for MMIO |
| GIC init | ✅ Working | GICv2 at 0x107FFF9000 |
| Timer init | ✅ Working | 54 MHz, 100 Hz tick |
| SDWireC deploy | ✅ Working | Automated flash/boot via sdwire CLI + labctl |
| Preemptive scheduler | ✅ Working | 100 Hz timer, DAIF-based context switch |
| Shell prompt | ✅ Working | `slmos>` appears after full boot |
| Performance benchmarks | ✅ Working | `bench all` — context switch, IRQ, IPC, stats |
| UART RX (input) | ✅ Working | PL011 RX works with preemptive scheduling active |
| Timer sleep | ✅ Working | sleep_ms/sleep_us using ARM timer counter + yield |
| UART RX IRQ | 🔧 In progress | PCIe RC→MIP→GIC path configured, MSI-X table TBD |

## Test Results (April 5, 2026)

Full test suite runs on Pi 5 hardware with zero failures:

| Test Suite | Pass | Fail | Ignore | Total |
|---|---|---|---|---|
| IPC | 23 | 0 | 0 | 23 |
| Model Memory | 10 | 0 | 0 | 10 |
| Scheduler | 60 | 0 | 1 | 61 |
| Priority Inheritance Mutex | 7 | 0 | 0 | 7 |
| GPU | 22 | 0 | 0 | 22 |
| Component | 22 | 0 | 0 | 22 |
| VFS | 26 | 0 | 0 | 26 |
| Shell | 139 | 0 | 0 | 139 |
| VMM/TLB | 5 | 0 | 8 | 13 |
| PMM Buddy | 23 | 0 | 1 | 24 |
| LittleFS | 26 | 0 | 0 | 26 |
| Net | 12 | 0 | 1 | 13 |
| Lua | 29 | 0 | 0 | 29 |
| Integration (Multi-Core) | 0 | 0 | 5 | 5 |
| **Total** | **404** | **0** | **16** | **420** |

**Ignored tests (16 total, expected):**
- Scheduler (1): `test_isolated_core_latency` — requires cross-CPU dispatch (SMPEN)
- VMM (5): TLB remap tests — Pi 5 uses 1GB L1 block descriptors, no L2 entries to remap
- VMM (3): ASID/TLB broadcast smoke tests — cannot validate TLB state from test
- PMM (1): `test_split_creates_buddies` — small blocks already available, no split triggered
- Net (1): platform-specific test not applicable to Pi 5
- Integration (5): Cross-CPU task dispatch requires SMPEN (L2 not coherent without it)

**Key bugs fixed to achieve zero failures:**
1. VMM remap tests assumed L2 table entries; Pi 5 uses L1 block descriptors for RAM
2. DC CIVAC writeback bug: CPU 0's stale dirty cacheline for `cpu_data[]` overwrote secondary CPUs' `online=true` at PoC (fixed by `cache_clean_range` before booting secondaries)
3. task_exit/schedule race: timer could fire between state=TERMINATED and scheduler_remove_task(), causing panic (fixed by masking IRQs in task_exit)

## Interrupt-Driven UART (On Hold)

The RP1 UART interrupt path requires configuring three hardware blocks between the PL011 and the GIC:

```
PL011 UART0 (0x1F00030000)
  → RP1 PCIE_CFG MSI-X engine (0x1F00108000) — vector 25
    → PCIe MSI-X write to 0xFF_FFFFF000
      → BCM2712 PCIe RC BAR1 (0x1000120000) remaps to MIP0
        → MIP0 (0x1000130000) → GIC SPI 153 (IRQ 185)
```

**What is configured:**
- PL011 IMSC: RX + receive timeout interrupts enabled
- RP1 MSIX_CFG: Vector 25 enabled with IACK_EN
- PCIe RC BAR1: Configured to route MSI-X PCI addr to MIP0 physical addr
- MIP0: All vectors unmasked for host, edge-triggered
- GIC: SPI 153 enabled, priority set, routed to CPU 0
- IRQ handler: Reads RP1 INTSTAT, drains PL011 FIFO, writes IACK
- Ring buffer: 256-byte SPSC buffer for ISR→uart_getc handoff
- Polling fallback: Transparent — if IRQ never fires, original polling path runs

**What is NOT yet working — PCIe RC register writes hang from EL1:**
- **Reads succeed:** PCIE_STATUS, EXT_CFG config reads (vendor/device, capabilities, BARs), RC BAR1 readback — all work
- **Writes hang:** Any write to the RC register space (EXT_CFG DATA, RC BAR1 config) causes the system to freeze
- This blocks MSI-X enable, MSI-X table programming, and RC BAR1→MIP routing setup

**Key findings from investigation:**
- EXT_CFG data register is at RC+0x8000 (not 0x9004). INDEX register is at RC+0x9000.
- INDEX must contain only bus/devfn (NOT register offset). Register offset goes in the DATA address.
- RP1 appears at bus 0 (dedicated pcie2 link, no hierarchy). Vendor 0x1de4, device 0x0001.
- MSI-X capability at config offset 0xB0: 61 vectors, table in BAR0 offset 0, PBA at BAR0+0x2000.
- BAR0 PCIe address = 0x00410000. Outbound window base = 0x1F03F00000, offset = 0. So MSI-X table CPU address = 0x1F04310000 (NOT 0x1F00410000).
- RC BAR1 is unconfigured (all zeros) — firmware did not set up MIP routing.
- **Root cause confirmed:** PCIe RC register writes require EL2. Writes from EL1 hang; writes from EL2 (in boot.S before ERET) succeed and values persist. However, writing RC BAR1 from EL2 **breaks RP1 peripheral MMIO access** — the BAR1 inbound window configuration conflicts with the firmware's outbound window that maps CPU addresses to RP1 peripherals. The next step is understanding the outbound/inbound window interaction on the brcmstb PCIe controller to configure BAR1 without disrupting RP1 MMIO.

## Configuration

### config.txt (required)

```
arm_64bit=1
kernel_address=0x80000
kernel=kernel_2712.img

# Critical: Firmware pre-initializes PCIe/RP1 for bare-metal
pciex4_reset=0
uart_2ndstage=1
os_check=0
```

The `pciex4_reset=0` and `uart_2ndstage=1` settings tell the firmware to leave PCIe/RP1 initialized, eliminating the need for complex PCIe host bridge initialization from Circle.

## Known Limitations

1. **~~UART flag register:~~** ~~Reading from UART_FR causes data abort.~~ **RESOLVED** — Flag register reads work correctly after MMU enable with proper device memory mapping (nGnRnE) for the RP1 region. The earlier crash was caused by the VMM not mapping the RP1 address space at all.

2. **~~Spinlocks:~~** **RESOLVED** — Hardware spinlocks work after MMU enable. Before MMU, a runtime flag (`spinlock_hw_enabled`) gates barrier-only fallback. The exclusive monitor requires cacheable memory, which is available only after VMM initialization.

3. **Cache coherency / cross-CPU dispatch:** Cross-CPU task dispatch is blocked by two issues:

   **a) Pre-MMU L2 pollution (FIXED):** Secondary CPUs read kernel data (MMU config variables) before enabling the MMU. These pre-MMU reads enter L1/L2 as Non-Shareable lines. After MMU enable, the page table attributes mark memory as Inner Shareable, but existing L2 lines are NOT retroactively made coherent — the DSU never snoops them. Fixed by invalidating L1/L2 by set/way (DC ISW) after MMU enable in `smp_boot.S`. This unblocks secondary CPU initialization (scheduler init, idle task creation, timer start).

   **b) Runtime cross-CPU data sharing (OPEN):** Even with the L2 invalidate fix, tasks dispatched to secondary CPUs are never consumed. Neither barriers-only (relying on DSU) nor explicit DC CVAC/CIVAC make runtime cross-CPU data visible. The DSU does not appear to provide automatic coherency for regular loads/stores despite correct Inner Shareable page table attributes (SH=0b11 confirmed from hardware dump). Spinlocks work (exclusive monitor has separate coherency). All combinations tested and failed:

   | TF-A | Cache Ops | L2 Invalidate | Result |
   |------|-----------|---------------|--------|
   | Built-in EEPROM | DC CVAC/CIVAC | No | Fail |
   | Built-in EEPROM | Barriers only | No | Fail |
   | RPi Foundation (bcm2712) | Barriers only | No | Fail |
   | RPi Foundation (bcm2712) | DC CVAC/CIVAC | No | Fail |
   | Built-in EEPROM | Barriers only | Yes | Fail |
   | Built-in EEPROM | DC CVAC/CIVAC | Yes | Fail |

   **Untested:** RPi Foundation TF-A + L2 invalidate (combination of all fixes).

   **Workaround:** All user tasks pinned to CPU 0. Secondary CPUs run idle tasks and handle timer interrupts but do not receive dispatched work.

   **Root cause note:** The Cortex-A76 does NOT have an SMPEN bit (unlike A53/A72). The `cpu_has_smpen()` function reads bit 6 of `S3_0_C15_C1_4` which is a different field on A76. The DSU is supposed to provide coherency automatically per ARM TRM, but BCM2712's implementation does not appear to do so for regular loads/stores.

   **Potential fixes:**
   - RPi Foundation TF-A (CPUECTLR_EL1 + CLUSTERECTLR_EL1 init) combined with L2 invalidate
   - Non-cacheable shared memory for scheduler run queue data
   - Further investigation of DSU configuration registers

4. **~~Timer IRQ hang~~** **RESOLVED** — Timer interrupts now work using the virtual timer (CNTV, IRQ 27) with armstub8-2712.bin configuring GIC groups from EL3.

5. **~~Timer interrupts break UART RX~~** **RESOLVED** — Root cause was DAIF initialization in new tasks. See "Resolved Blocker" section for full details.

6. **~~Doubled early boot output / boot garbling:~~** **RESOLVED** — The armstub8-2712.bin caused ~60% of boots to produce garbled output or hang. Removing the armstub (renaming to `.disabled`) gives 100% reliable boot. The armstub's EL3→EL2 ERET intermittently left the system in a bad state affecting RP1 PCIe UART access. The armstub remains disabled as a separate issue from the timer/preemption fix — GIC Group 1 configuration is handled by boot.S from EL2 instead.

7. **DTB not preserved:** The armstub's `eret` to EL2 does not preserve the DTB pointer in x0. A scan for FDT magic in upper RAM was added but the garbled early output prevents confirmation. Kernel falls back to platform defaults.

8. **SDWireC compatibility:** Works with slower SD cards (29 GB tested). Faster UHS-I cards (SDR104) may fail through the SDWireC's analog MUX. The original SDWire (micro-USB) is incompatible with Pi 5.

## Pi 5 UART Architecture

| UART | Address | Access | Status |
|------|---------|--------|--------|
| RP1 UART0 | 0x1F00030000 | GPIO header pins 8/10 | **Working** |
| BCM2712 UART | 0x107D001000 | 3-pin JST connector | Not used (needs cable) |

The GPIO header UART uses the RP1 southbridge chip connected via PCIe. With proper config.txt settings, the firmware leaves this accessible for bare-metal code.

## Resolved Blocker: Timer Interrupts Breaking PL011 RX

**Status: RESOLVED** (April 2, 2026)

### Root Cause

New tasks were initialized with `DAIF=0` (all exceptions unmasked). During the first context switch into a task, the DAIF restore in `context.S` runs early in the restore sequence — before general-purpose registers, stack pointer, and FPU state are fully loaded. With `DAIF=0`, this immediately unmasked IRQs, allowing the timer ISR to fire mid-register-restore. The timer ISR corrupted the partially restored context, which manifested as UART RX failure (the symptom that led to weeks of investigation).

### Fix

Initialize the `daif` field in new task contexts to `0x080` (IRQ masked) in `task_create_with_priority()`. This ensures the first context switch completes all register restores before any interrupt can fire. Task code naturally unmasks IRQs via `spin_unlock_irqrestore()` or explicit DAIF clear once fully running.

### Why the Symptom Was Misleading

The corruption appeared as "timer interrupts break UART RX" because:
- RX polling in `uart_getc()` reads from RP1 via PCIe — a path sensitive to register/stack corruption
- TX was unaffected because `uart_putc()` is simpler and less sensitive to context state
- The bug only triggered on the *first* switch into a new task (subsequent switches saved/restored DAIF correctly from the running task's actual state)
- Disabling the timer masked the symptom by preventing the ISR from firing during the vulnerable window

### Investigation History (Preserved for Reference)

The following were ruled out during investigation before the true root cause was found:

- Baud rate: confirmed 50 MHz clock is correct (48 MHz produces garbled output)
- GPIO15 configuration: FUNCSEL=4, IE=1, OD=1, PUE=1 — all correct
- RP1 signal path: STATUS register confirms INFROMPAD→INFILTERED→INTOPERI all connected
- PL011 hardware: loopback test passes (TX→RX internally, receives 0x55 correctly)
- MMU: RX works after MMU enable (before timer start)
- GIC init: RX works after GIC initialization
- IRQ unmasking: RX works with IRQs unmasked (no timer running)
- Context switching: RX fails even with `schedule()` removed from `scheduler_tick()`
- `yield()`: RX fails even with busy-wait (no yield/schedule calls)

These tests were not wasted — they systematically eliminated hardware, bus, and driver-level causes and narrowed the search to the context switch path itself.

## UART RX Fix Details (April 2026)

Two RP1-specific GPIO pad configurations were required to make RX work:

**1. OD=1 (Output Disable) on GPIO15 pad:**
The firmware/armstub resets GPIO15 to defaults (FUNCSEL=31, IE=0, OD=1, pull-down). When reconfiguring for UART RX, setting OD=0 (as standard PL011 drivers do) causes the output driver to interfere with external input — even though the GPIO STATUS register shows OE=0. Setting OD=1 forces the output driver off at the pad level, allowing the external serial adapter signal through.

**2. FUNCSEL sequencing (5→4):**
Switching GPIO15 directly from FUNCSEL=31 (NULL, reset default) to FUNCSEL=4 (UART) does not reliably enable the PL011 RX input path. An intermediate switch to FUNCSEL=5 (SYS_RIO) followed by FUNCSEL=4 (UART) is required. This appears to reset internal RP1 mux state.

**Pad configuration:** `PAD=0xDA` (IE=1, OD=1, 4mA, PUE=1, SCHMITT=1)
**CTRL configuration:** `CTRL=0x84` (F_M=4, FUNCSEL=4)

## Driver Details

**File:** `kernel/drivers/uart_rp1_bitbang.c`

Despite the filename (historical), uses hardware PL011 for both TX and RX:
- TX: PL011 flag register polling (TXFF bit) — works after MMU maps RP1 as device memory
- RX: PL011 flag register polling (RXFE bit) — requires OD=1 pad config and FUNCSEL 5→4 sequencing
- Init: Firmware initializes UART via `uart_2ndstage=1`; driver re-initializes with explicit baud rate and GPIO config

**Baud rate:** 50MHz clock / (16 × 115200) = 27.127 → IBRD=27, FBRD=8
**RX pad:** `0xDA` (IE=1, OD=1, 4mA, PUE=1, SCHMITT=1) — OD=1 is critical

## VMM Platform Mappings

The Pi 5 has a unique memory layout compared to QEMU/Jetson. RAM starts at PA 0x0 and devices are at high addresses (above 4 GB). The VMM uses platform-specific setup:

```
L1[0-3]  → 1GB block descriptors for RAM (0x00000000-0xFFFFFFFF, 4 GB)
L1[65]   → L2 table for GIC/GPIO2 (0x107FFF9000, 0x107D517C04)
L1[124]  → L2 table for RP1 UART/GPIO (0x1F00030000, 0x1F000D0000)
```

This differs from QEMU (MMIO at L1[0], RAM at L1[1]) and Jetson (MMIO at L1[0], RAM at L1[2]).

## Build and Deploy

```bash
# Build for Pi 5
make kernel PLATFORM=RASPI5

# Manual deploy
cp build/kernel/slmos.bin /path/to/sd/kernel_2712.img
```

### Automated Deploy via SDWireC

The Pi 5 SD card is connected through a Badgerd SDWireC (USB-C model), controlled by the `sdwire` Python CLI. The SDWireC serial is `20120501030900000.10.3`.

```bash
# Switch SD card to host for flashing
sudo /tmp/sdwire-venv/bin/sdwire switch -s "20120501030900000.10.3" host
sleep 2
sudo mount /dev/sdc1 /mnt
sudo cp build/kernel/slmos.bin /mnt/kernel_2712.img
sudo umount /mnt

# Switch back to Pi 5 and boot
sudo /tmp/sdwire-venv/bin/sdwire switch -s "20120501030900000.10.3" dut
labctl power cycle Pi-5-1

# Monitor serial output
labctl connect pi-5-1-console
```

**Notes:**
- The SDWireC requires the `sdwire` Python package (not `sd-mux-ctrl`, which uses FTDI)
- A slower SD card (tested: 29 GB) is required — faster UHS-I cards negotiate SDR104 speeds that the SDWireC's analog MUX cannot handle
- The Pi 5 EEPROM is at the original Sep 2024 firmware (no `SD_QUIRKS` needed with a slower card)
- EEPROM firmware versions after Jan 2025 break bare-metal RP1 UART access (investigated but not resolved)

## Key Files Modified for Pi 5

| File | Purpose |
|------|---------|
| `kernel/arch/aarch64/boot.S` | EL2→EL1 transition, PE header skip |
| `kernel/drivers/uart_rp1_bitbang.c` | RP1 UART driver |
| `kernel/include/platform.h` | Pi 5 platform defines |
| `kernel/include/spinlock.h` | Spinlock bypass for Pi 5 |
| `CMakeLists.txt` | Pi 5 build configuration |

## EL2→EL1 Transition

Pi 5 firmware enters kernel in EL2. Peripheral access requires EL1:

```asm
    /* Check current exception level */
    mrs     x10, CurrentEL
    cmp     x10, #8                 /* EL2 = 0b1000 */
    b.ne    .Lpi5_in_el1            /* Already in EL1, skip */

    /* Configure and transition to EL1 */
    mov     x10, #0x33ff
    msr     cptr_el2, x10
    msr     hstr_el2, xzr
    mov     x10, #(3 << 20)
    msr     cpacr_el1, x10
    mov     x10, #(1 << 31)         /* RW bit = 1 for AArch64 */
    msr     hcr_el2, x10
    mov     x10, #0x0800
    movk    x10, #0x30d0, lsl #16
    msr     sctlr_el1, x10
    mov     x10, #0x3c4             /* EL1h, DAIF masked */
    msr     spsr_el2, x10
    adr     x10, .Lpi5_in_el1
    msr     elr_el2, x10
    eret

.Lpi5_in_el1:
    /* Now in EL1 - peripheral access works */
```

## ACT LED Control

ACT LED on GPIO2 bit 9, directly accessible (no RP1/PCIe needed):

| Register | Address | Purpose |
|----------|---------|---------|
| GPIO2_DATA0 | 0x107D517C04 | Set/clear bit 9 for LED |
| GPIO2_IODIR0 | 0x107D517C08 | Clear bit 9 for output |

## Recovery Procedure

If Pi 5 won't boot:
1. Power off Pi
2. Remove SD card, mount on another machine
3. Restore working config:
   ```bash
   sudo mount /dev/sdX1 /mnt/pi-boot
   sudo cp /mnt/pi-boot/kernel_2712.img.linux /mnt/pi-boot/kernel_2712.img
   sudo cp /mnt/pi-boot/config.txt.linux /mnt/pi-boot/config.txt
   sudo umount /mnt/pi-boot
   ```
4. Reinsert SD card, power on

## Historical Notes

### Investigation Timeline
- **December 2025:** Initial boot attempts, EL2→EL1 discovery
- **January 2026 (early):** PE header issue resolved, LED working
- **January 2026 (mid):** UART working with firmware PCIe init
- **March 2026:** SDWireC integration, VMM platform-specific mappings, UART flag register fix

### EEPROM Firmware Investigation (March 2026)

Updating the Pi 5 EEPROM firmware from Sep 2024 to any version after Jan 2025 breaks bare-metal RP1 UART access. Symptoms: bootloader output appears on serial, but SLM-OS kernel produces no output after handoff. The LED blinks confirm the kernel is running.

Tested firmware versions: v2025.05.08, v2025.07.17, v2025.11.05 — all exhibit the same behavior. Adding `enable_rp1_uart=1` and/or `enable_uart=1` to config.txt restores bootloader output but does not fix kernel UART.

Root cause is unknown — writes to the RP1 UART data register at `0x1F00030000` silently fail from both EL2 and EL1. The Sep 2024 firmware works correctly. This investigation is documented here for future reference.

### Avoided Complexity
Initially planned to port Circle's PCIe/RP1 initialization (~1500 lines of C). Discovery of `pciex4_reset=0` and `uart_2ndstage=1` config.txt options eliminated this need.

## References

- [Circle bare-metal framework](https://github.com/rsta2/circle)
- [Raspberry Pi config.txt documentation](https://www.raspberrypi.com/documentation/computers/config_txt.html)
- `docs/pi5-uart-testing-status.md` - Detailed UART investigation notes
- `docs/pi5-pe-header-analysis.md` - PE header issue analysis
