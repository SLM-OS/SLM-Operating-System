/*
 * inference_device_hailo.c — Hailo-8 NPU backend for inference_device.h.
 *
 * Plumbs the Hailo driver (hef_parser / hef_header / hailo_control /
 * hailo_cs_translator / hailo_infer) through the common
 * inference_device vtable so scheduler and Lua bindings can route
 * inference to the NPU the same way they route to the CPU MLP.
 *
 * load_model (Phase 6.6, #179) drives the full context-switch load
 * sequence:
 *   1. Parse HEF outer header + proto body (hef_parser).
 *   2. Pick largest-pad input/output for slot sizing.
 *   3. Claim a slot.
 *   4. context_switch_load: allocate CCW + boundary DMA buffers +
 *      descriptor lists, translate HEF → wire-action contexts, and
 *      ship the 6-step RPC sequence firmware expects
 *      (RESET → SET_NETWORK_GROUP_HEADER → 4 × SET_CONTEXT_INFO →
 *      ENABLED).
 *   5. Populate slot->cfg + shapes for run().
 *
 * run() currently calls hailo_infer_run, which allocates its own
 * short-lived tensor + desc list per call. That mismatches the
 * boundary descriptor lists load_model bound to firmware via
 * OpenBoundary actions; firmware expects the boundary-channel IOVAs
 * it was told about in ACTIVATION, not whatever run() just allocated.
 * Wiring run() to reuse load_model's boundary lists (or switching to
 * a zero-copy submit path that targets them) is a follow-up — this
 * backend is today "load works end-to-end on real hardware with
 * hypothesis validation for #180; inference submit runs in QEMU only
 * via the mock auto-advance path."
 *
 * Model slots are a fixed-size table (HAILO_MAX_MODELS=4), handed
 * out as handles `idx + 1` so INF_BUILTIN_HANDLE (=0) stays reserved.
 * No dynamic allocation; all state is in BSS + DMA coherent regions
 * managed by hailo_tensor/hailo_vdma.
 */

#include "inference_device.h"
#include "hailo.h"
#include "hailo_control.h"
#include "hailo_cs_translator.h"
#include "hailo_infer.h"
#include "hailo_tensor.h"
#include "hailo_vdma.h"
#include "hef_header.h"
#include "hef_parser.h"
#include "spinlock.h"
#include "debug.h"
#include "timer.h"
#include "uart.h"
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Model slot table                                                            */
/* -------------------------------------------------------------------------- */

#define HAILO_MAX_MODELS 4

struct hailo_model_slot {
    bool                      in_use;
    struct hailo_infer_config cfg;
    /* Pad-derived input/output tensor shapes for validate-on-run.
     * Stored as H x W x C to match the HEF pad layout exactly so the
     * caller's inference_tensor_t.shape can be compared 1:1. */
    uint16_t input_shape[3];
    uint16_t output_shape[3];

    /* Context-switch load-time resources (Phase 6.6, #179). Kept in
     * the slot so free_model can release them; load allocates, run()
     * reads nothing from these directly (today). The boundary desc
     * lists were bound to firmware's VDMA channels via ACTIVATION's
     * OpenBoundary actions; a future run() refactor that reuses them
     * instead of allocating fresh lists per-call is a follow-up. */
    bool                          cs_loaded;
    struct hailo_tensor           ccw_tensor;
    struct hailo_vdma_desc_list   ccw_list;
    struct hailo_tensor           boundary_in_tensor;
    struct hailo_vdma_desc_list   boundary_in_list;
    struct hailo_tensor           boundary_out_tensor;
    struct hailo_vdma_desc_list   boundary_out_list;
};

static struct hailo_model_slot slots[HAILO_MAX_MODELS];

/*
 * Serialises slot-table mutations (in_use flag, cfg/shape init and
 * zero-out). load_model / free_model / shutdown all take this lock;
 * run() does NOT — it reads slot->cfg after load_model's IRQ-safe
 * unlock has stored the full config. Readers see either "slot
 * in_use=false" (run bails) or a fully-initialised cfg, never a torn
 * write.
 *
 * IRQ-disabling lock flavour is mandatory because run() is reachable
 * from scheduler-policy paths that can execute with IRQs disabled.
 */
static spinlock_t slots_lock = SPINLOCK_INIT;

/* Phase 8 progress tracker: updated at each cs_load stage, readable by
 * shell command `hailo stage` after a wedge. uart_puts diagnostics
 * corrupt mid-print on real HEFs for reasons unknown, so we use a
 * simple DRAM global instead — the shell recovers post-wedge and can
 * dump this to pinpoint which stage was last entered. Values:
 *   0    = idle / pre-cs_load
 *   10   = entered cs_load, ccw bounds ok
 *   11   = ccw_tensor allocated
 *   12   = ccw memcpy + prepare_for_device done
 *   13   = ccw desc_list allocated + programmed
 *   20   = boundary IN tensor done
 *   21   = boundary OUT tensor done
 *   40   = before translate_application_header
 *   41   = before translate_contexts
 *   42   = translate done
 *   50   = before CHANGE_STATUS(RESET)
 *   51   = RESET ok
 *   52   = before SET_NETWORK_GROUP_HEADER
 *   53   = NG_HEADER ok
 *   60+ix = before SET_CONTEXT_INFO(ix)  (ix = 0..3)
 *   61+ix = SET_CONTEXT_INFO(ix) ok
 *   70   = before CHANGE_STATUS(ENABLED)
 *   71   = ENABLED ok (load_model success)
 */
static volatile int cs_load_stage = 0;

static inline void cs_load_stage_set(int stage)
{
    cs_load_stage = stage;
    __asm__ volatile("dmb ish" ::: "memory");
}

int hailo_backend_get_cs_load_stage(void)
{
    return cs_load_stage;
}

/* Extern-callable setter for the translator (different compilation
 * unit) to mark sub-stage progress inside
 * hailo_cs_translate_contexts. Phase 8 diagnostic only. */
