/*
 * uart_tegra.c - Tegra UART driver for SLM-OS
 *
 * Supports: NVIDIA Jetson Orin Nano (Tegra234)
 *
 * The Tegra UART is NS16550-compatible with 32-bit register access.
 * Registers are memory-mapped with 4-byte spacing (reg-shift=2).
 *
 * This driver provides the low-level hardware interface:
 *   - uart_init()  - Initialize hardware
 *   - uart_putc()  - Send a character
 *   - uart_getc()  - Receive a character
 *
 * Higher-level functions (uart_puts, uart_printf) are in kprintf.c.
 *
 * References:
 * - Linux kernel: drivers/tty/serial/serial-tegra.c
 * - Device tree: arch/arm64/boot/dts/nvidia/tegra234.dtsi
 * - NS16550 datasheet
 */

#include "platform.h"
#include "uart.h"
#include "bpmp.h"
#include <stdbool.h>

#ifndef UART_TYPE_TEGRA
#error "uart_tegra.c included but UART_TYPE_TEGRA not defined"
#endif

/* NS16550 Register Offsets (byte offsets, will be shifted) */
#define NS16550_RBR     0   /* Receive Buffer Register (read) */
#define NS16550_THR     0   /* Transmit Holding Register (write) */
#define NS16550_IER     1   /* Interrupt Enable Register */
#define NS16550_IIR     2   /* Interrupt Identification Register (read) */
#define NS16550_FCR     2   /* FIFO Control Register (write) */
#define NS16550_LCR     3   /* Line Control Register */
#define NS16550_MCR     4   /* Modem Control Register */
#define NS16550_LSR     5   /* Line Status Register */
#define NS16550_MSR     6   /* Modem Status Register */
#define NS16550_SCR     7   /* Scratch Register */

/* Divisor Latch registers (when LCR.DLAB=1) */
#define NS16550_DLL     0   /* Divisor Latch Low */
#define NS16550_DLM     1   /* Divisor Latch High */

/* Line Control Register bits */
#define LCR_WLS_8       0x03    /* 8 data bits */
#define LCR_STB_1       0x00    /* 1 stop bit */
#define LCR_PEN_NONE    0x00    /* No parity */
#define LCR_DLAB        0x80    /* Divisor Latch Access Bit */

/* FIFO Control Register bits */
#define FCR_FIFO_EN     0x01    /* Enable FIFOs */
#define FCR_RXSR        0x02    /* Reset RX FIFO */
#define FCR_TXSR        0x04    /* Reset TX FIFO */
#define FCR_TRIGGER_14  0xC0    /* RX trigger level = 14 bytes */

/* Line Status Register bits */
#define LSR_DR          0x01    /* Data Ready */
#define LSR_OE          0x02    /* Overrun Error */
#define LSR_PE          0x04    /* Parity Error */
#define LSR_FE          0x08    /* Framing Error */
#define LSR_BI          0x10    /* Break Interrupt */
#define LSR_THRE        0x20    /* Transmit Holding Register Empty */
#define LSR_TEMT        0x40    /* Transmitter Empty */
#define LSR_RXFE        0x80    /* RX FIFO Error */

/* Modem Control Register bits */
#define MCR_DTR         0x01    /* Data Terminal Ready */
#define MCR_RTS         0x02    /* Request To Send */
#define MCR_OUT2        0x08    /* Enable interrupts (directly connected) */

/*
 * Register access with Tegra-specific shift.
 * Tegra UART has reg-shift=2, meaning each register is 4 bytes apart.
 */
#define REG_SHIFT       2
#define UART_REG(reg)   (*(volatile uint32_t *)(UART_BASE + ((reg) << REG_SHIFT)))

/*
 * Calculate baud rate divisor.
 * Divisor = UART_CLOCK / (16 * baud_rate)
 */
#define BAUD_DIVISOR(baud)  (UART_CLOCK / (16 * (baud)))

