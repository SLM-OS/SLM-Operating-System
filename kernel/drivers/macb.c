/**
 * Cadence MACB/GEM Ethernet Driver for Raspberry Pi 5 (#202)
 *
 * Drives the Gigabit Ethernet controller in the RP1 southbridge.
 * Despite the Broadcom SoC (BCM2712), the MAC is actually Cadence
 * MACB/GEM IP, not Broadcom GENET — Linux device tree identifies
 * it as `compatible = "raspberrypi,rp1-gem", "cdns,macb"`. GENET
 * is the Pi 4 / earlier Pi Ethernet; Pi 5 chose a different IP.
 *
 * Hardware:
 *   - Cadence MACB/GEM with Gigabit support (GEM mode)
 *   - BCM54213PE PHY on MDIO address 1
 *   - RP1 offset 0x100000 (MMIO)   = CPU phys 0x1F00100000
 *   - RP1 offset 0x018000 (clocks) = CPU phys 0x1F00018000
 *   - PHY reset via RP1 GPIO 32 (active low, 5 ms pulse)
 *   - IRQ: GIC 166 (MIP0 vec 6, SPI 134)
 *
 * Staged development per #202 plan:
 *   Stage 0 (landed): build scaffolding
 *   Stage 1 (this file): probe MID register, confirm IP revision
 *   Stage 2: RP1 clock enable + MDIO + PHY bring-up
 *   Stage 3: TX ring + polled send
 *   Stage 4: RX ring + polled receive
 *   Stage 5: lwIP + DHCP + ping
 *   Stage 6: regression tests + optional IRQ upgrade
 *
 * Polled-only by design today — Pi 5 NS-EL1 IRQ delivery is
 * unreliable (#134), so net_poll() drives tx_reap and recv.
 */

#include "platform.h"

#if defined(PLATFORM_RASPI5) && defined(ENABLE_NETWORKING)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "net.h"            /* net_stats_rx_no_buffers_inc */
#include "net_driver.h"
#include "debug.h"
#include "timer.h"          /* sleep_ms / sleep_us */
#include "cache.h"          /* cache_clean_range for DMA coherency */
#include "macb.h"

/* -------------------------------------------------------------------------- */
/* Cadence MACB/GEM register map                                               */
/*                                                                             */
/* Offsets taken from Linux drivers/net/ethernet/cadence/macb.h (cached at    */
/* docs/reference/linux-cadence-macb.h). Only the subset the driver actually   */
/* touches is defined here; the Linux header has the full set.                 */
/* -------------------------------------------------------------------------- */

#define MACB_NCR            0x0000  /* Network Control */
#define MACB_NCFGR          0x0004  /* Network Config */
#define MACB_NSR            0x0008  /* Network Status */
#define MACB_TSR            0x0014  /* Transmit Status */
#define MACB_RBQP           0x0018  /* RX Queue Base Pointer (low 32) */
#define MACB_TBQP           0x001C  /* TX Queue Base Pointer (low 32) */
#define MACB_RSR            0x0020  /* Receive Status */
#define MACB_ISR            0x0024  /* Interrupt Status */
#define MACB_IER            0x0028  /* Interrupt Enable */
#define MACB_IDR            0x002C  /* Interrupt Disable */
#define MACB_IMR            0x0030  /* Interrupt Mask */
#define MACB_MAN            0x0034  /* PHY Maintenance (MDIO) */
#define MACB_HRB            0x0090  /* Hash Address Bottom */
#define MACB_HRT            0x0094  /* Hash Address Top */
#define MACB_SA1B           0x0098  /* Specific Address 1 Bottom */
#define MACB_SA1T           0x009C  /* Specific Address 1 Top */
#define MACB_MID            0x00FC  /* Module ID — version probe */
#define MACB_TBQPH          0x04C8  /* TX Queue Base Pointer (upper 32) */
#define MACB_RBQPH          0x04D4  /* RX Queue Base Pointer (upper 32) */

/* GEM-mode registers at different offsets than MACB-base layout */
#define GEM_USRIO           0x000C
#define GEM_DMACFG          0x0010
#define GEM_JML             0x0048

/* MACB_MID layout:
 *   [31:16] identification number
 *   [15:12] revision
 *   [11:0]  fabrication identifier
 */
#define MACB_MID_IDNUM_SHIFT    16
#define MACB_MID_IDNUM_MASK     0xFFFF
#define MACB_MID_REV_SHIFT      12
#define MACB_MID_REV_MASK       0xF
#define MACB_MID_FAB_SHIFT      0
#define MACB_MID_FAB_MASK       0xFFF

/* MACB_NCR (Network Control Register) bit fields */
#define MACB_NCR_RE             (1u << 2)   /* Receive enable */
#define MACB_NCR_TE             (1u << 3)   /* Transmit enable */
#define MACB_NCR_MPE            (1u << 4)   /* Management port enable (MDIO) */
#define MACB_NCR_CLRSTAT        (1u << 5)   /* Clear statistics */
#define MACB_NCR_TSTART         (1u << 9)   /* Start transmission */

/* NCFGR layout differs between MACB-class (10/100) and GEM-class (1G).
 * Pi 5 is GEM (idnum 0x0007 Cadence GEM per MACB_MID probe):
 *   - bit 10:    GBE (Gigabit mode enable)
 *   - bits [20:18]: MDC clock divider
 * In MACB-class IPs these would be CLK at [12:10] and no GBE bit. */
