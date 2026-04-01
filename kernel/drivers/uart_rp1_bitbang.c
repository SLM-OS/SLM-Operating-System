/*
 * uart_rp1_bitbang.c - UART driver for Raspberry Pi 5 via RP1
 *
 * Uses the PL011 UART0 on RP1 (GPIO14=TXD, GPIO15=RXD).
 * Falls back to bit-banging for RX since PL011 RX timing is sensitive.
 *
 * The RP1 peripherals are accessed via PCIe at 0x1F00000000.
 * GPIO pins must be configured for UART function (funcsel=4).
 *
 * Baud rate: 115200 @ 50MHz clock (firmware default)
 */

#include "platform.h"
#include "uart.h"
#include <stdint.h>

#ifdef UART_TYPE_RP1_BITBANG

/* RP1 GPIO register base addresses */
#define RP1_GPIO_IO_BASE    0x1F000D0000ULL
#define RP1_GPIO_RIO_BASE   0x1F000E0000ULL
#define RP1_GPIO_PADS_BASE  0x1F000F0000ULL

/* RP1 UART0 (PL011) base address */
#define RP1_UART0_BASE      0x1F00030000ULL

/* GPIO pin numbers */
#define GPIO_TXD  14
#define GPIO_RXD  15

/* PL011 register offsets */
#define UART_DR     0x00    /* Data register */
#define UART_FR     0x18    /* Flag register */
#define UART_IBRD   0x24    /* Integer baud rate divisor */
#define UART_FBRD   0x28    /* Fractional baud rate divisor */
#define UART_LCR    0x2C    /* Line control register */
#define UART_CR     0x30    /* Control register */
#define UART_ICR    0x44    /* Interrupt clear register */

/* Flag register bits */
#define FR_TXFF     (1 << 5)    /* TX FIFO full */
#define FR_RXFE     (1 << 4)    /* RX FIFO empty */

/* Control register bits */
#define CR_UARTEN   (1 << 0)    /* UART enable */
#define CR_TXE      (1 << 8)    /* TX enable */
#define CR_RXE      (1 << 9)    /* RX enable */

/* Line control register bits */
#define LCR_FEN     (1 << 4)    /* FIFO enable */
#define LCR_WLEN_8  (3 << 5)    /* 8-bit word length */

/* Function select values */
#define FUNCSEL_UART    4   /* UART TXD/RXD function */
#define FUNCSEL_SYS_RIO 5   /* Direct GPIO control */

/* Pad configuration: IE=1, OD=0, 4mA drive */
#define PAD_CONFIG  0x56

/* Bit delay for 115200 baud (for RX bit-banging) */
#define BIT_DELAY  150

/*
 * Initialize UART0 on RP1 for 115200 baud.
 */
void uart_init(void)
{
    volatile uint32_t *gpio14_ctrl = (volatile uint32_t *)(RP1_GPIO_IO_BASE + GPIO_TXD * 8 + 4);
    volatile uint32_t *gpio15_ctrl = (volatile uint32_t *)(RP1_GPIO_IO_BASE + GPIO_RXD * 8 + 4);
    volatile uint32_t *gpio14_pads = (volatile uint32_t *)(RP1_GPIO_PADS_BASE + 4 + GPIO_TXD * 4);
    volatile uint32_t *gpio15_pads = (volatile uint32_t *)(RP1_GPIO_PADS_BASE + 4 + GPIO_RXD * 4);

    volatile uint32_t *uart_cr   = (volatile uint32_t *)(RP1_UART0_BASE + UART_CR);
    volatile uint32_t *uart_icr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_ICR);
    volatile uint32_t *uart_ibrd = (volatile uint32_t *)(RP1_UART0_BASE + UART_IBRD);
    volatile uint32_t *uart_fbrd = (volatile uint32_t *)(RP1_UART0_BASE + UART_FBRD);
    volatile uint32_t *uart_lcr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_LCR);

    /* Configure GPIO14 (TXD) pad and pin mux */
    *gpio14_pads = PAD_CONFIG;
    __asm__ volatile("dsb sy" ::: "memory");
    *gpio14_ctrl = FUNCSEL_UART;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Configure GPIO15 (RXD) pad: IE=1, pull-up for idle-high */
    *gpio15_pads = 0x5A;  /* IE=1, OD=0, 4mA, PUE=1, PDE=0, SCHMITT=1 */
    __asm__ volatile("dsb sy" ::: "memory");
    *gpio15_ctrl = FUNCSEL_UART;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Disable UART before configuration */
    *uart_cr = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Clear all interrupt flags */
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Set baud rate for 115200 @ 50MHz clock
     * Divisor = 50000000 / (16 * 115200) = 27.127
     * IBRD = 27, FBRD = 0.127 * 64 = 8
     *
     * NOTE: This assumes a 50 MHz UART clock. The actual RP1 UART clock
     * may differ. TX works at this rate but RX does not receive data —
     * likely a baud rate mismatch on RX. See docs/pi5-baremetal-status.md.
     */
    *uart_ibrd = 27;
    *uart_fbrd = 8;
    __asm__ volatile("dsb sy" ::: "memory");

    /* 8N1, FIFOs enabled */
    *uart_lcr = LCR_WLEN_8 | LCR_FEN;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Enable UART, TX, and RX */
    *uart_cr = CR_UARTEN | CR_TXE | CR_RXE;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Small delay for UART to stabilize */
    for (volatile int d = 0; d < 10000; d++);
}

/*
 * Send a single character via PL011 UART.
 *
 * After MMU is enabled with proper device memory mapping for RP1,
 * the flag register should be safely readable. Use it to wait for
 * TX FIFO space rather than a blind delay which breaks when caches
 * change loop timing.
 */
void uart_putc(char c)
{
    volatile uint32_t *uart_dr = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    /* Wait until TX FIFO is not full.
     * Use a timeout to detect if FR is stuck — fall back to delay. */
    int timeout = 100000;
    while ((*uart_fr & FR_TXFF) && --timeout > 0) {
        __asm__ volatile("" ::: "memory");
    }

    /* Write character to data register */
    *uart_dr = (uint32_t)c;

    /* If FR polling timed out, use a delay to pace output */
    if (timeout <= 0) {
        for (volatile int d = 0; d < 2000; d++);
    }
}

/*
 * Receive a single character via PL011 UART.
 *
 * Originally used GPIO bit-banging because flag register reads caused data
 * aborts. With proper device memory mapping (VMM maps RP1 as nGnRnE), the
 * PL011 hardware RX works correctly.
 */
char uart_getc(void)
{
    extern void yield(void);

    volatile uint32_t *uart_dr = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    /* Wait until RX FIFO has data */
    while (*uart_fr & FR_RXFE) {
        yield();
    }

    /* Read character (lower 8 bits of DR) */
    return (char)(*uart_dr & 0xFF);
}

#endif /* UART_TYPE_RP1_BITBANG */
