/*
 * uart_rp1_bitbang.c - UART driver for Raspberry Pi 5 via RP1
 *
 * Uses the PL011 UART0 on RP1 (GPIO14=TXD, GPIO15=RXD).
 * The RP1 peripherals are accessed via PCIe at 0x1F00000000.
 * GPIO pins must be configured for UART function (funcsel=4).
 *
 * Baud rate: 115200 @ 50MHz clock (confirmed by testing; 48 MHz
 * assumption from RP1 datasheet produces garbled output).
 *
 * RX requires two non-obvious RP1 pad/mux configurations:
 *   1. OD=1 (output disable) on the RX pad — without this, the output
 *      driver interferes with external input even though OE reads as 0.
 *   2. FUNCSEL sequencing: set SYS_RIO (5) before UART (4). Direct
 *      switch from reset default (31/NULL) to UART doesn't reliably
 *      enable the PL011 RX input path.
 *
 * Known limitation: Timer interrupts break PL011 RX on the RP1.
 * The shell currently runs without preemptive scheduling (no timer).
 * The root cause appears to be an interaction between the GIC interrupt
 * handling path and the RP1 PCIe bus. Investigation ongoing.
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
#define UART_RSRECR 0x04    /* Receive status / error clear */
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

/* Pad configuration for TX: IE=1, OD=0, 4mA drive */
#define PAD_TX  0x56

/*
 * Pad configuration for RX: IE=1, OD=1, 4mA, PUE=1, SCHMITT=1
 * OD=1 (output disable) is critical — see file header comment.
 */
#define PAD_RX  0xDA

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

    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    /*
     * PL011 proper shutdown sequence (per ARM TRM):
     * 1. Wait for any in-flight TX to complete
     * 2. Disable UART
     * 3. Flush FIFOs
     * 4. Reconfigure GPIO, baud rate, LCR
     * 5. Re-enable
     */

    /* Step 1: Wait for firmware TX to finish (BUSY=0) */
    {
        int timeout = 1000000;
        while ((*uart_fr & (1 << 3)) && --timeout > 0) {
            __asm__ volatile("" ::: "memory");
        }
    }

    /* Step 2: Disable UART */
    *uart_cr = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Step 3: Flush FIFOs by disabling them */
    *uart_lcr = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Step 4a: Clear all interrupt flags */
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Step 4b: Configure GPIO14 (TXD) pad and pin mux */
    *gpio14_pads = PAD_TX;
    __asm__ volatile("dsb sy" ::: "memory");
    *gpio14_ctrl = FUNCSEL_UART;
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Step 4c: Configure GPIO15 (RXD).
     * The firmware/armstub resets GPIO15 to defaults (FUNCSEL=31/NULL,
     * IE=0, OD=1, pull-down). Must reconfigure for UART RX.
     *
     * FUNCSEL sequencing: SYS_RIO (5) first, then UART (4).
     * See file header for explanation.
     */
    *gpio15_pads = PAD_RX;
    __asm__ volatile("dsb sy" ::: "memory");
    *gpio15_ctrl = (4 << 5) | FUNCSEL_SYS_RIO;  /* F_M=4, FUNCSEL=5 first */
    __asm__ volatile("dsb sy" ::: "memory");
    for (volatile int d = 0; d < 1000; d++);
    *gpio15_ctrl = (4 << 5) | FUNCSEL_UART;      /* F_M=4, FUNCSEL=4 */
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Set baud rate for 115200 @ 50MHz clock.
     * Divisor = 50000000 / (16 * 115200) = 27.127
     * IBRD = 27, FBRD = 0.127 * 64 = 8
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
 * Waits for TX FIFO space before writing. The FR register is accessed
 * via PCIe to the RP1, which may return unreliable data before the MMU
 * maps the region as device memory. A post-write delay ensures proper
 * character spacing regardless of FR reliability.
 */
void uart_putc(char c)
{
    volatile uint32_t *uart_dr = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    /* Wait until TX FIFO is not full (with timeout for robustness) */
    int timeout = 100000;
    while ((*uart_fr & FR_TXFF) && --timeout > 0) {
        __asm__ volatile("" ::: "memory");
    }

    /* Write character to data register */
    *uart_dr = (uint32_t)c;
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Receive a single character via PL011 UART.
 *
 * Polls FR_RXFE until data is available, yielding to the scheduler
 * between checks to allow other tasks to run.
 */
char uart_getc(void)
{
    extern void yield(void);

    volatile uint32_t *uart_dr = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    /* Wait until RX FIFO has data */
    while (*uart_fr & FR_RXFE) {
        __asm__ volatile("" ::: "memory");
    }

    /* Read character (lower 8 bits of DR) */
    return (char)(*uart_dr & 0xFF);
}

#endif /* UART_TYPE_RP1_BITBANG */
