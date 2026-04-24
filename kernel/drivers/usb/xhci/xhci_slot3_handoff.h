/*
 * xhci_slot3_handoff.h - Linux -> SLM-OS handoff block for the retained
 * full-speed slot-3 path on Jetson xHCI.
 *
 * Linux writes this block into SLM-OS's NC memory just before kexec.
 * SLM-OS reads it during xHCI init so it can recover the controller's
 * live slot-3 device-context pointer even when the inherited DCBAA
 * itself lives above SLM-OS's directly probeable RAM aperture.
 */

#ifndef XHCI_SLOT3_HANDOFF_H
#define XHCI_SLOT3_HANDOFF_H

#include <stdint.h>

#define XHCI_SLOT3_HANDOFF_MAGIC  0x58483348U  /* "XH3H" */
#define XHCI_SLOT3_HANDOFF_VERSION 1U
#define XHCI_SLOT3_HANDOFF_PHYS   0xBDE00000ULL

struct xhci_slot3_handoff {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_id;
    uint32_t root_port;
    uint64_t dcbaap;
    uint64_t devctx_phys;
    uint64_t ep0_deq_phys;
    uint32_t slot_ctx_dw[4];
    uint32_t ep0_ctx_dw[8];
};

_Static_assert(sizeof(struct xhci_slot3_handoff) == 88,
               "xhci_slot3_handoff layout changed");

#endif /* XHCI_SLOT3_HANDOFF_H */
