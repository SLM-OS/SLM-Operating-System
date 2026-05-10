/*
 * pcie_bcm2712.h — BCM2712 `pcie1` root-complex driver-private API.
 *
 * The full bring-up + ECAM + MSI implementation lives in pcie_bcm2712.c
 * and is reached through the cross-platform `pcie_*` API in
 * `kernel/include/pcie.h`. This header exposes the small surface that
 * other in-tree subsystems need to reach into the BCM2712 RC directly
 * — diagnostics that don't fit the abstract `pcie_host_ops` vtable.
 *
 * RASPI5-only — non-Pi5 platforms must not include this header.
 */

#ifndef PCIE_BCM2712_H
#define PCIE_BCM2712_H

#if defined(PLATFORM_RASPI5)

/*
 * Diagnostic-only: dump the BCM2712 RC bridge status + error
 * registers (PCIE_STATUS, UBUS_CTRL, AXI_INTF_CTRL, AXI_READ_ERROR_DATA,
 * MISC_CTRL_1) with the supplied label. Useful after a suspected
 * fw-side DMA stall to see whether the RC has captured a TLP
 * completion timeout, link downgrade, or AXI read-error substitution.
 *
 * Read-only — no side effects on the RC. Caller is responsible for
 * gating call sites under HAILO_WIRE_DEBUG (or equivalent) to avoid
 * UART noise on the production hot path.
 */
void pcie_bcm2712_dump_status_for_debug(const char *label);

#endif /* PLATFORM_RASPI5 */

#endif /* PCIE_BCM2712_H */
