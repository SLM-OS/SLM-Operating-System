# Raspberry Pi 5 Bare-Metal Boot Status

**Date:** April 2, 2026
**Status:** INTERACTIVE SHELL — Full boot, shell accepts input. Timer/preemption disabled (workaround).

## Summary

SLM-OS boots reliably (100%) to a fully interactive shell on Pi 5 hardware. All kernel subsystems initialize successfully: PMM, VMM, GIC, SMP (single-core), IPC, VFS, LittleFS, Rust runtime, component system, and Lua scripting.

**UART RX is working** — the shell accepts input and responds to commands. Two RP1-specific GPIO pad configurations were required (OD=1, FUNCSEL sequencing). Timer interrupts and the armstub are currently disabled; preemptive scheduling is not active. See "Current Blocker" section below.

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
| VMM (MMU) | ✅ Working | Platform-specific mappings, all tests pass |
| GIC init | ✅ Working | GICv2 at 0x107FFF9000 |
| Timer init | ✅ Working | 54 MHz, 100 Hz tick |
| SDWireC deploy | ✅ Working | Automated flash/boot via sdwire CLI + labctl |
| Preemptive scheduler | ⚠️ Disabled | Timer IRQs break RP1 UART RX — see blocker |
| Shell prompt | ✅ Working | `slmos>` appears after full boot |
| UART RX (input) | ✅ Working | PL011 RX works when timer IRQs are disabled |

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

2. **Spinlocks:** ARM exclusive monitor operations hang. `SPINLOCK_SKIP_LOCKING` defined for Pi 5.

3. **Single-core:** Due to spinlock limitation, multi-core support not available.

4. **~~Timer IRQ hang~~** **RESOLVED** — Timer interrupts now work using the virtual timer (CNTV, IRQ 27) with armstub8-2712.bin configuring GIC groups from EL3.

5. **Timer interrupts break UART RX:** PL011 RX works correctly without timer interrupts but fails when the timer fires. Timer/preemption disabled as workaround. See "Current Blocker" section.

6. **~~Doubled early boot output / boot garbling:~~** **RESOLVED** — The armstub8-2712.bin caused ~60% of boots to produce garbled output or hang. Removing the armstub (renaming to `.disabled`) gives 100% reliable boot. The armstub's EL3→EL2 ERET intermittently left the system in a bad state affecting RP1 PCIe UART access. Since the armstub is only needed for GIC Group 1 configuration (which requires timer interrupts, currently disabled), it is not needed.

7. **DTB not preserved:** The armstub's `eret` to EL2 does not preserve the DTB pointer in x0. A scan for FDT magic in upper RAM was added but the garbled early output prevents confirmation. Kernel falls back to platform defaults.

8. **SDWireC compatibility:** Works with slower SD cards (29 GB tested). Faster UHS-I cards (SDR104) may fail through the SDWireC's analog MUX. The original SDWire (micro-USB) is incompatible with Pi 5.

## Pi 5 UART Architecture

| UART | Address | Access | Status |
|------|---------|--------|--------|
| RP1 UART0 | 0x1F00030000 | GPIO header pins 8/10 | **Working** |
| BCM2712 UART | 0x107D001000 | 3-pin JST connector | Not used (needs cable) |

The GPIO header UART uses the RP1 southbridge chip connected via PCIe. With proper config.txt settings, the firmware leaves this accessible for bare-metal code.

## Current Blocker: Timer Interrupts Break PL011 RX

**Symptom:** PL011 UART RX works correctly when timer interrupts are disabled, but stops receiving external data as soon as the timer starts firing (100 Hz). TX is unaffected.

**Current workaround:** Timer and IRQs are disabled on Pi 5 (`scheduler_start()` skips `timer_start()` and IRQ unmasking). The shell runs cooperatively with busy-wait polling. Preemptive scheduling is not active.

**What was ruled out:**
- Baud rate: confirmed 50 MHz clock is correct (48 MHz produces garbled output)
- GPIO15 configuration: FUNCSEL=4, IE=1, OD=1, PUE=1 — all correct
- RP1 signal path: STATUS register confirms INFROMPAD→INFILTERED→INTOPERI all connected
- PL011 hardware: loopback test passes (TX→RX internally, receives 0x55 correctly)
- MMU: RX works after MMU enable (before timer start)
- GIC init: RX works after GIC initialization
- IRQ unmasking: RX works with IRQs unmasked (no timer running)
- Context switching: RX fails even with `schedule()` removed from `scheduler_tick()`
- `yield()`: RX fails even with busy-wait (no yield/schedule calls)

**Root cause (narrowed):** The timer interrupt handler itself — the act of taking an IRQ exception, acknowledging the GIC, and returning — disrupts PL011 RX on the RP1. This appears to be an interaction between the GIC interrupt handling path (BCM2712 internal bus at 0x107FFF9000) and the RP1 PL011 (PCIe bus at 0x1F00030000). The exact mechanism is unknown.

**Investigation ideas for next session:**
1. Check if the timer IRQ acknowledge (GIC IAR read) or completion (GIC EOIR write) has a side effect on PCIe transactions
2. Try using a different timer source (e.g., RP1's own timer instead of ARM generic timer)
3. Try PL011 interrupt-driven RX instead of polling — the PL011 RX interrupt might work even if polling doesn't
4. Check if the RP1 PL011's IMSC (interrupt mask) register needs RX interrupts enabled for the FIFO to operate correctly when other interrupts are active

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