#define GEM_NCFGR_GBE           (1u << 10)  /* Gigabit mode enable */
#define GEM_NCFGR_CLK_SHIFT     18          /* MDC divider, bits [20:18] */
#define GEM_NCFGR_CLK_MASK      0x7
/* MDC divider values: 0=÷8, 1=÷16, 2=÷32, 3=÷48, 4=÷64, 5=÷96, 6=÷128, 7=÷224 */
#define GEM_NCFGR_CLK_DIV96     0x5         /* Safe default for pclk around 100 MHz */

/* NCFGR bits the RX path needs — without DRFCS the 4-byte FCS hangs
 * off every received frame and confuses lwIP; without BIG the MAC
 * silently drops any frame > 1518 bytes. RBOF = 2 offsets the start
 * of data in each RX buffer so the IP header lands at a 4-byte
 * boundary (helps unaligned-access-sensitive code paths). NBC cleared
 * = accept broadcast. */
#define MACB_NCFGR_CAF          (1u << 4)   /* Copy all frames (promisc) */
#define MACB_NCFGR_NBC          (1u << 5)   /* No broadcast */
#define MACB_NCFGR_BIG          (1u << 8)   /* Receive 1536-byte frames */
#define MACB_NCFGR_PAE          (1u << 13)  /* Pause enable */
#define MACB_NCFGR_RBOF_SHIFT   14          /* RX buffer offset [15:14] */
#define MACB_NCFGR_RBOF_MASK    0x3
#define MACB_NCFGR_RBOF_2       0x2         /* 2-byte offset — IP align */
#define MACB_NCFGR_DRFCS        (1u << 17)  /* Discard RX FCS */

/* MACB_NSR (Network Status Register) bits */
#define MACB_NSR_IDLE           (1u << 2)   /* MDIO idle */
#define MACB_NSR_MDIO           (1u << 1)   /* MDIO input state (hardware only) */

/* MACB_MAN (PHY Maintenance Register — MDIO command) format */
#define MACB_MAN_SOF_C22        (1u << 30)  /* Start-of-frame for Clause 22 */
#define MACB_MAN_RW_READ        (2u << 28)
#define MACB_MAN_RW_WRITE       (1u << 28)
#define MACB_MAN_CODE           (2u << 16)  /* Turnaround code, must be 10b */
#define MACB_MAN_PHYA_SHIFT     23
#define MACB_MAN_REGA_SHIFT     18
#define MACB_MAN_DATA_MASK      0xFFFF

/* BCM54213PE PHY — sits at MDIO address 1 per Pi 5 DTS. Standard
 * MII register numbers: BMCR=0, BMSR=1, PHY_ID1=2, PHY_ID2=3. */
#define PHY_ADDR                1
#define MII_BMCR                0
#define MII_BMSR                1
#define MII_PHYID1              2
#define MII_PHYID2              3
#define MII_ADVERTISE           4
#define MII_LPA                 5
#define MII_CTRL1000            9       /* 1000BASE-T control */
#define MII_STAT1000            10      /* 1000BASE-T status */

/* BMCR (reg 0) bits */
#define BMCR_RESET              (1u << 15)
#define BMCR_LOOPBACK           (1u << 14)
#define BMCR_SPEED100           (1u << 13)
#define BMCR_ANENABLE           (1u << 12)
#define BMCR_PDOWN              (1u << 11)
#define BMCR_ISOLATE            (1u << 10)
#define BMCR_ANRESTART          (1u << 9)
#define BMCR_FULLDPLX           (1u << 8)
#define BMCR_SPEED1000          (1u << 6)

/* BMSR (reg 1) bits */
#define BMSR_LSTATUS            (1u << 2)   /* Link status (latched low) */
#define BMSR_ANEGCAPABLE        (1u << 3)
#define BMSR_ANEGCOMPLETE       (1u << 5)

/* Broadcom OUI — top 22 bits of PHY_ID encode the OUI; Broadcom is 0x001F */
#define BCM_PHY_OUI_MSB         0x0040      /* PHYID1 for BCM phys */
#define BCM54213PE_PHYID_LOW    0x600D      /* PHYID2 for BCM54213PE */

/* MACB_NCFGR speed/duplex bits */
#define MACB_NCFGR_SPD          (1u << 0)   /* 100 Mbps */
#define MACB_NCFGR_FD           (1u << 1)   /* Full duplex */
#define MACB_NCFGR_GIGE         (1u << 10)  /* Ugh actually GIGE is in NCR */

/* MACB_NSR / TSR — TSR bits used by the TX poll path */
#define MACB_TSR_UBR            (1u << 0)   /* Used bit read */
#define MACB_TSR_COL            (1u << 1)   /* Collision */
#define MACB_TSR_RLE            (1u << 2)   /* Retry limit exceeded */
#define MACB_TSR_TGO            (1u << 3)   /* TX go — set while transmitting */
#define MACB_TSR_COMP           (1u << 5)   /* TX complete (write-1-to-clear) */

/* MACB DMA descriptor */
struct macb_dma_desc {
    uint32_t addr;
    uint32_t ctrl;
};

/* TX descriptor ctrl-word bit fields */
#define MACB_TX_FRMLEN_MASK     0x3FFF      /* [13:0] — GEM uses 14 bits */
#define MACB_TX_LAST            (1u << 15)
#define MACB_TX_ERROR           (1u << 29)
#define MACB_TX_WRAP            (1u << 30)
#define MACB_TX_USED            (1u << 31)  /* Set by MAC when TX done */