void cs_load_stage_set_raw(int stage)
{
    cs_load_stage_set(stage);
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static int hailo_err_to_inf(int rc)
{
    switch (rc) {
    case HAILO_OK:          return INF_OK;
    case HAILO_ERR_INVAL:   return INF_ERR_INVAL;
    case HAILO_ERR_NODEV:   return INF_ERR_NODEV;
    case HAILO_ERR_NOMEM:   return INF_ERR_NOMEM;
    case HAILO_ERR_TIMEOUT: return INF_ERR_TIMEOUT;
    default:                return INF_ERR_NOSUPPORT;
    }
}

/* Padded dim falls back to the unpadded one when zero. HEFs emit the
 * padded value only when it differs from the base shape. */
static uint32_t pad_dim(uint32_t padded, uint32_t base)
{
    return padded ? padded : base;
}

/* Compute byte count for an H x W x C INT8 tensor. The Hailo device
 * operates on INT8 after DFC quantization, so every pad byte is one
 * element. Returns 0 if any dim is zero (invalid pad). */
static uint32_t pad_bytes(const struct hef_pad_info *p)
{
    if (!p->has_tensor_shape) return 0;
    uint32_t h = pad_dim(p->padded_height,   p->height);
    uint32_t w = pad_dim(p->padded_width,    p->width);
    uint32_t c = pad_dim(p->padded_features, p->features);
    if (!h || !w || !c) return 0;
    /* Overflow check — each pad is realistically KB-scale, but guard
     * anyway so a malicious HEF can't walk through the uint32 size. */
    uint64_t bytes = (uint64_t)h * w * c;
    if (bytes > UINT32_MAX) return 0;
    return (uint32_t)bytes;
}

/* Pick the largest input and output pads across the HEF's pad list.
 * Multi-head vision models have several pads in each direction; we
 * size the DMA buffer to the biggest. Returns 0 on success (and
 * populates the output pointers), -1 if either direction is empty. */
static int pick_largest_pads(const struct hef_info *info,
                             const struct hef_pad_info **in_pad_out,
                             const struct hef_pad_info **out_pad_out,
                             uint32_t *in_bytes_out,
                             uint32_t *out_bytes_out)
{
    const struct hef_pad_info *in_pad = NULL;
    const struct hef_pad_info *out_pad = NULL;
    uint32_t input_bytes = 0, output_bytes = 0;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *p = &info->pads[i];
        uint32_t b = pad_bytes(p);
        if (!b) continue;
        if (p->is_input) {
            if (b > input_bytes) { input_bytes = b; in_pad = p; }
        } else {
            if (b > output_bytes) { output_bytes = b; out_pad = p; }
        }
    }
    if (!input_bytes || !output_bytes || !in_pad || !out_pad) return -1;
    *in_pad_out    = in_pad;
    *out_pad_out   = out_pad;
    *in_bytes_out  = input_bytes;
    *out_bytes_out = output_bytes;
    return 0;
}

/* Upload the HEF's CCW weight stream (best-effort).
 *
 * For v0/v1 HEFs whose CCWs are inline `write_data_ccw` actions,
 * `hailo_control_upload_ccw` writes payloads to sequential firmware
 * addresses — the original Phase 5.3 path.
 *
 * For v2+ HEFs (DFC 3.33.1 scheduler_mlp_pi5.hef), CCWs are
 * `write_data_ccw_ptr` actions pointing into the separate CCWS
 * block. Real delivery on v2+ goes through the
 * CONTEXT_SWITCH_SET_CONTEXT_INFO opcode — firmware's context
 * switcher pulls CCW payloads from host-side DMA buffers. That's a
 * separate major subsystem (not yet implemented); our
 * WRITE_MEMORY-based uploader gets rejected by firmware.
 *
 * Treat failure as non-fatal so the policy path stays exercisable
 * (hailo_infer_run will then time out cleanly when firmware never
 * produces output). This lets `bench sched-policy` run through the
 * chain and observe the timeout, which is the correct signal until
 * the context-switch protocol lands. */
/* -------------------------------------------------------------------------- */
/* Context-switch load path (Phase 6.6, #179)                                   */
/*                                                                              */
/* Replaces the prior "best-effort" WRITE_MEMORY + CONFIG_STREAM flow (which    */
/* firmware v4.23 rejects for v2+ HEFs) with the full 6-step context-switch    */
/* sequence HailoRT uses:                                                        */
/*                                                                              */
/*   1. Allocate + program the CCW DMA buffer + descriptor list; copy weights  */
/*      from the HEF's ccws_offset region into the buffer.                      */
/*   2. Allocate + program boundary input / output DMA buffers + desc lists    */
/*      sized to the HEF's pad shapes.                                           */
/*   3. Build hailo_cs_translate_cfg referencing all three descriptor list     */
/*      IOVAs.                                                                   */
/*   4. Translate application header + 4 context buffers.                       */
/*   5. RPCs: CHANGE_CONTEXT_SWITCH_STATUS(RESET) → SET_NETWORK_GROUP_HEADER   */
/*      → 4 × SET_CONTEXT_INFO → CHANGE_CONTEXT_SWITCH_STATUS(ENABLED).         */
/*   6. Store tensor + list handles in the slot so free_model can reclaim.      */
/*                                                                              */
/* On failure anywhere in the sequence, we unwind whatever allocations already  */
/* landed and return without marking cs_loaded. free_model treats !cs_loaded   */
/* slots as having no context-switch resources to release.                      */
/* -------------------------------------------------------------------------- */

/* CCW + boundary page-size defaults shared between allocation and
 * translate_cfg. The MVP hardcodes both to match ctxsmoke's
 * shell-command sizing on pi-5-1. The matching config-VDMA-channel
 * default lives in hailo_cs_translator.h alongside the boundary-
 * channel offsets and their compile-time range guards. */
#define HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE   512u
#define HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE   4096u

/* Round `bytes` up to `page_size` and return the descriptor count
 * needed to cover the region, rounded UP to the next power of two
 * (hailo_vdma_desc_list_alloc requires a power-of-2 count — the
 * VDMA engine walks the ring via (index & mask) and non-power-of-2
 * counts would corrupt on wraparound). Minimum 2
 * (HAILO_VDMA_MIN_DESC_COUNT). The VDMA engine's num_avail /
 * num_proc counters are 16-bit, so the maximum count is 65536.
 * Returns 0 if a legitimate round-up exceeds that ceiling — the
 * caller must treat 0 as "HEF too large" (HAILO_ERR_INVAL) rather
 * than silently under-programming the descriptor list. */
static uint32_t desc_count_for(uint32_t bytes, uint16_t page_size)
{
    if (page_size == 0) return 2;
    uint32_t n = (bytes + page_size - 1) / page_size;
    if (n < 2) return 2;
    /* Round up to next power of two, capped at 65536 (the VDMA
     * ring's counter width). If the rounded-up value would exceed
     * the cap, signal failure rather than saturate. */
    uint32_t p = 2;
    while (p < n) {
        if (p >= 0x10000u) return 0;
        p <<= 1;
    }
    return p;
}

/* Release context-switch load resources. Safe to call on a slot with
 * cs_loaded=false (each tensor/list alloc is zero-initialised until
 * populated, and hailo_*_free handles zeroed inputs). */
static void context_switch_unwind(struct hailo_model_slot *slot)
{
    hailo_vdma_desc_list_free(&slot->boundary_out_list);
    hailo_tensor_free(&slot->boundary_out_tensor);
    hailo_vdma_desc_list_free(&slot->boundary_in_list);
    hailo_tensor_free(&slot->boundary_in_tensor);
    hailo_vdma_desc_list_free(&slot->ccw_list);
    hailo_tensor_free(&slot->ccw_tensor);
    slot->cs_loaded = false;
}

