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
static volatile int      uart_irq_mode;  /* 1 once IRQ 153 fires */
static volatile int      uart_irq_ready; /* 1 after uart_irq_init() configures path */
volatile uint32_t uart_irq_count; /* diagnostic: handler call count */

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

    /* Check ring buffer first (filled by IRQ handler if it ever fires) */
    if (rx_head != rx_tail) {
        uint8_t ch = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1) & (UART_RX_BUF_SIZE - 1);
        return (char)ch;
    }

    /* Polling fallback — MSIX_CFG engine doesn't generate TLPs for new
     * characters despite correct BAR3 routing. The IRQ storm test proves
     * TLP delivery works, but the engine doesn't fire on PL011 assertion. */
    volatile uint32_t *uart_dr = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);

    while (*uart_fr & FR_RXFE) {
        yield();
        if (rx_head != rx_tail) {
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
 * PCIe INTA chained handler for RP1 UART0 interrupt.
 *
 * Called from el1_irq_handler when GIC reports UART_IRQ (SPI 229).
 * Checks RP1 INTSTAT for UART0 (vector 25), drains PL011 FIFO,
 * clears PL011 ICR, then IACKs the RP1 MSIX_CFG vector.
 *
 * MUST NOT call uart_printf/uart_puts — deadlock risk.
 */
void uart_irq_handler(void)
{
    volatile uint32_t *intstatl = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_INTSTATL);
    uint32_t pending = *intstatl;

    if (!(pending & (1U << RP1_INT_UART0))) {
        return;  /* Not UART0 — some other RP1 peripheral on PCIe INTA */
    }

    volatile uint32_t *uart_dr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_DR);
    volatile uint32_t *uart_fr  = (volatile uint32_t *)(RP1_UART0_BASE + UART_FR);
    volatile uint32_t *uart_icr = (volatile uint32_t *)(RP1_UART0_BASE + UART_ICR);

    uart_irq_mode = 1;
    uart_irq_count++;

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

    /* IACK: re-arm MSIX_CFG for next interrupt */
    volatile uint32_t *msix_set = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_SET
                                                         + RP1_MSIX_CFG(RP1_INT_UART0));
    *msix_set = MSIX_CFG_IACK;
    __asm__ volatile("dsb sy" ::: "memory");
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
/* PCIe config space access (used by old MSI-X approach, kept for reference) */
#if 0
static uint32_t rp1_bus = 1;
static uint32_t rp1_cfg_read32(uint32_t reg)
{
    volatile uint32_t *idx = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_INDEX);
    /* INDEX gets bus/devfn only — register offset goes in the DATA read address */
    *idx = (rp1_bus << 20) | (0U << 12);
    __asm__ volatile("dsb sy" ::: "memory");
    return *(volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_DATA + (reg & 0xFFC));
}

static void rp1_cfg_write32(uint32_t reg, uint32_t val)
{
    volatile uint32_t *idx = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_INDEX);
    *idx = (rp1_bus << 20) | (0U << 12);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_DATA + (reg & 0xFFC)) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif /* config space access functions */

