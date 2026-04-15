/*
 * bringup.h — GSP-RM bringup state machine for x86-64 / Jetson (E3.4).
 *
 * Orchestrates the multi-step sequence from "GPU powered on, BAR0/1
 * mapped, FWSEC extracted" to "GSP-RM running on GPU RISC-V, RPC ring
 * alive". The five steps:
 *
 *   1. FWSEC-FRTS on GSP Falcon — sets up WPR2 region in FB
 *   2. Booter Load on SEC2 Falcon — validates + loads GSP-RM into WPR2
 *   3. GSP RISC-V startup — flip BCR_CTRL to RISC-V, release reset
 *   4. GSP-RM RPC handshake — wait for GSP_INIT_DONE event
 *   5. Ready
 *
 * All state carried in a caller-allocated `struct gsp_bringup`. Each
 * step is an independent function so the harness can run them in
 * isolation and print intermediate state — critical for E3 debug
 * where silent hangs are the norm.
 */

#ifndef GPU_NVIDIA_BRINGUP_H
#define GPU_NVIDIA_BRINGUP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "falcon.h"

enum gsp_bringup_state {
    GSP_BRINGUP_INIT,
    GSP_BRINGUP_FWSEC_FRTS_DONE,
    GSP_BRINGUP_BOOTER_LOAD_DONE,
    GSP_BRINGUP_RISCV_RUNNING,
    GSP_BRINGUP_GSP_RM_READY,
    GSP_BRINGUP_FAILED,
};

struct gsp_bringup {
    struct falcon gsp_flcn;     /* NV_PGSP_BASE */
    struct falcon sec2_flcn;    /* NV_PSEC2_BASE */

    /* FWSEC parts carved out of the VBIOS image. All pointers are
     * into VBIOS memory owned by `nvidia_vbios_platform_load`, no
     * copies. */
    const uint8_t *fwsec_desc;      /* 44-byte V3 descriptor header */
    const uint8_t *fwsec_sigs;      /* signature table */
    uint32_t       fwsec_sigs_size;
    const uint8_t *fwsec_imem;
    uint32_t       fwsec_imem_size;
    const uint8_t *fwsec_dmem;
    uint32_t       fwsec_dmem_size;
    uint32_t       fwsec_interface_offset;  /* byte offset into DMEM */
    uint32_t       fwsec_engine_id;
    uint32_t       fwsec_ucode_id;
    uint32_t       fwsec_pkc_data_off;       /* signature offset in DMEM */
    uint32_t       fwsec_imem_virt_base;     /* BOOTVEC */

    /* WPR2 region we asked FWSEC to set up. Address is in FB
     * (absolute GPU physical address). Verified post-boot by
     * reading WPR2_LO/HI BAR0 registers. */
    uint64_t       wpr2_addr;
    uint64_t       wpr2_size;

    /* DMA-mapped copies of the FWSEC IMEM/DMEM. Populated by
     * gsp_fwsec_frts() — allocated from the platform vtable so
     * the GPU can DMA from them via their IOVAs. */
    void          *dma_imem_va;
    uint64_t       dma_imem_iova;
    size_t         dma_imem_size;
    void          *dma_dmem_va;
    uint64_t       dma_dmem_iova;
    size_t         dma_dmem_size;

    enum gsp_bringup_state state;
    uint32_t last_error_phase;

    /* Diagnostic fields populated during fwsec_frts regardless of
     * success/failure — the harness reads these to show the sig
     * index selection math. */
    uint32_t diag_fuse_reg_off;
    uint32_t diag_fuse_reg_val;
    uint8_t  diag_sig_count;
    uint16_t diag_sig_versions;
    uint32_t diag_sig_index;
};

/*
 * Populate `struct falcon` for GSP + SEC2, extract FWSEC parts from
 * the already-loaded VBIOS, and compute the WPR2 target region.
 * Must be called before any of the phase functions. Returns 0 on
 * success, -1 if any step fails.
 *
 * Does NOT touch hardware beyond Falcon probe reads.
 */
int gsp_bringup_prepare(struct gsp_bringup *b);

/*
 * Phase 1: FWSEC-FRTS on GSP Falcon.
 *
 *   - Reset GSP Falcon.
 *   - DMA FWSEC IMEM + DMEM into GSP Falcon IMEM/DMEM.
 *   - Patch DMEMMAPPER to request FRTS with the wpr2 region.
 *   - Program GSP Falcon BROM (engine_id, ucode_id, PKC address).
 *   - Start, poll halt.
 *   - Read WPR2_LO/HI to confirm success.
 *
 * Returns 0 on FRTS success (WPR2 registers populated). -1 on any
 * failure with @last_error_phase set to a step identifier.
 */
int gsp_bringup_fwsec_frts(struct gsp_bringup *b);

/* E3.4 future steps (declared here, implemented in later commits): */
int gsp_bringup_booter_load(struct gsp_bringup *b);   /* TODO */
int gsp_bringup_riscv_start(struct gsp_bringup *b);   /* TODO */

#endif /* GPU_NVIDIA_BRINGUP_H */