/* Flag to track if UART is available */
static bool g_uart_available = false;

/*
 * Initialize the Tegra UART.
 * Configures 8N1 at 115200 baud with FIFOs enabled.
 *
 * On Jetson platforms, the UART clock must be enabled via BPMP
 * before we can access the UART registers.
 */
void uart_init(void)
{
    uint16_t divisor = BAUD_DIVISOR(115200);

    /*
     * UART initialization modes:
     * 0 = Skip UART entirely (silent mode)
     * 1 = Try BPMP to enable clock (full initialization)
     * 2 = Direct mode - assume clock is already enabled (after kexec)
     * 3 = Raw mode - just enable flag, don't touch any registers.
     *     Uses whatever baud rate/config the firmware left in place.
     *     Needed when clock frequency is unknown (e.g., UARTC after kexec).
     *
     * After kexec from Linux, the UART clock should still be enabled
     * from Linux's initialization, so we can skip BPMP communication
     * which may not work correctly after kexec.
     */
#define UART_INIT_MODE 3  /* Raw mode - don't reconfigure (EL2/UARTC boot) */

#if UART_INIT_MODE == 0
    /* Silent mode - no UART output */
    g_uart_available = false;
    return;

#elif UART_INIT_MODE == 1
    /*
     * Full BPMP mode - enable clock via BPMP.
     * Only works when BPMP is fully operational.
     */
    if (bpmp_init() != 0) {
        g_uart_available = false;
        return;
    }

    if (bpmp_reset_deassert(TEGRA234_RESET_UARTA) != 0) {
        g_uart_available = false;
        return;
    }

    if (bpmp_clk_enable(TEGRA234_CLK_UARTA) != 0) {
        g_uart_available = false;
        return;
    }

    g_uart_available = true;

#elif UART_INIT_MODE == 2
    /*
     * Direct mode - assume UART clock is already enabled.
     * After kexec from Linux, the UART hardware should still be
     * configured and clocked. Just reinitialize the UART settings.
     */
    g_uart_available = true;

#elif UART_INIT_MODE == 3
    /*
     * Raw mode - don't touch any UART registers.
     * Use whatever configuration the firmware/Linux left in place.
     * Useful when the UART clock frequency is unknown and reconfiguring
     * the baud rate would garble output.
     */
    g_uart_available = true;
    return;

#endif

    /* Disable interrupts */
    UART_REG(NS16550_IER) = 0x00;

    /* Enable DLAB to set baud rate */
    UART_REG(NS16550_LCR) = LCR_DLAB;

    /* Set divisor (115200 baud) */
    UART_REG(NS16550_DLL) = divisor & 0xFF;
    UART_REG(NS16550_DLM) = (divisor >> 8) & 0xFF;

    /* Configure line: 8 bits, no parity, 1 stop bit, disable DLAB */
    UART_REG(NS16550_LCR) = LCR_WLS_8 | LCR_STB_1 | LCR_PEN_NONE;

    /* Enable and reset FIFOs, set RX trigger level */
    UART_REG(NS16550_FCR) = FCR_FIFO_EN | FCR_RXSR | FCR_TXSR | FCR_TRIGGER_14;

    /* Enable DTR, RTS, and OUT2 (interrupt enable) */
    UART_REG(NS16550_MCR) = MCR_DTR | MCR_RTS | MCR_OUT2;

    /* Clear any pending interrupts */
    (void)UART_REG(NS16550_LSR);
    (void)UART_REG(NS16550_RBR);
    (void)UART_REG(NS16550_IIR);
    (void)UART_REG(NS16550_MSR);
}

/*
 * Send a single character.
 *
 * After kexec, the TCU/SPE firmware owns the UARTC hardware state.
 * Reading LSR can interfere with TCU's internal tracking, causing
 * garbled output. Instead, use a simple delay between characters
 * to avoid overrunning the TX FIFO (same approach as the assembly
 * EL2 probe which works reliably).
 *
 * At 115200 baud, one character takes ~87us. The 16-byte TX FIFO
 * gives us headroom, so a brief spin is sufficient.
 */