/* The full 6-step load sequence. Called from load_model after the
 * slot has been claimed and shapes stored. Returns HAILO_OK on
 * success (slot->cs_loaded=true, resources owned by the slot) or a
 * HAILO_ERR_* on any failure (caller must release the slot). */
static int context_switch_load(struct hailo_model_slot *slot,
                               const struct hef_info *info,
                               const void *model,
                               size_t     model_size,
                               const struct hef_outer_header *outer,
                               const struct hef_pad_info *in_pad,
                               const struct hef_pad_info *out_pad)
{
    int rc;

    /* Bound the CCWS region against the caller-provided blob size.
     * hef_parse_outer_header validates (magic, version, proto range)
     * but does not bound the CCWS region — a malformed HEF with
     * ccws_offset+ccws_size overflowing into the caller's buffer
     * would OOB-read via the memcpy below. Reject such HEFs here. */
    if (outer->ccws_size > 0) {
        if (outer->ccws_offset > model_size
         || outer->ccws_size > model_size - outer->ccws_offset) {
            WARN("hailo backend: HEF CCWS out of bounds "
                 "(offset=%lu size=%lu blob=%lu)",
                 (unsigned long)outer->ccws_offset,
                 (unsigned long)outer->ccws_size,
                 (unsigned long)model_size);
            return HAILO_ERR_INVAL;
        }
    }
    cs_load_stage_set(10);

    /* Step 1: CCW buffer + descriptor list.
     * CCWs block lives in the HEF at ccws_offset; size is ccws_size
     * (possibly zero for synthetic test HEFs). A zero-CCW HEF still
     * needs a valid descriptor list for ACTIVATE_CFG_CHANNEL — we
     * allocate a minimum 512-byte scratch so firmware's walker has
     * something to DMA. */
    uint32_t ccw_bytes = (outer->ccws_size > 0) ? outer->ccws_size
                                                : HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE;
    rc = hailo_tensor_alloc(ccw_bytes, &slot->ccw_tensor);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CCW tensor alloc failed (rc=%d, bytes=%u)",
             rc, ccw_bytes);
        goto fail;
    }
    cs_load_stage_set(11);
    if (outer->ccws_size > 0) {
        const uint8_t *ccws_base = (const uint8_t *)model + outer->ccws_offset;
        memcpy(slot->ccw_tensor.cpu_addr, ccws_base, outer->ccws_size);
    } else {
        memset(slot->ccw_tensor.cpu_addr, 0, ccw_bytes);
    }
    hailo_tensor_prepare_for_device(&slot->ccw_tensor);
    cs_load_stage_set(12);

    uint32_t ccw_desc_count =
        desc_count_for(ccw_bytes, HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE);
    if (ccw_desc_count == 0) {
        WARN("hailo backend: CCW region too large for VDMA desc list (%u bytes)",
             ccw_bytes);
        rc = HAILO_ERR_INVAL;
        goto fail;
    }
    rc = hailo_vdma_desc_list_alloc(ccw_desc_count,
                                    HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE,
                                    /*circular=*/false, &slot->ccw_list);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CCW desc_list alloc failed (rc=%d)", rc);
        goto fail;
    }
    int programmed = hailo_vdma_program_buffer(&slot->ccw_list, 0,
                                               slot->ccw_tensor.iova,
                                               ccw_bytes, /*data_id=*/0);
    if (programmed < 0) {
        WARN("hailo backend: CCW program_buffer failed (rc=%d)", programmed);
        rc = HAILO_ERR_IO;
        goto fail;
    }
    cs_load_stage_set(13);

    /* Step 2: boundary tensors + desc lists — only for pads the HEF
     * marked as stream-bound. Tests HEFs without edge_layers have
     * has_stream_info=false and skip the boundary allocation. */
    uint64_t boundary_in_iova  = 0;
    uint32_t boundary_in_desc_count = 0;
    if (in_pad->has_stream_info && in_pad->core_bytes_per_buffer) {
        /* Phase 8 fix: size to FULL frame bytes, not just a single
         * periph-buffer. For ResNet-18 at 224×224×3, the HEF declares
         * core_bytes_per_buffer=672 and core_buffers_per_frame=224 —
         * a single buffer is one row; a full frame is 150528 bytes.
         * Sizing the boundary DMA tensor to just 672 bytes caused
         * firmware to reject ACTIVATION with
         * HAILO_DATAFLOW_STATUS_INVALID_RECEIVE_COMMUNICATION
         * (0x40130016) because the programmed descriptor list and
         * the frame-size-embedded OpenBoundary action disagreed.
         * Earlier test HEFs were single-buffer (buffers_per_frame=1)
         * so this path wasn't hit. */
        uint32_t bpb    = in_pad->core_bytes_per_buffer;
        uint32_t bpf    = in_pad->core_buffers_per_frame
                            ? in_pad->core_buffers_per_frame : 1u;
        uint32_t in_bytes = bpb * bpf;
        rc = hailo_tensor_alloc(in_bytes, &slot->boundary_in_tensor);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary IN tensor alloc failed (rc=%d)", rc);
            goto fail;
        }
        memset(slot->boundary_in_tensor.cpu_addr, 0, in_bytes);
        hailo_tensor_prepare_for_device(&slot->boundary_in_tensor);

        boundary_in_desc_count =
            desc_count_for(in_bytes, HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE);
        if (boundary_in_desc_count == 0) {
            WARN("hailo backend: boundary IN region too large for VDMA desc list (%u)",
                 in_bytes);
            rc = HAILO_ERR_INVAL;
            goto fail;
        }
        rc = hailo_vdma_desc_list_alloc(boundary_in_desc_count,
                                        HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE,
                                        /*circular=*/false,
                                        &slot->boundary_in_list);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary IN desc_list alloc failed (rc=%d)", rc);
            goto fail;
        }
        int prog = hailo_vdma_program_buffer(&slot->boundary_in_list, 0,
                                             slot->boundary_in_tensor.iova,
                                             in_bytes,
                                             HAILO_VDMA_HOST_DMA_DATA_ID);
        if (prog < 0) {
            WARN("hailo backend: boundary IN program_buffer failed (rc=%d)", prog);
            rc = HAILO_ERR_IO;
            goto fail;
        }
        boundary_in_iova = slot->boundary_in_list.iova;
    }
    cs_load_stage_set(20);

    uint64_t boundary_out_iova  = 0;
    uint32_t boundary_out_desc_count = 0;
    if (out_pad->has_stream_info && out_pad->core_bytes_per_buffer) {
        /* Same full-frame sizing as the input path. */
        uint32_t obpb    = out_pad->core_bytes_per_buffer;
        uint32_t obpf    = out_pad->core_buffers_per_frame
                             ? out_pad->core_buffers_per_frame : 1u;
        uint32_t out_bytes = obpb * obpf;
        rc = hailo_tensor_alloc(out_bytes, &slot->boundary_out_tensor);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary OUT tensor alloc failed (rc=%d)", rc);
            goto fail;
        }
        memset(slot->boundary_out_tensor.cpu_addr, 0, out_bytes);
        hailo_tensor_prepare_for_device(&slot->boundary_out_tensor);

        boundary_out_desc_count =
            desc_count_for(out_bytes, HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE);
        if (boundary_out_desc_count == 0) {
            WARN("hailo backend: boundary OUT region too large for VDMA desc list (%u)",
                 out_bytes);
            rc = HAILO_ERR_INVAL;
            goto fail;
        }
        rc = hailo_vdma_desc_list_alloc(boundary_out_desc_count,
                                        HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE,
                                        /*circular=*/false,
                                        &slot->boundary_out_list);
        if (rc != HAILO_OK) {
            WARN("hailo backend: boundary OUT desc_list alloc failed (rc=%d)", rc);
            goto fail;
        }
        int prog = hailo_vdma_program_buffer(&slot->boundary_out_list, 0,
                                             slot->boundary_out_tensor.iova,
                                             out_bytes,
                                             HAILO_VDMA_HOST_DMA_DATA_ID);
        if (prog < 0) {
            WARN("hailo backend: boundary OUT program_buffer failed (rc=%d)", prog);
            rc = HAILO_ERR_IO;
            goto fail;
        }
        boundary_out_iova = slot->boundary_out_list.iova;
    }
    cs_load_stage_set(21);

    /* Step 3: translate_cfg. The boundary IOVA fields are zero if the
     * HEF has no boundary edge of that direction; translate_activation
     * skips OpenBoundary emission for pads without has_stream_info,
     * so the two views stay consistent. */
    struct hailo_cs_translate_cfg tcfg = {
        .config_vdma_channel              = HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL,
        .config_stream_index              = 0,
        .ccw_desc_list_iova               = slot->ccw_list.iova,
        .ccw_desc_page_size               = HAILO_CS_DEFAULT_CCW_DESC_PAGE_SIZE,
        .ccw_total_desc_count             = ccw_desc_count,
        .boundary_input_desc_list_iova    = boundary_in_iova,
        .boundary_input_total_desc_count  = boundary_in_desc_count,
        .boundary_output_desc_list_iova   = boundary_out_iova,
        .boundary_output_total_desc_count = boundary_out_desc_count,
        .boundary_desc_page_size          = HAILO_CS_DEFAULT_BOUNDARY_PAGE_SIZE,
    };

    /* Step 4: translate. */
    cs_load_stage_set(40);
    struct hailo_cs_application_header hdr;
    rc = hailo_cs_translate_application_header(info, &tcfg, &hdr);
    if (rc != HAILO_OK) {
        WARN("hailo backend: translate_application_header failed (rc=%d)", rc);
        goto fail;
    }
    /* bufs is ~2 KB — large but still fits on the 16 KB kernel stack
     * alongside hef_info (~1.2 KB) and the caller's frame. A prior
     * version used a file-scope static here and claimed slots_lock
     * protection, but slots_lock is released before context_switch_load
     * runs (load_model only holds it to claim the slot index) — two
     * concurrent loads would have raced through a shared static.
     * Stack-local is the simplest fix and keeps this function re-
     * entrant. Tensor + desc_list allocations earlier in this call
     * already consume kernel-stack frame; the extra 2 KB is budgeted. */
    cs_load_stage_set(41);
    struct hailo_cs_context_buffers cs_bufs;
    rc = hailo_cs_translate_contexts(info, &tcfg, &cs_bufs);
    if (rc != HAILO_OK) {
        WARN("hailo backend: translate_contexts failed (rc=%d)", rc);
        goto fail;
    }
    cs_load_stage_set(42);

    /* Step 5: RPC sequence. Each must succeed; abort on any failure.
     *
     * Phase 8 fix: inserted CLEAR_CONFIGURED_APPS + GET_HW_CONSTS
     * between RESET and SET_NETWORK_GROUP_HEADER. `hailo ctxsmoke`
     * does this handshake and its SET_CONTEXT_INFO calls return
     * rc=0. load_model skipped it and every real-HEF load failed
     * at SET_CONTEXT_INFO(ACTIVATION) with major=0x40130016
     * (HAILO_DATAFLOW_STATUS_INVALID_RECEIVE_COMMUNICATION). The
     * memory note for the CORE CPU 0xFFFFFFFF wedge resolution
     * (2026-04-20) confirmed this: v4.23 firmware requires the
     * pre-configure handshake before accepting network-group-
     * level setup. */
    cs_load_stage_set(50);
    rc = hailo_control_change_context_switch_status(
            HAILO_CS_STATE_RESET,
            HAILO_CS_IGNORE_APPLICATION_INDEX,
            /*batch_size=*/0, /*batch_count=*/0);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CHANGE_CONTEXT_SWITCH_STATUS(RESET) failed (rc=%d)", rc);
        goto fail;
    }
    cs_load_stage_set(51);

    /* Pre-configure handshake (matches ctxsmoke flow + HailoRT
     * convention). CLEAR_CONFIGURED_APPS drops any apps the
     * firmware had registered from a prior load; GET_HW_CONSTS
     * reads hardware constants firmware needs to have handy
     * before it can validate subsequent context-switch bytes. */
    rc = hailo_control_context_switch_clear_configured_apps();
    if (rc != HAILO_OK) {
        WARN("hailo backend: CLEAR_CONFIGURED_APPS failed (rc=%d)", rc);
        goto fail;
    }
    cs_load_stage_set(52);

    uint32_t hw_consts_len = 0;
    rc = hailo_control_get_hw_consts(&hw_consts_len);
    if (rc != HAILO_OK) {
        WARN("hailo backend: GET_HW_CONSTS failed (rc=%d)", rc);
        goto fail;
    }
    cs_load_stage_set(53);

    rc = hailo_control_set_network_group_header(&hdr);
    if (rc != HAILO_OK) {
        WARN("hailo backend: SET_NETWORK_GROUP_HEADER failed (rc=%d)", rc);
        goto fail;
    }
    cs_load_stage_set(53);

    const struct {
        enum hailo_cs_context_type type;
        const uint8_t *bytes;
        uint32_t       len;
        const char    *name;
    } ctxs[] = {
        { HAILO_CS_CONTEXT_TYPE_ACTIVATION,      cs_bufs.activation,
          (uint32_t)cs_bufs.activation_len,      "ACTIVATION" },
        { HAILO_CS_CONTEXT_TYPE_BATCH_SWITCHING, cs_bufs.batch_switching,
          (uint32_t)cs_bufs.batch_switching_len, "BATCH_SWITCHING" },
        { HAILO_CS_CONTEXT_TYPE_PRELIMINARY,     cs_bufs.preliminary,
          (uint32_t)cs_bufs.preliminary_len,     "PRELIMINARY" },
        { HAILO_CS_CONTEXT_TYPE_DYNAMIC,         cs_bufs.dynamic,
          (uint32_t)cs_bufs.dynamic_len,         "DYNAMIC" },
    };
    for (uint32_t i = 0; i < sizeof(ctxs) / sizeof(ctxs[0]); i++) {
        cs_load_stage_set(60 + (int)i * 2);     /* 60, 62, 64, 66 per context */
        rc = hailo_control_set_context_info(ctxs[i].type,
                                            ctxs[i].bytes, ctxs[i].len);
        if (rc != HAILO_OK) {
            /* Encode (rpc_index, rc) so `hailo stage` shows both.
             * 800_000 + ctx_index*1000 + |rc| — e.g.,
             * 800004 = SET_CONTEXT_INFO(0) returned rc=-4 (TIMEOUT). */
            int abs_rc = (rc < 0) ? -rc : rc;
            cs_load_stage_set(800000 + (int)i * 1000 + abs_rc);
            WARN("hailo backend: SET_CONTEXT_INFO(%s) failed (rc=%d)",
                 ctxs[i].name, rc);
            goto fail;
        }
        cs_load_stage_set(61 + (int)i * 2);
    }

    cs_load_stage_set(70);                      /* about to CHANGE_STATUS(ENABLED) */
    rc = hailo_control_change_context_switch_status(
            HAILO_CS_STATE_ENABLED,
            /*application_index=*/0,
            /*batch_size=*/1, /*batch_count=*/1);
    if (rc != HAILO_OK) {
        WARN("hailo backend: CHANGE_CONTEXT_SWITCH_STATUS(ENABLED) failed (rc=%d)", rc);
        goto fail;
    }

    cs_load_stage_set(71);
    INFO("hailo backend: context-switch load OK (CCW=%u B, IN=%u B, OUT=%u B)",
         ccw_bytes,
         in_pad->has_stream_info  ? in_pad->core_bytes_per_buffer  : 0u,
         out_pad->has_stream_info ? out_pad->core_bytes_per_buffer : 0u);
    slot->cs_loaded = true;
    return HAILO_OK;

