/*
 * uart_rp1.c - UART driver for Raspberry Pi 5 via RP1
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
 * Timer interrupts work correctly with preemptive scheduling active.
 * An earlier issue where timer IRQs appeared to break PL011 RX was
 * traced to a DAIF initialization bug in task creation — new tasks
 * started with DAIF=0 (IRQs unmasked), allowing the timer ISR to
 * fire during context restore in context.S. Fix: DAIF=0x080 (IRQ
 * masked) on new tasks. See docs/pi5-baremetal-status.md for details.
 */

#include "platform.h"
#include "uart.h"
#include "gic.h"
#include "debug.h"
#include <stdint.h>

#ifdef UART_TYPE_RP1

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
#define UART_IFLS   0x34    /* Interrupt FIFO Level Select register */
#define UART_IMSC   0x38    /* Interrupt mask set/clear register */
#define UART_RIS    0x3C    /* Raw Interrupt Status register */
#define UART_MIS    0x40    /* Masked Interrupt Status register */
#define UART_ICR    0x44    /* Interrupt clear register */

/* IMSC / MIS / ICR bit positions for RX interrupts */
#define IMSC_RXIM   (1 << 4)    /* Receive interrupt mask */
#define IMSC_RTIM   (1 << 6)    /* Receive timeout interrupt mask */

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

/* ============================================================================
 * RX ring buffer (single-producer ISR, single-consumer uart_getc)
 * ============================================================================ */

#define UART_RX_BUF_SIZE    256     /* Must be power of 2 */

static volatile uint8_t  rx_buf[UART_RX_BUF_SIZE];
static volatile uint32_t rx_head;       /* ISR writes here */
static volatile uint32_t rx_tail;       /* uart_getc reads here */
static volatile int      uart_irq_mode; /* 1 once IRQ 153 fires */

/* ============================================================================
 * Initialization
 * ============================================================================ */

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

    /* Step 4a: Disable all PL011 interrupts and clear pending flags.
     * The firmware may have left IMSC with RX/TX interrupts enabled.
     * With GIC active, an unhandled PL011 interrupt could jam the FIFO. */
    volatile uint32_t *uart_imsc = (volatile uint32_t *)(RP1_UART0_BASE + UART_IMSC);
    *uart_imsc = 0;         /* Disable all interrupt masks */
    __asm__ volatile("dsb sy" ::: "memory");
    *uart_icr = 0x7FF;      /* Clear all pending interrupt flags */
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
 * If UART RX interrupts are active (IRQ 153 fired at least once),
 * reads from the ring buffer populated by uart_irq_handler().
 * Otherwise falls back to direct PL011 polling.
 */
char uart_getc(void)
{
    extern void yield(void);

    if (uart_irq_mode) {
        /* Interrupt-driven path: read from ring buffer */
        while (rx_head == rx_tail) {
            yield();
        }
        uint8_t ch = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) & (UART_RX_BUF_SIZE - 1);
        return (char)ch;
    }

    /* Polling fallback (used when IRQ 153 never fires) */
    volatile uint32_t *uart_dr = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    while (*uart_fr & FR_RXFE) {
        yield();
        /* Check if interrupts became active while polling */
        if (uart_irq_mode) {
            while (rx_head == rx_tail) {
                yield();
            }
            uint8_t ch = rx_buf[rx_tail];
            rx_tail = (rx_tail + 1) & (UART_RX_BUF_SIZE - 1);
            return (char)ch;
        }
    }

    return (char)(*uart_dr & 0xFF);
}

/* ============================================================================
 * Interrupt-driven RX
 * ============================================================================ */

/*
 * UART RX interrupt handler.
 *
 * Called from el1_irq_handler when GIC reports UART_IRQ (153).
 * Drains the PL011 RX FIFO into the ring buffer.
 *
 * MUST NOT call uart_printf/uart_puts — the UART TX spinlock may be
 * held by the interrupted code, causing deadlock.
 */
/*
 * PCIe INTA chained handler — dispatches RP1 peripheral interrupts.
 *
 * Called from el1_irq_handler when GIC reports UART_IRQ (PCIe INTA, SPI 229).
 * Reads RP1 INTSTAT to determine which peripheral(s) fired, handles them,
 * and writes IACK to unmask level-triggered vectors.
 *
 * MUST NOT call uart_printf/uart_puts — deadlock risk.
 */
