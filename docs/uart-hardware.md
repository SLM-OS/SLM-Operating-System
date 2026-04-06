# UART Hardware Documentation

This document describes the UART hardware for each supported SLM-OS platform.

---

## Table of Contents

1. [Platform Summary](#platform-summary)
2. [PL011 UART (QEMU virt)](#pl011-uart-qemu-virt)
3. [Tegra186-UART (Jetson Orin Nano)](#tegra186-uart-jetson-orin-nano)
4. [RP1 UART (Raspberry Pi 5)](#rp1-uart-raspberry-pi-5)
5. [Abstraction Strategy](#abstraction-strategy)

---

## Platform Summary

| Platform | UART Type | Base Address | Driver | Notes |
|----------|-----------|--------------|--------|-------|
| QEMU virt | PL011 | 0x09000000 | uart_pl011.c | Primary development target |
| Raspberry Pi 5 | RP1 PL011 | 0x1F00030000 | uart_rp1_bitbang.c | Via RP1 southbridge |
| Jetson Orin Nano | Tegra186-UART | 0x03100000 | uart_tegra.c | UARTA (debug console) |

The PL011 is an ARM standard UART used by QEMU. Raspberry Pi 5 uses a PL011-like UART on the RP1 southbridge chip (accessed via PCIe). Jetson uses an NS16550-compatible Tegra UART with different register layout.

---

## PL011 UART (QEMU virt)

The PL011 is ARM's PrimeCell UART. It provides a full-featured serial interface with FIFOs, DMA support, and configurable baud rates.

### Base Address

| Platform | Base Address |
|----------|--------------|
| QEMU virt | 0x09000000 |

### Register Map

| Register | Offset | Access | Description |
|----------|--------|--------|-------------|
| DR | 0x000 | RW | Data Register |
| RSR/ECR | 0x004 | RW | Receive Status / Error Clear |
| FR | 0x018 | RO | Flag Register |
| ILPR | 0x020 | RW | IrDA Low-Power Counter |
| IBRD | 0x024 | RW | Integer Baud Rate Divisor |
| FBRD | 0x028 | RW | Fractional Baud Rate Divisor |
| LCR_H | 0x02C | RW | Line Control Register |
| CR | 0x030 | RW | Control Register |
| IFLS | 0x034 | RW | Interrupt FIFO Level Select |
| IMSC | 0x038 | RW | Interrupt Mask Set/Clear |
| RIS | 0x03C | RO | Raw Interrupt Status |
| MIS | 0x040 | RO | Masked Interrupt Status |
| ICR | 0x044 | WO | Interrupt Clear Register |
| DMACR | 0x048 | RW | DMA Control Register |

### Key Registers Detail

#### DR (Data Register) - Offset 0x000

| Bits | Field | Description |
|------|-------|-------------|
| 7:0 | DATA | Transmit/Receive data |
| 8 | FE | Framing Error (read) |
| 9 | PE | Parity Error (read) |
| 10 | BE | Break Error (read) |
| 11 | OE | Overrun Error (read) |

#### FR (Flag Register) - Offset 0x018

| Bits | Field | Description |
|------|-------|-------------|
| 0 | CTS | Clear To Send |
| 3 | BUSY | UART busy transmitting |
| 4 | RXFE | Receive FIFO empty |
| 5 | TXFF | Transmit FIFO full |
| 6 | RXFF | Receive FIFO full |
| 7 | TXFE | Transmit FIFO empty |

#### LCR_H (Line Control) - Offset 0x02C

| Bits | Field | Description |
|------|-------|-------------|
| 0 | BRK | Send break |
| 1 | PEN | Parity enable |
| 2 | EPS | Even parity select |
| 3 | STP2 | Two stop bits |
| 4 | FEN | FIFO enable |
| 6:5 | WLEN | Word length: 00=5, 01=6, 10=7, 11=8 bits |
| 7 | SPS | Stick parity select |

#### CR (Control Register) - Offset 0x030

| Bits | Field | Description |
|------|-------|-------------|
| 0 | UARTEN | UART enable |
| 7 | LBE | Loopback enable |
| 8 | TXE | Transmit enable |
| 9 | RXE | Receive enable |
| 14 | RTS | Request To Send |
| 15 | CTSEn | CTS hardware flow control |

### Minimal Initialization (PL011)

```c
#define PL011_CR_UARTEN  (1 << 0)
#define PL011_CR_TXE     (1 << 8)
#define PL011_CR_RXE     (1 << 9)
#define PL011_LCR_WLEN8  (3 << 5)
#define PL011_LCR_FEN    (1 << 4)
#define PL011_FR_TXFF    (1 << 5)

void pl011_init(uintptr_t base) {
    volatile uint32_t *cr   = (uint32_t *)(base + 0x030);
    volatile uint32_t *lcr  = (uint32_t *)(base + 0x02C);
    volatile uint32_t *ibrd = (uint32_t *)(base + 0x024);
    volatile uint32_t *fbrd = (uint32_t *)(base + 0x028);

    *cr = 0;                           // Disable UART
    *ibrd = 1;                         // Baud rate (placeholder)
    *fbrd = 0;
    *lcr = PL011_LCR_WLEN8 | PL011_LCR_FEN;  // 8N1, FIFO enabled
    *cr = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
}
```

### References

- [ARM PL011 Technical Reference Manual](https://developer.arm.com/documentation/ddi0183/latest/)

---

## Tegra186-UART (Jetson Orin Nano)

The Jetson Orin Nano uses Tegra186-compatible UARTs, which are NS16550-style with extensions. The register interface follows the classic 8250/16550 layout.

### Base Addresses

| UART | Base Address | Default Use |
|------|--------------|-------------|
| UARTA | 0x03100000 | Debug console |
| UARTB | 0x03110000 | Available |
| UARTC | 0x0C280000 | Available |
| UARTD | 0x03130000 | Available |

The debug console (UARTA) is exposed via USB-to-serial on the Developer Kit.

### Register Map

| Register | Offset | Access | Description |
|----------|--------|--------|-------------|
| THR | 0x000 | WO | Transmit Holding Register |
| RBR | 0x000 | RO | Receive Buffer Register |
| DLL | 0x000 | RW | Divisor Latch Low (when DLAB=1) |
| IER | 0x004 | RW | Interrupt Enable Register |
| DLH | 0x004 | RW | Divisor Latch High (when DLAB=1) |
| IIR | 0x008 | RO | Interrupt Identification Register |
| FCR | 0x008 | WO | FIFO Control Register |
| LCR | 0x00C | RW | Line Control Register |
| MCR | 0x010 | RW | Modem Control Register |
| LSR | 0x014 | RO | Line Status Register |
| MSR | 0x018 | RO | Modem Status Register |
| SPR | 0x01C | RW | Scratch Pad Register |

### Key Registers Detail

#### THR/RBR (Data) - Offset 0x000

| Bits | Field | Description |
|------|-------|-------------|
| 7:0 | DATA | Transmit (write) / Receive (read) data |

#### LSR (Line Status Register) - Offset 0x014

| Bits | Field | Description |
|------|-------|-------------|
| 0 | DR | Data Ready |
| 1 | OE | Overrun Error |
| 2 | PE | Parity Error |
| 3 | FE | Framing Error |
| 4 | BI | Break Interrupt |
| 5 | THRE | Transmit Holding Register Empty |
| 6 | TEMT | Transmitter Empty |
| 7 | FIFOE | FIFO Error |

#### LCR (Line Control Register) - Offset 0x00C

| Bits | Field | Description |
|------|-------|-------------|
| 1:0 | WLS | Word Length: 00=5, 01=6, 10=7, 11=8 bits |
| 2 | STB | Stop bits: 0=1, 1=1.5/2 |
| 3 | PEN | Parity Enable |
| 4 | EPS | Even Parity Select |
| 5 | SP | Stick Parity |
| 6 | SB | Set Break |
| 7 | DLAB | Divisor Latch Access Bit |

#### FCR (FIFO Control Register) - Offset 0x008

| Bits | Field | Description |
|------|-------|-------------|
| 0 | FIFOE | FIFO Enable |
| 1 | RFIFOR | Receive FIFO Reset |
| 2 | XFIFOR | Transmit FIFO Reset |
| 3 | DMAM | DMA Mode Select |
| 5:4 | TET | TX Empty Trigger |
| 7:6 | RT | Receive Trigger |

### Minimal Initialization (Tegra/NS16550)

```c
#define NS16550_LCR_DLAB  (1 << 7)
#define NS16550_LCR_8N1   (3 << 0)
#define NS16550_FCR_FIFO  (1 << 0)
#define NS16550_FCR_RXCLR (1 << 1)
#define NS16550_FCR_TXCLR (1 << 2)
#define NS16550_LSR_THRE  (1 << 5)

void ns16550_init(uintptr_t base) {
    volatile uint8_t *lcr = (uint8_t *)(base + 0x00C);
    volatile uint8_t *fcr = (uint8_t *)(base + 0x008);
    volatile uint8_t *dll = (uint8_t *)(base + 0x000);
    volatile uint8_t *dlh = (uint8_t *)(base + 0x004);

    *lcr = NS16550_LCR_DLAB;           // Enable divisor access
    *dll = 1;                          // Baud rate (placeholder)
    *dlh = 0;
    *lcr = NS16550_LCR_8N1;            // 8N1, disable DLAB
    *fcr = NS16550_FCR_FIFO | NS16550_FCR_RXCLR | NS16550_FCR_TXCLR;
}
```

### References

- [NVIDIA Jetson Orin Nano Developer Kit](https://developer.nvidia.com/embedded/jetson-orin-nano-developer-kit)
- [NS16550 Datasheet](https://www.ti.com/lit/ds/symlink/tl16c550c.pdf)

---

## RP1 UART (Raspberry Pi 5)

The Raspberry Pi 5's GPIO and UART peripherals are on the RP1 southbridge chip, connected to the BCM2712 SoC via PCIe. The UART is PL011-compatible but has quirks that require a custom driver.

### Architecture

```
BCM2712 SoC ──PCIe──> RP1 Southbridge
                         ├── GPIO controller
                         ├── UART0 (PL011)
                         ├── UART1-5
                         └── Other peripherals
```

### Memory Map

The RP1 is mapped via PCIe BAR at 0x1F00000000, which maps to the RP1's internal address space at 0x40000000:

| Peripheral | Host Address | RP1 Internal | Description |
|------------|--------------|--------------|-------------|
| GPIO IO | 0x1F000D0000 | 0x400D0000 | Pin control registers |
| GPIO RIO | 0x1F000E0000 | 0x400E0000 | Direct GPIO access |
| GPIO PADS | 0x1F000F0000 | 0x400F0000 | Pad configuration |
| UART0 | 0x1F00030000 | 0x40030000 | PL011 UART |

### GPIO Configuration

GPIO14 (TXD) and GPIO15 (RXD) must be configured for UART function:

| Register | Address | Value | Description |
|----------|---------|-------|-------------|
| GPIO14_PADS | 0x1F000F003C | 0x56 | Output enable, 4mA drive |
| GPIO14_CTRL | 0x1F000D0074 | 4 | FUNCSEL = UART TXD |
| GPIO15_PADS | 0x1F000F0040 | 0x56 | Input enable, 4mA drive |
| GPIO15_CTRL | 0x1F000D007C | 4 | FUNCSEL = UART RXD |

FUNCSEL values:
- 4 = UART function (TXD on GPIO14, RXD on GPIO15)
- 5 = SYS_RIO (direct GPIO control via RIO registers)

### Known Limitations

1. **Flag register reads crash:** Reading from UART_FR (0x1F00030018) causes a data abort. The driver uses blind writes with a fixed delay instead of polling TX ready.

2. **FIFOs unreliable:** With FIFOs enabled, character corruption occurs. The driver disables FIFOs (LCR_H bit 4 = 0).

3. **Requires firmware initialization:** The `config.txt` settings `pciex4_reset=0` and `uart_2ndstage=1` must be set so the firmware leaves PCIe/RP1 initialized.

### Initialization Sequence

```c
// 1. Configure GPIO pads and pin mux
*gpio14_pads = 0x56;
*gpio14_ctrl = 4;  // UART function
*gpio15_pads = 0x56;
*gpio15_ctrl = 4;

// 2. Disable UART
*uart_cr = 0;

// 3. Clear interrupts
*uart_icr = 0x7FF;

// 4. Set baud rate (50MHz clock, 115200 baud)
*uart_ibrd = 27;
*uart_fbrd = 8;

// 5. Configure 8N1, FIFOs DISABLED
*uart_lcr = (3 << 5);  // 8-bit, no FEN

// 6. Enable UART with TX and RX
*uart_cr = (1 << 0) | (1 << 8) | (1 << 9);
```

### Transmit (No Flag Polling)

```c
void uart_putc(char c) {
    *uart_dr = c;
    // Fixed delay - flag polling crashes
    for (volatile int d = 0; d < 15000; d++);
}
```

### Driver File

The driver is in `kernel/drivers/uart_rp1_bitbang.c`. Despite the filename (historical), it uses hardware UART for TX. RX is bit-banged via GPIO RIO for reliability.

### References

- [Raspberry Pi 5 config.txt documentation](https://www.raspberrypi.com/documentation/computers/config_txt.html)
- Circle framework for Pi 5 bare-metal reference
- See `docs/pi5-baremetal-status.md` for current UART status and driver details

---

## Abstraction Strategy

Since PL011 and NS16550 have different register layouts, SLM-OS uses compile-time platform selection.

### File Structure

```
kernel/
├── include/
│   └── uart.h           # Common UART interface
├── drivers/
│   ├── uart_pl011.c     # PL011 driver (QEMU, Pi 5)
│   └── uart_tegra.c     # Tegra/NS16550 driver (Jetson)
```

### Common Interface (uart.h)

```c
#ifndef UART_H
#define UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
void uart_puts(const char *s);
int  uart_printf(const char *fmt, ...);

#endif
```

### Platform Selection

In `platform.h`:

```c
#if defined(PLATFORM_QEMU_VIRT)
    #define UART_TYPE_PL011
    #define UART_BASE  0x09000000
#elif defined(PLATFORM_RASPI5)
    #define UART_TYPE_RP1_BITBANG
    #define UART_BASE  0x1F00030000
#elif defined(PLATFORM_JETSON_ORIN_NANO)
    #define UART_TYPE_NS16550
    #define UART_BASE  0x03100000
#endif
```

### Build System

CMake selects the appropriate driver based on platform:

```cmake
# UART driver selection in CMakeLists.txt
$<$<STREQUAL:${PLATFORM},QEMU_VIRT>:kernel/drivers/uart_pl011.c>
$<$<STREQUAL:${PLATFORM},RASPI5>:kernel/drivers/uart_rp1_bitbang.c>
$<$<STREQUAL:${PLATFORM},JETSON_ORIN_NANO>:kernel/drivers/uart_tegra.c>
```

---

## Decision Record

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Abstraction method | Compile-time `#ifdef` | No runtime overhead; platforms fixed at build |
| Initial target | PL011 (QEMU) | Primary development on QEMU virt |
| FIFO usage | Enabled | Better performance for printf output |
| Baud rate | Skip for QEMU | QEMU ignores baud settings |

---

*Last updated: January 2026*
