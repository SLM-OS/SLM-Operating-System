/*
 * xhci_regs.h - xHCI register map + bit definitions
 *
 * Subset of the Intel xHCI 1.2 specification used by SLM-OS's XHCI
 * host-controller driver (#266 Phase 3A). Only registers the driver
 * actually touches are defined — the full spec is ~500 pages and
 * most of it is USB 3.x SuperSpeed which Phase 3A explicitly avoids.
 *
 * Offsets are relative to the controller's MMIO base (on Jetson:
 * TEGRA_XHCI_HCD_BASE = 0x03610000). Three register banks:
 *
 *   Capability Registers: offset 0, fixed size determined by the
 *     CAPLENGTH byte at offset 0. Read-only (mostly).
 *
 *   Operational Registers: offset CAPLENGTH. Runtime control — start
 *     / stop / reset, DCBAAP, CRCR, port status.
 *
 *   Runtime Registers: offset RTSOFF (read from HCSPARAMS1). Interrupter
 *     state (event ring, IMAN/IMOD).
 *
 *   Doorbell Array: offset DBOFF (read from HCCPARAMS1). One 32-bit
 *     register per device slot (slot 0 = command ring).
 */

#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Capability Registers                                                        */
/* -------------------------------------------------------------------------- */

#define XHCI_CAP_CAPLENGTH          0x00    /* u8 — ops base offset */
#define XHCI_CAP_HCIVERSION         0x02    /* u16 — BCD (0x0120 = xHCI 1.20) */
#define XHCI_CAP_HCSPARAMS1         0x04    /* u32 */
#define XHCI_CAP_HCSPARAMS2         0x08    /* u32 */
#define XHCI_CAP_HCSPARAMS3         0x0C    /* u32 */
#define XHCI_CAP_HCCPARAMS1         0x10    /* u32 */
#define XHCI_CAP_DBOFF              0x14    /* u32 — doorbell array offset */
#define XHCI_CAP_RTSOFF             0x18    /* u32 — runtime regs offset */
#define XHCI_CAP_HCCPARAMS2         0x1C    /* u32 */

/* HCSPARAMS1 field extraction */
#define XHCI_HCS1_MAX_SLOTS(x)      ((x)        & 0xFF)
#define XHCI_HCS1_MAX_INTRS(x)      (((x) >>  8) & 0x7FF)
#define XHCI_HCS1_MAX_PORTS(x)      (((x) >> 24) & 0xFF)

/* HCCPARAMS1 field extraction */
#define XHCI_HCC1_AC64(x)           ( (x)        & 0x1)    /* 64-bit addressing */
#define XHCI_HCC1_CSZ(x)            (((x) >>  2) & 0x1)    /* 0: 32-byte ctx, 1: 64-byte ctx */
#define XHCI_HCC1_PPC(x)            (((x) >>  3) & 0x1)    /* port power control */
#define XHCI_HCC1_XECP(x)           (((x) >> 16) & 0xFFFF) /* extended cap offset, dword-stride */

/* -------------------------------------------------------------------------- */
/* Operational Registers (offset = CAPLENGTH)                                  */
/* -------------------------------------------------------------------------- */

#define XHCI_OP_USBCMD              0x00    /* u32 */
#define XHCI_OP_USBSTS              0x04    /* u32 */
#define XHCI_OP_PAGESIZE            0x08    /* u32 */
#define XHCI_OP_DNCTRL              0x14    /* u32 */
#define XHCI_OP_CRCR                0x18    /* u64 — Command Ring Control */
#define XHCI_OP_DCBAAP              0x30    /* u64 — Device Context Base Address Array Ptr */
#define XHCI_OP_CONFIG              0x38    /* u32 */

/* Port register pairs: PORTSC at op+0x400 + 0x10*n, then PORTPMSC, PORTLI, PORTHLPMC. */
#define XHCI_OP_PORTSC(n)           (0x400 + 0x10 * (n))

/* USBCMD bits */
#define XHCI_CMD_RUN                (1u << 0)
#define XHCI_CMD_HCRST              (1u << 1)   /* host controller reset */
#define XHCI_CMD_INTE               (1u << 2)
#define XHCI_CMD_HSEE               (1u << 3)

/* USBSTS bits */
#define XHCI_STS_HCH                (1u << 0)   /* HCHalted */
#define XHCI_STS_HSE                (1u << 2)   /* host system error */
#define XHCI_STS_EINT               (1u << 3)   /* event interrupt */
#define XHCI_STS_PCD                (1u << 4)   /* port change detect */
#define XHCI_STS_SSS                (1u << 8)   /* save state status */
#define XHCI_STS_RSS                (1u << 9)   /* restore state status */
#define XHCI_STS_SRE                (1u << 10)  /* save/restore error */
#define XHCI_STS_CNR                (1u << 11)  /* controller not ready */
#define XHCI_STS_HCE                (1u << 12)  /* host controller error */

/* PORTSC bits — only the ones the driver inspects today. */
#define XHCI_PORTSC_CCS             (1u << 0)   /* current connect status */
#define XHCI_PORTSC_PED             (1u << 1)   /* port enabled */
#define XHCI_PORTSC_PR              (1u << 4)   /* port reset */
#define XHCI_PORTSC_PLS_SHIFT       5
#define XHCI_PORTSC_PLS_MASK        (0xFu << XHCI_PORTSC_PLS_SHIFT)
#define XHCI_PORTSC_LWS             (1u << 16)  /* link state write strobe */
#define XHCI_PORTSC_PP              (1u << 9)   /* port power */
#define XHCI_PORTSC_SPEED_SHIFT     10
#define XHCI_PORTSC_SPEED_MASK      (0xFu << XHCI_PORTSC_SPEED_SHIFT)
#define XHCI_PORTSC_CSC             (1u << 17)  /* connect status change */
#define XHCI_PORTSC_PEC             (1u << 18)  /* port enable/disable change */
#define XHCI_PORTSC_PRC             (1u << 21)  /* port reset change */
#define XHCI_PORTSC_PLC             (1u << 22)  /* port link state change */
/* Write-1-to-clear change bits (RW1CS in the spec). */
#define XHCI_PORTSC_RW1CS_MASK      (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC)
/*
 * Minimal "neutral write-back" subset for the PORTSC fields this driver
 * actually models. This intentionally excludes PED: on xHCI, carrying
 * bit 1 through a link-state write is not neutral.
 */
#define XHCI_PORTSC_NEUTRAL_MASK    (XHCI_PORTSC_CCS | XHCI_PORTSC_PP | \
                                     XHCI_PORTSC_PLS_MASK | \
                                     XHCI_PORTSC_SPEED_MASK)

/* PORTSC speed values (tegra-xusb reports 1 = full, 2 = low, 3 = high on USB 2). */
#define XHCI_PORTSC_SPEED_FULL      1
#define XHCI_PORTSC_SPEED_LOW       2
#define XHCI_PORTSC_SPEED_HIGH      3
#define XHCI_PORTSC_SPEED_SUPER     4

/* PORTSC link-state values (xHCI 1.2 §5.4.8). */
#define XHCI_PLS_U0                 0
#define XHCI_PLS_U3                 3
#define XHCI_PLS_RESUME             15

#endif /* XHCI_REGS_H */