fail:
    context_switch_unwind(slot);
    return rc;
}

/* -------------------------------------------------------------------------- */
/* ops                                                                         */
/* -------------------------------------------------------------------------- */

static int hailo_backend_init(struct inference_device *dev)
{
    (void)dev;
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);
    return INF_OK;
}

static void hailo_backend_shutdown(struct inference_device *dev)
{
    (void)dev;
    /* Two-phase: copy out the DMA resource handles under the lock +
     * mark slots free, then release them outside the lock. Calling
     * hailo_*_free under spin_lock_irqsave is unsafe — the platform
     * dma_free may acquire another lock, trigger an IPI, or walk a
     * refcount that takes the PMM lock. Holding a kernel-wide IRQ-
     * disabled critical section across unbounded allocator work is
     * what to avoid; the out-of-lock path keeps the same ordering
     * guarantees (in_use=false is visible to other CPUs via the
     * unlock's release semantics before we free). */
    struct hailo_model_slot stash[HAILO_MAX_MODELS];
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memcpy(stash, slots, sizeof(stash));
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);

    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (stash[i].in_use && stash[i].cs_loaded) {
            context_switch_unwind(&stash[i]);
        }
    }
}

static int hailo_backend_load_model(struct inference_device *dev,
                                    const void *model, size_t size,
                                    inference_model_handle_t *out)
{
    (void)dev;

    if (!model || size == 0 || !out) return INF_ERR_INVAL;
    *out = INF_INVALID_HANDLE;

    /* Firmware must be booted before we can feed any model. */
    if (hailo_get_state() != HAILO_STATE_RUNNING) return INF_ERR_NODEV;

    /* 1. Outer header — validates magic + version + proto bounds. */
    struct hef_outer_header outer;
    int rc = hef_parse_outer_header(model, size, &outer);
    if (rc != HEF_OK) {
        uart_printf("[hailo] load_model: outer parse failed rc=%d\r\n", rc);
        return INF_ERR_BAD_MODEL;
    }
    uart_printf("[hailo] load_model: outer v=%u proto=%u@%u ccws=%lu@%lu (blob=%lu)\r\n",
                (unsigned)outer.version,
                (unsigned)outer.proto_size,
                (unsigned)outer.proto_offset,
                (unsigned long)outer.ccws_size,
                (unsigned long)outer.ccws_offset,
                (unsigned long)size);

    /* 2. Proto body — extract pad shapes for the first network group.
     * struct hef_info is ~1.2 KB; the 16 KB kernel stack has room. */
    struct hef_info info;
    const uint8_t *proto = (const uint8_t *)model + outer.proto_offset;
    rc = hef_parse_body(proto, outer.proto_size, &info);
    if (rc != HEF_OK) {
        uart_printf("[hailo] load_model: body parse failed rc=%d\r\n", rc);
        return INF_ERR_BAD_MODEL;
    }

    uart_printf("[hailo] load_model: parsed ngs=%u ops=%u pads=%u ccws=%u\r\n",
                (unsigned)info.network_group_count,
                (unsigned)info.op_count,
                (unsigned)info.pad_count,
                (unsigned)info.ccw_action_count);

    /* 3. Pick the largest pads in each direction. See pick_largest_pads
     * for the multi-head rationale. */
    const struct hef_pad_info *in_pad, *out_pad;
    uint32_t input_bytes, output_bytes;
    if (pick_largest_pads(&info, &in_pad, &out_pad, &input_bytes, &output_bytes) != 0) {
        uart_puts("[hailo] load_model: pick_largest_pads failed\r\n");
        /* Per-pad dump removed — was corrupting UART mid-print under
         * conditions not yet understood. Simpler puts() is enough to
         * confirm we reached the fallback. */

        /* Phase 8 diagnostic shortcut. If the parser found an input pad
         * but no output pad (the DFC 3.33.1 partial_network_groups +
         * fused_layers story isn't yet fully decoded), substitute a
         * plausible ImageNet-classifier output shape so the rest of
         * the load can proceed and hailo_backend_run can be exercised
         * for performance measurement. The resulting output bytes are
         * NOT semantically meaningful — this is a timing-only path. */
        in_pad = NULL;
        for (uint32_t i = 0; i < info.pad_count; i++) {
            if (info.pads[i].is_input && pad_bytes(&info.pads[i]) > 0) {
                in_pad = &info.pads[i];
                input_bytes = pad_bytes(&info.pads[i]);
                break;
            }
        }
        if (!in_pad) return INF_ERR_BAD_MODEL;

        /* Synthesize a 1000-byte output pad (ImageNet 1000-class INT8
         * softmax). Uses the input pad's sys_index + 100 as a distinct
         * stream id so the DMA descriptor wiring doesn't collide. */
        static struct hef_pad_info synthetic_out;
        memset(&synthetic_out, 0, sizeof(synthetic_out));
        synthetic_out.index                  = in_pad->index + 1;
        synthetic_out.is_input               = false;
        synthetic_out.has_tensor_shape       = true;
        synthetic_out.height                 = 1;
        synthetic_out.width                  = 1;
        synthetic_out.features               = 1000;
        synthetic_out.padded_height          = 1;
        synthetic_out.padded_width           = 1;
        synthetic_out.padded_features        = 1000;
        synthetic_out.has_stream_info        = true;
        synthetic_out.sys_index              = in_pad->sys_index + 100;
        synthetic_out.core_bytes_per_buffer  = 1000;
        synthetic_out.core_buffers_per_frame = 1;
        out_pad      = &synthetic_out;
        output_bytes = 1000;

        uart_printf("[hailo] load_model: SYNTHETIC output pad (1000 bytes)\r\n");
    } else
    uart_printf("[hailo] load_model: pads in=%u bytes out=%u bytes\r\n",
                (unsigned)input_bytes, (unsigned)output_bytes);

    /* 4. Claim a slot atomically — the check-then-set must be
     * lock-protected so concurrent loaders don't pick the same index.
     * Set in_use=true under the lock so later scanners skip us; the
     * cfg + shape fields fill in while other load/free callers still
     * see in_use=true (so they won't touch this slot). run() only
     * dereferences slot->cfg when in_use is true, and we publish
     * in_use=true BEFORE writing cfg, but that's safe because:
     *   - a concurrent run() with h pointing at this freshly-claimed
     *     slot would require the caller to already have the handle
     *     we haven't returned yet — can't happen;
     *   - an unrelated run() with a different h never reads this
     *     slot's cfg. */
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    int idx = -1;
    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (!slots[i].in_use) {
            slots[i].in_use = true;
            idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&slots_lock, flags);
    if (idx < 0) return INF_ERR_FULL;

    /* Stream parameters — prefer HEF-derived values (Phase 6.2b),
     * fall back to placeholders that work under the mock but time
     * out on real hardware. Channel indices are host-chosen; we use
     * 0 for input and 1 for output (standard convention).
     *
     * data_id mirrors the HEF's sys_index — firmware uses it to
     * route DMA to the correct on-chip buffer. Without the HEF
     * value the kernel sends 0, which the NPU rejects with a
     * stream-not-configured status.
     *
     * page_size is core_bytes_per_buffer for the input and the
     * periph side for the output; without HEF values we fall back
     * to 512 B which only happens to work for models whose real
     * buffer size is a multiple of it. */
    uint8_t  in_data_id   = in_pad->has_stream_info  ? (uint8_t) in_pad->sys_index
                                                     : 0;
    uint8_t  out_data_id  = out_pad->has_stream_info ? (uint8_t)out_pad->sys_index
                                                     : 0;
    uint16_t in_page_size = in_pad->has_stream_info && in_pad->core_bytes_per_buffer
                              ? (uint16_t)in_pad->core_bytes_per_buffer : 512;
    uint16_t out_page_size = out_pad->has_stream_info && out_pad->core_bytes_per_buffer
                               ? (uint16_t)out_pad->core_bytes_per_buffer : 512;

    /* in_use was set above; now populate the config + shapes. */
    slots[idx].cfg = (struct hailo_infer_config){
        .input_bytes      = input_bytes,
        .output_bytes     = output_bytes,
        .input_channel    = 0,
        .output_channel   = 1,
        .input_data_id    = in_data_id,
        .output_data_id   = out_data_id,
        .input_page_size  = in_page_size,
        .output_page_size = out_page_size,
        /* Deliberately tight: scheduler-policy path (ai_hailo) invokes
         * run() from contexts that may have IRQs disabled. A 500 ms
         * poll would stall the CPU and drop timer ticks. A real
         * Hailo-8 MLP inference completes in microseconds; 10 ms was
         * the original 500x safety margin but bumping to 500 ms for
         * real-HEF bring-up — first inference may include one-time
         * CCW upload latency and we don't yet know the real variance. */
        .timeout_us       = 500000,     /* 500 ms */
    };
    slots[idx].input_shape[0]  = (uint16_t)pad_dim(in_pad->padded_height,   in_pad->height);
    slots[idx].input_shape[1]  = (uint16_t)pad_dim(in_pad->padded_width,    in_pad->width);
    slots[idx].input_shape[2]  = (uint16_t)pad_dim(in_pad->padded_features, in_pad->features);
    slots[idx].output_shape[0] = (uint16_t)pad_dim(out_pad->padded_height,   out_pad->height);
    slots[idx].output_shape[1] = (uint16_t)pad_dim(out_pad->padded_width,    out_pad->width);
    slots[idx].output_shape[2] = (uint16_t)pad_dim(out_pad->padded_features, out_pad->features);

    /* 5. Context-switch load: replaces the prior best-effort
     * WRITE_MEMORY + CONFIG_STREAM path. Allocates VDMA desc lists
     * for CCW + boundary I/O, translates HEF → context-switch wire
     * bytes, and ships the 4-context sequence firmware needs. On
     * failure the slot is released so the caller can retry. */
    int csrc = context_switch_load(&slots[idx], &info, model, size, &outer,
                                   in_pad, out_pad);
    if (csrc != HAILO_OK) {
        irq_flags_t f = spin_lock_irqsave(&slots_lock);
        memset(&slots[idx], 0, sizeof(slots[idx]));
        spin_unlock_irqrestore(&slots_lock, f);
        return hailo_err_to_inf(csrc);
    }

    /* Handle numbering: 1..HAILO_MAX_MODELS. INF_BUILTIN_HANDLE (=0)
     * stays reserved for backends with compiled-in weights. */
    *out = (inference_model_handle_t)(idx + 1);
    return INF_OK;
}

