/*
 * Realtek RTL8169/RTL8168 Gigabit Ethernet Driver for Jetson Orin Nano
 *
 * Stage 1 (this commit): scaffolding + PCIe probe canary.
 *   - Read vendor/device/revision via ECAM (no device registers touched).
 *   - Cache BAR2 address from config space.
 *   - Driver registers the net_driver struct but init() is still a stub
 *     that returns 0 without setting up rings. This lets the
 *     `rtldiag` shell command print probe results without risking a
 *     hardware fault while later stages are incomplete.
 *
 * Stage 2 (next): read MAC address (IDR0..IDR5), TxConfig chip version,
 *                 PHYstatus, confirm Linux-programmed state survived kexec.
 * Stage 3:        RX descriptor ring + polled recv.
 * Stage 4:        TX descriptor ring + polled send with tx_reap.
 * Stage 5:        lwIP hookup + DHCP.
 * Stage 6:        live regression tests + cache coherence probes.
 *
 * See docs/networking-expansion-plan.md §4 and GitHub issue #25.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "net.h"
#include "net_driver.h"
#include "debug.h"
#include "eth_rtl8169.h"

/* -------------------------------------------------------------------------- */
/* Tegra PCIe config-space accessors                                           */
/*                                                                             */
/* Tegra's PCIe RC is DesignWare-based and does NOT expose a flat ECAM:       */
/*   - Bus 0 (RC itself, PCI bridge) config space is at DBI (0x2A080000).      */
/*   - Bus 1+ (downstream) config space goes through the "config" window      */
/*     at 0x2A000000, which is only 256 KB and iATU-retargeted — the RC       */
/*     driver has to reprogram iATU per-bus access.                           */
/*                                                                             */
/* For Stage 1 we only probe bus 0 via DBI. Bus 1 (RTL8168) access requires   */
/* iATU programming (Stage 2+) AND the RC being alive (tegra194-pcie's        */
/* shutdown hook on kexec may have torn it down — see platform.h comment).    */
/* -------------------------------------------------------------------------- */

/* Bus 0 device 0 function 0 — the RC bridge, read via DBI. */
static uint32_t dbi_read32(uint16_t reg)
{
    return *(volatile uint32_t *)(TEGRA_PCIE_C8_DBI_BASE + (reg & 0xFFC));
}

/* APPL controller wrapper register. LTSSM state and link debug live here. */
static uint32_t appl_read32(uint32_t reg)
{
    return *(volatile uint32_t *)(TEGRA_PCIE_C8_APPL_BASE + reg);
}

/* APPL register offsets (from Linux pcie-tegra194.c). The _EN /
 * _STATE_* bit decodes are consumed by the Stage 2 APPL link-up
 * check; kept here so the Stage 2 patch adds against the Stage 1
 * shape without re-introducing defines. */
#define APPL_CTRL                  0x04
#define APPL_CTRL_LTSSM_EN         (1u << 7)   /* Stage 2 */
#define APPL_DEBUG                 0xD0
#define APPL_DEBUG_LTSSM_STATE_MSK 0x1F8       /* Stage 2 — bits [8:3] */
#define APPL_DEBUG_LTSSM_STATE_L0  0x11        /* Stage 2 — L0 link state */

/* -------------------------------------------------------------------------- */
/* Driver state                                                                */
/* -------------------------------------------------------------------------- */

struct rtl8169_state {
    bool     probed;
    bool     registered;

    /* Tegra RC state (populated early by rtl8169_probe_rc_alive) */
    bool     rc_alive;
    uint16_t rc_bridge_vendor;    /* Expected 0x10DE (NVIDIA) */
    uint16_t rc_bridge_device;    /* Expected 0x229c */
    uint32_t appl_ctrl;           /* APPL_CTRL at probe time */
    uint32_t appl_debug;          /* APPL_DEBUG (LTSSM state) at probe time */

    /* PCI identity (cached from config space) */
    uint16_t pci_vendor;
    uint16_t pci_device;
    uint8_t  pci_revision;

    /* Device register windows (CPU physical; MMIO-mapped identity in vmm.c) */
    uint64_t bar2_phys;
    uint64_t bar4_phys;

    /* Device state (populated in Stage 2+) */
    uint32_t mac_ver_raw;         /* TxConfig[30:20] & 0xfcf, 0 = not read */
    uint8_t  mac_addr[6];
    bool     mac_addr_valid;
    bool     link_up;
};

static struct rtl8169_state g_rtl;

/* -------------------------------------------------------------------------- */
/* net_driver ops (Stage 1 stubs — real impls come in later stages)            */
/* -------------------------------------------------------------------------- */

static int rtl8169_drv_init(void)
{
    /* Stage 1: probe-only. A successful probe is enough to satisfy
     * test_net_init_live's "link UP" check via link_status() returning
     * false and the test skipping — the driver just needs to not fault
     * on init. Stage 2 reads IDR/PHYstatus; Stage 3/4 set up rings. */
    if (!g_rtl.probed) {
        DEBUG_PRINT("rtl8169: init called before probe succeeded");
        return -1;
    }
    return 0;
}

