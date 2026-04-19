/*
 * inference_device_hailo.c — Hailo-8 NPU backend for inference_device.h.
 *
 * Plumbs the Phase 5.3/5.4 Hailo driver (hef_parser / hef_header /
 * hailo_control_upload_ccw / hailo_infer_run) through the common
 * inference_device vtable so scheduler and Lua bindings can route
 * inference to the NPU the same way they route to the CPU MLP.
 *
 * Scope (Phase 6.2a): plumbing-only. `load_model` parses the HEF
 * outer header and the first network group's pad shapes to derive
 * input/output byte sizes; `run` forwards to `hailo_infer_run` with
 * those sizes. VDMA channel indices, data_ids, and per-descriptor
 * page sizes are placeholders (0/1 channels, data_id=0, page_size=
 * 512) — the values a real HEF-driven inference needs come out of
 * the CONFIG_STREAM response, which Phase 6.2b will extract from
 * the HEF's preliminary_config and thread into the slot.
 *
 * Until 6.2b lands, running this backend on real hardware will
 * time out with HAILO_ERR_TIMEOUT because firmware has no stream
 * context. In QEMU the mock_vdma_auto_advance path completes
 * successfully, so the plumbing is exercised end-to-end under test.
 *
 * Model slots are a fixed-size table (HAILO_MAX_MODELS=4), handed
 * out as handles `idx + 1` so INF_BUILTIN_HANDLE (=0) stays reserved.
 * No dynamic allocation; all state is in BSS.
 */