void uart_irq_handler(void)
{
    volatile uint32_t *intstatl = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_INTSTATL);

    uint32_t pending = *intstatl;

    /* Check if UART0 (vector 25) is pending */
    if (pending & (1U << RP1_INT_UART0)) {
        volatile uint32_t *uart_dr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
        volatile uint32_t *uart_fr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);
        volatile uint32_t *uart_icr = (volatile uint32_t *)(RP1_UART0_BASE + UART_ICR);

        uart_irq_mode = 1;

        /* Drain RX FIFO into ring buffer */
        while (!(*uart_fr & FR_RXFE)) {
            uint8_t ch = (uint8_t)(*uart_dr & 0xFF);
            uint32_t next = (rx_head + 1) & (UART_RX_BUF_SIZE - 1);
            if (next != rx_tail) {
                rx_buf[rx_head] = ch;
                rx_head = next;
            }
        }

        /* Clear PL011 interrupt flags */
        *uart_icr = IMSC_RXIM | IMSC_RTIM;
        __asm__ volatile("dsb sy" ::: "memory");

        /* IACK: acknowledge the RP1 MSI-X vector (unmasks for next interrupt).
         * Use the atomic SET alias so we don't disturb other bits. */
        volatile uint32_t *msix_set = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_SET
                                                             + RP1_MSIX_CFG(RP1_INT_UART0));
        *msix_set = MSIX_CFG_IACK;
        __asm__ volatile("dsb sy" ::: "memory");
    }
}

/*
 * Enable UART RX interrupts.
 *
 * Called after GIC init. Configures the full RP1→MIP→GIC interrupt
 * path including PCIe MSI-X table programming. Falls back to
 * polling transparently if any step fails.
 */

/*
 * PCIe config space access for RP1 (bus=1, devfn=0).
 * BCM2712 EXT_CFG: write index to 0x9000, read/write data at 0x9004+.
 */
/*
 * Try multiple bus numbers to find RP1. Firmware may enumerate it
 * on bus 0 (integrated) or bus 1 (standard PCIe enumeration).
 */
static uint32_t rp1_bus = 1;  /* Will be updated by uart_irq_init */

static uint32_t rp1_cfg_read32(uint32_t reg)
{
    volatile uint32_t *idx = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_INDEX);
    *idx = (rp1_bus << 20) | (0U << 12) | (reg & 0xFFF);
    __asm__ volatile("dsb sy" ::: "memory");
    return *(volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_DATA + (reg & 0xFFC));
}

static void rp1_cfg_write32(uint32_t reg, uint32_t val)
{
    volatile uint32_t *idx = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_INDEX);
    *idx = (rp1_bus << 20) | (0U << 12) | (reg & 0xFFF);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_DATA + (reg & 0xFFC)) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

