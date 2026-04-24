/*
 * Shared constants between arm_smmu_noshutdown.c and smmu_probe.c.
 * Kept in a single header so the two modules can't drift from each
 * other as the SLM-OS NC region layout evolves.
 *
 * Licence: GPL v2.
 */
#ifndef SLMOS_ARM_SMMU_NOSHUTDOWN_H
#define SLMOS_ARM_SMMU_NOSHUTDOWN_H

/*
 * SLM-OS's non-cacheable region on Jetson Orin Nano — the 2 MB block
 * at the top of RAM region 1, just below the OP-TEE carveout. Every
 * buffer SLM-OS's xHCI driver hands to the controller (DCBAA,
 * scratchpads, command/event ring TRBs, ERST) comes out of this
 * range via `ncmem_alloc`. See SLM-OS kernel/mm/vmm.c and
 * kernel/CLAUDE.md "Non-Cacheable Shared Memory" for why the region
 * is where it is.
 *
 * An identity mapping covering these 2 MB on the xusb stream's
 * iommu_domain lets SLM-OS program DCBAAP / CRCR with raw physical
 * addresses and have them translate through the SMMU unchanged
 * (IOVA == PA) without touching the SMMU's context bank layout.
 */
#define SLMOS_NC_BASE   0xBDE00000UL
#define SLMOS_NC_SIZE   (2UL * 1024 * 1024)   /* 2 MB */

#define SLMOS_XHCI_SLOT3_HANDOFF_PHYS   0xBDE00000UL
#define SLMOS_XHCI_SLOT3_HANDOFF_MAGIC  0x58483348U
#define SLMOS_XHCI_SLOT3_HANDOFF_VER    1U

struct slmos_xhci_slot3_handoff {
    u32 magic;
    u32 version;
    u32 slot_id;
    u32 root_port;
    u64 dcbaap;
    u64 devctx_phys;
    u64 ep0_deq_phys;
    u32 slot_ctx_dw[4];
    u32 ep0_ctx_dw[8];
};

#endif /* SLMOS_ARM_SMMU_NOSHUTDOWN_H */
