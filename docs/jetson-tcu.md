# Tegra Combined UART (TCU) - Research Notes

This document captures research into using the TCU (Tegra Combined UART) for debug output on Jetson Orin Nano. This approach was explored as an alternative to using a second USB-serial adapter for the 40-pin header UART.

**Status:** RESOLVED (April 2026) — TCU RX works via HSP mailbox at EL2

> **April 2026 Update:** The TCU is now fully functional for bare-metal serial I/O.
> TX: write directly to UARTC (0x0C280000), output routed through SPE/TCU to USB-C.
> RX: read from TOP0_HSP shared mailbox 0 (0x03C10000), where SPE deposits incoming bytes.
> The SPE firmware continues running after kexec and handles the USB-C ↔ UART multiplexing.
> See `docs/archive/investigations/jetson-el2-bringup.md` and `kernel/drivers/uart_tegra.c` for implementation.

---

## Background

The Jetson Orin Nano's USB-C debug port uses the **Tegra Combined UART (TCU)**, which multiplexes debug output from multiple processors through the SPE (Sensor Processing Engine) to a physical UART.

### Architecture

```
┌──────────────┐    ┌───────────────┐    ┌──────────────┐
│   CCPLEX     │───▶│               │    │              │
│  (our code)  │ TX │  AON HSP      │───▶│    SPE       │───▶ Physical UART
│              │◀───│  0x0c150000   │◀───│  (firmware)  │     (USB-C debug)
└──────────────┘ RX └───────────────┘    └──────────────┘
```

Debug stream producers (CCPLEX, BPMP, TZ, SPE, RCE, SCE) send messages via HSP mailboxes to SPE, which combines and forwards them to the physical UART.

---

## Technical Details (from TRM Section 8.6)

### HSP Instances on Orin

| Instance | Base Address | nSM | nDB | Description |
|----------|--------------|-----|-----|-------------|
| TOP0_HSP | 0x03c00000 | 8 | 10 | Top-level, has doorbells for all targets |
| AON_HSP | 0x0c150000 | 8 | 0 | AON domain (where SPE runs) |

### HSP Memory Layout

For an HSP instance with nSM=8, nSS=2, nAS=2:
```
Offset      Content
0x00000     Common registers (64KB)
0x10000     Shared Mailbox 0/1 pair (64KB, 32KB each)
0x20000     Shared Mailbox 2/3 pair
0x30000     Shared Mailbox 4/5 pair
0x40000     Shared Mailbox 6/7 pair
0x50000     Shared Semaphore 0 (64KB)
0x60000     Shared Semaphore 1
0x70000     Arbitrated Semaphore 0
0x80000     Arbitrated Semaphore 1
0x90000     Doorbells (64KB, stride=0x100 per doorbell)
```

### Shared Mailbox Register

Each shared mailbox has a 32-bit register at offset 0x00 from its base:
```
Bit 31:    TAG (1=full/valid, 0=empty)
Bits 30:0: DATA (31 bits of payload)
```

For 32KB-spaced mailboxes:
```
SM{n}_BASE = HSP_BASE + 0x10000 + (n * 0x8000)
```

### TCU Message Format

TCU packs up to 3 characters per mailbox message:
```
Bits 31:26: Reserved (bit 31 = TAG/FULL)
Bits 25:24: Byte count (1-3)
Bits 23:16: Third character (if count >= 3)
Bits 15:8:  Second character (if count >= 2)
Bits 7:0:   First character
```

### Device Tree Configuration (from Jetson)

```
TCU mailboxes:
├── RX: HSP@3c00000 (TOP0_HSP), type=1, index=0 (consumer)
└── TX: HSP@c150000 (AON_HSP), type=1, index=1 (producer, bit 31 set)
```

TX mailbox address: `0x0c150000 + 0x10000 + (1 * 0x8000) = 0x0c168000`

### Doorbell Mapping (Table 8.61)

| Doorbell | Target |
|----------|--------|
| 0 | CCPMU |
| 1 | CCPLEX TrustZone not secure |
| 2 | CCPLEX TrustZone secure |
| 3 | BPMP |
| 4 | **AON** (SPE) |
| 5 | SCE |
| 6 | APE |
| 7 | RCE |
| 8 | DCE |
| 9 | PSC |

Doorbell 4 trigger: `TOP0_HSP_BASE + 0x90000 + (4 * 0x100) = 0x03c90400`

---

## What We Tried

### Attempt 1: SBSA UART (0x31d0000)

The device tree shows an SBSA (PL011-compatible) UART at 0x31d0000 with status "okay". We tried writing directly to it, but no output appeared on USB-C debug console.

### Attempt 2: Direct Mailbox Write

Wrote to AON_HSP shared mailbox 1 with TCU message format:
```c
#define AON_HSP_BASE      0x0c150000UL
#define TX_SM_OFFSET      (0x10000 + (1 * 0x8000))
#define TX_SM_REG         (*(volatile uint32_t *)(AON_HSP_BASE + TX_SM_OFFSET))

// Write: TAG=1, count=1, char
TX_SM_REG = (1UL << 31) | (1UL << 24) | (uint32_t)c;
```

