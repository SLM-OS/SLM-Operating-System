/**
 * BCM GENET Ethernet Driver for Raspberry Pi 5 (#202)
 *
 * Drives the Gigabit Ethernet controller integrated into the RP1
 * southbridge on BCM2712. The controller is GENET v5, the same IP
 * family used on earlier Raspberry Pi models but here sitting behind
 * the RP1 PCIe link — MMIO reaches it through BAR1 at
 * RP1_ETH_IP_BASE / RP1_ETH_CFG_BASE (both already mapped by
 * vmm_setup_platform for RASPI5).
 *
 * Staged development per #202 plan:
 *   Stage 0 (this file): build scaffolding + driver registration
 *                        stub. genet_init returns -1 so net_init
 *                        fails cleanly, but every wire between
 *                        CMake / main.c / net_register_driver is
 *                        in place for subsequent stages to fill in.
 *   Stage 1: probe SYS_REV register, confirm hardware presence.
 *   Stage 2: MDIO + BCM54213PE PHY bring-up.
 *   Stage 3: TX descriptor ring + polled send.
 *   Stage 4: RX descriptor ring + polled receive.
 *   Stage 5: wire into lwIP, DHCP, ping.
 *   Stage 6: regression tests + optional IRQ upgrade.
 *
 * Polling-only by design at the MVP — Pi 5's NS-EL1 IRQ delivery
 * is unreliable (#134), and the virtio-pci fallback path
 * demonstrates that polled tx_reap via net_poll() is an acceptable
 * pattern. IRQ enablement via gic_register_handler can come later.
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
#include "genet.h"

/* -------------------------------------------------------------------------- */
/* Driver state                                                                */
/* -------------------------------------------------------------------------- */

static struct {
    bool     initialized;
    uint8_t  mac[6];
    bool     link_up;
} genet_state;

/* -------------------------------------------------------------------------- */
/* net_driver ops — Stage 0 stubs                                              */
/*                                                                             */
/* These return -1 / 0 / false to keep the full net pipeline honest about     */
/* the driver being non-functional today. Later stages replace each stub      */
/* with real logic without touching net_driver wiring.                         */
/* -------------------------------------------------------------------------- */

int genet_init(void)
{
    INFO("Initializing BCM GENET driver (Pi 5 RP1)...");
    INFO("  ETH IP base:  0x%lx", (unsigned long)RP1_ETH_IP_BASE);
    INFO("  ETH CFG base: 0x%lx", (unsigned long)RP1_ETH_CFG_BASE);
    INFO("  GENET IRQ:    %u (MIP0 vec %u → SPI %u)",
         (unsigned)GENET_IRQ, (unsigned)RP1_INT_ETH,
         (unsigned)(MIP0_BASE_SPI + RP1_INT_ETH));

    /* Stage 0 stub — hardware probe lands in Stage 1. */
    INFO("GENET driver stub: init not yet implemented (Stage 0)");
    return -1;
}

static int genet_send(const void *buf, size_t len)
{
    (void)buf; (void)len;
    return NET_E_NOT_INIT;
}

static int genet_recv(void *buf, size_t max_len)
{
    (void)buf; (void)max_len;
    return 0;
}

static void genet_get_mac(uint8_t mac[6])
{
    if (genet_state.initialized) {
        memcpy(mac, genet_state.mac, 6);
    } else {
        memset(mac, 0, 6);
    }
}

static bool genet_link_status(void)
{
    return genet_state.initialized && genet_state.link_up;
}

/* tx_reap is optional (NULL = driver doesn't need async completion
 * drain). Left NULL in Stage 0; populated in Stage 3 when we have
 * a real TX path to drain. */
static const struct net_driver genet_driver = {
    .name        = "bcm-genet",
    .init        = genet_init,
    .send        = genet_send,
    .recv        = genet_recv,
    .get_mac     = genet_get_mac,
    .link_status = genet_link_status,
    .tx_reap     = NULL,
};

void genet_register(void)
{
    net_register_driver(&genet_driver);
}

#endif /* PLATFORM_RASPI5 && ENABLE_NETWORKING */
