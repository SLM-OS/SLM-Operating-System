# Pi 5 UART Testing Status

**Date:** 1 January 2026
**Status:** No TX output achieved yet

---

## What Works

1. **Boot to C code confirmed** - LED blinks prove kernel_main() is reached
2. **EL2→EL1 transition works** - Required for peripheral access
3. **RP1 memory writes don't fault** - Can write to 0x1F000xxxxx addresses without crashing
4. **ACT LED (GPIO2) works** - Can blink LED at 0x107D517C04

---

## What Doesn't Work

1. **RP1 UART TX** - No signal on GPIO14 (pin 8) with any configuration tested
2. **RP1 GPIO output** - Toggling GPIO14 as plain GPIO output showed no signal
3. **UART flag register reads** - Code hangs when waiting for TX ready (suggests UART clock not running)

---

## Tests Performed

### Alt Function Brute Force (Current Test)
- Cycles through FUNCSEL 0-8 for GPIO14/GPIO15
- Blink code: N+1 blinks for alt function N (1 blink = alt 0, 9 blinks = alt 8)
- ~20-30 second pause between iterations
- No TX activity observed on any iteration
- **Code location:** `kernel/src/main.c` lines 211-255

### GPIO Direct Toggle Test
- Configured GPIO14 as plain GPIO output (FUNCSEL=5)
- Used RIO registers to toggle pin
- No signal observed (suggests RP1 GPIO controller not responding)

### UART Register Configurations Tried
- Baud: 115200 with 44MHz clock (IBRD=23, FBRD=56)
- Baud: 115200 with 48MHz clock (IBRD=26, FBRD=3)
- Line: 8N1 (LCRH = 3 << 5)
- Control: UART enable + TX enable (CR = 0x101)

---

## Key Addresses

### RP1 GPIO (via PCIe at 0x1F00000000)
```
GPIO14_CTRL  = 0x1F000D0074  (pin function select)
GPIO14_PADS  = 0x1F000F003C  (pad configuration)
GPIO15_CTRL  = 0x1F000D007C
GPIO15_PADS  = 0x1F000F0040
RIO_OE_SET   = 0x1F000E2004  (output enable)
RIO_OUT_SET  = 0x1F000E2000  (output high)
RIO_OUT_CLR  = 0x1F000E3000  (output low)
```

### RP1 UART0 (PL011)
```
UART_DR      = 0x1F00030000  (data register)
UART_FR      = 0x1F00030018  (flag register - reads hang!)
UART_IBRD    = 0x1F00030024  (integer baud divisor)
UART_FBRD    = 0x1F00030028  (fractional baud divisor)
UART_LCRH    = 0x1F0003002C  (line control)
UART_CR      = 0x1F00030030  (control register)
```

### BCM2712 GPIO2 (ACT LED - works)
```
GPIO2_DATA   = 0x107D517C04  (bit 9 = ACT LED)
```

---

## Likely Root Cause

The RP1 southbridge chip is connected via PCIe. While the Pi 5 firmware sets up PCIe memory mapping (writes don't fault), it appears the RP1's internal clocks/peripherals are not initialized for bare-metal use.

Evidence:
1. Reading UART flag register hangs (UART clock not running)
2. GPIO toggle has no effect (GPIO controller not clocked?)
3. Circle framework requires ~1500 lines of PCIe host bridge init

---

## Next Steps

1. **Option A:** Port Circle's full PCIe/RP1 initialization
   - Files: `bcmpciehostbridge.cpp` (~1300 lines), `southbridge.cpp` (~285 lines)
   - Complex but proven to work

2. **Option B:** Find RP1 clock enable registers
   - RP1 has internal clock controller
   - May just need to enable UART/GPIO clocks
   - Less code but requires RP1 documentation

3. **Option C:** Use BCM2712 built-in UART
   - Address: 0x107D001000
   - Requires JST cable to dedicated debug header (not available)

---

## Reference: Circle Framework Files

Located in `/tmp/circle-framework/`:
- `lib/bcmpciehostbridge.cpp` - PCIe host bridge init
- `lib/southbridge.cpp` - RP1 wrapper
- `lib/gpiopin2712.cpp` - GPIO pin muxing
- `lib/gpioclock-rp1.cpp` - RP1 clock configuration
- `lib/serial.cpp` - UART driver (shows GPIO14/15 use alt func 4)
- `include/circle/bcm2712.h` - Address definitions

---

## Current Code State

The kernel currently has the alt-function brute-force test:
```c
#if defined(PLATFORM_RASPI5)
    /* Brute-force test: cycle through alt functions 0-8 */
    for (int alt = 0; alt <= 8; alt++) {
        /* Blink (alt+1) times to identify iteration */
        ...
        *gpio14_ctrl = alt;  /* Try this FUNCSEL */
        ...
        /* Send "SLM" multiple times */
        ...
    }
#endif
```

To restore normal operation, this block needs to be replaced with working UART init once we figure out the clock/init issue.