static int hailo_backend_run(struct inference_device *dev,
                             inference_model_handle_t h,
                             const inference_tensor_t *in,
                             inference_tensor_t *out)
{
    (void)dev;

    if (!in || !out || !in->data || !out->data) return INF_ERR_INVAL;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return INF_ERR_INVAL;

    struct hailo_model_slot *slot = &slots[h - 1];
    if (!slot->in_use) return INF_ERR_INVAL;

    /* Hailo operates on INT8 tensors after DFC quantization. FP32
     * callers need a pre/post quantization layer — that's the
     * ai_policy_hailo wrapper's job, not this backend's. */
    if (in->dtype != INF_DTYPE_INT8 || out->dtype != INF_DTYPE_INT8) {
        return INF_ERR_BAD_TENSOR;
    }
    if (in->n_elems != slot->cfg.input_bytes
     || out->n_elems != slot->cfg.output_bytes) {
        return INF_ERR_BAD_TENSOR;
    }

    /* #338: submit via the slot's load-bound boundary tensors +
     * descriptor lists rather than allocating fresh ones. Firmware
     * was told about these specific IOVAs in ACTIVATION's
     * OpenBoundaryInput/Output actions; using different buffers at
     * submit time would either be rejected by firmware or cause the
     * NN engine to DMA from the zero-initialized load-time buffers.
     *
     * If context_switch_load didn't allocate boundary resources
     * (synthetic HEF without has_stream_info pads), fall back to
     * the legacy hailo_infer_run path — it allocates its own
     * buffers and runs self-contained. That path is used by
     * `hailo infer <size>` shell diagnostics that don't load a HEF. */
    if (!slot->cs_loaded || slot->boundary_in_tensor.cpu_addr == NULL
                         || slot->boundary_out_tensor.cpu_addr == NULL) {
        int rc = hailo_infer_run(&slot->cfg, in->data, out->data, NULL);
        return hailo_err_to_inf(rc);
    }

    /* Bounds already checked against slot->cfg.input_bytes/output_bytes
     * above; the boundary tensors were sized from the same pad values
     * (core_bytes_per_buffer) at load time so the memcpy target has
     * matching capacity. Defensive re-check since cfg and tensor
     * sizing come from different sources and mismatches would be
     * memory-safety-critical, not just functional bugs. */
    if (in->n_elems  > slot->boundary_in_tensor.tensor_bytes
     || out->n_elems > slot->boundary_out_tensor.tensor_bytes) {
        WARN("hailo backend: tensor size > boundary buffer (in=%u/%u, out=%u/%u)",
             in->n_elems,  slot->boundary_in_tensor.tensor_bytes,
             out->n_elems, slot->boundary_out_tensor.tensor_bytes);
        return INF_ERR_BAD_TENSOR;
    }

    /* The VDMA channel indices that receive these submits must be
     * the ones ACTIVATION opened. translate_activation packed them
     * as config_vdma + BOUNDARY_INPUT_CHANNEL_OFFSET (1) and
     * BOUNDARY_OUTPUT_CHANNEL_OFFSET (2). The low nibble of
     * packed_vdma_channel_id is the channel number — same value
     * both sides need. */
    uint8_t in_channel  = (uint8_t)(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                                  + HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET);
    uint8_t out_channel = (uint8_t)(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                                  + HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET);

    /* #253 CORE-CPU liveness probe: issue an empty-body CORE_IDENTIFY
     * right before the submit. If this returns rc=0 quickly, fw's
     * CORE-CPU RPC thread is alive and the submit blocker is inside
     * the inference-task / burst-credits state machine. If it times
     * out, the whole CORE CPU is wedged (e.g. inference task crashed
     * and starved the RPC thread). Strip once the blocker lifts. */
    {
        uint64_t t_probe_start = timer_get_count();
        uint32_t resp_len = 0;
        int probe_rc = hailo_control_core_identify(&resp_len);
        uint64_t t_probe_end = timer_get_count();
        uint64_t probe_us = (t_probe_end - t_probe_start) * 1000000ULL
                             / timer_get_frequency();
        uart_printf("[hailo] run: pre-submit CORE_IDENTIFY rc=%d "
                    "resp_len=%u latency=%lu us\r\n",
                    probe_rc, (unsigned)resp_len,
                    (unsigned long)probe_us);
    }

    /* Copy caller's input into the pre-allocated DMA buffer +
     * cache-clean so the device picks up the fresh bytes.
     * Corresponding invalidate+copy for output happens after submit. */
    memcpy(slot->boundary_in_tensor.cpu_addr, in->data, in->n_elems);
    hailo_tensor_prepare_for_device(&slot->boundary_in_tensor);

    /* Re-program the input/output descriptor lists against the fresh
     * tensor bytes for this inference. The desc list structure is
     * shared with firmware (bound via OpenBoundary at load time) but
     * the per-submit num_avail signal tells firmware how many pages
     * to consume from the current buffer contents.
     *
     * Do NOT call channel_start here — firmware owns the boundary
     * channels after context-switch ACTIVATION+ENABLED. An early
     * experiment re-issued channel_start each inference which wrote
     * ABORT_PAUSE (then START) into the CONTROL byte; this clobbered
     * firmware's channel state and num_proc never advanced on IN.
     * Reference behaviour (hailo-vdma-common.c:hailo_vdma_start_channel)
     * is a one-time setup during resource manager activation, not
     * per-transfer. */
    /* Program the FULL boundary-tensor size, not in->n_elems. Firmware
     * was told bytes_in_pattern = bpb*bpf (the HEF's periph-aligned
     * frame size) in OpenBoundary at load time. A partial program at
     * run time would describe a shorter frame than the one firmware
     * expects, and firmware silently refuses to advance num_proc on
     * the boundary channel. The caller's payload (in->n_elems) was
     * memcpy'd into the tensor above; positions beyond it stay at the
     * zero-fill from load_model, giving firmware a periph-padded frame
     * matching OpenBoundary's declaration. */
    int programmed = hailo_vdma_program_buffer(
        &slot->boundary_in_list, 0,
        slot->boundary_in_tensor.iova,
        slot->boundary_in_tensor.tensor_bytes,
        HAILO_VDMA_HOST_DMA_DATA_ID);
    if (programmed < 0) {
        return INF_ERR_NOSUPPORT;
    }
    uint16_t in_num_avail = (uint16_t)programmed;

    programmed = hailo_vdma_program_buffer(
        &slot->boundary_out_list, 0,
        slot->boundary_out_tensor.iova,
        slot->boundary_out_tensor.tensor_bytes,
        HAILO_VDMA_HOST_DMA_DATA_ID);
    if (programmed < 0) {
        return INF_ERR_NOSUPPORT;
    }
    uint16_t out_num_avail = (uint16_t)programmed;

    /* Phase 8 #253: dump programmed descriptors + channel regs so we
     * can compare byte-for-byte against HailoRT's reference output.
     * Runs once per hailo_backend_run call; `hailo runmodel <h> 1`
     * gives exactly one dump per inference. Strip when resolved. */
    hailo_vdma_dump_desc_list(&slot->boundary_in_list,  "IN",  4);
    hailo_vdma_dump_channel_regs(in_channel,  "IN pre-submit");
    hailo_vdma_dump_desc_list(&slot->boundary_out_list, "OUT", 4);
    hailo_vdma_dump_channel_regs(out_channel, "OUT pre-submit");

    int rc;
    uint64_t t_in_submit  = timer_get_count();
    rc = hailo_vdma_submit_and_wait(in_channel, in_num_avail,
                                    slot->cfg.timeout_us);
    uint64_t t_in_done = timer_get_count();
    if (rc != HAILO_OK) {
        uart_printf("[hailo] run: IN submit_and_wait rc=%d (avail=%u)\r\n",
                    rc, (unsigned)in_num_avail);
        goto run_out;
    }
    uart_printf("[hailo] run: IN ok in %lu us\r\n",
                (unsigned long)((t_in_done - t_in_submit) * 1000000ULL
                                / timer_get_frequency()));

    uint64_t t_out_submit = timer_get_count();
    rc = hailo_vdma_submit_and_wait(out_channel, out_num_avail,
                                    slot->cfg.timeout_us);
    uint64_t t_out_done = timer_get_count();
    if (rc != HAILO_OK) {
        uart_printf("[hailo] run: OUT submit_and_wait rc=%d (avail=%u)\r\n",
                    rc, (unsigned)out_num_avail);
        goto run_out;
    }
    uart_printf("[hailo] run: OUT ok in %lu us\r\n",
                (unsigned long)((t_out_done - t_out_submit) * 1000000ULL
                                / timer_get_frequency()));

    hailo_tensor_prepare_for_host(&slot->boundary_out_tensor);
    memcpy(out->data, slot->boundary_out_tensor.cpu_addr, out->n_elems);
    rc = HAILO_OK;

run_out:
    /* Do NOT stop the boundary channels on success — firmware expects
     * them live for subsequent inferences. On error, leaving them
     * running is also safe: the next submit will fail the same way
     * until the model is freed (which tears the channels down via
     * context_switch_unwind → hailo_backend_free_model). */
    return hailo_err_to_inf(rc);
}

