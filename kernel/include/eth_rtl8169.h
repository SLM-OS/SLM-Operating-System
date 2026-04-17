/*
 * Realtek RTL8169/RTL8168 Gigabit Ethernet Driver for Jetson Orin Nano (#25)
 *
 * Drives the RTL8168-family NIC on PCIe root complex C8 of the Super
 * Developer Kit carrier board. Device: 0x10EC:0x8168 rev 0x15 at bus
 * 0008:01:00.0. BAR2 (MMIO regs) at 0x3528004000.
 *
 * Linux trains the PCIe link + brings up the PHY before kexec; SLM-OS
 * inherits that state. No PCIe RC bring-up code is required for this
 * driver — we rely on the fact that kexec leaves the device + RC
 * intact.
 *
 * The driver is polled-only in the MVP path. Jetson IRQ delivery at
 * EL2 has the same class of concerns as Pi 5 (#134), so the
 * `net_pump` task drives `net_poll()` at ~100 Hz, and both TX
 * completion and RX reception are polled from there.
 */

#ifndef ETH_RTL8169_H
#define ETH_RTL8169_H

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stdint.h>
#include <stdbool.h>

/* Registration hook — called from main.c before net_init(). Probes the
 * PCIe config space; if the device is found, hooks the driver up to
 * the net_driver interface. Safe to call even if the device is absent
 * (driver stays unregistered). */
void rtl8169_register(void);

/* Re-read APPL / DBI state from the Tegra PCIe RC. Warning: may
 * external-abort if the RC has been torn down (post-kexec state is
 * uncertain until slmos-kexec is updated to skip .shutdown). Called
 * from `rtldiag` shell command, not from boot. */
void rtl8169_refresh_rc_state(void);

/* Diagnostic accessors used by the `rtldiag` shell command. */
bool     rtl8169_is_probed(void);
bool     rtl8169_get_rc_alive(void);
uint16_t rtl8169_get_rc_bridge_vendor(void);
uint16_t rtl8169_get_rc_bridge_device(void);
uint32_t rtl8169_get_appl_ctrl(void);
uint32_t rtl8169_get_appl_debug(void);
uint16_t rtl8169_get_pci_vendor(void);
uint16_t rtl8169_get_pci_device(void);
uint8_t  rtl8169_get_pci_revision(void);
uint64_t rtl8169_get_bar2(void);
uint32_t rtl8169_get_mac_ver_raw(void);   /* TxConfig[30:20] & 0xfcf */
const uint8_t *rtl8169_get_mac_address(void);
bool     rtl8169_get_link_up(void);

#endif /* PLATFORM_JETSON_ORIN_NANO */
#endif /* ETH_RTL8169_H */
