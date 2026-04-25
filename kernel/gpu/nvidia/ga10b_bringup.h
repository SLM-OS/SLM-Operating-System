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

/* Phase 7: Submit host-family SEMAPHORE_RELEASE as smoke test.
 * Returns 0 iff the semaphore is observed at its target VA within
 * timeout. PBDMA-decoded; bypasses GR. */
int ga10b_bringup_smoke_test(struct ga10b_bringup *b);

/* Phase 7 (compute variant): submit AMPERE_COMPUTE_B-class
 * SEMAPHORE_RELEASE. Same success criterion. Validates that the GR
 * engine's compute pipeline accepts method dispatch on the inherited
 * channel — a prerequisite for future QMD-based compute kernel
 * dispatch. Unblocked by PR #295 (method-header encoding fix); the
 * earlier MME_FE1 blocker (#291) was a symptom of the broken
 * encoding, not a missing MME init. */
int ga10b_bringup_smoke_test_compute(struct ga10b_bringup *b);

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
 * routed on AMPERE_CHANNEL_GPFIFO_A on GA10B). Method headers carry
 * `method_id = byte_off / 4` at bits [12:0] (NVC56F_METHOD_HEADER_INC
 * macro in ga10b_bringup.c). A prior iteration placed byte_off at
 * [11:0] instead — PBDMA advanced GP_GET on the malformed header but
 * silently discarded the method, so the sema never fired. Validated
 * live on jetson-nano-2 (2026-04-18) against nvgpu's own gv11b sema
 * cmdbuf literal encoding.
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

/* AMPERE_COMPUTE_B class ID — what SET_OBJECT binds to a subchannel
 * in the compute-class pushbuffer path. Authoritative per NVIDIA
 * clc7c0.h and nvgpu's gr_compute_class_v() in hw_gr_ga10b.h.
 * Exposed in the header so host tests assert against the symbolic
 * name rather than a bare literal. */
#define GA10B_AMPERE_COMPUTE_B_CLASS_ID  0xC7C0u

/* Phase 7 pushbuffer builder — compute-class variant (pure logic).
 *
 * Writes GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS dwords to `pb`,
 * encoding:
 *
 *   1. SET_OBJECT (method 0) binding AMPERE_COMPUTE_B (class 0xC7C0)
 *      to subchannel 1. NVK's nv_push.h pins compute classes
 *      (0x90C0..0xC7C0) to subch 1 and graphics classes
 *      (0x9097..0xC797) to subch 0; nvgpu enforces this via
 *      validate_class_veid_pbdma and rejects the (COMPUTE_B, subch 0,
 *      veid>=1) tuple as CLASS_SUBCH_MISMATCH. This was the root
 *      cause of #273.
 *   2. AMPERE_COMPUTE_B's own REPORT_SEMAPHORE_* methods at byte
 *      offsets 0x158..0x168 (from clc7c0.h), all on subch 1. These
 *      differ from the host-channel semaphore family at 0x5C..0x6C
 *      both in byte offset and in the EXECUTE encoding — COMPUTE_B
 *      uses OPERATION_RELEASE=0 (not 1), plus STRUCTURE_SIZE bits
 *      [4:3] that must be SEMAPHORE_ONE_WORD (1<<3) for a 32-bit
 *      payload.
 *
 * **Buffer contract:** `pb` must point to at least
 * GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS uint32_t slots. Returns the
 * dword count (always GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS). */
uint32_t ga10b_build_compute_sema_release_pushbuffer(uint32_t *pb,
                                                     uint64_t sem_gpu_va,
                                                     uint32_t payload);

/* Size of the compute-kernel-launch pushbuffer in dwords.
 *
 * Layout (13 dwords):
 *   [0]     INC header SET_OBJECT
 *   [1]     class_id (AMPERE_COMPUTE_B)
 *   [2]     INC header count=2 SET_SHADER_SHARED_MEMORY_WINDOW_A
 *   [3-4]   upper (0) / lower (0xfe000000) of shared-mem window base
 *   [5]     INC header count=2 SET_SHADER_LOCAL_MEMORY_WINDOW_A
 *   [6-7]   upper (0) / lower (0xff000000) of local-mem window base
 *   [8]     IMMD INVALIDATE_SKED_CACHES
 *   [9]     IMMD INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI
 *   [10]    INC header SEND_PCAS_A
 *   [11]    QMD address shifted right 8
 *   [12]    IMMD SEND_SIGNALING_PCAS2_B action=INVALIDATE_COPY_SCHEDULE */