static int hailo_backend_free_model(struct inference_device *dev,
                                    inference_model_handle_t h)
{
    (void)dev;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return INF_ERR_INVAL;
    struct hailo_model_slot *slot = &slots[h - 1];
    /* Two-phase: under the lock, copy out the DMA handles + clear
     * the slot; outside the lock, free the DMA resources. Holding
     * spin_lock_irqsave across dma_free would block other CPUs on
     * any allocator-internal contention and — depending on the
     * platform's free path — risk nesting locks held at interrupt
     * priority. The lock ordering is preserved: any concurrent
     * load_model scanning for a free slot sees in_use=false only
     * after the slot is fully zeroed. */
    struct hailo_model_slot stash;
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return INF_ERR_INVAL;
    }
    stash = *slot;
    memset(slot, 0, sizeof(*slot));
    spin_unlock_irqrestore(&slots_lock, flags);

    if (stash.cs_loaded) {
        context_switch_unwind(&stash);
    }
    return INF_OK;
}

/* -------------------------------------------------------------------------- */
/* Test introspection + test fixture helper                                    */
/*                                                                              */
/* Always present (not ENABLE_BOOT_TESTS-gated) so test_hailo.c — which is     */
/* linked into both test and non-test kernel builds on non-x86 — always       */
/* resolves. The cost is 16 bytes of code in production kernels and a         */
/* byte-level dispatcher that no non-test caller ever hits.                    */
/* -------------------------------------------------------------------------- */