void uart_irq_init(void)
{
    rx_head = 0;
    rx_tail = 0;
    uart_irq_mode = 0;

    /*
     * MIP-based interrupt path (IRQ 185 = MIP0 vec 25 → SPI 153).
     * SPIs in range 128-191 are non-secure writable on BCM2712.
     * SPIs >= 224 (including INTA at SPI 229) are secure-only.
     */

    /* 0. Disable MSI-X, program table, then re-enable.
     * boot.S enables MSI-X from EL2 before the table is programmed.
     * The RP1 may latch table addresses at Enable time. Toggling
     * Enable around table programming ensures the RP1 picks up our
     * addresses (0xFF_FFFFF000 for BAR3→MIP0 routing).
     *
     * MSI-X capability at RP1 config offset 0xB0 (via EXT_CFG).
     * Bit 31 = Enable, Bit 30 = Function Mask. */
    volatile uint32_t *ext_cfg_idx = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_INDEX);
    volatile uint32_t *msix_cap = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_EXT_CFG_DATA + 0xB0);
    *ext_cfg_idx = 0;  /* bus=0, devfn=0 */
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t cap_val = *msix_cap;
    /* Disable MSI-X (clear Enable, set Function Mask) */
    cap_val &= ~(1U << 31);  /* Clear Enable */
    cap_val |= (1U << 30);   /* Set Function Mask */
    /* Note: config space writes may hang from EL1 on BCM2712.
     * If this hangs, the table re-latch theory is wrong. */
    *msix_cap = cap_val;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Program MSI-X table with BAR3→MIP0 target address */
    for (int i = 0; i < RP1_MSIX_TABLE_SIZE; i++) {
        volatile uint32_t *entry = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + i * 16);
        entry[0] = MSIX_MSG_ADDR_LO;
        entry[1] = MSIX_MSG_ADDR_HI;
        entry[2] = (uint32_t)i;
        entry[3] = 0x00000000;  /* Unmasked */
    }
    __asm__ volatile("dsb sy" ::: "memory");
    DEBUG_PRINT("MSI-X table: %d entries programmed", RP1_MSIX_TABLE_SIZE);

    /* Re-enable MSI-X (RP1 should latch new table addresses) */
    cap_val |= (1U << 31);   /* Set Enable */
    cap_val &= ~(1U << 30);  /* Clear Function Mask */
    *msix_cap = cap_val;
    __asm__ volatile("dsb sy" ::: "memory");

    /* 1. GIC: enable SPI with higher priority than timer */
    gic_set_priority(UART_IRQ, 0x40);
    gic_enable_irq(UART_IRQ);

    /* 2. MSIX_CFG: enable vector 25, then set IACK_EN.
     * Linux rp1_irqchip does these as SEPARATE writes to the SET alias:
     *   irq_activate: writel(ENABLE, SET + MSIX_CFG(n))
     *   irq_set_type: writel(IACK_EN, SET + MSIX_CFG(n))  [for level-triggered]
     * Our previous single write (ENABLE|IACK_EN) may not properly initialize
     * the engine — the RP1 may need ENABLE to take effect before IACK_EN. */
    volatile uint32_t *msix_set = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_SET
                                                         + RP1_MSIX_CFG(RP1_INT_UART0));
    *msix_set = MSIX_CFG_ENABLE;
    __asm__ volatile("dsb sy" ::: "memory");
    *msix_set = MSIX_CFG_IACK_EN;
    __asm__ volatile("dsb sy" ::: "memory");

    /* 3. PL011: configure, drain FIFO, clear ICR, enable IMSC */
    volatile uint32_t *uart_ifls = (volatile uint32_t *)(RP1_UART0_BASE + UART_IFLS);
    *uart_ifls = (*uart_ifls & ~(0x7 << 3)) | (0x0 << 3);
    __asm__ volatile("dsb sy" ::: "memory");

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

    volatile uint32_t *uart_imsc = (volatile uint32_t *)(RP1_UART0_BASE + UART_IMSC);

    /* 4. Clear ALL stale state with IMSC disabled, then IACK, then enable IMSC.
     * IMSC must be 0 when we IACK so PL011 line is LOW and MSIX_CFG doesn't
     * immediately re-fire + auto-mask (level-triggered livelock). */
    *uart_imsc = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");
    while (!(*uart_fr & FR_RXFE)) {
        (void)*uart_dr;
    }
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");

    {
        uint32_t pend_reg = UART_IRQ / 32;
        uint32_t pend_bit = UART_IRQ % 32;
        volatile uint32_t *icpendr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x280 + 4 * pend_reg);
        *icpendr = (1U << pend_bit);
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /* IACK with PL011 line LOW (IMSC=0) — re-arms MSIX_CFG */
    *msix_set = MSIX_CFG_IACK;
    __asm__ volatile("dsb sy" ::: "memory");

    /* NOW enable PL011 IMSC */
    *uart_imsc = IMSC_RXIM | IMSC_RTIM;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Diagnostic: check if IMSC enable triggered MSI-X→MIP0 delivery */
    {
        volatile uint32_t *mip_stat = (volatile uint32_t *)(MIP0_BASE + 0x80);
        volatile uint32_t *mip_raised = (volatile uint32_t *)(MIP0_BASE + MIP_INT_RAISED);
        volatile uint32_t *gic_ispendr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x200 + (UART_IRQ/32)*4);
        uint32_t mip_s = *mip_stat;
        uint32_t mip_r = *mip_raised;
        uint32_t gic_p = *gic_ispendr;
        uint32_t mis = *(volatile uint32_t *)(RP1_UART0_BASE + UART_MIS);
        uint32_t intstat = *(volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_INTSTATL);
        INFO("UART IRQ: post-init diag: MIS=0x%x INTSTAT=0x%x MIP_status=0x%x MIP_raised=0x%x GIC_ISPENDR=0x%x",
             mis, intstat, mip_s, mip_r, gic_p);
    }
    /* Mark IRQ path ready — uart_getc will now use ring buffer exclusively */
    uart_irq_ready = 1;

    INFO("UART IRQ enabled (GIC IRQ %d = SPI %d, RP1 vec %d)",
         UART_IRQ, UART_IRQ - 32, RP1_INT_UART0);
}

