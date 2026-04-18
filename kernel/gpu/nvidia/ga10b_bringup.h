/*
 * ga10b_bringup.h — GA10B (Jetson integrated Ampere) nvgpu-native bringup.
 *
 * Sibling to bringup.h, which covers discrete Ampere (GSP-RM, SEC2 Booter,
 * WPR2). GA10B ships different firmware and uses a different boot model:
 *
 *   1. ACR-GSP on GSP Falcon — the root of trust; loads + verifies the
 *      LS (Low-Security) ucodes into WPR and hands control to each.
 *   2. FECS (GR front-end) — context-switch ucode on GR Falcon.
 *   3. GPCCS (per-GPC front-end) — context-switch ucode on GPC Falcon.
 *   4. PMU — power / thermal management on PMU Falcon.
 *   5. Address-space / inst-block setup — minimal GMMU config.
 *   6. Channel creation — runlist + pushbuffer ring.
 *   7. First method submission — NOP + SEMAPHORE_RELEASE smoke test.
 *
 * All 11 functions of `struct gsp_platform_ops` (gsp.h) still apply —
 * we reuse the vtable dispatch layer, the Falcon driver (falcon.c),
 * and the gpu_platform MMIO helpers unchanged. The phases themselves
 * are net-new.
 *
 * See docs/archive/investigations/jetson-nvgpu-bringup-research.md for the architectural
 * motivation and per-phase details.
 */

#ifndef GPU_NVIDIA_GA10B_BRINGUP_H
#define GPU_NVIDIA_GA10B_BRINGUP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "falcon.h"

enum ga10b_bringup_state {
    GA10B_BRINGUP_INIT,
    GA10B_BRINGUP_ACR_RUNNING,
    GA10B_BRINGUP_FECS_UP,
    GA10B_BRINGUP_GPCCS_UP,
    GA10B_BRINGUP_PMU_UP,
    GA10B_BRINGUP_ENGINES_READY,
    GA10B_BRINGUP_CHANNEL_OPEN,
    GA10B_BRINGUP_METHOD_ACCEPTED,
    GA10B_BRINGUP_FAILED,
};

/*
 * GA10B firmware blob identifiers. Mirrors enum gsp_firmware_kind in
 * gsp.h but uses a distinct enum because the ucode set is entirely
 * different (no GSP-RM, no Booter Load, no bootloader).
 */
enum ga10b_firmware_kind {
    GA10B_FW_ACR_TEXT = 0,
    GA10B_FW_ACR_DATA,
    GA10B_FW_ACR_MANIFEST,
    GA10B_FW_FECS,              /* ucode descriptor; actual code inside ACR */
    GA10B_FW_FECS_SIG,          /* PKC signature for FECS */
    GA10B_FW_GPCCS,
    GA10B_FW_GPCCS_SIG,
    GA10B_FW_PMU_IMAGE,
    GA10B_FW_PMU_DESC,          /* v4 descriptor — IMEM/DMEM layout */
    GA10B_FW_PMU_SIG,
    GA10B_FW_NET_A,             /* cold-boot NET image (default pick) */
    GA10B_FW_NET_B,
    GA10B_FW_NET_C,
    GA10B_FW_NET_D,
    GA10B_FW_SAFETY_TEXT,       /* safety scheduler — optional */
    GA10B_FW_SAFETY_DATA,
    GA10B_FW_SAFETY_MANIFEST,
    GA10B_FW_KIND_COUNT,
};

struct ga10b_firmware_blob {
    const uint8_t *data;
    size_t         size;
};

/*
 * Fetch a firmware blob. Returns 0 on success, -1 if the build was not
 * configured with -DGA10B_FIRMWARE_DIR=... (no firmware embedded).
 *
 * The memory backing `*out` is read-only rodata in the kernel image;
 * callers must not free it. Lifetime is the kernel's.
 */
int ga10b_firmware_get(enum ga10b_firmware_kind kind,
                       struct ga10b_firmware_blob *out);

/*
 * Top-level state carried across phases. Allocate on the caller's
 * stack (it's ~200 bytes) or as a static. Passed to every phase
 * function so all diagnostic state is discoverable from outside.
 */
struct ga10b_bringup {
    struct falcon gsp_flcn;     /* GSP Falcon — runs ACR, then idle */
    struct falcon fecs_flcn;    /* GR front-end Falcon */
    struct falcon gpccs_flcn;   /* GPC Falcon (first GPC only for GA10B) */
    struct falcon pmu_flcn;     /* PMU Falcon */

    enum ga10b_bringup_state state;
    int last_error_phase;       /* -1 = no failure */

    /* WPR region carved out of FB/unified memory for ACR */
    uint64_t wpr_phys;          /* GPU-visible base */
    uint32_t wpr_size;          /* bytes */

    /* Instance block for the first channel (phase 6) */
    uint64_t inst_block_phys;
    uint32_t inst_block_size;
};

/* ---- Phase entry points ----
 *
 * Each function advances `b->state` on success. On failure, sets
 * b->last_error_phase and returns -1. The harness calls these in
 * order for an end-to-end bringup, or individually for debugging.
 */

int ga10b_bringup_prepare(struct ga10b_bringup *b);