uint32_t hailo_backend_in_use_slots(void)
{
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    uint32_t n = 0;
    for (int i = 0; i < HAILO_MAX_MODELS; i++)
        if (slots[i].in_use) n++;
    spin_unlock_irqrestore(&slots_lock, flags);
    return n;
}

/* Test-only: expose load-time boundary IOVAs so run-path tests can
 * assert that submit happens via the same IOVAs firmware was told
 * about in ACTIVATION. Out-of-range / unloaded handle returns 0 for
 * both. */
void hailo_backend_get_boundary_iovas_for_tests(
    inference_model_handle_t h, uint64_t *in_iova, uint64_t *out_iova)
{
    if (in_iova)  *in_iova  = 0;
    if (out_iova) *out_iova = 0;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return;
    struct hailo_model_slot *slot = &slots[h - 1];
    if (!slot->in_use || !slot->cs_loaded) return;
    if (in_iova)  *in_iova  = slot->boundary_in_list.iova;
    if (out_iova) *out_iova = slot->boundary_out_list.iova;
}

/* Public helper for callers (e.g. the Lua shell binding) that need
 * to size a tensor buffer against a loaded model without going
 * through inference_run. Returns HAILO_OK and fills both out-params
 * on success, HAILO_ERR_INVAL for an out-of-range / unloaded
 * handle. Safe to call with either pointer NULL (skips that output).
 *
 * Takes slots_lock so a concurrent hailo_backend_free_model can't
 * zero cfg.input_bytes / cfg.output_bytes between our in_use check
 * and the reads. Matches hailo_backend_in_use_slots' locking model
 * (writers mutate slot state under slots_lock, readers that care
 * about consistency must take it too). */