#if 0 /* Old code preserved for reference */
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
     * IMSC, RP1 MSIX_CFG, RC BAR3→MIP routing, and MIP registers are
     * all configured below and ready for when PCIe config access works.
     *
     * Polling fallback works transparently in the meantime.
     */
    /*
     * Read PCIe RC PCIE_STATUS to verify link is active.
     * This is a basic accessibility test for the RC register block.
     */
    {
        volatile uint32_t *pcie_status = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_PCIE_STATUS);
        uint32_t status = *pcie_status;
        DEBUG_PRINT("PCIe RC status at 0x%lx = 0x%x (DL_ACTIVE=%d)",
                    (uint64_t)(PCIE_RC_BASE + PCIE_RC_PCIE_STATUS),
                    status, (status >> 5) & 1);

        if (!(status & (1 << 5))) {
            INFO("UART IRQ: PCIe link not active, skipping MSI-X setup");
            goto skip_msix;
        }

        /* Use 0x8000 for reads (works from EL1, writes done from EL2) */
        rp1_bus = 0;
        uint32_t rp1_id = rp1_cfg_read32(0x00);
        DEBUG_PRINT("RP1 config: id=0x%x (vendor=0x%x)", rp1_id, rp1_id & 0xFFFF);

        if ((rp1_id & 0xFFFF) != 0x1de4) {
            INFO("UART IRQ: RP1 not found (id=0x%x), polling fallback", rp1_id);
            goto skip_msix;
        }

        /* Ensure Bus Master is enabled */
        uint32_t cmd_status = rp1_cfg_read32(0x04);
        uint16_t cmd = cmd_status & 0xFFFF;
        if (!(cmd & (1 << 2))) {
            rp1_cfg_write32(0x04, cmd_status | (1 << 2));
            DEBUG_PRINT("RP1: enabled Bus Master");
        }

        /* Walk capability list to find MSI-X (cap ID 0x11).
         * Config reads must be 4-byte aligned; extract bytes by shift. */
        {
            uint32_t word34 = rp1_cfg_read32(0x34);
            uint8_t cap_ptr = word34 & 0xFF;
            int limit = 48;
            while (cap_ptr >= 0x40 && cap_ptr != 0xFF && limit-- > 0) {
                uint32_t aligned_addr = cap_ptr & ~3U;
                uint32_t word = rp1_cfg_read32(aligned_addr);
                uint32_t byte_offset = cap_ptr & 3;
                uint8_t cap_id = (word >> (byte_offset * 8)) & 0xFF;
                uint8_t next = (word >> (byte_offset * 8 + 8)) & 0xFF;
                DEBUG_PRINT("  cap at 0x%x: id=0x%x next=0x%x", cap_ptr, cap_id, next);
                if (cap_id == 0x11) {
                    msix_cap = cap_ptr;
                    break;
                }
                cap_ptr = next;
            }
        }

        if (!msix_cap) {
            INFO("UART IRQ: MSI-X cap not found in RP1 config");
            goto skip_msix;
        }

        /* MSI-X Message Control is at cap_offset + 2 (16-bit).
         * Read the 4-byte aligned word containing the cap header. */
        uint32_t msix_aligned = msix_cap & ~3U;
        uint32_t msix_word = rp1_cfg_read32(msix_aligned);
        uint32_t byte_off = msix_cap & 3;
        /* Message Control is at cap+2, which is 2 bytes above cap_id */
        uint16_t msix_flags = (uint16_t)(msix_word >> ((byte_off + 2) * 8));
        if (byte_off + 2 >= 4) {
            /* Flags span into next 32-bit word */
            msix_flags = (uint16_t)(rp1_cfg_read32(msix_aligned + 4));
        }
        uint16_t table_size = (msix_flags & 0x7FF) + 1;
        DEBUG_PRINT("RP1 MSI-X: cap=0x%x flags=0x%x table_size=%d",
                    msix_cap, msix_flags, table_size);

        /* Skip MSI-X enable for now — just read BAR info for diagnostics */
        DEBUG_PRINT("RP1 MSI-X: cap found, skipping enable for diagnostics");
    }

    /* Read the MSI-X Table BIR and offset from config space to find the actual table location */
    {
        uint32_t table_off_bir = rp1_cfg_read32(msix_cap + 4);
        uint32_t pba_off_bir = rp1_cfg_read32(msix_cap + 8);
        uint32_t table_bir = table_off_bir & 0x7;
        uint32_t table_offset = table_off_bir & ~0x7;
        uint32_t pba_bir = pba_off_bir & 0x7;
        uint32_t pba_offset = pba_off_bir & ~0x7;
        DEBUG_PRINT("RP1 MSI-X: table BIR=%u offset=0x%x, PBA BIR=%u offset=0x%x",
                    table_bir, table_offset, pba_bir, pba_offset);

        /* Read BAR0 to find MSI-X table address */
        uint32_t bar0_lo = rp1_cfg_read32(0x10);
        uint32_t bar0_hi = rp1_cfg_read32(0x14);
        DEBUG_PRINT("RP1 BAR0=0x%x_%08x (MSI-X table at BAR0+0x%x)",
                    bar0_hi, bar0_lo, table_offset);
    }

    /* Read outbound window config to determine CPU→PCIe address mapping.
     * The outbound window translates CPU physical addresses to PCIe addresses
     * that the RP1 sees. BAR0 PCIe addr = 0x00410000 → CPU addr = ? */
    /* Dump firmware's PCIe RC window configuration */
    {
        /* Outbound window 0 (CPU to PCIe) */
        uint32_t w0_lo  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x400C);
        uint32_t w0_hi  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4010);
        uint32_t w0_bl  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4070);
        uint32_t w0_bhi = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4080);
        uint32_t w0_lhi = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4084);
        DEBUG_PRINT("Out win0: pci=0x%x_%08x baselim=0x%08x bhi=0x%x lhi=0x%x",
                    w0_hi, w0_lo, w0_bl, w0_bhi, w0_lhi);

        /* Inbound BARs */
        uint32_t b1_lo  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x402C);
        uint32_t b1_hi  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4030);
        uint32_t b1_rl  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x40AC);
        uint32_t b1_rh  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x40B0);
        DEBUG_PRINT("BAR1: cfg=0x%x_%08x remap=0x%x_%08x", b1_hi, b1_lo, b1_rh, b1_rl);

        uint32_t b2_lo  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4034);
        uint32_t b2_hi  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4038);
        uint32_t b2_rl  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x40B4);
        uint32_t b2_rh  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x40B8);
        DEBUG_PRINT("BAR2: cfg=0x%x_%08x remap=0x%x_%08x", b2_hi, b2_lo, b2_rh, b2_rl);

        uint32_t b3_lo  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x403C);
        uint32_t b3_hi  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4040);
        uint32_t b3_rl  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x40BC);
        uint32_t b3_rh  = *(volatile uint32_t *)(PCIE_RC_BASE + 0x40C0);
        DEBUG_PRINT("BAR3: cfg=0x%x_%08x remap=0x%x_%08x", b3_hi, b3_lo, b3_rh, b3_rl);

        uint32_t misc = *(volatile uint32_t *)(PCIE_RC_BASE + 0x4008);
        DEBUG_PRINT("MISC_CTRL=0x%08x", misc);

        /* Read first few MSI-X table entries at BAR0 (0x1F00410000) */
        for (int i = 0; i < 3; i++) {
            volatile uint32_t *e = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + i * 16);
            DEBUG_PRINT("MSIX[%d]: addr=0x%x_%08x data=0x%x ctrl=0x%x",
                        i, e[1], e[0], e[2], e[3]);
        }
        /* Also check vector 25 (UART0) */
        {
            volatile uint32_t *e = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + 25 * 16);
            DEBUG_PRINT("MSIX[25/UART0]: addr=0x%x_%08x data=0x%x ctrl=0x%x",
                        e[1], e[0], e[2], e[3]);
        }

        /* Program MSI-X table entries from EL1 (BAR0 space, not RC) */
        DEBUG_PRINT("Programming MSI-X table (%d entries)...", RP1_MSIX_TABLE_SIZE);
        for (int i = 0; i < RP1_MSIX_TABLE_SIZE; i++) {
            volatile uint32_t *e = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + i * 16);
            e[0] = MSIX_MSG_ADDR_LO;    /* 0xFFFFF000 */
            e[1] = MSIX_MSG_ADDR_HI;    /* 0x000000FF */
            e[2] = (uint32_t)i;          /* data = vector number */
            e[3] = 0x00000000;           /* ctrl: unmasked */
        }
        __asm__ volatile("dsb sy" ::: "memory");

        /* Verify one entry */
        {
            volatile uint32_t *e = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + 25 * 16);
            DEBUG_PRINT("MSIX[25] after write: addr=0x%x_%08x data=0x%x ctrl=0x%x",
                        e[1], e[0], e[2], e[3]);
        }
    }