No output appeared.

### Attempt 3: Multiple Mailboxes

Tried writing to 4 different mailboxes simultaneously:
- AON_HSP SM0 (0x0c160000)
- AON_HSP SM1 (0x0c168000)
- TOP0_HSP SM0 (0x03c10000)
- TOP0_HSP SM1 (0x03c18000)

No output from any of them.

### Attempt 4: Doorbell + Mailbox

Rang the AON doorbell (DB4) to wake SPE before sending mailbox messages:
```c
#define DB4_TRIGGER (*(volatile uint32_t *)(0x03c90400))
DB4_TRIGGER = 1;  // Ring doorbell
// ... then send mailbox messages
```

Still no output.

---

## Why It Didn't Work

The TCU architecture requires **SPE firmware** to:
1. Poll the shared mailbox for incoming data
2. Read messages and clear the TAG bit
3. Forward characters to the physical UART

When `kexec` executes, Linux shutdown likely stops or resets SPE firmware, leaving no listener for mailbox writes. The doorbell only works if SPE is running and waiting for interrupts - it cannot resurrect a halted processor.

---

## Code for Future Reference

If SPE initialization becomes possible, here is the working mailbox code:

```c
/*
 * TCU via HSP Shared Mailbox
 *
 * Prerequisites:
 * - SPE firmware must be running and polling AON_HSP mailboxes
 * - May need doorbell ring to wake SPE after cold boot
 */

#define TOP0_HSP_BASE     0x03c00000UL
#define AON_HSP_BASE      0x0c150000UL

/* Doorbell 4 (AON) in TOP0_HSP */
#define DB_PAGE_OFFSET    0x90000UL
#define DB_STRIDE         0x100UL
#define DB_AON            4
#define DB4_TRIGGER       (*(volatile uint32_t *)(TOP0_HSP_BASE + DB_PAGE_OFFSET + DB_AON * DB_STRIDE))

/* Shared mailbox 1 in AON_HSP */
#define SM_OFFSET(idx)    (0x10000 + ((idx) * 0x8000))
#define AON_SM1           (*(volatile uint32_t *)(AON_HSP_BASE + SM_OFFSET(1)))
#define HSP_SM_FULL       (1UL << 31)

/* Ring AON doorbell to wake SPE */
static inline void tcu_ring_doorbell(void)
{
    DB4_TRIGGER = 1;
    for (volatile int d = 0; d < 100000; d++) { }
}

/* Send character via TCU mailbox */
static inline void tcu_putchar(char c)
{
    /* Wait for mailbox empty (TAG=0) with timeout */
    int timeout = 10000;
    while ((AON_SM1 & HSP_SM_FULL) && --timeout > 0) { }

    /* Write: TAG=1, count=1, data=char */
    AON_SM1 = HSP_SM_FULL | (1UL << 24) | ((uint32_t)c & 0xFF);
}

/* Send string with \n -> \r\n conversion */
static inline void tcu_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') tcu_putchar('\r');
        tcu_putchar(*s++);
    }
}

/* Usage:
 * tcu_ring_doorbell();  // Wake SPE (if sleeping)
 * tcu_puts("Hello from SLM-OS!\n");
 */
```

---

## Alternative Approaches

### 1. USB-Serial Adapter (Recommended)

Connect a USB-to-TTL adapter to 40-pin header:
- Pin 8: UART TX (GPIO)
- Pin 10: UART RX (GPIO)
- Pin 6: GND

This uses UARTA (0x03100000), which is a standard NS16550-compatible UART that works independently of SPE.

### 2. Boot via U-Boot Instead of kexec

U-Boot may preserve SPE state better than kexec. Worth investigating if TCU is needed.

### 3. SPE Firmware Modification

Would require access to SPE firmware source (not publicly available) or deep reverse engineering.

---

## References

- [Orin TRM Section 8.6](https://developer.nvidia.com/orin-series-soc-technical-reference-manual) - HSP documentation
- [Linux tegra-tcu.c](https://github.com/torvalds/linux/blob/master/drivers/tty/serial/tegra-tcu.c) - TCU driver
- [Linux tegra-hsp.c](https://github.com/torvalds/linux/blob/master/drivers/mailbox/tegra-hsp.c) - HSP mailbox driver
- [eLinux TCU overview](https://elinux.org/Jetson/AGX_Xavier_Tegra_Combined_UART)

---

## See Also

**`docs/archive/investigations/jetson-nvidia-support.md`** — Comprehensive documentation of all Jetson blockers. Even with USB-serial adapter, bare-metal UART access is blocked by CBB firewall.

---

*Created: December 2025*
*Updated: January 2026*
*Status: TCU not viable for bare-metal. USB-serial adapter also blocked by CBB firewall.*