/* RX descriptor addr-word bit fields (different from TX — on RX the
 * USED/WRAP bits live in the ADDR field, buffer address is bits [31:2]
 * which implies a 4-byte alignment requirement for RX buffers). */
#define MACB_RX_USED            (1u << 0)   /* Set by MAC when frame written */
#define MACB_RX_WRAP            (1u << 1)
#define MACB_RX_ADDR_MASK       0xFFFFFFFC  /* Buffer address (4-byte aligned) */

/* RX descriptor ctrl-word bit fields */
#define MACB_RX_FRMLEN_MASK     0x1FFF      /* [12:0] — jumbo support wider */
#define MACB_RX_SOF             (1u << 14)
#define MACB_RX_EOF             (1u << 15)

/* DMACFG (GEM DMA configuration) field definitions we use */
#define GEM_DMACFG_FBL_SHIFT    0           /* Fixed burst length [4:0] */
#define GEM_DMACFG_FBL_MASK     0x1F
#define GEM_DMACFG_FBL_16       16          /* Matches raspberrypi_rp1_config */
#define GEM_DMACFG_ENDIA_PKT    (1u << 7)
#define GEM_DMACFG_RXBMS_SHIFT  8           /* RX packet buffer size select [9:8] */
#define GEM_DMACFG_RXBMS_MASK   0x3
#define GEM_DMACFG_TXPBMS       (1u << 10)  /* TX packet buffer size = full */
#define GEM_DMACFG_RXBS_SHIFT   16          /* DMA receive buffer size units of 64B */
#define GEM_DMACFG_RXBS_MASK    0xFF
#define GEM_DMACFG_DDRP         (1u << 24)  /* Discard-when-no-AHB */

/* Ring sizes (powers of 2 for cheap masking) */
#define MACB_TX_RING_SIZE       16
#define MACB_TX_BUF_SIZE        2048        /* Headroom above Ethernet MTU */
#define MACB_RX_RING_SIZE       16
#define MACB_RX_BUF_SIZE        1536        /* Exactly one Ethernet frame */
#define MACB_RX_BUF_UNITS_64    (MACB_RX_BUF_SIZE / 64)  /* 1536/64 = 24 */

/* -------------------------------------------------------------------------- */
/* MMIO helpers                                                                */
/* -------------------------------------------------------------------------- */

static inline uint32_t macb_readl(uint32_t reg)
{
    return *(volatile uint32_t *)(RP1_ETH_IP_BASE + reg);
}

static inline void macb_writel(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(RP1_ETH_IP_BASE + reg) = val;
}

static inline uint32_t rp1_clk_readl(uint32_t reg)
{
    return *(volatile uint32_t *)(RP1_CLOCKS_BASE + reg);
}

static inline void rp1_clk_writel(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(RP1_CLOCKS_BASE + reg) = val;
}

/* -------------------------------------------------------------------------- */
/* MDIO (PHY management) — Clause 22 via MACB_MAN                              */
/* -------------------------------------------------------------------------- */

/* Wait up to ~10 ms for MDIO to become idle. Returns 0 on success, -1 on
 * timeout — caller treats timeout as a probe failure and logs it. Uses
 * sleep_us + timer counter so the wait respects cooperative scheduling. */
static int macb_mdio_wait_idle(void)
{
    for (int i = 0; i < 1000; i++) {
        if (macb_readl(MACB_NSR) & MACB_NSR_IDLE) {
            return 0;
        }
        sleep_us(10);
    }
    return -1;
}

/* Read one 16-bit PHY register via MDIO. Returns value on success, or
 * 0xFFFF on timeout (every PHY MMD defaults to 0xFFFF for unimplemented
 * registers, so the caller can still distinguish via context). */
static uint16_t macb_mdio_read(uint8_t phy, uint8_t reg)
{
    if (macb_mdio_wait_idle() < 0)
        return 0xFFFF;

    macb_writel(MACB_MAN,
                MACB_MAN_SOF_C22 |
                MACB_MAN_RW_READ |
                ((uint32_t)phy << MACB_MAN_PHYA_SHIFT) |
                ((uint32_t)reg << MACB_MAN_REGA_SHIFT) |
                MACB_MAN_CODE);

    if (macb_mdio_wait_idle() < 0)
        return 0xFFFF;

    return (uint16_t)(macb_readl(MACB_MAN) & MACB_MAN_DATA_MASK);
}

__attribute__((unused))
static int macb_mdio_write(uint8_t phy, uint8_t reg, uint16_t val)
{
    if (macb_mdio_wait_idle() < 0)
        return -1;

    macb_writel(MACB_MAN,
                MACB_MAN_SOF_C22 |
                MACB_MAN_RW_WRITE |
                ((uint32_t)phy << MACB_MAN_PHYA_SHIFT) |
                ((uint32_t)reg << MACB_MAN_REGA_SHIFT) |
                MACB_MAN_CODE |
                ((uint32_t)val & MACB_MAN_DATA_MASK));

    if (macb_mdio_wait_idle() < 0)
        return -1;

    return 0;
}

/* -------------------------------------------------------------------------- */
/* Driver state                                                                */
/* -------------------------------------------------------------------------- */

static struct {
    bool     initialized;
    uint8_t  mac[6];
    bool     link_up;
    uint32_t macb_mid;          /* MID register contents (diagnostics) */
    uint16_t phy_id1;           /* MII_PHYID1 — Broadcom OUI high */
    uint16_t phy_id2;           /* MII_PHYID2 — OUI low + model + rev */
    uint32_t link_speed_mbps;   /* 10 / 100 / 1000 after auto-neg */
    bool     link_full_duplex;
    unsigned tx_head;           /* Next slot we'll fill */
    unsigned tx_tail;           /* Next slot to reap for completion */
    unsigned rx_head;           /* Next slot to inspect for incoming frames */
} macb_state;

