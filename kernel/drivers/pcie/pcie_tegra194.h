/*
 * pcie_tegra194.h — Tegra234 PCIe C8 root complex bring-up (private).
 *
 * Brings up the Tegra PCIe controller from scratch using BPMP IPC + direct
 * MMIO to the APPL wrapper + DesignWare DBI + iATU. Targets the RTL8168
 * NIC behind PCIe C8 on the Jetson Orin Nano Super Dev Kit.
 *
 * Reference: ~/slmos-ref/linux/linux-pcie-tegra194.c
 *            ~/slmos-ref/linux/linux-pcie-designware-host.c
 *            ~/slmos-ref/linux/linux-pcie-designware.c
 *
 * Flow:
 *    pcie_tegra_host_init()       - BPMP UPHY/clock/reset + APPL regs
 *    pcie_tegra_start_link()      - toggle PEX_RST + LTSSM_EN, wait for L0
 *    pcie_tegra_probe_endpoint()  - iATU program for bus 1, read VID/DID
 */

#ifndef PCIE_TEGRA194_H
#define PCIE_TEGRA194_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Full APPL + clock/reset/UPHY bring-up for PCIe C8.
 * Returns 0 on success, negative on any failure.
 */
int pcie_tegra_host_init(void);

/*
 * Toggle PEX_RST on the PCIe slot, set LTSSM_EN, then poll APPL_DEBUG
 * for LTSSM state = L0 (0x11).
 *   timeout_ms  Max time to wait for link training.
 *   *ltssm_out  If non-NULL, filled with the final LTSSM state reached.
 * Returns 0 on link up, -1 on timeout, -2 on not initialised.
 */
int pcie_tegra_start_link(uint32_t timeout_ms, uint32_t *ltssm_out);

/*
 * After link is up, program iATU region 0 for CFG1 access at bus 1 dev 0
 * func 0 and return the 32-bit word at CFG_BASE + 0 (VID/DID).
 * Returns 0 on success, negative on error; *vid_did is populated on success.
 */
int pcie_tegra_probe_endpoint(uint32_t *vid_did_out);

/* Diagnostic snapshot used by the pcietrain shell command. */
struct pcie_tegra_snapshot {
    uint32_t appl_ctrl;
    uint32_t appl_debug;
    uint32_t appl_pinmux;
    uint32_t appl_link_status;
    uint32_t dbi_bus0_vid_did;
    uint32_t ltssm_state;      /* APPL_DEBUG[8:3] */
    bool     ltssm_en;
    bool     rc_alive;
};

void pcie_tegra_read_snapshot(struct pcie_tegra_snapshot *out);

#endif /* PCIE_TEGRA194_H */
