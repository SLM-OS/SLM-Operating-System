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
/* Tegra PCIe ECAM accessors                                                   */
/*                                                                             */
/* ECAM layout per PCIe spec: base + (bus << 20) | (dev << 15) | (func << 12)  */
/* + reg. Tegra C8 RC maps bus 0 (bridge) and bus 1 (endpoint).                */
/* -------------------------------------------------------------------------- */

static volatile uint8_t *pcie_ecam_base(void)
{
    return (volatile uint8_t *)TEGRA_PCIE_C8_ECAM_BASE;
}

static uint32_t pcie_config_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                    uint16_t reg)
{
    uintptr_t offset = ((uintptr_t)bus  << 20) |
                       ((uintptr_t)dev  << 15) |
                       ((uintptr_t)func << 12) |
                       (reg & 0xFFC);
    return *(volatile uint32_t *)(pcie_ecam_base() + offset);
}

static uint16_t pcie_config_read16(uint8_t bus, uint8_t dev, uint8_t func,
                                    uint16_t reg)
{
    uint32_t w = pcie_config_read32(bus, dev, func, reg & ~0x3u);
    return (uint16_t)(w >> ((reg & 0x2u) * 8));
}

static uint8_t pcie_config_read8(uint8_t bus, uint8_t dev, uint8_t func,
                                  uint16_t reg)
{
    uint32_t w = pcie_config_read32(bus, dev, func, reg & ~0x3u);
    return (uint8_t)(w >> ((reg & 0x3u) * 8));
}

/* -------------------------------------------------------------------------- */
/* Driver state                                                                */
/* -------------------------------------------------------------------------- */

struct rtl8169_state {
    bool     probed;
    bool     registered;

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

/* Read the 64-bit BAR at the given offset. Returns 0 if the BAR is
 * unmapped or not memory-type. RTL8168 uses 64-bit BARs for BAR2/BAR4
 * (each pair of 32-bit BARs combines). */
static uint64_t rtl8169_probe_bar64(uint8_t bus, uint8_t dev, uint8_t func,
                                     uint16_t reg_low)
{
    uint32_t lo = pcie_config_read32(bus, dev, func, reg_low);
    uint32_t hi = pcie_config_read32(bus, dev, func, reg_low + 4);

    /* Bit 0 = 0 → memory BAR; bits [2:1] encode 32- vs 64-bit. */
    if (lo & 0x1) return 0;             /* I/O BAR, skip */
    if (((lo >> 1) & 0x3) != 0x2) {
        /* 32-bit memory BAR — treat hi as zero */
        return (uint64_t)(lo & ~0xFu);
    }
    return ((uint64_t)hi << 32) | (uint64_t)(lo & ~0xFu);
}

static bool rtl8169_probe(void)
{
    /* Read vendor/device at fixed BDF. If the RC hasn't initialized the
     * link or the device is absent, vendor comes back 0xFFFF. */
    uint16_t vendor = pcie_config_read16(RTL8169_PCI_BUS, RTL8169_PCI_DEV,
                                          RTL8169_PCI_FUNC, 0x00);
    uint16_t device = pcie_config_read16(RTL8169_PCI_BUS, RTL8169_PCI_DEV,
                                          RTL8169_PCI_FUNC, 0x02);

    if (vendor == 0xFFFF || vendor == 0x0000) {
        DEBUG_PRINT("rtl8169: PCIe config read returned 0x%04x — device absent "
                    "or RC link not trained", vendor);
        return false;
    }
    if (vendor != RTL8169_PCI_VENDOR || device != RTL8169_PCI_DEVICE) {
        DEBUG_PRINT("rtl8169: unexpected device %04x:%04x at 0008:01:00.0",
                    vendor, device);
        g_rtl.pci_vendor = vendor;
        g_rtl.pci_device = device;
        return false;
    }

    g_rtl.pci_vendor   = vendor;
    g_rtl.pci_device   = device;
    g_rtl.pci_revision = pcie_config_read8(RTL8169_PCI_BUS, RTL8169_PCI_DEV,
                                            RTL8169_PCI_FUNC, 0x08);

    /* BAR2 (offset 0x18) — main register bank.
     * BAR4 (offset 0x20) — extended registers. Both 64-bit. */
    g_rtl.bar2_phys = rtl8169_probe_bar64(RTL8169_PCI_BUS, RTL8169_PCI_DEV,
                                           RTL8169_PCI_FUNC, 0x18);
    g_rtl.bar4_phys = rtl8169_probe_bar64(RTL8169_PCI_BUS, RTL8169_PCI_DEV,
                                           RTL8169_PCI_FUNC, 0x20);

    if (g_rtl.bar2_phys == 0) {
        DEBUG_PRINT("rtl8169: BAR2 not assigned — Linux should have "
                    "configured this. Did the PCIe RC survive kexec?");
        return false;
    }

    g_rtl.probed = true;
    return true;
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