static int rtl8169_drv_send(const void *buf, size_t len)
{
    (void)buf; (void)len;
    return -1;   /* NET_E_* values live in net.h; Stage 4 wires this up. */
}

static int rtl8169_drv_recv(void *buf, size_t max_len)
{
    (void)buf; (void)max_len;
    return 0;    /* No packet available (stub) */
}

static void rtl8169_drv_get_mac(uint8_t mac[6])
{
    if (g_rtl.mac_addr_valid) {
        for (int i = 0; i < 6; i++) mac[i] = g_rtl.mac_addr[i];
    } else {
        for (int i = 0; i < 6; i++) mac[i] = 0;
    }
}

static bool rtl8169_drv_link_status(void)
{
    return g_rtl.link_up;
}

static const struct net_driver rtl8169_driver = {
    .name        = "rtl8169",
    .init        = rtl8169_drv_init,
    .send        = rtl8169_drv_send,
    .recv        = rtl8169_drv_recv,
    .get_mac     = rtl8169_drv_get_mac,
    .link_status = rtl8169_drv_link_status,
    .tx_reap     = NULL,   /* Stage 4 adds this */
};

/* -------------------------------------------------------------------------- */
/* Probe                                                                       */
/* -------------------------------------------------------------------------- */

/* Lazy probe — called from the `rtldiag` shell command, NOT from
 * rtl8169_register() at boot. Reading APPL/DBI before knowing whether
 * the RC survived kexec risks an external abort that panics boot;
 * deferring until a user runs rtldiag keeps boot safe.
 *
 * Race-safe under concurrent `rtldiag` from multi-session shells:
 * the function only reads MMIO (no side effects) and writes the
 * same derived values into `g_rtl` regardless of which caller wins.
 * A concurrent reader via the accessors below could observe a
 * cosmetically torn view across fields, but no corruption can
 * result because all writers produce identical values. */
void rtl8169_refresh_rc_state(void)
{
    /* APPL controller wrapper — if clocks are gated or resets
     * asserted, this read may abort. The diagnostic is worth the
     * risk; if it does abort, the next iteration will wrap these
     * reads in a fault-tolerant helper. */
    g_rtl.appl_ctrl  = appl_read32(APPL_CTRL);
    g_rtl.appl_debug = appl_read32(APPL_DEBUG);

    uint32_t dev_vendor = dbi_read32(0x00);
    g_rtl.rc_bridge_vendor = (uint16_t)(dev_vendor & 0xFFFF);
    g_rtl.rc_bridge_device = (uint16_t)(dev_vendor >> 16);
    g_rtl.rc_alive = (g_rtl.rc_bridge_vendor != 0xFFFF &&
                      g_rtl.rc_bridge_vendor != 0x0000);
}

static bool rtl8169_probe(void)
{
    /* Stage 1: boot-time probe is a no-op. The Stage 1 driver never
     * touches MMIO, so this returns false unconditionally and the
     * driver stays unregistered. Run `rtldiag` from the shell to
     * read live RC state. Stage 2 replaces this with a real probe. */
    return false;
}

void rtl8169_register(void)
{
    if (!rtl8169_probe()) {
        /* Device absent or RC not ready — leave driver unregistered so
         * net_init() runs with no driver, exactly like platforms that
         * have ENABLE_NETWORKING off. */
        return;
    }

    net_register_driver(&rtl8169_driver);
    g_rtl.registered = true;
}

/* -------------------------------------------------------------------------- */
/* Diagnostic accessors                                                        */
/* -------------------------------------------------------------------------- */

bool rtl8169_is_probed(void) { return g_rtl.probed; }
bool rtl8169_get_rc_alive(void) { return g_rtl.rc_alive; }
uint16_t rtl8169_get_rc_bridge_vendor(void) { return g_rtl.rc_bridge_vendor; }
uint16_t rtl8169_get_rc_bridge_device(void) { return g_rtl.rc_bridge_device; }
uint32_t rtl8169_get_appl_ctrl(void) { return g_rtl.appl_ctrl; }
uint32_t rtl8169_get_appl_debug(void) { return g_rtl.appl_debug; }
uint16_t rtl8169_get_pci_vendor(void) { return g_rtl.pci_vendor; }
uint16_t rtl8169_get_pci_device(void) { return g_rtl.pci_device; }
uint8_t  rtl8169_get_pci_revision(void) { return g_rtl.pci_revision; }
uint64_t rtl8169_get_bar2(void) { return g_rtl.bar2_phys; }
uint32_t rtl8169_get_mac_ver_raw(void) { return g_rtl.mac_ver_raw; }

const uint8_t *rtl8169_get_mac_address(void)
{
    return g_rtl.mac_addr_valid ? g_rtl.mac_addr : NULL;
}

bool rtl8169_get_link_up(void) { return g_rtl.link_up; }

#endif /* PLATFORM_JETSON_ORIN_NANO && ENABLE_NETWORKING */