#define GA10B_LAUNCH_KERNEL_PB_DWORDS 13u

/* Pure-logic builder for the compute-kernel-launch pushbuffer. QMD
 * must be 256-byte aligned so that qmd_gpu_va >> 8 fits the
 * SEND_PCAS_A_QMD_ADDRESS_SHIFTED8 field.
 *
 * **Ampere vs Turing split:** Mesa's NVK uses SEND_SIGNALING_PCAS_B
 * for `cls_compute <= TURING_COMPUTE_A` and SEND_SIGNALING_PCAS2_B
 * for newer (Ampere+). GA10B is Ampere, so this builder always emits
 * PCAS2_B with PCAS_ACTION = INVALIDATE_COPY_SCHEDULE (0xA). Using
 * the Turing-era PCAS_B on Ampere silently no-ops the dispatch
 * while still advancing GP_GET — observed empirically on
 * jetson-nano-1 (2026-04-21) before the fix; documented here so a
 * future cross-arch refactor doesn't regress.
 *
 * Buffer contract: `pb` must point to at least
 * GA10B_LAUNCH_KERNEL_PB_DWORDS uint32_t slots. */
uint32_t ga10b_build_launch_kernel_pushbuffer(uint32_t *pb,
                                              uint64_t qmd_gpu_va);

/* Phase 8: launch a pre-uploaded compute kernel.
 *
 * Requires a v3 channel handoff (shader / QMD / output pre-populated
 * by scripts/gpu-kernel-launch.c --preserve-for-kexec). Zeros the
 * output buffer, posts the launch pushbuffer, rings the doorbell,
 * and polls output_phys for GA10B_SMOKETEST_SEM_PAYLOAD.
 *
 * Returns 0 iff the kernel output reads back as the expected
 * payload. Returns -1 if:
 *   - `b` is NULL or state is not CHANNEL_OPEN / METHOD_ACCEPTED;
 *   - `g_handoff.version` is below 3 (v2 channel-only handoff —
 *     Phase 8 needs the v3 kernel-state extension);
 *   - `g_handoff.qmd_gpu_va` or `g_handoff.output_phys` is zero
 *     (v3 handoff is present but its kernel-launch fields were
 *     never populated — the helper was run without
 *     --preserve-for-kexec, or the kernel pre-kexec run failed
 *     before the handoff write);
 *   - the payload does not land at `output_phys` within the
 *     2 s poll timeout.
 * On payload-timeout the shared submit helper logs whether PBDMA
 * consumed the pushbuffer, so the caller can tell "GPU didn't see
 * our submit" from "GPU saw it but the shader didn't fire". */
int ga10b_bringup_launch_kernel(struct ga10b_bringup *b);

/*
 * Top-level runner. Walks phases 1–7 in order and returns 0 iff the
 * smoke test passes. Equivalent to gsp_init() for the nvgpu path.
 */
int ga10b_bringup_run(struct ga10b_bringup *b);

/*
 * Read the final pipeline op's output buffer into `out`. Used after a
 * successful ga10b_bringup_launch_kernel() on a v5 handoff to extract
 * the model's classifier output.
 *
 * The buffer copied from is the LAST op's `output_phys` in the v5
 * pipeline_ops array. Per the launcher's convention this is the
 * SENTINEL CELL address (= buffer base + sentinel_cell_idx × 4); for
 * MNIST the final op (AddBias on logits) uses sentinel_cell_idx = 0
 * so the sentinel address equals the logits buffer base, and reading
 * `cap` bytes from it gets the full 10-element fp32 logits vector.
 *
 * Models whose final op picks a non-zero sentinel cell would need a
 * separate "buffer base" carried in the v5 op struct; defer to a
 * follow-up if any such model lands.
 *
 * Returns the number of bytes copied on success, -1 if no v5
 * pipeline is loaded or `out`/`b` is NULL.
 */
int ga10b_bringup_read_pipeline_output(struct ga10b_bringup *b,
                                        void *out, size_t cap);

#endif /* GPU_NVIDIA_GA10B_BRINGUP_H */
