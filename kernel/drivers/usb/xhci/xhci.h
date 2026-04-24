/*
 * xhci.h - SLM-OS xHCI host controller driver (#266 Phase 3A)
 *
 * Jetson-only today. Drives the Tegra234 XHCI controller at
 * TEGRA_XHCI_HCD_BASE with the USB 2.0 host-mode subset of the
 * xHCI 1.2 spec. Scope deliberately excludes USB 3.x SuperSpeed
 * and hub topology per docs/jetson-usb-networking-plan.md §6.
 *
 * Public entry points are wired from platform init (main.c) on
 * Jetson only. On every other platform this file is a no-op (the
 * init function is defined behind `#if defined(PLATFORM_JETSON_*)`).
 */

#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include <stdbool.h>

/*
 * One-time probe: map the controller base (already in the VMM's MMIO
 * L2), parse the capability registers, halt and reset the controller,
 * and cache the parsed shape in the driver's state. Does NOT register
 * with usb_core yet — ring / port / transfer support comes in later
 * Phase-3A steps. Returns 0 on success, negative on failure.
 */
int xhci_init(void);

/* Diagnostic dump used by the `xhci` shell command. Safe to call at
 * any time after xhci_init; returns false if the driver is not live. */
bool xhci_dump_info(void);

/*
 * Issue a command-ring NO_OP against the live controller. Returns 0 on
 * success, negative on timeout / transport failure.
 */
int xhci_cmd_noop_probe(void);

/*
 * Test-visible accessors (no test harness yet; these let unit tests
 * and the shell read parsed capabilities without touching MMIO).
 */
struct xhci_caps {
    uint8_t  cap_length;
    uint16_t hci_version;
    uint32_t hcs_params1;
    uint32_t hcs_params2;
    uint32_t hcs_params3;
    uint32_t hcc_params1;
    uint32_t hcc_params2;
    uint32_t db_off;
    uint32_t rts_off;
    uint8_t  max_slots;
    uint8_t  max_ports;
    uint16_t max_intrs;
    bool     ac64;
    bool     ctx_64;
    uint16_t xecp;
};

/* Returns the cached caps. Zeroed struct if xhci_init hasn't run. */
void xhci_get_caps(struct xhci_caps *out);

#endif /* XHCI_H */