void uart_irq_init(void)
{
    rx_head = 0;
    rx_tail = 0;
    uart_irq_mode = 0;

    /*
     * Step 0a: Enable MSI-X in RP1's PCIe config space.
     *
     * Access RP1 config space via the RC's EXT_CFG mechanism:
     * write bus=1/devfn=0 to INDEX, then read/write at DATA + offset.
     * Walk the capability list to find MSI-X (cap ID 0x11).
     */
    /*
     * PCIe config space access and MSI-X table programming.
     *
     * TODO: The BCM2712 PCIe RC at 0x1000120000 is mapped but
     * accessing registers (e.g., PCIE_STATUS at +0x4068, EXT_CFG at
     * +0x9000) causes a hang — likely a data abort that halts the
     * system. The RC registers may need a different memory attribute,
     * or the firmware may not leave them accessible from EL1.
     *
     * For now, skip the PCIe config space / MSI-X table programming.
     * The MSI-X table, Bus Master Enable, and MSI-X Enable bits are
     * the remaining pieces needed for interrupt-driven UART. The PL011
     * IMSC, RP1 MSIX_CFG, RC BAR1→MIP routing, and MIP registers are
     * all configured below and ready for when PCIe config access works.
     *
     * Polling fallback works transparently in the meantime.
     */
    INFO("UART IRQ: PCIe config space access not yet implemented (polling fallback)");
    goto skip_msix;

    /* Ensure Bus Master is enabled (Command register bit 2) */
    {
        uint32_t cmd_status = rp1_cfg_read32(0x04);
        uint16_t cmd = cmd_status & 0xFFFF;
        if (!(cmd & (1 << 2))) {
            rp1_cfg_write32(0x04, cmd_status | (1 << 2));
            DEBUG_PRINT("RP1: enabled Bus Master");
        }
    }

    /* Walk capability list to find MSI-X (cap ID 0x11) */
    uint8_t msix_cap = 0;
    {
        uint8_t cap_ptr = (uint8_t)(rp1_cfg_read32(0x34) & 0xFF);
        int limit = 48;  /* Safety limit to prevent infinite loop */
        while (cap_ptr && cap_ptr != 0xFF && limit-- > 0) {
            uint32_t cap_hdr = rp1_cfg_read32(cap_ptr);
            uint8_t cap_id = cap_hdr & 0xFF;
            if (cap_id == 0x11) {
                msix_cap = cap_ptr;
                break;
            }
            cap_ptr = (cap_hdr >> 8) & 0xFF;
        }
    }

    if (msix_cap) {
        /* Enable MSI-X with Function Mask (program table, then unmask) */
        uint32_t msix_word = rp1_cfg_read32(msix_cap);
        uint16_t flags = (msix_word >> 16) & 0xFFFF;
        uint16_t new_flags = flags | 0xC000;  /* Enable + MaskAll */
        rp1_cfg_write32(msix_cap, (msix_word & 0x0000FFFF) | ((uint32_t)new_flags << 16));
        DEBUG_PRINT("RP1 MSI-X cap at 0x%x: flags 0x%x → 0x%x, table_size=%d",
                    msix_cap, flags, new_flags, (flags & 0x7FF) + 1);
    } else {
        INFO("UART IRQ: MSI-X cap not found in RP1 config");
    }

    /*
     * Step 0b: Program MSI-X table entries.
     *
     * Each entry tells the RP1 MSI-X engine what PCIe address to write
     * and what data value to use for each vector. The RC BAR1 catches
     * writes to 0xFF_FFFFF000 and routes them to MIP0.
     */
    for (int i = 0; i < RP1_MSIX_TABLE_SIZE; i++) {
        volatile uint32_t *entry = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + i * 16);
        entry[0] = MSIX_MSG_ADDR_LO;   /* msg_addr low */
        entry[1] = MSIX_MSG_ADDR_HI;   /* msg_addr high */
        entry[2] = (uint32_t)i;         /* msg_data = vector number */
        entry[3] = 0x00000000;           /* vector_ctrl: unmasked */
    }
    __asm__ volatile("dsb sy" ::: "memory");

    DEBUG_PRINT("RP1 MSI-X: programmed %d table entries", RP1_MSIX_TABLE_SIZE);

    /* Clear Function Mask now that table is programmed */
    if (msix_cap) {
        uint32_t msix_word = rp1_cfg_read32(msix_cap);
        uint16_t flags = (msix_word >> 16) & 0xFFFF;
        uint16_t new_flags = (flags | 0x8000) & ~0x4000;  /* Keep Enable, clear MaskAll */
        rp1_cfg_write32(msix_cap, (msix_word & 0x0000FFFF) | ((uint32_t)new_flags << 16));
        DEBUG_PRINT("RP1 MSI-X: function mask cleared (flags: 0x%x)", new_flags);
    }

