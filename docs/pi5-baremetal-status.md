# Raspberry Pi 5 Bare-Metal Boot Status

**Date:** January 1, 2026
**Status:** WORKING - Full kernel boots to C code

## Summary

Pi 5 bare-metal boot is now working. Key discoveries:
1. Pi 5 firmware enters kernel in EL2; peripheral access requires EL1 transition
2. PE header stripping broke symbol offset calculations (root cause identified and fixed)

The solution uses conditional compilation to skip the PE header for RASPI5 builds, producing a raw binary that loads correctly at 0x80000.

## Hardware Setup

- **Board:** Raspberry Pi 5 (BCM2712)
- **Serial adapter:** USB-TTL connected to 40-pin GPIO header pins 8/10
- **Serial port on Ubuntu host:** `/dev/ttyUSB2`
- **Baud rate:** 115200
- **Pi 5 IP (when running Linux):** 192.168.4.91
- **Credentials:** pi/raspberry

## What Works

1. **Circle bare-metal framework** - LED blink sample runs successfully
2. **Minimal SLM-OS LED test** - Blinks 5 times with EL2→EL1 transition
3. **Code execution at 0x80000** - Confirmed via infinite loop test (Pi hangs as expected)
4. **Linux boots successfully** with serial console on GPIO header

## Key Discovery: EL2→EL1 Transition Required

Pi 5 firmware enters the kernel in **EL2** (Exception Level 2). GPIO2 peripheral access fails silently when in EL2. The solution is to transition to EL1 before any peripheral access, following Circle's approach.

### Working EL2→EL1 Transition Code

```asm
    /* Check current exception level */
    mrs     x10, CurrentEL
    cmp     x10, #8                 /* EL2 = 0b1000 */
    b.ne    .Lpi5_in_el1            /* Already in EL1, skip */

    /* We're in EL2 - configure and transition to EL1 */

    /* Disable coprocessor traps to EL2 */
    mov     x10, #0x33ff
    msr     cptr_el2, x10
    msr     hstr_el2, xzr

    /* Enable FP/SIMD at EL1 */
    mov     x10, #(3 << 20)
    msr     cpacr_el1, x10

    /* Initialize HCR_EL2 for 64-bit EL1 */
    mov     x10, #(1 << 31)         /* RW bit = 1 for AArch64 */
    msr     hcr_el2, x10

    /* SCTLR_EL1 - safe initial value */
    mov     x10, #0x0800
    movk    x10, #0x30d0, lsl #16
    msr     sctlr_el1, x10

    /* Set up return to EL1h with interrupts masked */
    mov     x10, #0x3c4             /* EL1h, DAIF masked */
    msr     spsr_el2, x10
    adr     x10, .Lpi5_in_el1
    msr     elr_el2, x10
    eret

.Lpi5_in_el1:
    /* Now in EL1 - peripheral access works */
```

## Pi 5 ACT LED Control

The ACT LED is on **GPIO2 bit 9**, directly accessible on BCM2712 (no PCIe/RP1 needed).

| Register | Address | Purpose |
|----------|---------|---------|
| GPIO2_BASE | 0x107D517C00 | Base address (ARM_IO_BASE + 0x1517C00) |
| GPIO2_DATA0 | 0x107D517C04 | Data register (set/clear bit 9) |
| GPIO2_IODIR0 | 0x107D517C08 | Direction register (clear bit 9 for output) |

**Note:** The address is 40-bit (`0x107D517C00`), not 32-bit. Early attempts using `0x7D001000` failed due to incorrect address.

### LED Blink Code

```asm
    /* Load GPIO2 base address (0x107D517C00) */
    movz    x10, #0x7C00            /* bits [15:0] */
    movk    x10, #0x7D51, lsl #16   /* bits [31:16] */
    movk    x10, #0x0010, lsl #32   /* bits [47:32] */

    /* Set pin 9 to output */
    ldr     w11, [x10, #0x08]
    bic     w11, w11, #(1 << 9)
    str     w11, [x10, #0x08]
    dsb     sy

    /* LED ON */
    ldr     w11, [x10, #0x04]
    orr     w11, w11, #(1 << 9)
    str     w11, [x10, #0x04]
```

## Current Status

### Completed
- [x] Circle framework builds and runs on Pi 5
- [x] Identified EL2→EL1 transition requirement
- [x] Minimal LED test works (standalone binary)
- [x] Updated boot.S with EL transition code
- [x] Updated boot.S with position-independent addressing for UEFI compatibility
- [x] **PE header issue fixed** - conditional compilation skips PE header for RASPI5

