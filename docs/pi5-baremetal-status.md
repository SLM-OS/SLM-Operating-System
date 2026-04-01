# Raspberry Pi 5 Bare-Metal Boot Status

**Date:** March 31, 2026
**Status:** BOOTS TO SHELL - Full boot with timer interrupts, shell prompt appears. RX input not working.

## Summary

SLM-OS boots to an interactive shell prompt (`slmos>`) on Pi 5 hardware. All kernel subsystems initialize successfully: PMM, VMM, GIC, timer (with preemptive scheduling), SMP (single-core), IPC, VFS, LittleFS, Rust runtime, component system, and Lua scripting. The armstub8-2712.bin configures GIC interrupt groups from EL3.

**Remaining issue:** UART RX (serial input) does not work — the PL011 RX FIFO never receives characters. TX output is fully functional. See "Current Blocker" section below.

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
| Serial RX | ✅ Working | Bit-banged via RIO |
| PMM (buddy) | ✅ Working | 4GB RAM detected and managed |
| VMM (MMU) | ✅ Working | Platform-specific mappings, all tests pass |
| GIC init | ✅ Working | GICv2 at 0x107FFF9000 |
| Timer init | ✅ Working | 54 MHz, 100 Hz tick |
| SDWireC deploy | ✅ Working | Automated flash/boot via sdwire CLI + labctl |
| Preemptive scheduler | ✅ Working | Timer interrupts, task switching |
| Shell prompt | ✅ Working | `slmos>` appears after full boot |
| UART RX (input) | ☐ Broken | PL011 FR_RXFE never clears — see blocker below |

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

5. **UART RX not working:** PL011 RX FIFO never receives characters (FR_RXFE always 1). The PL011 CR shows RXE=1, GPIO15 is configured for UART function with IE=1 and pull-up. TX works perfectly. Suspected baud rate mismatch: `uart_init()` sets IBRD=27/FBRD=8 (50 MHz clock assumption), but the actual RP1 UART clock may differ. TX is tolerant of small baud errors; RX is not. See "Current Blocker" section.

6. **Doubled early boot output:** Characters are doubled in the first few lines of boot output (before `uart_init()` re-configures the UART). Caused by the armstub's EL3→EL2 transition affecting the firmware's UART state. Cosmetic only.

7. **DTB not preserved:** The armstub's `eret` to EL2 does not preserve the DTB pointer in x0. A scan for FDT magic in upper RAM was added but the garbled early output prevents confirmation. Kernel falls back to platform defaults.

8. **SDWireC compatibility:** Works with slower SD cards (29 GB tested). Faster UHS-I cards (SDR104) may fail through the SDWireC's analog MUX. The original SDWire (micro-USB) is incompatible with Pi 5.

## Pi 5 UART Architecture

| UART | Address | Access | Status |
|------|---------|--------|--------|
| RP1 UART0 | 0x1F00030000 | GPIO header pins 8/10 | **Working** |
| BCM2712 UART | 0x107D001000 | 3-pin JST connector | Not used (needs cable) |

The GPIO header UART uses the RP1 southbridge chip connected via PCIe. With proper config.txt settings, the firmware leaves this accessible for bare-metal code.

## Current Blocker: UART RX Not Working

**Symptom:** The shell prompt (`slmos>`) appears but typing on the serial console produces no response. The PL011 flag register `FR_RXFE` (bit 4) never clears, indicating the RX FIFO is always empty.

**What works:** TX output is perfect — clean text at 115200 baud.

**Verified correct:**
- `CR=0x301` — UARTEN=1, TXE=1, RXE=1 (RX is enabled)
- GPIO15 pad: `0x5A` — IE=1 (input enabled), pull-up, schmitt trigger
- GPIO15 funcsel: 4 (UART function)
- PL011 registers accessible (no data abort on FR read)

**Suspected root cause:** Baud rate mismatch on RX. `uart_init()` configures IBRD=27/FBRD=8, assuming a 50 MHz UART clock. This produces 115108 baud (0.08% error). TX works because the receiver (PC) tolerates small errors. RX fails because the PL011 receiver is less tolerant of timing mismatches — even 2-3% error can cause framing errors that prevent data from entering the FIFO.

**Investigation needed:**
1. Determine the actual RP1 UART clock frequency. The firmware's original IBRD/FBRD values (before `uart_init()` overwrites them) would reveal this. Read IBRD/FBRD BEFORE `uart_init()` runs and log them.
2. Alternatively, try different IBRD/FBRD values:
   - 48 MHz clock: IBRD=26, FBRD=3
   - 44.2 MHz clock: IBRD=24, FBRD=0
3. Another approach: don't overwrite firmware's baud rate. But an empty `uart_init()` breaks TX (tested — no output). The firmware's UART state is lost during the armstub's EL3→EL2 transition.
4. The firmware might use a different clock for the PL011. Circle uses a 48 MHz reference. Try IBRD=26/FBRD=3 (48 MHz).
5. Check if the issue is electrical: try a different USB-serial adapter. The CH340 adapter might have RX wiring issues with the RP1 GPIO voltage levels.

**Workaround considered:** Revert to GPIO bit-banged RX, but this broke with caches enabled (timing loops run at wrong speed after MMU enable). Would need a timer-based bit-bang implementation.

## Driver Details

**File:** `kernel/drivers/uart_rp1_bitbang.c`

Despite the filename (historical), uses hardware PL011 for TX:
- TX: PL011 flag register polling (TXFF bit) — works after MMU maps RP1 as device memory
- RX: Bit-banged via GPIO RIO for reliability
- Init: Firmware initializes UART via `uart_2ndstage=1`; driver re-initializes with explicit baud rate config

**Baud rate:** 50MHz clock / (16 × 115200) = 27.127 → IBRD=27, FBRD=8

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