skip_msix:
    (void)0;

    /*
     * Step 1: Configure PCIe RC BAR1 → MIP0 routing.
     *
     * When RP1 fires an MSI-X, it writes to PCIe address 0xFF_FFFFF000.
     * RC BAR1 catches that write and remaps it to physical 0x10_00130000
     * (MIP0), which converts the MSI-X vector into a GIC SPI.
     *
     * The firmware may have already configured this (it uses RP1 for
     * HDMI/USB), but we set it explicitly to be safe.
     */
    volatile uint32_t *rc_bar1_lo   = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_BAR1_CONFIG_LO);
    volatile uint32_t *rc_bar1_hi   = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_BAR1_CONFIG_HI);
    volatile uint32_t *rc_remap_lo  = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_UBUS_BAR1_REMAP);
    volatile uint32_t *rc_remap_hi  = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_UBUS_BAR1_REMAP_HI);

    *rc_bar1_lo  = 0xFFFFF01C;  /* PCIe addr low + 4KB size encoding */
    *rc_bar1_hi  = 0x000000FF;  /* PCIe addr high */
    *rc_remap_lo = 0x00130001;  /* MIP0 phys addr low + access enable */
    *rc_remap_hi = 0x00000010;  /* MIP0 phys addr high */
    __asm__ volatile("dsb sy" ::: "memory");

    DEBUG_PRINT("PCIe RC BAR1→MIP0 routing configured");

    /*
     * Step 2: Initialize MIP0 (MSI-X Interrupt Peripheral).
     *
     * Unmask all vectors for host CPU, mask all for VPU.
     * Set all vectors to edge-triggered (MSI-X writes are edges).
     */
    volatile uint32_t *mip_maskl_host = (volatile uint32_t *)(MIP0_BASE + MIP_INT_MASKL_HOST);
    volatile uint32_t *mip_maskh_host = (volatile uint32_t *)(MIP0_BASE + MIP_INT_MASKH_HOST);
    volatile uint32_t *mip_maskl_vpu  = (volatile uint32_t *)(MIP0_BASE + MIP_INT_MASKL_VPU);
    volatile uint32_t *mip_maskh_vpu  = (volatile uint32_t *)(MIP0_BASE + MIP_INT_MASKH_VPU);
    volatile uint32_t *mip_cfgl_host  = (volatile uint32_t *)(MIP0_BASE + MIP_INT_CFGL_HOST);
    volatile uint32_t *mip_cfgh_host  = (volatile uint32_t *)(MIP0_BASE + MIP_INT_CFGH_HOST);

    *mip_maskl_host = 0x00000000;   /* Unmask vectors 0-31 for host */
    *mip_maskh_host = 0x00000000;   /* Unmask vectors 32-63 for host */
    *mip_maskl_vpu  = 0xFFFFFFFF;   /* Mask all for VPU */
    *mip_maskh_vpu  = 0xFFFFFFFF;
    *mip_cfgl_host  = 0xFFFFFFFF;   /* Edge-triggered */
    *mip_cfgh_host  = 0xFFFFFFFF;
    __asm__ volatile("dsb sy" ::: "memory");

    DEBUG_PRINT("MIP0 initialized (64 vectors, edge-triggered, host-unmasked)");

    /*
     * Step 3: Configure PL011 UART for interrupt-driven RX.
     */

    /* Set RX FIFO trigger level to 1/8 full (2 bytes) for low latency */
    volatile uint32_t *uart_ifls = (volatile uint32_t *)(RP1_UART0_BASE + UART_IFLS);
    *uart_ifls = (*uart_ifls & ~(0x7 << 3)) | (0x0 << 3);
    __asm__ volatile("dsb sy" ::: "memory");

    /* Clear pending PL011 interrupt flags and drain stale FIFO data */
    volatile uint32_t *uart_icr = (volatile uint32_t *)(RP1_UART0_BASE + UART_ICR);
    volatile uint32_t *uart_fr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);
    volatile uint32_t *uart_dr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);

    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");
    while (!(*uart_fr & FR_RXFE)) {
        (void)*uart_dr;
    }
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Enable PL011 RX and receive timeout interrupts */
    volatile uint32_t *uart_imsc = (volatile uint32_t *)(RP1_UART0_BASE + UART_IMSC);
    *uart_imsc = IMSC_RXIM | IMSC_RTIM;
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Step 4: Enable RP1 MSI-X vector 25 (UART0).
     *
     * IACK_EN: auto-mask on assert (required for level-triggered PL011).
     * After servicing, the handler must write IACK to re-enable.
     */
    volatile uint32_t *msix_set = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_SET
                                                         + RP1_MSIX_CFG(RP1_INT_UART0));
    *msix_set = MSIX_CFG_ENABLE | MSIX_CFG_IACK_EN;
    __asm__ volatile("dsb sy" ::: "memory");

    /* IACK any pending state */
    *msix_set = MSIX_CFG_IACK;
    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Step 5: Enable GIC SPI for UART0.
     *
     * MIP0 vector 25 → GIC SPI 153 → GIC IRQ 185 (UART_IRQ).
     * Set edge-triggered since MIP converts MSI-X writes to edge pulses.
     */
    gic_set_priority(UART_IRQ, GIC_PRIORITY_DEFAULT);
    gic_enable_irq(UART_IRQ);

    INFO("UART IRQ enabled (GIC IRQ %d = SPI %d, RP1 vec %d)",
         UART_IRQ, UART_IRQ - 32, RP1_INT_UART0);
}

/*
 * Check if UART is in interrupt-driven mode.
 * Returns 1 if IRQ 153 has fired at least once, 0 if polling.
 */
int uart_is_irq_mode(void)
{
    return uart_irq_mode;
}

#endif /* UART_TYPE_RP1 */