int hailo_backend_model_sizes(inference_model_handle_t h,
                              uint32_t *in_bytes,
                              uint32_t *out_bytes)
{
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return HAILO_ERR_INVAL;
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    struct hailo_model_slot *slot = &slots[h - 1];
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return HAILO_ERR_INVAL;
    }
    uint32_t in  = slot->cfg.input_bytes;
    uint32_t out = slot->cfg.output_bytes;
    spin_unlock_irqrestore(&slots_lock, flags);
    if (in_bytes)  *in_bytes  = in;
    if (out_bytes) *out_bytes = out;
    return HAILO_OK;
}

/* Public helper: report the maximum number of concurrent models the
 * backend can hold. Static at compile time today (HAILO_MAX_MODELS),
 * but exposed as a function so callers (slm.hailo.status) don't need
 * to duplicate the constant. */
uint32_t hailo_backend_slots_max(void)
{
    return (uint32_t)HAILO_MAX_MODELS;
}

void hailo_backend_reset_slots_for_tests(void)
{
    /* Same two-phase dance as hailo_backend_shutdown — see that
     * function for the rationale (dma_free outside spin_lock). */
    struct hailo_model_slot stash[HAILO_MAX_MODELS];
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memcpy(stash, slots, sizeof(stash));
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);

    for (int i = 0; i < HAILO_MAX_MODELS; i++) {
        if (stash[i].in_use && stash[i].cs_loaded) {
            context_switch_unwind(&stash[i]);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Registration                                                                */
/* -------------------------------------------------------------------------- */

static const struct inference_device_ops hailo_ops = {
    .name       = "hailo-8",
    .caps       = INF_CAP_INT8 | INF_CAP_LOAD_MODEL | INF_CAP_DMA,
    .init       = hailo_backend_init,
    .shutdown   = hailo_backend_shutdown,
    .load_model = hailo_backend_load_model,
    .run        = hailo_backend_run,
    .free_model = hailo_backend_free_model,
};

static struct inference_device hailo_device = {
    .ops = &hailo_ops,
};

/*
 * Called once from platform init, AFTER the Hailo platform shim has
 * installed ops (hailo_init succeeded). Registering before that leaves
 * the backend present but load_model returns INF_ERR_NODEV until the
 * firmware is booted — which is the intended behavior (scheduler can
 * look up the device by name at any time, but actually using it
 * requires the hardware to be live).
 */
int inference_device_hailo_register(void)
{
    return inference_device_register(&hailo_device);
}