/* TX ring + buffer pool. 4 KB alignment is overkill for the descriptor
 * ring (needs 8-byte alignment) but matches the cacheline-pair alignment
 * the Pi 5 cache maintenance helpers assume and keeps TBQP setup
 * trivial. Buffers get 16-byte alignment, same as the virtio drivers. */
static struct macb_dma_desc
    tx_ring[MACB_TX_RING_SIZE] __attribute__((aligned(4096)));
static uint8_t
    tx_buffers[MACB_TX_RING_SIZE][MACB_TX_BUF_SIZE] __attribute__((aligned(16)));

/* RX ring + buffer pool. Buffers are 4-byte aligned at minimum (addr
 * field's low 2 bits are USED/WRAP). We use 64-byte alignment to match
 * RXBS granularity and keep cacheline semantics clean. */
static struct macb_dma_desc
    rx_ring[MACB_RX_RING_SIZE] __attribute__((aligned(4096)));
static uint8_t
    rx_buffers[MACB_RX_RING_SIZE][MACB_RX_BUF_SIZE] __attribute__((aligned(64)));

/* -------------------------------------------------------------------------- */
/* Stage 2 helpers — clock enable, MACB bring-up, PHY bring-up                 */
/* -------------------------------------------------------------------------- */

/* Enable the two RP1 clocks the MACB needs. They may already be on
 * (Pi firmware often leaves them running), but the enable bit is
 * idempotent so writing unconditionally is safe. No parent/divider
 * change is attempted — whatever firmware set is honoured. */
static void macb_enable_clocks(void)
{
    uint32_t ctrl;

    ctrl = rp1_clk_readl(RP1_CLK_ETH_CTRL);
    rp1_clk_writel(RP1_CLK_ETH_CTRL, ctrl | RP1_CLK_CTRL_ENABLE);

    ctrl = rp1_clk_readl(RP1_CLK_ETH_TSU_CTRL);
    rp1_clk_writel(RP1_CLK_ETH_TSU_CTRL, ctrl | RP1_CLK_CTRL_ENABLE);

    /* Let clocks stabilize before touching MACB registers. */
    sleep_us(100);
}

/* Bring up just enough of the MACB to talk to the PHY over MDIO.
 * Sets a conservative MDC divider and enables the management port.
 * Data-path enables (RE/TE) stay off until the rings are built in
 * Stages 3/4. */
static void macb_mdio_bringup(void)
{
    uint32_t ncfgr = macb_readl(MACB_NCFGR);
    ncfgr &= ~(GEM_NCFGR_CLK_MASK << GEM_NCFGR_CLK_SHIFT);
    ncfgr |= (GEM_NCFGR_CLK_DIV96 << GEM_NCFGR_CLK_SHIFT);
    macb_writel(MACB_NCFGR, ncfgr);

    uint32_t ncr = macb_readl(MACB_NCR);
    ncr |= MACB_NCR_MPE;
    macb_writel(MACB_NCR, ncr);
}

/* Drive RP1 GPIO 32 high to release BCM54213PE's reset line.
 *
 * Per Pi 5 DTS the PHY reset-gpio is GPIO 32, active low, 5 ms.
 * GPIO 32 sits in RP1 IO_BANK1 (at RP1+0x0D4000), RIO_BANK1 (at
 * +0x0E4000), PADS_BANK1 (at +0x0F4000). Bank 1 indexing offsets
 * the pin number by -28 internally (GPIO 28-33 occupy bank 1).
 *
 * MVP: blindly configure the pin as SYS_RIO output-driving-high.
 * Kept compact — a full GPIO framework would be a separate
 * refactor. If this stage fails on later boards, look here first. */
static void macb_release_phy_reset(void)
{
    #define RP1_IO_BANK1_BASE    0x1F000D4000UL
    #define RP1_RIO_BANK1_BASE   0x1F000E4000UL
    #define RP1_PADS_BANK1_BASE  0x1F000F4000UL
    #define BANK1_PIN(gpio)      ((gpio) - 28)   /* GPIO 28-33 → 0-5 */
    #define FUNCSEL_SYS_RIO      5
    #define RIO_SET_OFFSET       0x2000          /* Atomic set alias */
    #define PADS_INPUT_EN        (1u << 6)
    #define PADS_OUTPUT_DIS      (1u << 7)

    const uint32_t phy_reset_gpio = 32;
    const uint32_t bank_pin = BANK1_PIN(phy_reset_gpio);

    volatile uint32_t *ctrl = (volatile uint32_t *)(RP1_IO_BANK1_BASE + bank_pin * 8 + 4);
    volatile uint32_t *pads = (volatile uint32_t *)(RP1_PADS_BANK1_BASE + 4 + bank_pin * 4);
    volatile uint32_t *rio_oe_set  = (volatile uint32_t *)(RP1_RIO_BANK1_BASE + 0x04 + RIO_SET_OFFSET);
    volatile uint32_t *rio_out_set = (volatile uint32_t *)(RP1_RIO_BANK1_BASE + 0x00 + RIO_SET_OFFSET);

    /* Pad: enable output (clear OUTPUT_DIS), disable input */
    uint32_t p = *pads;
    p &= ~PADS_OUTPUT_DIS;
    p &= ~PADS_INPUT_EN;
    *pads = p;

    /* Function select: SYS_RIO so we drive the pin directly */
    *ctrl = FUNCSEL_SYS_RIO;

    /* Drive pin low (assert reset), pulse, then high (release) */
    volatile uint32_t *rio_out_clr = (volatile uint32_t *)(RP1_RIO_BANK1_BASE + 0x00 + 0x3000);
    *rio_out_clr = (1u << bank_pin);
    *rio_oe_set  = (1u << bank_pin);
    sleep_ms(10);
    *rio_out_set = (1u << bank_pin);
    sleep_ms(20);   /* Spec: BCM54213PE needs up to 10 ms post-reset */

    INFO("  Released PHY reset via GPIO %u", phy_reset_gpio);
}

