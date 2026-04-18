/*
 * xhci_tegra.h - Tegra234 XUSB wrapper register definitions
 *
 * Separate from `xhci_regs.h` (which is the Intel-spec xHCI register
 * map). This file covers the Tegra-specific wrapper banks — FPCI at
 * 0x03600000 and BAR2 at 0x03650000 — that must be programmed BEFORE
 * the Intel-spec xHCI aperture at 0x03610000 will respond to RUN=1.
 *
 * Source of truth for these offsets is Linux's tegra-xusb driver
 * (`docs/reference/linux-xhci-tegra.c`). Every macro below cites the
 * upstream line number so future audits can cross-check.
 *
 * Only Tegra234 is addressed here — earlier Tegras (210/186/194) use
 * a different CSB path (`XUSB_CFG_CSB_BASE_ADDR` via FPCI) and also
 * have an IPFS wrapper that Tegra234 drops. Since SLM-OS only targets
 * Jetson Orin Nano (Tegra234), the earlier variants are out of scope.
 */

#ifndef XHCI_TEGRA_H
#define XHCI_TEGRA_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* FPCI CFG registers (relative to TEGRA_XHCI_FPCI_BASE = 0x03600000)          */
/* linux-xhci-tegra.c:40-60                                                    */
/* -------------------------------------------------------------------------- */

#define XUSB_CFG_1                      0x004U
#define  XUSB_IO_SPACE_EN               (1u << 0)
#define  XUSB_MEM_SPACE_EN              (1u << 1)
#define  XUSB_BUS_MASTER_EN             (1u << 2)

#define XUSB_CFG_4                      0x010U
#define  XUSB_BASE_ADDR_SHIFT           15
#define  XUSB_BASE_ADDR_MASK            0x1ffffU

#define XUSB_CFG_7                      0x01cU
#define  XUSB_BASE2_ADDR_SHIFT          16
#define  XUSB_BASE2_ADDR_MASK           0xffffU

#define XUSB_CFG_ARU_C11_CSBRANGE       0x41cU
#define XUSB_CFG_CSB_BASE_ADDR          0x800U

/*
 * FPCI config space at offset 0 is a standard PCI header — Tegra234
 * XHCI reports 0x229810de (NVIDIA Tegra XHCI). Used as a sanity check
 * post-kexec to confirm the wrapper aperture is reachable before the
 * driver starts writing BARs.
 */
#define XUSB_FPCI_DEV_VENDOR_ID         0x000U
#define XUSB_FPCI_DEV_VENDOR_EXPECTED   0x229810deU

/* -------------------------------------------------------------------------- */
/* BAR2 wrapper registers (relative to TEGRA_XHCI_BAR2_BASE = 0x03650000)      */
/* linux-xhci-tegra.c:82-94                                                    */
/* -------------------------------------------------------------------------- */

#define XUSB_BAR2_ARU_MBOX_CMD                  0x004U
#define XUSB_BAR2_ARU_MBOX_DATA_IN              0x008U
#define XUSB_BAR2_ARU_MBOX_DATA_OUT             0x00cU
#define XUSB_BAR2_ARU_MBOX_OWNER                0x010U
#define XUSB_BAR2_ARU_SMI_INTR                  0x014U
#define XUSB_BAR2_ARU_SMI_ARU_FW_SCRATCH_DATA0  0x01cU
#define XUSB_BAR2_ARU_IFRDMA_CFG0               0x0e0U
#define XUSB_BAR2_ARU_IFRDMA_CFG1               0x0e4U
#define XUSB_BAR2_ARU_IFRDMA_STREAMID_FIELD     0x0e8U
#define XUSB_BAR2_ARU_C11_CSBRANGE              0x09cU
#define XUSB_BAR2_ARU_FW_SCRATCH                0x1000U
#define XUSB_BAR2_CSB_BASE_ADDR                 0x2000U

/* -------------------------------------------------------------------------- */
/* CSB paging — the 512-byte windowed access path into Falcon state.           */
/* linux-xhci-tegra.c:112-117                                                  */
/* -------------------------------------------------------------------------- */

#define XUSB_CSB_PAGE_SELECT_SHIFT      9
#define XUSB_CSB_PAGE_SELECT_MASK       0x7fffffU
#define XUSB_CSB_PAGE_OFFSET_MASK       0x1ffU

static inline uint32_t xusb_csb_page_select(uint32_t addr)
{
    return (addr >> XUSB_CSB_PAGE_SELECT_SHIFT) & XUSB_CSB_PAGE_SELECT_MASK;
}
static inline uint32_t xusb_csb_page_offset(uint32_t addr)
{
    return addr & XUSB_CSB_PAGE_OFFSET_MASK;
}

/* -------------------------------------------------------------------------- */
/* Falcon CSB registers (accessed via CSB paging window, NOT directly)         */
/* linux-xhci-tegra.c:118-151                                                  */
/* -------------------------------------------------------------------------- */

/* Falcon processor control */
#define XUSB_FALC_CPUCTL                0x100U
#define  XUSB_FALC_CPUCTL_STARTCPU          (1u << 1)
#define  XUSB_FALC_CPUCTL_STATE_HALTED      (1u << 4)
#define  XUSB_FALC_CPUCTL_STATE_STOPPED     (1u << 5)
#define XUSB_FALC_BOOTVEC               0x104U
#define XUSB_FALC_DMACTL                0x10cU

/* CSB ARU scratch (general purpose; safe test target) */
#define XUSB_CSB_ARU_SCRATCH0           0x100100U

/* CSB memory-pool: firmware load state (populated by IFR at boot) */
#define XUSB_CSB_MP_ILOAD_BASE_LO       0x101a04U
#define XUSB_CSB_MP_ILOAD_BASE_HI       0x101a08U
#define XUSB_CSB_MP_APMAP               0x10181cU
#define  XUSB_CSB_MP_APMAP_BOOTPATH         (1u << 31)

/* -------------------------------------------------------------------------- */
/* Firmware header IOCTL — used to read the IFR's creation-timestamp header    */
/* linux-xhci-tegra.c:153-156                                                  */
/* -------------------------------------------------------------------------- */

#define XUSB_FW_IOCTL_TYPE_SHIFT        24
#define XUSB_FW_IOCTL_CFGTBL_READ       17

/*
 * Byte offset of the IFR's firmware-image creation time (UTC, seconds
 * since epoch) within the Tegra XUSB firmware header. Linux computes
 * this as offsetof(struct tegra_xusb_fw_header, fwimg_created_time),
 * which lands at 44 for the current header layout (see the struct
 * definition at linux-xhci-tegra.c:160-192). Used as the canonical
 * "is the Falcon mailbox alive?" probe because the expected value is
 * recognisable as a plausible Unix timestamp (roughly 0x64000000 ..
 * 0x68000000 for firmware built 2023-2025).
 */
#define XUSB_FW_HDR_FWIMG_CREATED_TIME_OFF  44U

#endif /* XHCI_TEGRA_H */
