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

    /* Booter Load state (E3.4.d). The booter ucode is a small HS-
     * signed Falcon program shipped as booter_load-535.113.01.bin.
     * It runs on SEC2 and reads MAILBOX0/1 as a phys address pointing
     * at a `GspFwWprMeta` struct in sysmem. */
    void          *dma_booter_va;        /* mutable copy of booter image */
    uint64_t       dma_booter_iova;
    size_t         dma_booter_size;
    uint32_t       booter_dmem_sign;     /* DMEM byte offset for sig patch */
    uint32_t       booter_engine_id;     /* SEC2 == 0x1 */
    uint32_t       booter_ucode_id;      /* per-engine fuse-locked id */
    uint32_t       booter_imem_sec_off;  /* IMEM offset where sec section lands */
    uint32_t       booter_imem_sec_size;
    uint32_t       booter_imem_ns_size;  /* non-secure section comes first */
    uint32_t       booter_dmem_offset;   /* file byte offset of DMEM portion */
    uint32_t       booter_dmem_size;
    uint32_t       booter_boot_addr;     /* BOOTVEC = NS code start */
    uint32_t       booter_mbox0_post;    /* MAILBOX0 read after halt — diagnostics */

    /* WprMeta DMA buffer (E3.4.d input). Booter reads this struct
     * via the MAILBOX-handed pointer to find GSP-RM ELF, signature,
     * bootloader, etc. For E3.4 we allocate the buffer but only
     * partially populate it — the booter halts with a non-zero
     * MAILBOX0 error code in that case, which is enough to prove
     * the bringup plumbing reached SEC2. Full WprMeta layout is
     * tracked as part of the GSP RPC work in E4. */
    void          *dma_wpr_meta_va;
    uint64_t       dma_wpr_meta_iova;
    size_t         dma_wpr_meta_size;

    /* GSP RISC-V state (E3.4.e). Set after gsp_bringup_riscv_start. */
    bool           gsp_riscv_active;
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

/*
 * Phase 2 (E3.4.d): Booter Load on SEC2.
 *
 *   - Load booter_load-535.113.01.bin via the platform firmware
 *     accessor and parse its nvfw_bin_hdr → hs_header_v2 →
 *     load_header_v2 chain.
 *   - Allocate a DMA-mapped, mutable copy of the booter's data
 *     section and patch signature 0 into the location declared by
 *     the HS header (the BROM verifies it via PARAADDR0).
 *   - Allocate a DMA buffer for GspFwWprMeta and zero it (full
 *     layout populated in the E4 RPC step).
 *   - Reset SEC2, pre-PIO setup, PIO upload non-secure IMEM,
 *     secure IMEM, then DMEM (matches nouveau's `gm200_flcn_fw`
 *     loader path that GA10x SEC2 booter uses).
 *   - Program SEC2 BROM (PARAADDR0/UCODE_ID/ENGIDMASK/MOD_SEL=RSA3K).
 *   - Set MAILBOX0/1 = wpr_meta phys addr lo/hi, BOOTVEC = NS
 *     section start, kick STARTCPU, poll halt.
 *   - On halt, read MAILBOX0; capture into @booter_mbox0_post for
 *     diagnostics. 0 == booter completed without error; non-zero
 *     == booter halted but reported a status (commonly because
 *     WprMeta is incomplete).
 *
 * Returns 0 if SEC2 halts within the timeout regardless of mailbox
 * value (the halt itself is the milestone — interpretation of
 * MAILBOX0 is the caller's job). -1 on timeout / setup failure
 * with @last_error_phase populated.
 */
int gsp_bringup_booter_load(struct gsp_bringup *b);

/*
 * Phase 3 (E3.4.e): flip GSP into RISC-V mode and start the core.
 *
 * Preconditions: Booter Load ran (the booter set up the GSP-RM
 * image inside WPR2 and pre-loaded the RISC-V bootloader). This
 * function does the post-booter dance:
 *
 *   - Reset GSP Falcon.
 *   - Write BCR_CTRL = VALID | CORE_SELECT(=RISC-V) | BRFETCH at
 *     0x111668.
 *   - Set GSP boot vector / mailbox handoff (libos addr).
 *   - Release reset via STARTCPU.
 *   - Poll RISCV_CPUCTL.ACTIVE_STAT (bit 7) for "RISC-V running".
 *
 * Sets @gsp_riscv_active = true on success. Returns 0 on success,
 * -1 on timeout. Reaching this state proves the bringup chain up
 * through the RISC-V handoff worked; the next milestone (waiting
 * for GSP_INIT_DONE on the message ring) is owned by the E4 RPC
 * code.
 */
int gsp_bringup_riscv_start(struct gsp_bringup *b);

/* ---- Pure-logic helpers exposed for unit testing ----
 *
 * These are called from inside the FWSEC-FRTS state machine but
 * have no GPU dependency — they're plain byte-shuffling and
 * arithmetic that's worth testing in isolation. */

/*
 * Patch the DMEMMAPPER application interface in @dmem to request
 * FRTS, with the WPR2 region pointed at by (@wpr_addr, @wpr_size).
 *
 * Walks the app-interface table at @dmem[interface_off] looking for
 * id = 0x04, then writes:
 *   - init_cmd = 0x15 (FRTS) at app.dmem_base + 0x2c
 *   - read_vbios sub-struct (24 bytes) at app.cmd_in_buffer_offset
 *   - frts_region (20 bytes) at app.cmd_in_buffer_offset + 24
 *
 * Returns 0 on success, -1 if the interface table is malformed,
 * DMEMMAPPER isn't present, or any write would overrun @dmem_size.
 *
 * Pure function (no platform-vtable calls) — caller is responsible
 * for memcpy'ing the result into the DMA buffer.
 */
int gsp_bringup_patch_dmemmapper_frts(uint8_t *dmem, uint32_t dmem_size,
                                      uint32_t interface_off,
                                      uint64_t wpr_addr, uint64_t wpr_size);

/*
 * Pick the FWSEC signature index to patch into DMEM. Wraps the
 * exact algorithm from nouveau ga102_gsp_fwsec_signature.
 *
 *   @fuse_reg     — value read from BAR0 fuse register
 *                   (0x8241C0 + (ucode_id-1)*4)
 *   @sig_versions — V3 descriptor's SignatureVersions field
 *   @sig_count    — V3 descriptor's SignatureCount field
 *
 * Returns the chosen index (0..sig_count-1) on success. Returns
 * -1 if no signature in the ucode matches the chip's fuse version.
 *
 * Pure function — no register reads, all inputs explicit.
 */
int gsp_bringup_select_sig_index(uint32_t fuse_reg, uint16_t sig_versions,
                                 uint8_t sig_count);

#endif /* GPU_NVIDIA_BRINGUP_H */