#include "inference_device.h"
#include "hailo.h"
#include "hailo_control.h"
#include "hailo_infer.h"
#include "hef_header.h"
#include "hef_parser.h"
#include "spinlock.h"
#include "debug.h"
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
    /* No driver-level release — Hailo state is managed by the
     * device lifecycle (hailo_init / hailo_boot), not this backend. */
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);
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
    if (rc != HEF_OK) return INF_ERR_BAD_MODEL;

    /* 2. Proto body — extract pad shapes for the first network group.
     * struct hef_info is ~1.2 KB; the 16 KB kernel stack has room. */
    struct hef_info info;
    const uint8_t *proto = (const uint8_t *)model + outer.proto_offset;
    rc = hef_parse_body(proto, outer.proto_size, &info);
    if (rc != HEF_OK) return INF_ERR_BAD_MODEL;

    /* 3. Pick input and output tensor sizes. In the presence of
     * multiple input or output pads (rare for scheduler-class MLPs,
     * common for multi-head vision models), pick the LARGEST in each
     * direction so the allocated DMA buffer covers all of them. A
     * future 6.2b refinement can index per-pad slots so multi-head
     * inference routes each head to its own channel. */
    const struct hef_pad_info *in_pad = NULL;
    const struct hef_pad_info *out_pad = NULL;
    uint32_t input_bytes = 0, output_bytes = 0;
    for (uint32_t i = 0; i < info.pad_count; i++) {
        const struct hef_pad_info *p = &info.pads[i];
        uint32_t b = pad_bytes(p);
        if (!b) continue;
        if (p->is_input) {
            if (b > input_bytes) { input_bytes = b; in_pad = p; }
        } else {
            if (b > output_bytes) { output_bytes = b; out_pad = p; }
        }
    }
    if (!input_bytes || !output_bytes || !in_pad || !out_pad) {
        return INF_ERR_BAD_MODEL;
    }

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
         * Hailo-8 MLP inference completes in microseconds; 10 ms is
         * a ~500x safety margin that still caps pathological waits
         * at a recoverable duration. */
        .timeout_us       = 10000,      /* 10 ms */
    };
    slots[idx].input_shape[0]  = (uint16_t)pad_dim(in_pad->padded_height,   in_pad->height);
    slots[idx].input_shape[1]  = (uint16_t)pad_dim(in_pad->padded_width,    in_pad->width);
    slots[idx].input_shape[2]  = (uint16_t)pad_dim(in_pad->padded_features, in_pad->features);
    slots[idx].output_shape[0] = (uint16_t)pad_dim(out_pad->padded_height,   out_pad->height);
    slots[idx].output_shape[1] = (uint16_t)pad_dim(out_pad->padded_width,    out_pad->width);
    slots[idx].output_shape[2] = (uint16_t)pad_dim(out_pad->padded_features, out_pad->features);

    /* 5. Upload the CCW weight stream (best-effort, v0/v1 HEFs only).
     *
     * For v0/v1 HEFs whose CCWs are inline `write_data_ccw` actions,
     * `hailo_control_upload_ccw` writes payloads to sequential
     * firmware addresses — this is the original Phase 5.3 path and
     * still works against simple CCW streams.
     *
     * For v2+ HEFs (the DFC 3.33.1 scheduler_mlp_pi5.hef belongs
     * here), CCWs are `write_data_ccw_ptr` actions pointing into the
     * separate CCWS block. Real delivery on v2+ goes through the
     * CONTEXT_SWITCH_SET_CONTEXT_INFO opcode — firmware's context
     * switcher pulls CCW payloads from host-side DMA buffers as
     * inference context switching fires. That's a separate major
     * subsystem (not yet implemented); our WRITE_MEMORY-based
     * uploader gets rejected by firmware (status 0x40030098) because
     * address 0 is protected memory on the device side.
     *
     * Treat upload failure as a WARNING rather than a fatal error,
     * so the policy path stays exercisable (hailo_infer_run will
     * then time out cleanly when firmware never produces output).
     * This lets `bench sched-policy` run through the chain and
     * observe the timeout, which is the correct signal until the
     * context-switch protocol lands. */
    if (info.ccw_action_count > 0) {
        const uint8_t *ccws_base = (const uint8_t *)model + outer.ccws_offset;
        uint64_t uploaded = 0;
        int urc = hailo_control_upload_ccw(&info, proto, ccws_base,
                                           /*device_base=*/0, &uploaded);
        if (urc == HAILO_OK) {
            INFO("hailo backend: CCW upload OK — %lu bytes across %u actions",
                 (unsigned long)uploaded, info.ccw_action_count);
        } else {
            WARN("hailo backend: CCW upload failed (rc=%d after %lu bytes); "
                 "proceeding — v2+ HEFs require context-switch protocol which "
                 "is not yet implemented (inference will time out)",
                 urc, (unsigned long)uploaded);
            /* Not fatal — continue to CONFIG_STREAM so the slot is still
             * usable for the downstream handshake measurement. */
        }
    }

    /* 6. CONFIG_STREAM for input + output (best-effort, same rationale
     * as the CCW upload above — v2+ HEFs need CONTEXT_SWITCH_SET_CONTEXT_INFO
     * first so firmware knows about the streams. Without that,
     * CONFIG_STREAM returns 0x40030050 [STREAM__INVALID_CONFIG_STREAM_INDEX]).
     *
     * Synthetic test HEFs without has_stream_info on pads skip this
     * step entirely (no stream params to send). Real HEFs attempt
     * the handshake and log a WARN on failure; the slot stays live
     * so downstream inference_run calls can still be issued (they
     * will time out cleanly when firmware never produces output —
     * the correct signal until the context-switch protocol lands). */
    if (in_pad->has_stream_info && out_pad->has_stream_info) {
        uint8_t in_dmid = 0, out_dmid = 0;
        struct hailo_stream_pcie_config scfg_in = {
            .stream_index          = 0,
            .is_input              = true,
            .skip_nn_stream_config = false,
            .pcie_channel_index    = slots[idx].cfg.input_channel,
            .pcie_dataflow_type    = 2,     /* PCIE_CONTINUOUS */
        };
        scfg_in.nn_stream_config.core_bytes_per_buffer   = slots[idx].cfg.input_page_size;
        scfg_in.nn_stream_config.core_buffers_per_frame  =
            (uint16_t)(in_pad->core_buffers_per_frame ? in_pad->core_buffers_per_frame : 1);
        scfg_in.nn_stream_config.periph_bytes_per_buffer = slots[idx].cfg.input_page_size;
        scfg_in.nn_stream_config.periph_buffers_per_frame = 1;
        int src_in = hailo_control_config_stream_pcie(&scfg_in, &in_dmid);

        struct hailo_stream_pcie_config scfg_out = {
            .stream_index          = 0,
            .is_input              = false,
            .skip_nn_stream_config = false,
            .pcie_channel_index    = slots[idx].cfg.output_channel,
            .desc_page_size        = slots[idx].cfg.output_page_size,
        };
        scfg_out.nn_stream_config.core_bytes_per_buffer   = slots[idx].cfg.output_page_size;
        scfg_out.nn_stream_config.core_buffers_per_frame  =
            (uint16_t)(out_pad->core_buffers_per_frame ? out_pad->core_buffers_per_frame : 1);
        scfg_out.nn_stream_config.periph_bytes_per_buffer = slots[idx].cfg.output_page_size;
        scfg_out.nn_stream_config.periph_buffers_per_frame = 1;
        int src_out = hailo_control_config_stream_pcie(&scfg_out, &out_dmid);

        if (src_in == HAILO_OK && src_out == HAILO_OK) {
            INFO("hailo backend: streams configured (in dmid=%u, out dmid=%u)",
                 (unsigned)in_dmid, (unsigned)out_dmid);
        } else {
            WARN("hailo backend: CONFIG_STREAM best-effort failed "
                 "(in rc=%d, out rc=%d); v2+ HEFs need CONTEXT_SWITCH "
                 "protocol — not yet implemented. Slot stays live; "
                 "inference will time out.", src_in, src_out);
        }
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

    int rc = hailo_infer_run(&slot->cfg, in->data, out->data, NULL);
    return hailo_err_to_inf(rc);
}

static int hailo_backend_free_model(struct inference_device *dev,
                                    inference_model_handle_t h)
{
    (void)dev;
    if (h <= 0 || (uint32_t)h > HAILO_MAX_MODELS) return INF_ERR_INVAL;
    struct hailo_model_slot *slot = &slots[h - 1];
    /* Lock-protected clear so a concurrent load_model scanning for a
     * free slot doesn't observe in_use=false with stale cfg bytes. */
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    if (!slot->in_use) {
        spin_unlock_irqrestore(&slots_lock, flags);
        return INF_ERR_INVAL;
    }
    memset(slot, 0, sizeof(*slot));
    spin_unlock_irqrestore(&slots_lock, flags);
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

void hailo_backend_reset_slots_for_tests(void)
{
    irq_flags_t flags = spin_lock_irqsave(&slots_lock);
    memset(slots, 0, sizeof(slots));
    spin_unlock_irqrestore(&slots_lock, flags);
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
