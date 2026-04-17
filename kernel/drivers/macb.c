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

/* -------------------------------------------------------------------------- */
/* MMIO helpers                                                                */
/* -------------------------------------------------------------------------- */

static inline uint32_t macb_readl(uint32_t reg)
{
    return *(volatile uint32_t *)(RP1_ETH_IP_BASE + reg);
}

__attribute__((unused))
static inline void macb_writel(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(RP1_ETH_IP_BASE + reg) = val;
}

/* -------------------------------------------------------------------------- */
/* Driver state                                                                */
/* -------------------------------------------------------------------------- */

static struct {
    bool     initialized;
    uint8_t  mac[6];
    bool     link_up;
    uint32_t macb_mid;          /* MID register contents (diagnostics) */
} macb_state;

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

    /* Stage 1: probe Module ID register. MACB_MID at offset 0xFC is a
     * read-only identifier. Surrounding registers (NCR/NCFGR/NSR) also
     * read for diagnostics — firmware usually leaves NCFGR populated
     * with speed/duplex defaults, so a sensible NCFGR value
     * corroborates that BAR1 + MACB decode are healthy. */
    uint32_t ncr    = macb_readl(MACB_NCR);
    uint32_t ncfgr  = macb_readl(MACB_NCFGR);
    uint32_t nsr    = macb_readl(MACB_NSR);
    uint32_t mid    = macb_readl(MACB_MID);

    INFO("  NCR=0x%08x NCFGR=0x%08x NSR=0x%08x MID=0x%08x",
         ncr, ncfgr, nsr, mid);

    macb_state.macb_mid = mid;

    if (mid == 0xFFFFFFFF) {
        ERROR("MACB_MID=0xFFFFFFFF — address decoded to nothing "
              "(RP1 BAR1 mapping wrong?)");
        return -1;
    }

    if (mid == 0) {
        ERROR("MACB_MID=0 — IP not responding (clock gated?)");
        return -1;
    }

    uint32_t idnum = (mid >> MACB_MID_IDNUM_SHIFT) & MACB_MID_IDNUM_MASK;
    uint32_t rev   = (mid >> MACB_MID_REV_SHIFT) & MACB_MID_REV_MASK;
    uint32_t fab   = (mid >> MACB_MID_FAB_SHIFT) & MACB_MID_FAB_MASK;
    INFO("  Cadence MACB/GEM detected: idnum=0x%04x rev=0x%x fab=0x%03x",
         idnum, rev, fab);

    /* Stage 2+ lands here: clock enable, PHY reset + MDIO, ring setup.
     * Until those exist, return -1 so net_init() reports no network
     * and the shell comes up without it. */
    INFO("MACB driver: Stage 1 probe OK — remaining stages pending");
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