/* Scan MDIO addresses 0-31, log any responders. Used when the
 * expected PHY address doesn't answer. */
static void macb_mdio_scan(void)
{
    int found = 0;
    for (int addr = 0; addr < 32; addr++) {
        uint16_t id1 = macb_mdio_read(addr, MII_PHYID1);
        if (id1 != 0 && id1 != 0xFFFF) {
            uint16_t id2 = macb_mdio_read(addr, MII_PHYID2);
            INFO("  MDIO scan: PHY found at addr %d (ID1=0x%04x ID2=0x%04x)",
                 addr, id1, id2);
            found++;
        }
    }
    if (found == 0) {
        INFO("  MDIO scan: no PHY responded on any address 0-31");
    }
}

/* Probe + configure the PHY. Returns 0 once link is up (speed/duplex
 * known), -1 on any failure along the way. */
static int macb_phy_bringup(void)
{
    macb_release_phy_reset();

    macb_state.phy_id1 = macb_mdio_read(PHY_ADDR, MII_PHYID1);
    macb_state.phy_id2 = macb_mdio_read(PHY_ADDR, MII_PHYID2);

    INFO("  PHY @addr %u: ID1=0x%04x ID2=0x%04x",
         (unsigned)PHY_ADDR, macb_state.phy_id1, macb_state.phy_id2);

    if (macb_state.phy_id1 == 0xFFFF && macb_state.phy_id2 == 0xFFFF) {
        ERROR("PHY not responding on MDIO addr %u — running MDIO scan:",
              PHY_ADDR);
        macb_mdio_scan();
        return -1;
    }

    /* Kick auto-negotiation. BCM54213 advertises 1000BASE-T by default
     * from its bootstrap pins, so all we need is: power up, enable
     * auto-neg, request a fresh negotiation. */
    macb_mdio_write(PHY_ADDR, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

    /* Poll BMSR for link up. Cable connected to a lab switch typically
     * negotiates in <1 s; give it 5 s before giving up (stage 5 will
     * revisit this budget once real DHCP exchange runs the clock). */
    INFO("  Waiting for PHY auto-negotiate / link up...");
    for (int i = 0; i < 50; i++) {
        uint16_t bmsr = macb_mdio_read(PHY_ADDR, MII_BMSR);
        if ((bmsr & BMSR_LSTATUS) && (bmsr & BMSR_ANEGCOMPLETE)) {
            INFO("  PHY link up (BMSR=0x%04x after %d×100 ms)", bmsr, i);
            macb_state.link_up = true;

            /* Decode speed + duplex from BCM54213's Auxiliary Status
             * would require a vendor-specific read. For an MVP we
             * default to 1000/full when ANEG completes and let the
             * MAC run at that rate; real hardware on a Gb switch is
             * overwhelmingly likely to negotiate to 1000/full. */
            macb_state.link_speed_mbps = 1000;
            macb_state.link_full_duplex = true;
            INFO("  Assumed 1000 Mbps full-duplex (MVP — no vendor AUX read)");
            return 0;
        }
        sleep_ms(100);
    }

    ERROR("PHY auto-negotiate timed out — no link");
    return -1;
}

/* -------------------------------------------------------------------------- */
/* Stage 3 — TX descriptor ring + polled send                                  */
/* -------------------------------------------------------------------------- */

/* Initialize the TX ring. Every descriptor starts owned by the MAC
 * (USED=1) so it won't spontaneously try to transmit stale state.
 * Last entry carries the WRAP bit so the hardware re-loads from
 * descriptor 0 after it. */
static void macb_tx_ring_init(void)
{
    for (unsigned i = 0; i < MACB_TX_RING_SIZE; i++) {
        tx_ring[i].addr = 0;
        tx_ring[i].ctrl = MACB_TX_USED |
                          ((i == MACB_TX_RING_SIZE - 1) ? MACB_TX_WRAP : 0);
    }
    macb_state.tx_head = 0;
    macb_state.tx_tail = 0;

    /* Flush descriptors to DRAM so the MAC sees the initial state */
    cache_clean_range(tx_ring, sizeof(tx_ring));

    /* Point the hardware at the ring */
    macb_writel(MACB_TBQP, (uint32_t)(uintptr_t)tx_ring);

    /* Clear any stale TSR bits */
    macb_writel(MACB_TSR, 0xFFFFFFFF);
}

/* Configure speed/duplex in MACB NCFGR + enable receive-path bits
 * the MAC needs to actually accept real-world frames. Must run after
 * macb_mdio_bringup (MDC divider) and before macb_enable_rx. */
static void macb_apply_link_config(void)
{
    uint32_t ncfgr = macb_readl(MACB_NCFGR);

    /* Clear bits we manage: speed, duplex, gigabit, BIG/DRFCS to
     * start from a known state. NBC stays cleared = accept broadcast. */
    ncfgr &= ~(MACB_NCFGR_SPD | MACB_NCFGR_FD | GEM_NCFGR_GBE |
               MACB_NCFGR_BIG | MACB_NCFGR_DRFCS | MACB_NCFGR_NBC |
               MACB_NCFGR_CAF);

    /* Speed / duplex from auto-neg */
    if (macb_state.link_speed_mbps == 100) {
        ncfgr |= MACB_NCFGR_SPD;
    } else if (macb_state.link_speed_mbps == 1000) {
        ncfgr |= GEM_NCFGR_GBE;
    }
    if (macb_state.link_full_duplex) {
        ncfgr |= MACB_NCFGR_FD;
    }

    /* RX behaviour */
    ncfgr |= MACB_NCFGR_BIG;     /* Accept 1500+ byte frames */
    ncfgr |= MACB_NCFGR_DRFCS;   /* Strip FCS before DMA */

    macb_writel(MACB_NCFGR, ncfgr);
}

/* Program a locally-administered MAC address into the specific-address
 * 1 registers (SA1B/SA1T). Without a valid source address, switches
 * and end-hosts drop our frames at the MAC layer. For production we'd
 * read a board-unique ID from EEPROM; the OUI 02:00:00 is the safe
 * locally-administered unicast prefix per IEEE 802. */
static void macb_program_mac_address(void)
{
    /* Fixed MVP MAC — fine for a single-board lab. Replace with a
     * board-unique derivation (board serial, MPIDR, etc.) if more
     * than one Pi 5 ever shares a subnet. */
    macb_state.mac[0] = 0x02;
    macb_state.mac[1] = 0x00;
    macb_state.mac[2] = 0x00;
    macb_state.mac[3] = 0x5A;   /* "Z" for SLM-OS */
    macb_state.mac[4] = 0x00;
    macb_state.mac[5] = 0x01;

    /* SA1B is bytes [3..0] of the MAC, SA1T is bytes [5..4]. */
    uint32_t bottom = (uint32_t)macb_state.mac[0]
                    | ((uint32_t)macb_state.mac[1] << 8)
                    | ((uint32_t)macb_state.mac[2] << 16)
                    | ((uint32_t)macb_state.mac[3] << 24);
    uint32_t top    = (uint32_t)macb_state.mac[4]
                    | ((uint32_t)macb_state.mac[5] << 8);
    macb_writel(MACB_SA1B, bottom);
    macb_writel(MACB_SA1T, top);
}

/* Enable TX. Called after the TX ring is populated and after
 * macb_rx_ring_init has set RBQP. RX is enabled by macb_enable_rx. */
static void macb_enable_tx(void)
{
    uint32_t ncr = macb_readl(MACB_NCR);
    ncr |= MACB_NCR_TE;
    macb_writel(MACB_NCR, ncr);
}

/* Initialize the RX ring. Every descriptor gets a buffer address in
 * its addr field, USED bit CLEAR (so the MAC owns it), WRAP on last.
 * ctrl starts at 0 — the MAC will populate it when a frame arrives. */
static void macb_rx_ring_init(void)
{
    for (unsigned i = 0; i < MACB_RX_RING_SIZE; i++) {
        uint32_t buf_addr = (uint32_t)(uintptr_t)rx_buffers[i];
        /* Buffer must be 4-byte aligned (bits 0-1 are USED/WRAP). */
        rx_ring[i].addr = (buf_addr & MACB_RX_ADDR_MASK) |
                          ((i == MACB_RX_RING_SIZE - 1) ? MACB_RX_WRAP : 0);
        rx_ring[i].ctrl = 0;
    }
    macb_state.rx_head = 0;

    cache_clean_range(rx_ring, sizeof(rx_ring));

    /* Program DMA configuration via RMW to preserve any firmware-set
     * bits we don't explicitly manage (endianness, AXI pipeline hints,
     * etc.). Fields we touch:
     *   FBL   = 16         (burst length, matches Linux rp1-gem config)
     *   RXBMS = 3          (maximum RX pbuf memory — 2 bits, -1 pattern)
     *   TXPBMS= 1          (full TX pbuf memory)
     *   RXBS  = 24         (1536-byte RX buffers, matches our buffer size)
     *   DDRP  = 1          (discard frames when no AHB bandwidth — safer
     *                       than hanging the MAC on backpressure)
     *   ENDIA_PKT = 0      (little-endian packet data, ARM native) */
    uint32_t dmacfg = macb_readl(GEM_DMACFG);
    dmacfg &= ~(GEM_DMACFG_FBL_MASK  << GEM_DMACFG_FBL_SHIFT);
    dmacfg &= ~(GEM_DMACFG_RXBS_MASK << GEM_DMACFG_RXBS_SHIFT);
    dmacfg &= ~(GEM_DMACFG_RXBMS_MASK << GEM_DMACFG_RXBMS_SHIFT);
    dmacfg &= ~GEM_DMACFG_ENDIA_PKT;
    dmacfg |= (GEM_DMACFG_FBL_16 << GEM_DMACFG_FBL_SHIFT);
    dmacfg |= ((uint32_t)MACB_RX_BUF_UNITS_64 << GEM_DMACFG_RXBS_SHIFT);
    dmacfg |= (0x3u << GEM_DMACFG_RXBMS_SHIFT);
    dmacfg |= GEM_DMACFG_TXPBMS;
    dmacfg |= GEM_DMACFG_DDRP;
    macb_writel(GEM_DMACFG, dmacfg);

    /* Point the hardware at the ring */
    macb_writel(MACB_RBQP, (uint32_t)(uintptr_t)rx_ring);

    /* Clear any stale RSR bits */
    macb_writel(MACB_RSR, 0xFFFFFFFF);
}

/* Flip NCR.RE to start accepting frames. */
static void macb_enable_rx(void)
{
    uint32_t ncr = macb_readl(MACB_NCR);
    ncr |= MACB_NCR_RE;
    macb_writel(MACB_NCR, ncr);
}

/* Attempt to pull one frame out of the RX ring. Returns bytes copied,
 * 0 if no frame ready, or a negative NET_E_* on error. Scans from the
 * current head forward, skipping over partial frames — MACB will
 * split oversized packets but with our RXBS=1536 each frame fits in
 * one descriptor, so the common case is a single owner-returned entry. */
static int macb_rx_one(void *out_buf, size_t max_len)
{
    /* Invalidate descriptor ring so we see fresh USED/ctrl bits */
    cache_invalidate_range(rx_ring, sizeof(rx_ring));

    unsigned slot = macb_state.rx_head;
    uint32_t addr_word = rx_ring[slot].addr;

    if (!(addr_word & MACB_RX_USED)) {
        return 0;   /* No frame in this slot */
    }

    /* USED=1 — MAC wrote a frame. ctrl has frmlen + SOF/EOF flags. */
    uint32_t ctrl = rx_ring[slot].ctrl;
    uint32_t frmlen = ctrl & MACB_RX_FRMLEN_MASK;

    /* SOF+EOF both set = single-descriptor frame (the common case
     * with RXBS=1536). Partial frames (SOF without EOF or vice versa)
     * are rare but can happen; for the MVP, drop them — re-assembly
     * adds complexity we don't need for DHCP/ping-class traffic. */
    if (!((ctrl & MACB_RX_SOF) && (ctrl & MACB_RX_EOF))) {
        WARN("RX: slot %u has partial frame (ctrl=0x%08x) — dropping",
             slot, ctrl);
        frmlen = 0;
    }

    int ret;
    if (frmlen == 0 || frmlen > max_len) {
        ret = 0;
    } else {
        /* Invalidate the buffer cacheline before the copy so the CPU
         * sees what the MAC DMA'd in. */
        cache_invalidate_range(rx_buffers[slot], frmlen);
        memcpy(out_buf, rx_buffers[slot], frmlen);
        ret = (int)frmlen;
    }

    /* Return the descriptor to the MAC: clear USED, preserve WRAP,
     * restore buffer address. Then push back to DRAM. */
    uint32_t new_addr = ((uint32_t)(uintptr_t)rx_buffers[slot] & MACB_RX_ADDR_MASK) |
                        ((slot == MACB_RX_RING_SIZE - 1) ? MACB_RX_WRAP : 0);
    rx_ring[slot].addr = new_addr;
    rx_ring[slot].ctrl = 0;
    cache_clean_range(&rx_ring[slot], sizeof(rx_ring[slot]));
    __asm__ volatile("dsb sy" ::: "memory");

    macb_state.rx_head = (slot + 1) & (MACB_RX_RING_SIZE - 1);
    return ret;
}

/* Reap any completed TX descriptors by scanning from tail forward.
 * A descriptor is "reapable" when MACB has set USED=1, meaning the
 * frame is transmitted. We don't free buffers here because the TX
 * buffer pool is re-used in-place; the caller (send path) just picks
 * the next free slot. */
static void macb_tx_reap_locked(void)
{
    /* Invalidate the ring before reading so we see updates from DMA */
    cache_invalidate_range(tx_ring, sizeof(tx_ring));

    while (macb_state.tx_tail != macb_state.tx_head) {
        uint32_t ctrl = tx_ring[macb_state.tx_tail].ctrl;
        if (!(ctrl & MACB_TX_USED)) {
            /* Not yet transmitted — stop, preserve FIFO order */
            break;
        }
        if (ctrl & MACB_TX_ERROR) {
            WARN("TX descriptor %u reports error (ctrl=0x%08x)",
                 macb_state.tx_tail, ctrl);
        }
        macb_state.tx_tail = (macb_state.tx_tail + 1) & (MACB_TX_RING_SIZE - 1);
    }
}

/* Polled TX: copy the frame into a pool buffer, mark the descriptor
 * ready, kick TSTART, wait for completion. Used as a Stage 3 proof —
 * later we'll hand this to the lwIP adapter so it can batch. */
static int macb_tx_one(const void *buf, size_t len)
{
    if (len > MACB_TX_BUF_SIZE || len < 14) {
        return NET_E_TOO_LARGE;
    }

    /* Reap any completed TX before picking a slot */
    macb_tx_reap_locked();

    unsigned next_head = (macb_state.tx_head + 1) & (MACB_TX_RING_SIZE - 1);
    if (next_head == macb_state.tx_tail) {
        return NET_E_BUSY;   /* Ring full — caller retries via tx_reap */
    }

    unsigned slot = macb_state.tx_head;

    /* Copy the frame into the pool buffer */
    memcpy(tx_buffers[slot], buf, len);
    cache_clean_range(tx_buffers[slot], len);

    /* Populate the descriptor. WRAP stays on the last slot — we
     * preserve it here because we don't re-initialize. Note: USED
     * bit is CLEARED by this write, which is what hands the slot
     * over to the MAC. */
    tx_ring[slot].addr = (uint32_t)(uintptr_t)tx_buffers[slot];
    tx_ring[slot].ctrl = ((uint32_t)len & MACB_TX_FRMLEN_MASK) |
                         MACB_TX_LAST |
                         ((slot == MACB_TX_RING_SIZE - 1) ? MACB_TX_WRAP : 0);
    cache_clean_range(&tx_ring[slot], sizeof(tx_ring[slot]));
    __asm__ volatile("dsb sy" ::: "memory");

    /* Kick the MAC — NCR |= TSTART */
    uint32_t ncr = macb_readl(MACB_NCR);
    macb_writel(MACB_NCR, ncr | MACB_NCR_TSTART);

    macb_state.tx_head = next_head;

    /* Polled wait: USED bit goes high when MAC finishes. Budget
     * ~100 ms which is far more than any Ethernet frame time. */
    for (int i = 0; i < 10000; i++) {
        cache_invalidate_range(&tx_ring[slot], sizeof(tx_ring[slot]));
        if (tx_ring[slot].ctrl & MACB_TX_USED) {
            if (tx_ring[slot].ctrl & MACB_TX_ERROR) {
                WARN("TX error after completion (slot %u, ctrl=0x%08x)",
                     slot, tx_ring[slot].ctrl);
                return NET_E_GENERIC;
            }
            return 0;
        }
        sleep_us(10);
    }

    ERROR("TX timed out after 100 ms (slot %u, TSR=0x%08x)",
          slot, macb_readl(MACB_TSR));
    return NET_E_TIMEOUT;
}

/* -------------------------------------------------------------------------- */
/* net_driver ops                                                              */
/* -------------------------------------------------------------------------- */

int macb_init(void)
{
    INFO("Initializing Cadence MACB/GEM driver (Pi 5 RP1)...");
    INFO("  ETH IP base:  0x%lx", (unsigned long)RP1_ETH_IP_BASE);
    INFO("  ETH CFG base: 0x%lx", (unsigned long)RP1_ETH_CFG_BASE);
    INFO("  MACB IRQ:     %u (MIP0 vec %u → SPI %u)",
         (unsigned)MACB_IRQ, (unsigned)RP1_INT_ETH,
         (unsigned)(MIP0_BASE_SPI + RP1_INT_ETH));

    /* Stage 2: enable RP1 Ethernet clocks. Idempotent — firmware
     * may have already turned them on. */
    macb_enable_clocks();

    /* Stage 1 carried forward: probe MID for a sanity check that the
     * MACB is responsive post-clock-enable. */
    uint32_t mid = macb_readl(MACB_MID);
    macb_state.macb_mid = mid;
    if (mid == 0xFFFFFFFF || mid == 0) {
        ERROR("MACB_MID=0x%x — IP not responding after clock enable", mid);
        return -1;
    }
    uint32_t idnum = (mid >> MACB_MID_IDNUM_SHIFT) & MACB_MID_IDNUM_MASK;
    uint32_t rev   = (mid >> MACB_MID_REV_SHIFT)   & MACB_MID_REV_MASK;
    uint32_t fab   = (mid >> MACB_MID_FAB_SHIFT)   & MACB_MID_FAB_MASK;
    INFO("  Cadence MACB/GEM: idnum=0x%04x rev=0x%x fab=0x%03x",
         idnum, rev, fab);

    /* Stage 2: MACB MDIO path + PHY bring-up. */
    macb_mdio_bringup();

    if (macb_phy_bringup() < 0) {
        return -1;
    }

    /* Stage 3: bring up the TX ring. RX ring lands in Stage 4; until
     * then the net_driver.recv op returns 0 so lwIP sees "no packets".
     * lwIP-driven sends (ARP / DHCP solicit) will still work end-to-
     * end only when Stage 4 lands, but net_init() can succeed now. */
    macb_apply_link_config();
    macb_program_mac_address();
    macb_tx_ring_init();
    macb_rx_ring_init();

    INFO("  MAC address: %02x:%02x:%02x:%02x:%02x:%02x",
         macb_state.mac[0], macb_state.mac[1], macb_state.mac[2],
         macb_state.mac[3], macb_state.mac[4], macb_state.mac[5]);

    /* Enable both directions. Order matters: RX first so we don't
     * lose inbound frames while still enabling TX — trivial on a
     * fresh boot but matters after a re-init. */
    macb_enable_rx();
    macb_enable_tx();

    macb_state.initialized = true;
    INFO("MACB driver: Stage 4 TX + RX up — network operational");
    return 0;
}

static int macb_send(const void *buf, size_t len)
{
    if (!macb_state.initialized) {
        return NET_E_NOT_INIT;
    }
    return macb_tx_one(buf, len);
}

static int macb_recv(void *buf, size_t max_len)
{
    if (!macb_state.initialized) {
        return 0;
    }
    return macb_rx_one(buf, max_len);
}

static void macb_get_mac(uint8_t mac[6])
{
    if (macb_state.initialized) {
        memcpy(mac, macb_state.mac, 6);
    } else {
        memset(mac, 0, 6);
    }
}

static bool macb_link_status(void)
{
    return macb_state.initialized && macb_state.link_up;
}

static const struct net_driver macb_driver = {
    .name        = "cdns-macb",
    .init        = macb_init,
    .send        = macb_send,
    .recv        = macb_recv,
    .get_mac     = macb_get_mac,
    .link_status = macb_link_status,
    .tx_reap     = NULL,
};

void macb_register(void)
{
    net_register_driver(&macb_driver);
}

#endif /* PLATFORM_RASPI5 && ENABLE_NETWORKING */
