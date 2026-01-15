# Raspberry Pi 5 Bare-Metal Boot Status

**Date:** January 14, 2026
**Status:** FULLY WORKING - Kernel boots with serial console

## Summary

Pi 5 bare-metal support is fully functional. The kernel boots to the shell with serial console output via GPIO header UART.

**Key achievements:**
1. EL2→EL1 transition for peripheral access
2. RP1 UART via firmware-initialized PCIe (no manual PCIe init required)
3. Serial console output at 115200 baud on GPIO14/15
4. Full kernel boot with shell prompt

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
| Serial TX | ✅ Working | RP1 UART0 via PL011 |
| Serial RX | ✅ Working | Bit-banged via RIO |
| Full kernel boot | ✅ Working | Banner, PMM, VMM, shell |

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

1. **UART flag register:** Reading from UART_FR causes data abort. Driver uses blind writes with delay.

2. **Spinlocks:** ARM exclusive monitor operations hang. `SPINLOCK_SKIP_LOCKING` defined for Pi 5.

3. **Single-core:** Due to spinlock limitation, multi-core support not available.

4. **Character corruption:** Occasional bit errors due to electrical noise (hardware issue, not software).

5. **SD-Wire incompatible:** Pi 5's SD interface doesn't work through SD-Wire device.

## Pi 5 UART Architecture

| UART | Address | Access | Status |
|------|---------|--------|--------|
| RP1 UART0 | 0x1F00030000 | GPIO header pins 8/10 | **Working** |
| BCM2712 UART | 0x107D001000 | 3-pin JST connector | Not used (needs cable) |

The GPIO header UART uses the RP1 southbridge chip connected via PCIe. With proper config.txt settings, the firmware leaves this accessible for bare-metal code.

## Driver Details

**File:** `kernel/drivers/uart_rp1_bitbang.c`

Despite the filename (historical), uses hardware PL011 for TX:
- TX: Write to UART DR with fixed delay (no flag polling)
- RX: Bit-banged via GPIO RIO for reliability

**Baud rate:** 50MHz clock / (16 × 115200) = 27.127 → IBRD=27, FBRD=8

## Build and Deploy

```bash
# Build for Pi 5
make kernel PLATFORM=RASPI5

# Copy to SD card
cp build/kernel/slmos.bin /path/to/sd/kernel_2712.img
```

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

### Avoided Complexity
Initially planned to port Circle's PCIe/RP1 initialization (~1500 lines of C). Discovery of `pciex4_reset=0` and `uart_2ndstage=1` config.txt options eliminated this need.

## References

- [Circle bare-metal framework](https://github.com/rsta2/circle)
- [Raspberry Pi config.txt documentation](https://www.raspberrypi.com/documentation/computers/config_txt.html)
- `docs/pi5-uart-testing-status.md` - Detailed UART investigation notes
- `docs/pi5-pe-header-analysis.md` - PE header issue analysis
