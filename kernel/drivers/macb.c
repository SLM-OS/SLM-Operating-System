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

/* MACB_NCFGR (Network Config Register) fields */
#define MACB_NCFGR_CLK_SHIFT    10          /* MDIO clock divider, bits [12:10] */
#define MACB_NCFGR_CLK_MASK     0x7
/* MDC divider values: 0=÷8, 1=÷16, 2=÷32, 3=÷48, 4=÷64, 5=÷96, 6=÷128, 7=÷224 */
#define MACB_NCFGR_CLK_DIV64    0x4

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
} macb_state;

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
 * Clears MDC divider, sets MPE to enable management. Data-path
 * enables (RE/TE) stay off until the rings are built in Stages 3/4. */
static void macb_mdio_bringup(void)
{
    uint32_t ncfgr = macb_readl(MACB_NCFGR);
    ncfgr &= ~(MACB_NCFGR_CLK_MASK << MACB_NCFGR_CLK_SHIFT);
    ncfgr |= (MACB_NCFGR_CLK_DIV64 << MACB_NCFGR_CLK_SHIFT);
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

    INFO("MACB driver: Stage 2 PHY up — TX/RX rings pending (Stages 3/4)");
    /* Stages 3+ land the data path. Until then return -1 so net_init()
     * reports no network (PHY being up with no rings means we can't
     * actually move frames yet). */
    return -1;
}

static int macb_send(const void *buf, size_t len)
{
    (void)buf; (void)len;
    return NET_E_NOT_INIT;
}

static int macb_recv(void *buf, size_t max_len)
{
    (void)buf; (void)max_len;
    return 0;
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