skip_msix:
    (void)0;

    /*
     * Step 1: Verify PCIe RC BAR3 → MIP0 routing (configured from EL2 in boot.S).
     *
     * When RP1 fires an MSI-X, it writes to PCIe address 0xFF_FFFFF000.
     * RC BAR3 catches that write and remaps it to physical 0x10_00130000
     * (MIP0), which converts the MSI-X vector into a GIC SPI.
     * BAR1 is used by firmware for RP1 peripheral MMIO — do not touch.
     */
    {
        volatile uint32_t *rc_bar3_lo = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_LO);
        volatile uint32_t *rc_remap_lo = (volatile uint32_t *)(PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP);
        uint32_t bar3 = *rc_bar3_lo;
        uint32_t remap = *rc_remap_lo;
        if (bar3 == 0xFFFFF01C && remap == 0x00130001) {
            DEBUG_PRINT("RC BAR3→MIP0 routing verified (configured from EL2)");
        } else {
            INFO("UART IRQ: RC BAR3 not configured (lo=0x%x remap=0x%x)", bar3, remap);
        }
    }

    /* MIP0 initialization is done from EL2 in boot.S (write access requires EL2).
     * Verify the MIP0 mask registers are set correctly. */
    {
        volatile uint32_t *mip_maskl = (volatile uint32_t *)(MIP0_BASE + MIP_INT_MASKL_HOST);
        uint32_t expected = ~(1U << RP1_INT_UART0);  /* All masked except UART0 */
        uint32_t actual = *mip_maskl;
        if (actual == expected) {
            DEBUG_PRINT("MIP0 verified (UART0 unmasked, configured from EL2)");
        } else {
            DEBUG_PRINT("MIP0 mask: expected 0x%x got 0x%x", expected, actual);
        }
    }

    /*
     * Enable the interrupt path in reverse order (sink→source) so the
     * handler is ready before the interrupt can fire:
     *   1. GIC (handler ready)
     *   2. RP1 MSIX_CFG (MSI-X vector forwarding)
     *   3. PL011 IMSC (interrupt source — last, arms the trigger)
     */

    /*
     * Enable the interrupt chain. Critical ordering:
     * 1. GIC handler ready first
     * 2. MSIX_CFG enabled (will auto-mask on first assertion)
     * 3. PL011 configured: drain FIFO, clear ICR, enable IMSC
     * 4. Clear all stale state (GIC pending, PL011 ICR)
     * 5. IACK MSIX_CFG LAST — this unmasks for the first real interrupt
     *
     * The IACK must be the absolute last step because with IACK_EN,
     * any PL011 assertion while MSIX_CFG is unmasked will immediately
     * fire an MSI-X and auto-mask. So we must ensure the PL011 is
     * quiescent (FIFO empty, ICR cleared) before the final IACK.
     */

    /* 1. GIC: enable SPI with higher priority than timer (lower value = higher priority) */
    gic_set_priority(UART_IRQ, 0x40);  /* Higher priority than timer (0x80) */
    gic_enable_irq(UART_IRQ);

    /* 2. MSIX_CFG: enable vector 25 with IACK_EN (auto-mask after first fire) */
    volatile uint32_t *msix_set = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_SET
                                                         + RP1_MSIX_CFG(RP1_INT_UART0));
    *msix_set = MSIX_CFG_ENABLE | MSIX_CFG_IACK_EN;
    __asm__ volatile("dsb sy" ::: "memory");

    /* 3. PL011: configure FIFO level, drain stale data, enable IMSC */
    volatile uint32_t *uart_ifls = (volatile uint32_t *)(RP1_UART0_BASE + UART_IFLS);
    *uart_ifls = (*uart_ifls & ~(0x7 << 3)) | (0x0 << 3);
    __asm__ volatile("dsb sy" ::: "memory");

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

    volatile uint32_t *uart_imsc = (volatile uint32_t *)(RP1_UART0_BASE + UART_IMSC);
    *uart_imsc = IMSC_RXIM | IMSC_RTIM;
    __asm__ volatile("dsb sy" ::: "memory");

    /* 4. Clear all stale interrupt state */
    /* Clear PL011 ICR one more time (IMSC enable may have triggered) */
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");
    /* Drain any last-moment FIFO data */
    while (!(*uart_fr & FR_RXFE)) {
        (void)*uart_dr;
    }
    *uart_icr = 0x7FF;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Clear GIC pending (from any MSI-X that fired during init) */
    {
        uint32_t pend_reg = UART_IRQ / 32;
        uint32_t pend_bit = UART_IRQ % 32;
        volatile uint32_t *icpendr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x280 + 4 * pend_reg);
        *icpendr = (1U << pend_bit);
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /* 5. IACK — unmask MSIX_CFG for the first real interrupt.
     * PL011 should be quiescent now (FIFO empty, ICR cleared).
     * The next character that arrives will trigger the full path. */
    *msix_set = MSIX_CFG_IACK;
    __asm__ volatile("dsb sy" ::: "memory");

#endif /* old MSI-X approach */

/*
 * Check if UART is in interrupt-driven mode.
 * Returns 1 if IRQ 153 has fired at least once, 0 if polling.
 */
int uart_is_irq_mode(void)
{
    return uart_irq_mode;
}

#endif /* UART_TYPE_RP1 */