void uart_putc(char c)
{
    if (!g_uart_available) {
        return;  /* UART not available, silently drop */
    }

    /*
     * Wait for TX FIFO space using LSR, but with DSB barrier before read.
     * After kexec, the TCU/SPE firmware owns UARTC. Without a barrier,
     * speculative or out-of-order reads of LSR can return stale data,
     * causing us to write THR when the FIFO is full — garbling output.
     */
    __asm__ volatile("dsb sy" ::: "memory");
    while ((UART_REG(NS16550_LSR) & LSR_THRE) == 0) {
        __asm__ volatile("dsb sy" ::: "memory");
    }

    UART_REG(NS16550_THR) = c;
}

/*
 * TCU RX buffer.
 *
 * The TCU HSP mailbox delivers 1-3 bytes per read. We buffer extra bytes
 * here so uart_getc() can return one character at a time.
 */
#ifdef TCU_RX_MBOX
static char tcu_rx_buf[3];
static int tcu_rx_count = 0;
static int tcu_rx_pos = 0;
#endif

/*
 * Receive a single character.
 * Blocks until data is available.
 *
 * On Jetson, RX comes through the TCU HSP mailbox (TOP0_HSP SM0 at
 * 0x03C10000), NOT through UARTC's RBR register. The SPE firmware
 * reads bytes from the USB-C physical UART and packs 1-3 bytes into
 * a 32-bit mailbox message.
 */
char uart_getc(void)
{
    if (!g_uart_available) {
        /* UART not available, spin forever (kernel will halt) */
        while (1) {
            __asm__ volatile("wfe");
        }
    }

#ifdef TCU_RX_MBOX
    /* Return buffered bytes from previous mailbox read */
    if (tcu_rx_pos < tcu_rx_count) {
        return tcu_rx_buf[tcu_rx_pos++];
    }

    /* Poll the TCU RX mailbox until data arrives */
    volatile uint32_t *mbox = (volatile uint32_t *)TCU_RX_MBOX;
    uint32_t val;

    for (;;) {
        val = *mbox;
        if (val & TCU_MBOX_TAG_BIT)
            break;
        /* Yield CPU briefly while waiting */
        __asm__ volatile("yield");
    }

    /* Clear the mailbox so SPE can send more */
    *mbox = 0;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Extract bytes from mailbox message */
    int count = (int)((val >> 24) & 0x3);  /* bits 25:24 = byte count */
    if (count == 0) count = 1;             /* shouldn't happen, but be safe */

    tcu_rx_buf[0] = (char)(val & 0xFF);
    tcu_rx_buf[1] = (char)((val >> 8) & 0xFF);
    tcu_rx_buf[2] = (char)((val >> 16) & 0xFF);
    tcu_rx_count = count;
    tcu_rx_pos = 1;  /* Return byte 0 now, buffer the rest */

    return tcu_rx_buf[0];
#else
    /*
     * Non-TCU path: read directly from UART RBR.
     *
     * Issue `dsb sy` before each LSR read. After kexec from Linux,
     * speculative MMIO reads return stale data — the same failure mode
     * documented for uart_putc (see the "UART LSR Read After Kexec" note
     * in the root CLAUDE.md). Without the barrier, LSR_DR may appear
     * clear when data is actually present (or vice versa), causing this
     * loop to hang or pull garbage from RBR.
     */
    for (;;) {
        __asm__ volatile("dsb sy" ::: "memory");
        if ((UART_REG(NS16550_LSR) & LSR_DR) != 0) break;
    }

    __asm__ volatile("dsb sy" ::: "memory");
    return (char)(UART_REG(NS16550_RBR) & 0xFF);
#endif
}