/* Inherit: detect Linux's already-bootstrapped Falcon state (Path 3
 * of #190). After a --no-gpu-suspend kexec, the GPU stays powered and
 * ACR/FECS/GPCCS are already in PASS state. This function verifies
 * that and jumps the state machine directly to PMU_UP, skipping
 * phases 1–4. Returns 0 on success, -1 if the state isn't clean. */
int ga10b_bringup_inherit(struct ga10b_bringup *b);

/* Phase 1: ACR on GSP Falcon — establishes WPR, loads LS ucodes. */
int ga10b_bringup_acr(struct ga10b_bringup *b);

/* Phase 2: FECS boot — GR front-end context switch. */
int ga10b_bringup_fecs(struct ga10b_bringup *b);

/* Phase 3: GPCCS boot — GPC context switch. */
int ga10b_bringup_gpccs(struct ga10b_bringup *b);

/* Phase 4: PMU boot — power/thermal management. */
int ga10b_bringup_pmu(struct ga10b_bringup *b);

/* Phase 5: FECS method gateway — submit DISCOVER_IMAGE_SIZE via the
 * FECS push registers to verify the GR engine is alive post-inherit.
 * No channel, GMMU, or page tables needed. Advances state to
 * ENGINES_READY on success. */
int ga10b_bringup_address_space(struct ga10b_bringup *b);

/* Phase 6: Channel + pushbuffer allocation. */
int ga10b_bringup_channel(struct ga10b_bringup *b);

/* Phase 7: Submit NOP + SEMAPHORE_RELEASE as smoke test. Returns 0
 * iff the semaphore is observed at its target VA within timeout. */
int ga10b_bringup_smoke_test(struct ga10b_bringup *b);

/* Size of the Phase 7 SEMAPHORE_RELEASE pushbuffer in dwords. */
#define GA10B_SEMA_RELEASE_PB_DWORDS  10u

/* Phase 7 pushbuffer builder (pure logic — no MMIO, no globals).
 *
 * Writes GA10B_SEMA_RELEASE_PB_DWORDS dwords to `pb`, encoding a host
 * SEMAPHORE_RELEASE that will cause the GPU to write `payload` (32-bit)
 * to `sem_gpu_va` after completing all prior work (RELEASE_WFI_EN).
 *
 * The encoding uses the Volta+ new-style host-semaphore methods at
 * byte offsets 0x5C–0x6C (legacy SEMAPHOREA/B/C/D at 0x10–0x1C aren't
 * routed on AMPERE_CHANNEL_GPFIFO_A on GA10B). Method headers follow
 * the Fermi-family [12:2] METHOD_ADDRESS layout — common prior bug is
 * shifting method-index values right by 2, which lands them at the
 * wrong bit position and PBDMA decodes them as different methods.
 *
 * **Buffer contract:** `pb` must point to at least
 * GA10B_SEMA_RELEASE_PB_DWORDS uint32_t slots. Returns the dword
 * count written (always GA10B_SEMA_RELEASE_PB_DWORDS). Exposed in the
 * public header so host tests can verify the encoding without running
 * on hardware. */
uint32_t ga10b_build_sema_release_pushbuffer(uint32_t *pb,
                                             uint64_t sem_gpu_va,
                                             uint32_t payload);

/* Size of the COMPUTE_B SEMAPHORE_RELEASE pushbuffer in dwords.
 *
 * Layout: SET_OBJECT header+data + 5 semaphore method header+data
 * pairs = 12 dwords. */
#define GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS  12u

/* Phase 7 pushbuffer builder — compute-class variant (pure logic).
 *
 * Writes GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS dwords to `pb`,
 * encoding:
 *
 *   1. SET_OBJECT (method 0) binding AMPERE_COMPUTE_B (class 0xC7C0)
 *      to subchannel 0. Without this, the GR engine raises
 *      CLASS_SUBCH_MISMATCH on the first real compute method.
 *   2. AMPERE_COMPUTE_B's own REPORT_SEMAPHORE_* methods at byte
 *      offsets 0x158..0x168 (from clc7c0.h). These differ from the
 *      host-channel semaphore family at 0x5C..0x6C both in byte
 *      offset and in the EXECUTE encoding — COMPUTE_B uses
 *      OPERATION_RELEASE=0 (not 1), plus STRUCTURE_SIZE bits [4:3]
 *      that must be SEMAPHORE_ONE_WORD (1<<3) for a 32-bit payload.
 *
 * **State needed at dispatch time for this to succeed:** the
 * channel's GR context must be loaded and the subchannel must be
 * able to accept a class bind. As of 2026-04-18 this fails with
 * GR FE CLASS_SUBCH_MISMATCH on channels set up via the Linux
 * helper — the bind itself is rejected before the semaphore methods
 * are processed. Tracked in issue #273. This builder is committed
 * so whoever resolves #273 can call it directly without re-deriving
 * the COMPUTE_B encoding.
 *
 * **Buffer contract:** `pb` must point to at least
 * GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS uint32_t slots. Returns the
 * dword count (always GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS). */
uint32_t ga10b_build_compute_sema_release_pushbuffer(uint32_t *pb,
                                                     uint64_t sem_gpu_va,
                                                     uint32_t payload);

/*
 * Top-level runner. Walks phases 1–7 in order and returns 0 iff the
 * smoke test passes. Equivalent to gsp_init() for the nvgpu path.
 */
int ga10b_bringup_run(struct ga10b_bringup *b);

#endif /* GPU_NVIDIA_GA10B_BRINGUP_H */