### Verified Working
- [x] Full SLM-OS kernel boots on Pi 5 hardware
- [x] Boot.S: 3 slow LED blinks (EL2→EL1 transition)
- [x] C code: 10 fast LED blinks (kernel_main reached)

### Pending
- [ ] PCIe/RP1 initialization for GPIO header UART
- [ ] Serial console output

## PE Header Issue: RESOLVED

**Root cause:** When stripping the 64KB PE header, `adr x0, _start` calculated an address 0x10000 bytes before the actual binary. This caused all symbol offsets (stack, BSS, exception vectors) to be wrong.

**Solution implemented:** Conditional compilation in `boot.S`:
- For RASPI5: `_start` = `real_start` (same address, no PE header)
- For other platforms: PE header included as before

**Build process now:**
```bash
# Build for Pi 5 (no manual stripping needed)
make kernel PLATFORM=RASPI5
cp build/kernel/slmos.bin /path/to/sd/kernel_2712.img
```

See `docs/pi5-pe-header-analysis.md` for detailed root cause analysis.

## Pi 5 UART Architecture

| UART | Address | Connection | Status |
|------|---------|------------|--------|
| BCM2712 built-in | 0x107D001000 | Dedicated 3-pin JST connector | Needs JST cable (not available) |
| RP1 UART0 | 0x1F00030000 | 40-pin GPIO header pins 8/10 | Requires PCIe/RP1 init (~1000 lines of C code from Circle) |

The GPIO header UART goes through the RP1 southbridge, connected via PCIe. Circle initializes this in `bcmpciehostbridge.cpp` and `southbridge.cpp`.

## Circle Framework Analysis

Circle's approach for Pi 5:
1. **startup64.S** - EL2→EL1 transition, stack setup, exception vectors
2. **sysinit.cpp** - Memory system, interrupts, southbridge (PCIe/RP1) init
3. **Only after sysinit** can GPIO header UART or RP1 peripherals be used

Key files in Circle (`/tmp/circle-framework/`):
- `lib/startup64.S` - Boot code with EL transition
- `lib/sysinit.cpp` - System initialization including southbridge
- `lib/bcmpciehostbridge.cpp` - PCIe host bridge for RP1 access
- `lib/southbridge.cpp` - RP1 initialization
- `lib/actled.cpp` - ACT LED control
- `include/circle/bcm2712.h` - Pi 5 peripheral addresses

## Config.txt for Bare-Metal

Circle's minimal config (works):
```
arm_64bit=1
kernel_address=0x80000
kernel=kernel_2712.img
```

No `enable_uart`, `uart_2ndstage`, or device tree settings needed for LED blink.

## Files on SD Card

Boot partition contents:
- `config.txt` - Active configuration (currently Circle's minimal config)
- `config.txt.linux` - Backup of working Linux config
- `config.txt.slmos` - Backup of SLM-OS config attempts
- `kernel_2712.img` - Active kernel
- `kernel_2712.img.linux` - Linux kernel backup
- `kernel_2712.img.slmos` - SLM-OS kernel backup
- `slmos.img` - SLM-OS with PE header
- `bcm2712-rpi-5-b.dtb` - Device tree blob
- `overlays/` - DTB overlays

## Recovery Procedure

If Pi 5 is stuck:
1. Power off Pi
2. Remove SD card, insert in Ubuntu machine
3. Mount: `sudo mount /dev/sde1 /mnt/pi-boot`
4. Restore Linux:
   ```bash
   sudo cp /mnt/pi-boot/kernel_2712.img.linux /mnt/pi-boot/kernel_2712.img
   sudo cp /mnt/pi-boot/config.txt.linux /mnt/pi-boot/config.txt
   ```
5. Unmount: `sudo umount /mnt/pi-boot`
6. Insert SD card, power on

## Next Steps

1. **Test on Pi 5 hardware** - Deploy `build/kernel/slmos.bin` as `kernel_2712.img`
2. **Verify LED blinks 3 times** - Confirms boot code executes correctly
3. **Port PCIe/RP1 init** - For GPIO header UART serial output
4. **Full kernel integration** - Once serial works, full kernel debugging possible

## References

- [Circle bare-metal framework](https://github.com/rsta2/circle)
- [Circle Pi 5 documentation](https://circle-rpi.readthedocs.io/en/50.0/appendices/raspberry-pi-5.html)
- [BCM2712 peripherals](https://datasheets.raspberrypi.com/bcm2712/bcm2712-peripherals.pdf)
