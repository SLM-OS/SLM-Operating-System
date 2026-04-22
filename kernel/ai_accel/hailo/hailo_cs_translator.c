/*
 * hailo_cs_translator.c — see hailo_cs_translator.h for the spec.
 *
 * Design: thin composition over hailo_cs_builder. Each per-context
 * function accepts a builder around its caller-provided buffer and
 * appends the actions firmware expects. The outer hailo_cs_translate
 * orchestrates the four sub-translators + records the resulting
 * byte lengths.
 *
 * What this module does NOT do (yet):
 *  - Per-HEF-action translation for TriggerSequencer, DisableLcu,
 *    WaitForSequencer, AllowInputDataflow in the DYNAMIC context.
 *    EnableLcu is translated today (6.4g); the others follow the
 *    same pattern once their per-action extraction lands in
 *    hef_parser.
 *  - Boundary-channel ACTIVATION actions (OpenBoundaryInput/Output).
 *    For single-CCW-channel smoke loads the BURST_CREDITS_TASK_RESET
 *    alone is accepted; real I/O streams land alongside parser work
 *    on edge_layer → boundary_channel mapping.
 *  - Multi-dynamic-context dispatch. translate_dynamic filters
 *    captured EnableLcu entries to context_index == 0 and will
 *    need per-context loops when dynamic_contexts_count > 1.
 */

#include <string.h>

#include "debug.h"
#include "hailo_cs_builder.h"
#include "hailo_cs_translator.h"
#include "hef.pb.h"   /* ProtoHEFAction_*_tag constants */

/* "periph frame size" = periph_bytes_per_buffer * periph_buffers_per_frame.
 * This is the value firmware expects in:
 *   - OpenBoundary{Input,Output}.host_buffer_info.bytes_in_pattern
 *   - OpenBoundaryInput.frame_periph_size / periph_bytes_per_buffer
 *   - AllowInputDataflow.frame_periph_size
 *   - Activate_Boundary.host_buffer_info.bytes_in_pattern
 *
 * HailoRT v4.23 synthesizes nn_stream_config such that
 *   INPUT  periph_bytes_per_buffer  = h * w * features, periph_buffers=1
 *   OUTPUT periph_bytes_per_buffer  = core_bytes_per_buffer,   periph_buffers=1
 * so periph_frame boils down to tensor size for input and core size
 * for output. Verified byte-for-byte against HailoRT's MNIST wire
 * capture (docs/reference/pios_{ACTIVATION,DYNAMIC}.bin). */
/* Resolve the OUTPUT boundary desc page size with fallback to the
 * INPUT default. Kept inline so both translate_open_boundary_for_pad
 * and emit_dynamic_boundary_prologue agree on the value they emit. */
static inline uint16_t cfg_output_page_size(
    const struct hailo_cs_translate_cfg *cfg)
{
    return cfg->boundary_output_desc_page_size
             ? cfg->boundary_output_desc_page_size
             : cfg->boundary_desc_page_size;
}

static uint32_t pad_periph_frame_size(const struct hef_pad_info *p)
{
    uint32_t bpb = p->core_bytes_per_buffer;
    uint32_t bpf = p->core_buffers_per_frame ? p->core_buffers_per_frame : 1u;
    if (p->is_input) {
        /* Input: periph view is the full tensor as one buffer. */
        if (p->has_tensor_shape) {
            return (uint32_t)p->height * p->width * p->features;
        }
        /* Fall back to core size if shape missing — keeps submit flow
         * alive with a plausible value, same as pre-2026-04-22 behavior. */
        return bpb * bpf;
    }
    /* Output: periph bytes matches the core buffer size (padding is
     * encoded separately via buffer_padding{,_payload}). Multiply by
     * periph_buffers_per_frame (=1) to keep the shape symmetric with
     * the input case. */
    return bpb;
}

/* Compute `config_vdma_channel + offset` and narrow to u8 for the
 * wire. Returns HAILO_OK + writes `*out` on success, HAILO_ERR_INVAL
 * and logs a WARN if the sum exceeds 0xFF (would silently truncate).
 * `tag` is a short label included in the WARN so the emitter call
 * site is identifiable in logs ("ActivationInput", "BatchSwitching",
 * etc). */
static int pack_boundary_vdma(const struct hailo_cs_translate_cfg *cfg,
                              uint32_t offset,
                              const char *tag,
                              uint8_t *out)
{
    uint32_t raw = (uint32_t)cfg->config_vdma_channel + offset;
    if (raw > 0xFFu) {
        WARN("hailo translator: %s packed_vdma overflows u8 "
             "(config_vdma=%u, offset=%u)",
             tag, cfg->config_vdma_channel, offset);
        return HAILO_ERR_INVAL;
    }
    *out = (uint8_t)raw;
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* Application header                                                           */
/* -------------------------------------------------------------------------- */

int hailo_cs_translate_application_header(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_application_header *out)
{
    if (!info || !cfg || !out) return HAILO_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    /* For a single network group load: 1 network, 1 dynamic context.
     * dynamic_contexts_count grows once multi-context HEFs land. */
    out->networks_count         = 1;
    out->dynamic_contexts_count = 1;
    out->batch_size             = 1;

    /* csm_buffer_size must match the VDMA descriptor page size —
     * firmware validates this against the host_buffer_info fields
     * in ACTIVATE_CFG_CHANNEL. Use the caller-provided ccw_desc_page_size
     * so the two sources agree by construction. */
    out->csm_buffer_size = cfg->ccw_desc_page_size;

    /* Control-channel action list path only (no DDR backing). Zero
     * would be read as a valid DDR pointer and rejected. */
    out->external_action_list_address = HAILO_CS_NO_DDR_ACTION_LIST;

    /* Declare the config channel to firmware so it can bind the
     * subsequent ACTIVATE_CFG_CHANNEL action. For v4.23 the array
     * is capped at HAILO_CS_MAX_CFG_CHANNELS=4; single-channel loads
     * occupy slot 0. */
    out->config_channels_count       = 1;
    out->config_channel_packed_id[0] = cfg->config_vdma_channel;

    /* boundary_channels_bitmap: bit per VDMA channel used for
     * boundary dataflow (input and output streams bound via
     * OpenBoundary actions in ACTIVATION). Config channel is NOT
     * included — that's tracked separately via
     * config_channel_packed_id[]. If we have a boundary input pad,
     * bit (config_vdma + INPUT_OFFSET) is set; boundary output pad
     * → bit (config_vdma + OUTPUT_OFFSET).
     *
     * Earlier draft set the config_vdma bit here; that is wrong on
     * two counts: (a) config channel doesn't belong in the boundary
     * bitmap by convention, (b) the actual boundary channels
     * (config+1, config+2) weren't represented at all. Firmware
     * treats the bitmap as authoritative for which channels it
     * should walk during BURST_CREDITS_TASK_START — a bitmap that
     * doesn't include the real boundary channels causes the task
     * to read uninitialized channel state. */
    bool has_input_boundary  = false;
    bool has_output_boundary = false;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *pad = &info->pads[i];
        if (!pad->has_stream_info) continue;
        if (pad->is_input)  has_input_boundary  = true;
        else                has_output_boundary = true;
    }
    uint32_t bitmap = 0;
    if (has_input_boundary) {
        bitmap |= 1u << ((cfg->config_vdma_channel
                        + HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET) & 0x1Fu);
    }
    if (has_output_boundary) {
        bitmap |= 1u << ((cfg->config_vdma_channel
                        + HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET) & 0x1Fu);
    }
    out->boundary_channels_bitmap[0] = bitmap;

    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* ACTIVATION context                                                           */
/* -------------------------------------------------------------------------- */

/* ACTIVATION context (Phase 6.5, #178).
 *
 * Per HailoRT's fill_activation_config_recepies (v4.23 source trail
 * documented in docs/pi5-ai-hat-plan.md §6.3):
 *
 *   1. BURST_CREDITS_TASK_RESET (zero body) — resets per-channel
 *      burst credit counters so the new network's config channel
 *      starts clean.
 *   2. OPEN_BOUNDARY_INPUT_CHANNEL per boundary input edge — binds
 *      a host→device VDMA descriptor list to the channel firmware
 *      will pull tensor data through during inference.
 *   3. OPEN_BOUNDARY_OUTPUT_CHANNEL per boundary output edge — binds
 *      device→host channel for the response tensor.
 *
 * Boundary edges are signaled in hef_info by pads[].has_stream_info
 * (the pad was joined to an edge_layer during parse; edge_layers are
 * the boundary streams exposed to the host). is_input selects the
 * direction. A single-input / single-output MLP emits exactly one
 * INPUT_CHANNEL + one OUTPUT_CHANNEL on top of the BURST_CREDITS
 * reset — three actions total, 28+20 = 48 bytes of body plus three
 * 5-byte common headers = 63 bytes of ACTIVATION. Well under the
 * HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES cap.
 *
 * Multi-stream HEFs (multiple inputs or outputs) require threading
 * stream_index and a wider boundary-desc-list table through cfg;
 * single-stream is the MVP.
 *
 * firmware expectation (hypothesis, #180): the 0xFFFFFFFF response
 * pattern on subsequent CORE-CPU RPCs after ACTIVATION suggests fw
 * v4.23 waits for the full OpenBoundary set before accepting the
 * next context. Landing this translator function is the concrete
 * test of that hypothesis. */
static int translate_open_boundary_for_pad(
    const struct hef_pad_info *pad,
    const struct hailo_cs_translate_cfg *cfg,
    uint8_t stream_index,
    struct hailo_cs_builder *b)
{
    if (pad->is_input) {
        uint8_t packed_vdma;
        int rc = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET,
                                    "ActivationInput", &packed_vdma);
        if (rc != HAILO_OK) return rc;
        if (cfg->boundary_input_desc_list_iova == 0) {
            WARN("hailo translator: HEF has boundary input edge (sys_index=%u) "
                 "but cfg->boundary_input_desc_list_iova is 0",
                 pad->sys_index);
            return HAILO_ERR_INVAL;
        }
        /* Periph transfer size for INPUT = h*w*features (one whole-frame
         * periph buffer); see pad_periph_frame_size() comment for why
         * the proto's core_bytes_per_buffer value isn't the right
         * quantity here — HailoRT synthesizes the periph split from
         * direction + shape + DFC flags, not directly from the proto.
         * Byte-verified against pios_ACTIVATION.bin / pios_DYNAMIC.bin
         * for the MNIST HEF on pi-5-1 (#253). */
        uint32_t periph_frame = pad_periph_frame_size(pad);
        struct hailo_cs_act_open_boundary_input_channel body = {
            .packed_vdma_channel_id = packed_vdma,
            .host_buffer_info = {
                .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
                .dma_address      = cfg->boundary_input_desc_list_iova,
                .desc_page_size   = cfg->boundary_desc_page_size,
                .total_desc_count = cfg->boundary_input_total_desc_count,
                .bytes_in_pattern = periph_frame,
            },
            .stream_index             = stream_index,
            .network_index            = 0,
            .periph_bytes_per_buffer  =
                (uint16_t)((periph_frame > 0xFFFFu) ? 0xFFFFu : periph_frame),
            .frame_periph_size        = periph_frame,
        };
        return hailo_cs_builder_append(
            b, HAILO_CS_ACT_OPEN_BOUNDARY_INPUT_CHANNEL,
            &body, sizeof(body));
    } else {
        uint8_t packed_vdma;
        int rc = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET,
                                    "ActivationOutput", &packed_vdma);
        if (rc != HAILO_OK) return rc;
        if (cfg->boundary_output_desc_list_iova == 0) {
            WARN("hailo translator: HEF has boundary output edge "
                 "(sys_index=%u) but cfg->boundary_output_desc_list_iova "
                 "is 0", pad->sys_index);
            return HAILO_ERR_INVAL;
        }
        uint32_t periph_frame = pad_periph_frame_size(pad);
        struct hailo_cs_act_open_boundary_output_channel body = {
            .packed_vdma_channel_id = packed_vdma,
            .host_buffer_info = {
                .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
                .dma_address      = cfg->boundary_output_desc_list_iova,
                .desc_page_size   = cfg_output_page_size(cfg),
                .total_desc_count = cfg->boundary_output_total_desc_count,
                /* Output: periph frame == core_bytes_per_buffer per
                 * pad_periph_frame_size. bytes_in_pattern = 16 for
                 * MNIST's 1x1x10 (padded) output on the wire. */
                .bytes_in_pattern = periph_frame,
            },
        };
        (void)stream_index;   /* OUTPUT body omits stream_index today. */
        return hailo_cs_builder_append(
            b, HAILO_CS_ACT_OPEN_BOUNDARY_OUTPUT_CHANNEL,
            &body, sizeof(body));
    }
}

/* Walk pads once per direction, emitting OpenBoundary actions for
 * pads matching `emit_inputs`. stream_index counts boundary pads
 * per direction (0 = first input boundary, etc.). Helper for
 * translate_activation, which calls this twice (outputs then inputs)
 * to match HailoRT's emission order. */
static int emit_open_boundary_pass(const struct hef_info *info,
                                   const struct hailo_cs_translate_cfg *cfg,
                                   bool emit_inputs,
                                   struct hailo_cs_builder *b)
{
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *pad = &info->pads[i];
        if (!pad->has_stream_info) continue;
        if (pad->is_input != emit_inputs) continue;
        /* OpenBoundary's stream_index must be the pad's HEF
         * sys_index, not a 0-based counter. HailoRT wire capture
         * (pios_ACTIVATION.bin) emits stream_index=1 for MNIST's
         * input pad (sys_index=1); FETCH_DATA_FROM_VDMA in DYNAMIC
         * uses the same value so fw correlates the two by
         * matching stream_index. A mismatch caused fw to activate
         * the boundary under stream slot 0 while DYNAMIC fetched
         * from slot 1, leaving ch=2 num_proc stuck at 0 despite
         * num_avail being latched correctly. */
        int rc = translate_open_boundary_for_pad(pad, cfg,
                                                 (uint8_t)pad->sys_index, b);
        if (rc != HAILO_OK) return rc;
    }
    return HAILO_OK;
}

static int translate_activation(const struct hef_info *info,
                                const struct hailo_cs_translate_cfg *cfg,
                                struct hailo_cs_builder *b)
{
    /* Step 1: BURST_CREDITS_TASK_RESET. */
    int rc = hailo_cs_builder_append(b,
                 HAILO_CS_ACT_BURST_CREDITS_TASK_RESET, NULL, 0);
    if (rc != HAILO_OK) return rc;

    /* Steps 2 & 3: emit OpenBoundary per boundary edge, OUTPUT before
     * INPUT to match HailoRT v4.23 byte-for-byte
     * (resource_manager_builder.cpp:1059-1075 iterates
     * get_output_layer_infos before get_input_layer_infos). The #180
     * bisect did not prove firmware *requires* this order, but
     * matching HailoRT removes one variable from any future
     * regression triage. */
    rc = emit_open_boundary_pass(info, cfg, /*emit_inputs=*/false, b);
    if (rc != HAILO_OK) return rc;
    return emit_open_boundary_pass(info, cfg, /*emit_inputs=*/true, b);
}

/* -------------------------------------------------------------------------- */
/* BATCH_SWITCHING context                                                      */
/* -------------------------------------------------------------------------- */

/* BATCH_SWITCHING context. Per HailoRT's
 * fill_batch_switching_context_config_recepies_for_multi_context
 * (resource_manager_builder.cpp L1306-1333, docs/reference/
 * hailort-context-switch-orchestration.md), the minimal sequence
 * for a boundary-I/O network is:
 *
 *   DDR_BUFFERING_RESET
 *   CHANGE_BOUNDARY_INPUT_BATCH × N_H2D_boundary
 *   BURST_CREDITS_TASK_START
 *
 * The middle action is load-bearing: BURST_CREDITS_TASK_START spins
 * up a firmware task that walks boundary H2D channels and expects
 * each one's batch state to have been set by CHANGE_BOUNDARY_INPUT_
 * BATCH. Skipping the batch programming is what crashed pi-5-1 fw
 * v4.23 in #180 — firmware read uninitialised channel state, the
 * control CPU crashed, and the PCIe link dropped entirely.
 *
 * HEFs with zero boundary H2D channels (synthetic tests) fall back
 * to just DDR_BUFFERING_RESET + BURST_CREDITS_TASK_START, matching
 * the pre-#180 behavior. */
/* HailoRT v4.23 wire capture of MNIST BATCH_SWITCHING
 * (pios_BATCH_SWITCHING.bin) opens with a REPEATED_ACTION carrying
 * 15 SWITCH_LCU_BATCH sub-actions, one per LCU in use across the
 * network's 3 clusters. Without this prologue fw sees uninitialised
 * LCU batch state on the subsequent BURST_CREDITS_TASK_START, the
 * inference scheduler never grants credits to the boundary-IN
 * fetch, and ch=2 num_proc stays 0 forever (Phase 8 submit blocker).
 *
 * LCU IDs are HEF-specific — they come from the HEF's compiled-in
 * partial_clusters / nn_stream_config, which our parser does not
 * currently extract. For now this emits a fixed template keyed to
 * the MNIST HEF we test against, identified by ccw_action_count=28
 * + ccws_total_bytes=112256 + input shape 28x28x1. Any other HEF
 * falls through to the pre-template path (DDR_BUFFERING_RESET +
 * CHANGE_BOUNDARY_INPUT_BATCH + BURST_CREDITS_TASK_START only).
 *
 * Generalization is tracked under #253; the proper fix is to parse
 * nn_stream_config's LCU list at HEF-load time. */
static const struct hailo_cs_act_switch_lcu_batch
mnist_switch_lcu_batch_template[] = {
    /* Values from docs/reference/pios_BATCH_SWITCHING.bin decoded
     * 2026-04-22 — kernel_done_count=2 for every LCU. */
    { .packed_lcu_id = 0x00, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x10, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x0e, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x13, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x08, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x05, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x03, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x04, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x0a, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x06, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x07, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x09, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x0b, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x0f, .network_index = 0, .kernel_done_count = 2 },
    { .packed_lcu_id = 0x01, .network_index = 0, .kernel_done_count = 2 },
};

_Static_assert(sizeof(mnist_switch_lcu_batch_template) /
               sizeof(mnist_switch_lcu_batch_template[0]) == 15,
               "MNIST switch_lcu_batch template must be 15 entries");

/* MNIST template signature: 28×28×1 input + 1×1×10 output + 28 CCW
 * actions + DFC sdk_version string prefix "3.33" + ccw_total_bytes
 * matching the reference build (112256 B). The MNIST sequencer_config
 * and LCU sweep byte tables below are compiled specifically from
 * this HEF; applying them to a lookalike HEF from a different DFC
 * revision would silently corrupt fw state. The signature is
 * intentionally tight — new MNIST HEFs (e.g. re-quantised, alternate
 * batch size) should fall through to the generic path until the
 * translator learns to synthesize sequencer_config from the HEF's
 * own compiled actions. */
#define HAILO_MNIST_TEMPLATE_CCW_ACTION_COUNT  28u
#define HAILO_MNIST_TEMPLATE_CCW_TOTAL_BYTES   112256u
#define HAILO_MNIST_TEMPLATE_SDK_VERSION       "3.33"

static bool hef_matches_mnist_template(const struct hef_info *info)
{
    if (info->ccw_action_count != HAILO_MNIST_TEMPLATE_CCW_ACTION_COUNT)
        return false;
    if (info->ccw_total_bytes != HAILO_MNIST_TEMPLATE_CCW_TOTAL_BYTES)
        return false;
    /* sdk_version is a NUL-terminated C string; check its prefix so
     * "3.33.1" et al all match. A HEF compiled on a newer DFC will
     * likely carry different sequencer_config bytes and must fall
     * through. */
    const char *want = HAILO_MNIST_TEMPLATE_SDK_VERSION;
    for (size_t i = 0; want[i] != '\0'; i++) {
        if (info->sdk_version[i] != want[i]) return false;
    }
    /* Input pad shape 28x28x1 + output pad shape 1x1x10 — the
     * structural fingerprint of the MNIST HEF. */
    bool saw_in = false, saw_out = false;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *p = &info->pads[i];
        if (p->is_input && p->has_tensor_shape &&
            p->height == 28 && p->width == 28 && p->features == 1) {
            saw_in = true;
        } else if (!p->is_input && p->has_tensor_shape &&
                   p->height == 1 && p->width == 1 && p->features == 10) {
            saw_out = true;
        }
    }
    return saw_in && saw_out;
}

static int translate_batch_switching(const struct hef_info *info,
                                     const struct hailo_cs_translate_cfg *cfg,
                                     struct hailo_cs_builder *b)
{
    /* #253 Phase 8: emit HailoRT's 15-entry SWITCH_LCU_BATCH
     * prologue when we recognise the MNIST HEF shape. Generic
     * HEFs skip this until we parse nn_stream_config's LCU list. */
    if (hef_matches_mnist_template(info)) {
        int rc = hailo_cs_builder_append_repeated(
            b, HAILO_CS_ACT_SWITCH_LCU_BATCH,
            (uint8_t)(sizeof(mnist_switch_lcu_batch_template) /
                      sizeof(mnist_switch_lcu_batch_template[0])),
            mnist_switch_lcu_batch_template,
            sizeof(mnist_switch_lcu_batch_template[0]));
        if (rc != HAILO_OK) return rc;
    }

    int rc = hailo_cs_builder_append(b, HAILO_CS_ACT_DDR_BUFFERING_RESET,
                                     NULL, 0);
    if (rc != HAILO_OK) return rc;

    /* Emit CHANGE_BOUNDARY_INPUT_BATCH per boundary H2D pad, matching
     * the VDMA channel IDs ACTIVATION used in OpenBoundaryInput. */
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *pad = &info->pads[i];
        if (!pad->has_stream_info || !pad->is_input) continue;

        uint8_t packed_vdma;
        rc = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET,
                                "BatchSwitching", &packed_vdma);
        if (rc != HAILO_OK) return rc;
        struct hailo_cs_act_change_boundary_input_batch body = {
            .packed_vdma_channel_id = packed_vdma,
        };
        rc = hailo_cs_builder_append(b,
                 HAILO_CS_ACT_CHANGE_BOUNDARY_INPUT_BATCH,
                 &body, sizeof(body));
        if (rc != HAILO_OK) return rc;
    }

    return hailo_cs_builder_append(b, HAILO_CS_ACT_BURST_CREDITS_TASK_START,
                                   NULL, 0);
}

/* -------------------------------------------------------------------------- */
/* PRELIMINARY context (CCW upload)                                             */
/* -------------------------------------------------------------------------- */

/* ACTIVATE_CFG_CHANNEL binds a config stream to the VDMA channel
 * firmware will DMA-pull CCW payloads through. Followed by a
 * REPEATED_ACTION wrapping one or more AddCcwBurst sub-actions
 * that tell firmware how many bursts to pull from that channel.
 *
 * Direct FETCH_CCW_BURSTS (without the REPEATED_ACTION wrapper)
 * gets rejected in PRELIMINARY on Hailo-8L with 0x402a0001 =
 * CONFIG_MANAGER_WRAPPER_STATUS_ACTION_TYPE_NOT_SUPPORTED —
 * confirmed via HailoRT v4.23 wire capture (docs/reference/
 * hailort-v4.23.0-wire-capture-mobilenet.txt). The wrapped form
 * is accepted.
 *
 * Phase 6.10 step 2: this function now emits the wrapper with a
 * single sub-action carrying info->ccw_action_count as the burst
 * count. Multi-sub-action layouts (needed for multi-config-channel
 * HEFs, which aren't in our current test set) land when the HEF
 * parser learns to extract per-context add-ccw-burst sequences.
 */
/* HailoRT v4.23 PRELIMINARY NN-core arming template for MNIST HEF.
 * Reference: docs/reference/pios_PRELIMINARY.bin decoded 2026-04-22.
 * Emits the LCU-sweep + sequencer-trigger + LCU-enable sequence that
 * arms the NN core's compute pipeline. Without this, fw's inference
 * scheduler never grants credits to the boundary-IN fetch, and the
 * VDMA engine's num_proc stays 0 forever (Phase 8 submit blocker).
 *
 * Phases implemented here:
 *   Phase 3a: DISABLE_LCU + 14x REPEATED_DISABLE_LCU  (clean slate)
 *   Phase 3b: MODULE_CONFIG_DONE + TRIGGER_SEQUENCER x 2 clusters
 *   Phase 3c: SEQUENCER_DONE x 2 + MODULE_CONFIG_DONE x 3
 *   Phase 4a: 4 groups of REPEATED_ENABLE_LCU + MODULE_CONFIG_DONE
 *   Phase 4b: MODULE_CONFIG_DONE tail x 2 + DEACTIVATE_CFG_CHANNEL
 *
 * Phase 2 from the reference (boundary channel re-activate + FETCH_DATA
 * + BURST_CREDITS_TASK_START) is INTENTIONALLY SKIPPED because we
 * already emit ACTIVATE_BOUNDARY_* in ACTIVATION and the DYNAMIC
 * prologue. Re-issuing them in PRELIMINARY would re-program fw state
 * that's still live.
 *
 * LCU IDs and sequencer_config byte patterns are HEF-specific; gated
 * at the call site behind hef_matches_mnist_template(). */
static const uint8_t MNIST_DISABLE_LCU_SWEEP[14] = {
    0x0f, 0x0b, 0x09, 0x07, 0x06, 0x0a, 0x04, 0x03,
    0x05, 0x08, 0x13, 0x0e, 0x10, 0x00,
};

/* sequencer_config bytes for clusters 0 and 1 (43 B each) verbatim
 * from HailoRT wire capture. HEF-compiled constants — same on every
 * load of the MNIST HEF, IOVA-independent. */
static const uint8_t MNIST_SEQ_CFG_CLUSTER_0[43] = {
    0x00, 0xc0, 0x7f, 0xbf, 0x3b, 0x01, 0x00, 0x79,
    0xe5, 0x13, 0x80, 0x70, 0xe0, 0x06, 0x0a, 0x53,
    0x40, 0x00, 0x20, 0x30, 0x40, 0x06, 0x02, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x20, 0x3c,
    0x00, 0x0c, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};
static const uint8_t MNIST_SEQ_CFG_CLUSTER_1[43] = {
    0x00, 0xc0, 0x7f, 0x21, 0x00, 0x00, 0x00, 0x38,
    0x00, 0x00, 0x00, 0x30, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

_Static_assert(sizeof(MNIST_SEQ_CFG_CLUSTER_0) ==
               sizeof(struct hailo_cs_sequencer_config),
               "MNIST cluster_0 sequencer_config must be 43 bytes");
_Static_assert(sizeof(MNIST_SEQ_CFG_CLUSTER_1) ==
               sizeof(struct hailo_cs_sequencer_config),
               "MNIST cluster_1 sequencer_config must be 43 bytes");

/* LCU groups enabled in PRELIMINARY phase 4. Each group's LCUs are
 * followed by two MODULE_CONFIG_DONE_INTERRUPTs — see ref sequence.
 * The groups are fed to emit_enable_lcu_group(), which stages the
 * sub-bodies on a stack-local array sized for MNIST_ENABLE_LCU_GROUP_MAX
 * entries. Enlarging any group without bumping the cap would return
 * HAILO_ERR_INVAL silently on the MNIST gate — the _Static_asserts
 * below fail the build instead. */
#define MNIST_ENABLE_LCU_GROUP_MAX 4u
static const uint8_t MNIST_ENABLE_LCU_GRP0[4] = { 0x00, 0x10, 0x0e, 0x13 };
static const uint8_t MNIST_ENABLE_LCU_GRP1[4] = { 0x08, 0x05, 0x03, 0x04 };
static const uint8_t MNIST_ENABLE_LCU_GRP2[4] = { 0x0a, 0x06, 0x07, 0x09 };
static const uint8_t MNIST_ENABLE_LCU_GRP3[3] = { 0x0b, 0x0f, 0x01 };

_Static_assert(sizeof(MNIST_ENABLE_LCU_GRP0) <= MNIST_ENABLE_LCU_GROUP_MAX,
               "MNIST_ENABLE_LCU_GRP0 exceeds emit_enable_lcu_group cap");
_Static_assert(sizeof(MNIST_ENABLE_LCU_GRP1) <= MNIST_ENABLE_LCU_GROUP_MAX,
               "MNIST_ENABLE_LCU_GRP1 exceeds emit_enable_lcu_group cap");
_Static_assert(sizeof(MNIST_ENABLE_LCU_GRP2) <= MNIST_ENABLE_LCU_GROUP_MAX,
               "MNIST_ENABLE_LCU_GRP2 exceeds emit_enable_lcu_group cap");
_Static_assert(sizeof(MNIST_ENABLE_LCU_GRP3) <= MNIST_ENABLE_LCU_GROUP_MAX,
               "MNIST_ENABLE_LCU_GRP3 exceeds emit_enable_lcu_group cap");

/* Expand a packed-lcu-id array into enable_lcu_default sub-bodies
 * ({packed_lcu_id, network_index=0}) so they can be fed to
 * hailo_cs_builder_append_repeated. Stack-sized scratch array;
 * max 4 LCUs per group keeps footprint trivial. */
static int emit_enable_lcu_group(struct hailo_cs_builder *b,
                                  const uint8_t *lcus, uint8_t count)
{
    struct hailo_cs_act_enable_lcu_default bodies[MNIST_ENABLE_LCU_GROUP_MAX];
    if (count > MNIST_ENABLE_LCU_GROUP_MAX) return HAILO_ERR_INVAL;
    for (uint8_t i = 0; i < count; i++) {
        bodies[i].packed_lcu_id = lcus[i];
        bodies[i].network_index = 0;
    }
    return hailo_cs_builder_append_repeated(
        b, HAILO_CS_ACT_ENABLE_LCU_DEFAULT, count,
        bodies, sizeof(bodies[0]));
}

static int emit_module_config_done(struct hailo_cs_builder *b, uint8_t module)
{
    struct hailo_cs_act_module_config_done_interrupt body = {
        .module_index = module,
    };
    return hailo_cs_builder_append(
        b, HAILO_CS_ACT_MODULE_CONFIG_DONE_INTERRUPT,
        &body, sizeof(body));
}

static int emit_sequencer_done(struct hailo_cs_builder *b, uint8_t seq_idx)
{
    struct hailo_cs_act_sequencer_interrupt body = {
        .sequencer_index = seq_idx,
    };
    return hailo_cs_builder_append(
        b, HAILO_CS_ACT_SEQUENCER_DONE_INTERRUPT,
        &body, sizeof(body));
}

static int emit_trigger_sequencer(struct hailo_cs_builder *b,
                                   uint8_t cluster,
                                   const uint8_t *seq_cfg_43_bytes)
{
    struct hailo_cs_act_trigger_sequencer body;
    body.cluster_index = cluster;
    memcpy(&body.sequencer_config, seq_cfg_43_bytes,
           sizeof(body.sequencer_config));
    return hailo_cs_builder_append(
        b, HAILO_CS_ACT_TRIGGER_SEQUENCER, &body, sizeof(body));
}

static int emit_disable_lcu(struct hailo_cs_builder *b, uint8_t packed_lcu)
{
    struct hailo_cs_act_disable_lcu body = {
        .packed_lcu_id = packed_lcu,
    };
    return hailo_cs_builder_append(
        b, HAILO_CS_ACT_DISABLE_LCU, &body, sizeof(body));
}

static int emit_deactivate_cfg_channel(struct hailo_cs_builder *b,
                                        uint8_t packed, uint8_t sidx)
{
    struct hailo_cs_act_deactivate_cfg_channel body = {
        .packed_vdma_channel_id = packed,
        .config_stream_index    = sidx,
    };
    return hailo_cs_builder_append(
        b, HAILO_CS_ACT_DEACTIVATE_CFG_CHANNEL, &body, sizeof(body));
}

/* Forward decls — defined later alongside DYNAMIC prologue emit. */
static int find_boundary_pads(const struct hef_info *info,
                              const struct hef_pad_info **in_pad,
                              const struct hef_pad_info **out_pad);
static int emit_dynamic_boundary_prologue(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_builder *b);

static int translate_preliminary_mnist_arming(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_builder *b)
{
    int rc;

    /* Phase 3a: clean slate — disable LCU 0x01 + (if dual-channel
     * mode) an initial REPEATED(2x FETCH_CFG_CHANNEL_DESCRIPTORS)
     * handshake, then the 14-LCU REPEATED DISABLE sweep, then a
     * second REPEATED(2x FETCH_CFG_CHANNEL_DESCRIPTORS) that pulls
     * the main CCW payload. Order verbatim from
     * pios_PRELIMINARY.bin. */
    rc = emit_disable_lcu(b, 0x01);
    if (rc != HAILO_OK) return rc;

    bool dual = cfg->cfg_channel_1_desc_list_iova != 0;
    if (dual) {
        struct hailo_cs_act_fetch_cfg_channel_descriptors initial_fetch[2] = {
            /* HailoRT: sub[0]=010000  sub[1]=010001 — one desc each
             * to kick fw's per-channel descriptor walker. */
            { .descriptors_count = 1,
              .packed_vdma_channel_id = cfg->cfg_channel_1_packed_vdma },
            { .descriptors_count = 1,
              .packed_vdma_channel_id = cfg->config_vdma_channel },
        };
        rc = hailo_cs_builder_append_repeated(
            b, HAILO_CS_ACT_FETCH_CFG_CHANNEL_DESCRIPTORS,
            /*count=*/2, initial_fetch, sizeof(initial_fetch[0]));
        if (rc != HAILO_OK) return rc;
    }

    rc = hailo_cs_builder_append_repeated(
        b, HAILO_CS_ACT_DISABLE_LCU,
        (uint8_t)(sizeof(MNIST_DISABLE_LCU_SWEEP) /
                  sizeof(MNIST_DISABLE_LCU_SWEEP[0])),
        MNIST_DISABLE_LCU_SWEEP, 1);
    if (rc != HAILO_OK) return rc;

    if (dual) {
        /* Bulk CCW pull: one more desc on the small channel, then
         * cfg->ccw_fetch_bulk_desc_count descs on the bulk channel.
         * HailoRT picks the real data-size count (109 for MNIST's
         * 55792 bytes), not the pow2-rounded list size. */
        uint32_t bulk_descs = cfg->ccw_fetch_bulk_desc_count;
        if (bulk_descs > UINT16_MAX) bulk_descs = UINT16_MAX;

        struct hailo_cs_act_fetch_cfg_channel_descriptors bulk_fetch[2] = {
            { .descriptors_count = 1,
              .packed_vdma_channel_id = cfg->config_vdma_channel },
            { .descriptors_count = (uint16_t)bulk_descs,
              .packed_vdma_channel_id = cfg->cfg_channel_1_packed_vdma },
        };
        rc = hailo_cs_builder_append_repeated(
            b, HAILO_CS_ACT_FETCH_CFG_CHANNEL_DESCRIPTORS,
            /*count=*/2, bulk_fetch, sizeof(bulk_fetch[0]));
        if (rc != HAILO_OK) return rc;
    }

    /* Phase 3b: module-config-done + TRIGGER_SEQUENCER per cluster. */
    rc = emit_module_config_done(b, 0x05);           if (rc) return rc;
    rc = emit_trigger_sequencer(b, 0, MNIST_SEQ_CFG_CLUSTER_0);
    if (rc) return rc;
    rc = emit_module_config_done(b, 0x06);           if (rc) return rc;
    rc = emit_trigger_sequencer(b, 1, MNIST_SEQ_CFG_CLUSTER_1);
    if (rc) return rc;

    /* Phase 3c: sequencer-done waits + 3 module-done interrupts. */
    rc = emit_sequencer_done(b, 0);                  if (rc) return rc;
    rc = emit_sequencer_done(b, 1);                  if (rc) return rc;
    rc = emit_module_config_done(b, 0x12);           if (rc) return rc;
    rc = emit_module_config_done(b, 0x13);           if (rc) return rc;
    rc = emit_module_config_done(b, 0x11);           if (rc) return rc;

    /* Phase 2 (reordered to match HailoRT's ref sequence):
     * ACTIVATE_BOUNDARY_OUTPUT/INPUT + RESUME_VDMA (H2D) +
     * FETCH_DATA_FROM_VDMA + BURST_CREDITS_TASK_START. HailoRT emits
     * this AFTER Phase 3c (MODULE_CONFIG_DONE 0x11) and BEFORE Phase 4
     * LCU enables, per pios_PRELIMINARY.bin. Fw seems to need the
     * boundary channels armed + credits task started *while the NN
     * core is mid-arming* — emitting these only in ACTIVATION +
     * DYNAMIC isn't enough. */
    rc = emit_dynamic_boundary_prologue(info, cfg, b);
    if (rc != HAILO_OK) return rc;

    /* FETCH_DATA_FROM_VDMA for the input boundary — mirror of what
     * translate_allow_input_dataflow emits in DYNAMIC. */
    const struct hef_pad_info *in_pad = NULL, *out_pad = NULL;
    if (find_boundary_pads(info, &in_pad, &out_pad) != 0) {
        /* MNIST HEF must have both boundary pads; defensive early-out
         * so the template doesn't emit garbage if pad capture missed. */
        return HAILO_ERR_INVAL;
    }
    uint8_t fetch_packed;
    rc = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET,
                            "PreliminaryFetch", &fetch_packed);
    if (rc != HAILO_OK) return rc;
    struct hailo_cs_act_fetch_data_from_vdma fetch = {
        .packed_vdma_channel_id = fetch_packed,
        .stream_index           = (uint8_t)in_pad->sys_index,
        .network_index          = 0,
        .frame_periph_size      = pad_periph_frame_size(in_pad),
        .credit_type            = 1,   /* CREDIT_IN_BYTES */
        .host_buffer_type       = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
    };
    rc = hailo_cs_builder_append(b, HAILO_CS_ACT_FETCH_DATA_FROM_VDMA_CHANNEL,
                                 &fetch, sizeof(fetch));
    if (rc != HAILO_OK) return rc;

    /* Phase 2 tail: start the credit task. There are TWO
     * BURST_CREDITS_TASK_START actions in HailoRT's flow — one here
     * in PRELIMINARY and one in BATCH_SWITCHING (ours emits that
     * already). Both are required. */
    rc = hailo_cs_builder_append(b, HAILO_CS_ACT_BURST_CREDITS_TASK_START,
                                 NULL, 0);
    if (rc != HAILO_OK) return rc;

    /* Phase 4: 4 LCU enable groups + 2 module-config-done waits each.
     * Matches ref ordering: grp0 (cluster 0/2 LCUs), module 14/15,
     * grp1 (cluster 1 LCUs), module 16/17, grp2 (cluster 0/1),
     * module 18/19, grp3 (cluster 1), module 0d/0e. */
    rc = emit_enable_lcu_group(b, MNIST_ENABLE_LCU_GRP0, 4); if (rc) return rc;
    rc = emit_module_config_done(b, 0x14); if (rc) return rc;
    rc = emit_module_config_done(b, 0x15); if (rc) return rc;

    rc = emit_enable_lcu_group(b, MNIST_ENABLE_LCU_GRP1, 4); if (rc) return rc;
    rc = emit_module_config_done(b, 0x16); if (rc) return rc;
    rc = emit_module_config_done(b, 0x17); if (rc) return rc;

    rc = emit_enable_lcu_group(b, MNIST_ENABLE_LCU_GRP2, 4); if (rc) return rc;
    rc = emit_module_config_done(b, 0x18); if (rc) return rc;
    rc = emit_module_config_done(b, 0x19); if (rc) return rc;

    rc = emit_enable_lcu_group(b, MNIST_ENABLE_LCU_GRP3, 3); if (rc) return rc;
    rc = emit_module_config_done(b, 0x0d); if (rc) return rc;
    rc = emit_module_config_done(b, 0x0e); if (rc) return rc;

    /* Phase 4b: tear down cfg channels in the same order HailoRT
     * does (bulk/packed=0 before small/packed=1). The single-cfg
     * path emits only the primary deactivation. */
    if (cfg->cfg_channel_1_desc_list_iova != 0) {
        rc = emit_deactivate_cfg_channel(b, cfg->cfg_channel_1_packed_vdma,
                                         cfg->cfg_channel_1_stream_index);
        if (rc) return rc;
    }
    rc = emit_deactivate_cfg_channel(b, cfg->config_vdma_channel,
                                     cfg->config_stream_index);
    if (rc) return rc;

    return HAILO_OK;
}

static int translate_preliminary(const struct hef_info *info,
                                 const struct hailo_cs_translate_cfg *cfg,
                                 struct hailo_cs_builder *b)
{
    bool dual = cfg->cfg_channel_1_desc_list_iova != 0;
    int rc;

    /* Open the bulk cfg channel first on dual-channel paths (HailoRT
     * emits packed=0 before packed=1 per pios_PRELIMINARY.bin). */
    if (dual) {
        struct hailo_cs_act_activate_cfg_channel bulk = {
            .packed_vdma_channel_id = cfg->cfg_channel_1_packed_vdma,
            .config_stream_index    = cfg->cfg_channel_1_stream_index,
            .host_buffer_info = {
                .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
                .dma_address      = cfg->cfg_channel_1_desc_list_iova,
                .desc_page_size   = cfg->ccw_desc_page_size,
                .total_desc_count = cfg->cfg_channel_1_total_desc_count,
                .bytes_in_pattern = cfg->cfg_channel_1_bytes_in_pattern,
            },
        };
        rc = hailo_cs_builder_append(b, HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL,
                                     &bulk, sizeof(bulk));
        if (rc != HAILO_OK) return rc;
    }

    struct hailo_cs_act_activate_cfg_channel act = {
        .packed_vdma_channel_id = cfg->config_vdma_channel,
        .config_stream_index    = cfg->config_stream_index,
        .host_buffer_info = {
            .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
            .dma_address      = cfg->ccw_desc_list_iova,
            .desc_page_size   = cfg->ccw_desc_page_size,
            .total_desc_count = cfg->ccw_total_desc_count,
            .bytes_in_pattern = cfg->ccw_bytes_in_pattern,
        },
    };
    rc = hailo_cs_builder_append(b, HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL,
                                 &act, sizeof(act));
    if (rc != HAILO_OK) return rc;

    /* Single-channel path: one FETCH_CFG_CHANNEL_DESCRIPTORS for the
     * entire CCW buffer, wrapped in REPEATED_ACTION (Hailo-8L requires
     * the wrapper; the bare action gets rejected as
     * CONFIG_MANAGER_WRAPPER_STATUS_ACTION_TYPE_NOT_SUPPORTED).
     * Dual-channel path emits the HailoRT-shaped 2x(2xFETCH) sequence
     * inside translate_preliminary_mnist_arming, interleaved with
     * the LCU-disable sweep. */
    if (!dual) {
        uint32_t descs = cfg->ccw_total_desc_count;
        if (descs == 0) descs = 1;               /* firmware rejects 0 */
        if (descs > UINT16_MAX) descs = UINT16_MAX;

        struct hailo_cs_act_fetch_cfg_channel_descriptors sub = {
            .descriptors_count      = (uint16_t)descs,
            .packed_vdma_channel_id = cfg->config_vdma_channel,
        };
        rc = hailo_cs_builder_append_repeated(
            b, HAILO_CS_ACT_FETCH_CFG_CHANNEL_DESCRIPTORS,
            /*count=*/1, &sub, sizeof(sub));
        if (rc != HAILO_OK) return rc;
    }

    /* #253 Phase 8: emit the MNIST-shaped NN-core arming sequence
     * after CCW upload. Without this, fw's inference scheduler never
     * grants credits to the boundary-IN fetch and ch=2 num_proc stays
     * 0. Gated on MNIST shape detection; other HEFs fall through to
     * the minimal preliminary. Generalization requires parsing
     * nn_stream_config's LCU + sequencer tables from the HEF. */
    if (hef_matches_mnist_template(info)) {
        rc = translate_preliminary_mnist_arming(info, cfg, b);
        if (rc != HAILO_OK) return rc;
    }

    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* DYNAMIC context                                                              */
/* -------------------------------------------------------------------------- */

/* DYNAMIC context translation. Emits one ENABLE_LCU_* per captured
 * EnableLcu action in hef_info.enable_lcu_actions[], followed by an
 * APPLICATION_CHANGE_INTERRUPT tail marker (legal only at tail of
 * single-dynamic-context loads per HailoRT).
 *
 * HEF 4.23 loads we've seen carry EnableLcu for their compute
 * clusters; fall through to just the tail marker on loads without
 * any captured actions (firmware accepts the context structurally
 * though no compute happens).
 *
 * Future action types (TriggerSequencer, AllowInputDataflow,
 * WaitForSequencer) slot in here as their per-action parameter
 * extraction lands in hef_parser. The translator's dispatch is
 * linear — iterate context_actions[].action_types[] and translate
 * each; the unextracted kinds are skipped silently for now.
 */
static bool enable_lcu_has_non_default_fields(
    const struct hef_enable_lcu_action *a)
{
    /* Non-default encoding is required when EITHER kernel_done_count
     * or kernel_done_address is non-zero. HailoRT's selector is the
     * same. */
    return a->lcu_kernel_done_count != 0 ||
           a->lcu_kernel_done_address != 0;
}

static int translate_enable_lcu(const struct hef_enable_lcu_action *a,
                                struct hailo_cs_builder *b)
{
    bool clamped = false;
    uint8_t packed = hailo_cs_pack_lcu_id_checked(a->cluster_index,
                                                  a->lcu_index,
                                                  &clamped);
    if (clamped) {
        WARN("hailo translator: EnableLcu cluster=%u lcu=%u exceeds 4-bit "
             "range; packed_lcu_id truncated to 0x%02x",
             a->cluster_index, a->lcu_index, packed);
    }
    if (enable_lcu_has_non_default_fields(a)) {
        struct hailo_cs_act_enable_lcu_non_default body = {
            .packed_lcu_id       = packed,
            .network_index       = (uint8_t)a->network_index,
            .kernel_done_address = (uint16_t)a->lcu_kernel_done_address,
            .kernel_done_count   = a->lcu_kernel_done_count,
        };
        return hailo_cs_builder_append(
            b, HAILO_CS_ACT_ENABLE_LCU_NON_DEFAULT, &body, sizeof(body));
    }
    struct hailo_cs_act_enable_lcu_default body = {
        .packed_lcu_id = packed,
        .network_index = (uint8_t)a->network_index,
    };
    return hailo_cs_builder_append(
        b, HAILO_CS_ACT_ENABLE_LCU_DEFAULT, &body, sizeof(body));
}

/* DisableLcu → DISABLE_LCU (1-byte packed_lcu_id). */
static int translate_disable_lcu(const struct hef_disable_lcu_action *a,
                                 struct hailo_cs_builder *b)
{
    bool clamped = false;
    uint8_t packed = hailo_cs_pack_lcu_id_checked(a->cluster_index,
                                                  a->lcu_index,
                                                  &clamped);
    if (clamped) {
        WARN("hailo translator: DisableLcu cluster=%u lcu=%u exceeds 4-bit "
             "range; packed_lcu_id truncated to 0x%02x",
             a->cluster_index, a->lcu_index, packed);
    }
    struct hailo_cs_act_disable_lcu body = { .packed_lcu_id = packed };
    return hailo_cs_builder_append(b, HAILO_CS_ACT_DISABLE_LCU,
                                   &body, sizeof(body));
}

/* WaitForSequencer → SEQUENCER_DONE_INTERRUPT (1-byte body).
 * On Hailo-8, sequencer_index is the cluster_index verbatim. */
static int translate_wait_sequencer(const struct hef_wait_sequencer_action *a,
                                    struct hailo_cs_builder *b)
{
    if (a->cluster_index > 0xFFu) {
        WARN("hailo translator: WaitForSequencer cluster=%u exceeds u8; "
             "truncating to 0x%02x", a->cluster_index,
             (unsigned)(a->cluster_index & 0xFFu));
    }
    struct hailo_cs_act_sequencer_interrupt body = {
        .sequencer_index = (uint8_t)a->cluster_index,
    };
    return hailo_cs_builder_append(b, HAILO_CS_ACT_SEQUENCER_DONE_INTERRUPT,
                                   &body, sizeof(body));
}

/* EnableSequencer → TRIGGER_SEQUENCER (cluster + 43-byte
 * sequencer_config). The HEF's initial_l3_offset is a u32 that fits
 * in the wire's u16 for all realistic values; initial_l3_cut is a
 * u32 narrowed to u8. */
static int translate_trigger_sequencer(const struct hef_trigger_sequencer_action *a,
                                       struct hailo_cs_builder *b)
{
    struct hailo_cs_act_trigger_sequencer body = {
        .cluster_index = (uint8_t)a->cluster_index,
        .sequencer_config = {
            .initial_l3_cut    = (uint8_t)a->initial_l3_cut,
            .initial_l3_offset = (uint16_t)a->initial_l3_offset,
            .active_apu        = a->active_apu_bitmap,
            .active_ia         = a->active_ia_bitmap,
            .active_sc         = a->active_sc_bitmap,
            .active_l2         = a->active_l2_bitmap,
            .l2_offset_0       = a->l2_offset_0,
            .l2_offset_1       = a->l2_offset_1,
        },
    };
    if (a->cluster_index      > 0xFFu ||
        a->initial_l3_cut     > 0xFFu ||
        a->initial_l3_offset  > 0xFFFFu) {
        WARN("hailo translator: TriggerSequencer narrows — cluster=%u "
             "l3_cut=%u l3_offset=%u",
             a->cluster_index, a->initial_l3_cut, a->initial_l3_offset);
    }
    return hailo_cs_builder_append(b, HAILO_CS_ACT_TRIGGER_SEQUENCER,
                                   &body, sizeof(body));
}

/* AllowInputDataflow → FETCH_DATA_FROM_VDMA_CHANNEL (9-byte body).
 * The HEF's sys_index identifies the boundary stream; translator
 * maps it to the host-chosen VDMA channel via the pad table, then
 * fills geometry from the matching hef_pad_info. */
static int translate_allow_input_dataflow(
    const struct hef_allow_input_dataflow_action *a,
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_builder *b)
{
    /* Look up the pad by sys_index to recover frame geometry. A
     * zero-byte fetch is never legitimate — firmware has no clean
     * error for `frame_periph_size=0`, so fail fast here and let
     * the caller surface the missing pad.
     *
     * #253 (2026-04-22): frame_periph_size here must match the value
     * OpenBoundaryInputChannel declared in ACTIVATION and the one
     * ACTIVATE_BOUNDARY_INPUT carries in the DYNAMIC prologue —
     * all three use pad_periph_frame_size() for a single
     * direction-aware formula (tensor size for input, core size
     * for output). For the MNIST HEF: h=28, w=28, features=1 ->
     * periph_frame=784 (was 896 when it was bpb*bpf). */
    uint32_t frame_size = 0;
    uint8_t  pad_sys_index = 0;
    bool pad_found = false;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        if (info->pads[i].sys_index == a->sys_index) {
            frame_size    = pad_periph_frame_size(&info->pads[i]);
            pad_sys_index = info->pads[i].sys_index;
            pad_found     = true;
            break;
        }
    }
    if (!pad_found || frame_size == 0) {
        WARN("hailo translator: AllowInputDataflow sys_index=%u not found "
             "in pads[] (or zero frame_size)", a->sys_index);
        return HAILO_ERR_INVAL;
    }

    /* Input stream uses the translator's boundary-input channel
     * offset (HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET). The same
     * constant is consumed by ACTIVATION's OpenBoundaryInput
     * emitter, so the two ends agree by construction rather than
     * by separately-written magic numbers. */
    uint8_t packed_vdma;
    int rc_pack = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET,
                                     "AllowInputDataflow", &packed_vdma);
    if (rc_pack != HAILO_OK) return rc_pack;

    /* stream_index in FETCH_DATA_FROM_VDMA must match the pad's
     * sys_index, not a fixed 0. Fw uses this to correlate the fetch
     * with ACTIVATE_BOUNDARY_INPUT's stream_index (also sys_index in
     * emit_dynamic_boundary_prologue); a mismatch leaves the periph
     * engine waiting on the wrong stream slot. HailoRT wire capture
     * for MNIST on pi-5-1 shows stream_index=1 here (input pad's
     * sys_index), not 0. */
    struct hailo_cs_act_fetch_data_from_vdma body = {
        .packed_vdma_channel_id = packed_vdma,
        .stream_index           = pad_sys_index,
        .network_index          = 0,
        .frame_periph_size      = frame_size,
        .credit_type            = 1,   /* CREDIT_IN_BYTES */
        .host_buffer_type       = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
    };
    return hailo_cs_builder_append(b, HAILO_CS_ACT_FETCH_DATA_FROM_VDMA_CHANNEL,
                                   &body, sizeof(body));
}

/* -------------------------------------------------------------------------- */
/* DYNAMIC context — order-preserving dispatch                                 */
/* -------------------------------------------------------------------------- */

/* HEF order matters: the compiler emits enable/disable/trigger/wait
 * sequences that firmware executes in the given sequence. The
 * extracted per-kind arrays preserve intra-kind order but a
 * straight dump-by-kind would shuffle the inter-kind ordering.
 *
 * Instead we walk context_actions[0].action_types[] in emit order
 * and, for each tag, pull the next entry from the matching extract
 * array via a per-kind read cursor. Cursors are initialized to 0
 * and advanced past entries that belong to earlier contexts.
 *
 * Today only context 0 is supported (context_actions_count > 1 is
 * rejected upstream) and the parser stamps every captured action
 * with context_index == 0 in the single-context path. The
 * "skip non-target-context entries" while-loops below are therefore
 * dead code against real HEFs today — they fire only in tests that
 * hand-populate mixed context_index values. Kept in as defense-in-
 * depth for when multi-context dispatch lands (tracked alongside
 * #178/#179): the scaffolding stays correct by construction rather
 * than needing to be reintroduced later.
 */
struct dynamic_cursors {
    uint32_t enable_lcu;
    uint32_t disable_lcu;
    uint32_t trigger_sequencer;
    uint32_t wait_sequencer;
    uint32_t allow_input_dataflow;
};

/* Locate the input + output boundary pads. Returns -1 if either is
 * missing; emit_dynamic_boundary_prologue skips silently in that case
 * (no boundary => nothing to activate). */
static int find_boundary_pads(const struct hef_info *info,
                              const struct hef_pad_info **in_pad,
                              const struct hef_pad_info **out_pad)
{
    *in_pad = NULL;
    *out_pad = NULL;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *p = &info->pads[i];
        if (!p->has_stream_info) continue;
        if (p->is_input  && !*in_pad)  *in_pad  = p;
        if (!p->is_input && !*out_pad) *out_pad = p;
    }
    return (*in_pad && *out_pad) ? 0 : -1;
}

/* Fill a stream_reg_info from pad geometry, matching HailoRT v4.23
 * byte-for-byte (cross-checked against pios_DYNAMIC.bin for the
 * MNIST HEF).
 *
 * HailoRT synthesizes `nn_stream_config` at load time from
 * ProtoHEFEdgeLayerBase + direction + HW padding support
 * (HefConfigurator::parse_nn_stream_config in
 * hailo-hef-internal.hpp:538). The proto itself does NOT expose
 * `periph_bytes_per_buffer`, `periph_buffers_per_frame`,
 * `buffer_padding_payload`, `buffer_padding` — we compute them the
 * same way:
 *
 *   INPUT:
 *     periph_bytes_per_buffer  = h*w*features   (whole-frame view)
 *     periph_buffers_per_frame = 1
 *     buffer_padding_payload   = 0
 *     buffer_padding           = 0
 *     (HW handles the core-vs-periph alignment inside the NN engine.)
 *
 *   OUTPUT:
 *     periph_bytes_per_buffer  = core_bytes_per_buffer
 *     periph_buffers_per_frame = 1
 *     buffer_padding_payload   = h*w*features / bpf  (real bytes per buffer)
 *     buffer_padding           = core_bytes_per_buffer - payload
 *     (The core emits padded buffers; the buffer_padding pair encodes
 *     the payload/padding split that downstream code uses to strip
 *     padding when presenting to the host.)
 *
 * Verified numerically against HailoRT wire capture for MNIST on
 * pi-5-1 fw v4.23:
 *   INPUT  (28x28x1, bpb=32, bpf=28):
 *     periph=(784,1,0,0)  ✓
 *   OUTPUT (1x1x10, bpb=16, bpf=1):
 *     periph=(16,1,10,6)  ✓
 *
 * is_core_hw_padding_config_in_dfc=1 marks the HEF as having been
 * compiled with DFC's post-3.33 padding convention (the only shape
 * we've seen in the wild so far). */
static void fill_stream_reg_info_from_pad(const struct hef_pad_info *p,
                                          struct hailo_cs_stream_reg_info *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t bpb = p->core_bytes_per_buffer;
    uint32_t bpf = p->core_buffers_per_frame ? p->core_buffers_per_frame : 1u;

    out->core_bytes_per_buffer    = (uint16_t)bpb;
    out->core_buffers_per_frame   = (uint16_t)bpf;

    uint32_t tensor_total = p->has_tensor_shape
        ? (uint32_t)p->height * p->width * p->features : bpb * bpf;

    if (p->is_input) {
        /* Input: periph view is the full tensor as one buffer. */
        out->periph_bytes_per_buffer  = (uint16_t)tensor_total;
        out->periph_buffers_per_frame = 1u;
        /* feature_padding_payload / buffer_padding{,_payload} left 0
         * — HW handles core/periph reshape without host-side info. */
    } else {
        /* Output: periph matches core; per-buffer padding split is
         * encoded in buffer_padding_payload / buffer_padding. */
        out->periph_bytes_per_buffer  = (uint16_t)bpb;
        out->periph_buffers_per_frame = 1u;
        uint32_t payload = (bpf > 0) ? (tensor_total / bpf) : 0;
        /* Clamp pathological cases: HEFs with core smaller than the
         * tensor shape (shouldn't happen for real v4.23 compilations,
         * but cheap to guard). */
        if (payload > bpb) payload = bpb;
        uint32_t padding = bpb - payload;
        out->buffer_padding_payload = payload;
        out->buffer_padding         = (uint16_t)padding;
    }
    out->is_core_hw_padding_config_in_dfc = 1u;
}

/* Fill host_buffer_info from the caller's desc-list iova + geometry. */
static void fill_host_buffer_info(uint64_t iova, uint16_t page_size,
                                  uint32_t desc_count, uint32_t transfer_size,
                                  struct hailo_cs_host_buffer_info *out)
{
    memset(out, 0, sizeof(*out));
    out->buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC;
    out->dma_address      = iova;
    out->desc_page_size   = page_size;
    out->total_desc_count = desc_count;
    out->bytes_in_pattern = transfer_size;
}

/* Emit the DYNAMIC-context prologue that HailoRT v4.23 issues for
 * every boundary-I/O inference: activate the output and input
 * boundary channels with their stream_reg_info + host_buffer_info,
 * then resume the VDMA channels. Without these actions firmware
 * never primes device-side num_avail and every submit times out
 * (see #253 root-cause analysis + docs/reference/pios_DYNAMIC.bin). */
static int emit_dynamic_boundary_prologue(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_builder *b)
{
    const struct hef_pad_info *in_pad = NULL, *out_pad = NULL;
    if (find_boundary_pads(info, &in_pad, &out_pad) != 0) return HAILO_OK;

    uint8_t in_ch, out_ch;
    int rc = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET,
                                "DynamicActivateIn", &in_ch);
    if (rc != HAILO_OK) return rc;
    rc = pack_boundary_vdma(cfg, HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET,
                            "DynamicActivateOut", &out_ch);
    if (rc != HAILO_OK) return rc;

    /* host_buffer_info.bytes_in_pattern = periph_frame_size; same
     * direction-aware formula as ACTIVATION's OpenBoundary and
     * AllowInputDataflow's frame_periph_size. */
    uint32_t in_frame  = pad_periph_frame_size(in_pad);
    uint32_t out_frame = pad_periph_frame_size(out_pad);

    /* 1. ACTIVATE_BOUNDARY_OUTPUT (v4.23 emits output first). */
    struct hailo_cs_act_activate_boundary_output out_body;
    memset(&out_body, 0, sizeof(out_body));
    out_body.packed_vdma_channel_id = out_ch;
    out_body.stream_index           = (uint8_t)out_pad->sys_index;
    out_body.network_index          = 0;
    fill_stream_reg_info_from_pad(out_pad, &out_body.stream_reg_info);
    fill_host_buffer_info(cfg->boundary_output_desc_list_iova,
                          cfg_output_page_size(cfg),
                          cfg->boundary_output_total_desc_count,
                          out_frame, &out_body.host_buffer_info);
    rc = hailo_cs_builder_append(b, HAILO_CS_ACT_ACTIVATE_BOUNDARY_OUTPUT,
                                 &out_body, sizeof(out_body));
    if (rc != HAILO_OK) return rc;

    /* 2. ACTIVATE_BOUNDARY_INPUT. initial_credit_size = 0x10000 (64 KB)
     * matches HailoRT's value on the wire; represents a full credit
     * window for the engine's num_avail gate. */
    struct hailo_cs_act_activate_boundary_input in_body;
    memset(&in_body, 0, sizeof(in_body));
    in_body.packed_vdma_channel_id = in_ch;
    in_body.stream_index           = (uint8_t)in_pad->sys_index;
    fill_stream_reg_info_from_pad(in_pad, &in_body.stream_reg_info);
    fill_host_buffer_info(cfg->boundary_input_desc_list_iova,
                          cfg->boundary_desc_page_size,
                          cfg->boundary_input_total_desc_count,
                          in_frame, &in_body.host_buffer_info);
    in_body.initial_credit_size = 0x10000u;
    rc = hailo_cs_builder_append(b, HAILO_CS_ACT_ACTIVATE_BOUNDARY_INPUT,
                                 &in_body, sizeof(in_body));
    if (rc != HAILO_OK) return rc;

    /* 3. RESUME_VDMA_CHANNEL for the input boundary channel.
     * HailoRT's v4.23 wire capture (pios_DYNAMIC.bin) emits this for
     * the H2D input side only -- the output side is activated via
     * ACTIVATE_BOUNDARY_OUTPUT without a matching RESUME (probably
     * because the output channel wasn't paused between contexts the
     * same way as input). (void)out_ch prevents an unused warning. */
    (void)out_ch;
    struct hailo_cs_act_resume_vdma_channel resume_in = {
        .packed_vdma_channel_id = in_ch,
        .edge_layer_direction   = HAILO_CS_EDGE_DIR_H2D,
    };
    return hailo_cs_builder_append(b, HAILO_CS_ACT_RESUME_VDMA_CHANNEL,
                                   &resume_in, sizeof(resume_in));
}

static int translate_dynamic(const struct hef_info *info,
                             const struct hailo_cs_translate_cfg *cfg,
                             struct hailo_cs_builder *b)
{
    const uint32_t target_ctx = 0;
    struct dynamic_cursors cur;
    memset(&cur, 0, sizeof(cur));

    /* #253: Emit the DYNAMIC prologue BEFORE processing HEF-derived
     * action_types[]. HailoRT synthesizes ACTIVATE_BOUNDARY_{OUT,IN}
     * and RESUME_VDMA_CHANNEL from the network graph at runtime — our
     * parser only reads the literal action list so we have to inject
     * them here based on the boundary pads. Skips silently if the HEF
     * doesn't have both a boundary input and a boundary output. */
    int pre_rc = emit_dynamic_boundary_prologue(info, cfg, b);
    if (pre_rc != HAILO_OK) return pre_rc;

    /* If the HEF captured zero contexts, fall through to the tail-
     * only path. Otherwise walk context_actions[0].action_types[]. */
    if (info->context_actions_count == 0) {
        return hailo_cs_builder_append(
            b, HAILO_CS_ACT_APPLICATION_CHANGE_INTERRUPT, NULL, 0);
    }

    /* Multi-context HEFs are not fully supported — full dispatch
     * would require emitting one SET_CONTEXT_INFO per dynamic context
     * plus APPLICATION_CHANGE_INTERRUPT glue in the preceding
     * context. Phase 8 compromise: translate ctx0 only and log a
     * warning. Firmware will execute ctx0 and then hit the missing
     * change-context transition. For capstone-stage performance
     * measurement this still exercises the full DMA-in → NPU-compute
     * (partial) → DMA-out path, which is what we need timing data
     * for. Correct multi-context dispatch is follow-on work.
     *
     * Before Phase 8 this branch returned HAILO_ERR_INVAL and
     * refused the HEF entirely; that blocked every DFC 3.33.1
     * compile (ResNet-18 has 2 contexts, etc.). */
    if (info->context_actions_count > 1) {
        INFO("hailo translator: multi-context HEF (%u contexts) — "
             "translating ctx0 only; execution will truncate at "
             "the first context-switch boundary",
             info->context_actions_count);
    }

    const struct hef_context_actions *ctx = &info->context_actions[target_ctx];

    /* Refuse to translate a context whose action list was truncated
     * at parse time: action_types[] would be missing entries the
     * firmware expects to execute. The per-kind arrays are checked
     * inline below because truncation there shifts the cursor-to-
     * entry correspondence and silently emits the wrong action at
     * the wrong stream position. */
    if (ctx->truncated) {
        WARN("hailo translator: context[%u].action_types truncated "
             "(%u > %u) — refusing to translate partial stream",
             target_ctx, ctx->action_count,
             (unsigned)HEF_PARSER_MAX_CONTEXT_ACTIONS);
        return HAILO_ERR_INVAL;
    }
    if (info->enable_lcu_truncated || info->disable_lcu_truncated ||
        info->trigger_sequencer_truncated || info->wait_sequencer_truncated ||
        info->allow_input_dataflow_truncated) {
        WARN("hailo translator: per-kind action array truncated — "
             "refusing to translate (cursor positions would drift)");
        return HAILO_ERR_INVAL;
    }

    uint32_t count = ctx->action_count;

    for (uint32_t i = 0; i < count; i++) {
        uint8_t tag = ctx->action_types[i];
        int rc = HAILO_OK;

        switch (tag) {
        case ProtoHEFAction_enable_lcu_tag: {
            /* Advance cursor past non-target-context entries. */
            while (cur.enable_lcu < info->enable_lcu_count &&
                   info->enable_lcu_actions[cur.enable_lcu].context_index != target_ctx) {
                cur.enable_lcu++;
            }
            if (cur.enable_lcu >= info->enable_lcu_count) {
                WARN("hailo translator: action_types[%u]=enable_lcu but "
                     "per-kind array exhausted", i);
                return HAILO_ERR_INVAL;
            }
            rc = translate_enable_lcu(
                &info->enable_lcu_actions[cur.enable_lcu++], b);
            break;
        }
        case ProtoHEFAction_disable_lcu_tag: {
            while (cur.disable_lcu < info->disable_lcu_count &&
                   info->disable_lcu_actions[cur.disable_lcu].context_index != target_ctx) {
                cur.disable_lcu++;
            }
            if (cur.disable_lcu >= info->disable_lcu_count) {
                WARN("hailo translator: action_types[%u]=disable_lcu but "
                     "per-kind array exhausted", i);
                return HAILO_ERR_INVAL;
            }
            rc = translate_disable_lcu(
                &info->disable_lcu_actions[cur.disable_lcu++], b);
            break;
        }
        case ProtoHEFAction_enable_sequencer_tag: {
            while (cur.trigger_sequencer < info->trigger_sequencer_count &&
                   info->trigger_sequencer_actions[cur.trigger_sequencer].context_index != target_ctx) {
                cur.trigger_sequencer++;
            }
            if (cur.trigger_sequencer >= info->trigger_sequencer_count) {
                WARN("hailo translator: action_types[%u]=enable_sequencer but "
                     "per-kind array exhausted", i);
                return HAILO_ERR_INVAL;
            }
            rc = translate_trigger_sequencer(
                &info->trigger_sequencer_actions[cur.trigger_sequencer++], b);
            break;
        }
        case ProtoHEFAction_wait_for_seqeuncer_tag: {
            while (cur.wait_sequencer < info->wait_sequencer_count &&
                   info->wait_sequencer_actions[cur.wait_sequencer].context_index != target_ctx) {
                cur.wait_sequencer++;
            }
            if (cur.wait_sequencer >= info->wait_sequencer_count) {
                WARN("hailo translator: action_types[%u]=wait_for_seqeuncer but "
                     "per-kind array exhausted", i);
                return HAILO_ERR_INVAL;
            }
            rc = translate_wait_sequencer(
                &info->wait_sequencer_actions[cur.wait_sequencer++], b);
            break;
        }
        case ProtoHEFAction_allow_input_dataflow_tag: {
            while (cur.allow_input_dataflow < info->allow_input_dataflow_count &&
                   info->allow_input_dataflow_actions[cur.allow_input_dataflow].context_index != target_ctx) {
                cur.allow_input_dataflow++;
            }
            if (cur.allow_input_dataflow >= info->allow_input_dataflow_count) {
                WARN("hailo translator: action_types[%u]=allow_input_dataflow "
                     "but per-kind array exhausted", i);
                return HAILO_ERR_INVAL;
            }
            rc = translate_allow_input_dataflow(
                &info->allow_input_dataflow_actions[cur.allow_input_dataflow++],
                info, cfg, b);
            break;
        }
        default:
            /* The parser recorded this tag but no translator exists for
             * it yet. Emitting the stream with this action omitted
             * would produce a wire-valid but semantically wrong
             * sequence — firmware would execute N-1 actions without
             * detecting the gap. Fail loudly so the missing translator
             * is prioritized. */
            WARN("hailo translator: unsupported action tag %u at "
                 "action_types[%u] — stream would be incomplete", tag, i);
            return HAILO_ERR_INVAL;
        }

        if (rc != HAILO_OK) return rc;
    }

    /* #253: HailoRT v4.23 emits BURST_CREDITS_TASK_START as the
     * penultimate action in DYNAMIC (right before APPLICATION_CHANGE_
     * INTERRUPT). We already emit it in BATCH_SWITCHING for the batch,
     * but firmware also expects it per-DYNAMIC to re-arm the burst
     * credit task for this context's boundary channels. Gated on
     * having both boundary pads — burst credits are meaningless
     * without boundary I/O, and the tests that exercise only
     * compute actions would see spurious trailing bytes otherwise. */
    {
        const struct hef_pad_info *in_pad = NULL, *out_pad = NULL;
        if (find_boundary_pads(info, &in_pad, &out_pad) == 0) {
            int bc_rc = hailo_cs_builder_append(b,
                HAILO_CS_ACT_BURST_CREDITS_TASK_START, NULL, 0);
            if (bc_rc != HAILO_OK) return bc_rc;
        }
    }

    /* Tail marker. Firmware requires this as the last action of the
     * final dynamic context; it signals "this dynamic context is
     * complete, fire the application-change interrupt when the
     * action list finishes executing". */
    return hailo_cs_builder_append(b, HAILO_CS_ACT_APPLICATION_CHANGE_INTERRUPT,
                                   NULL, 0);
}

/* -------------------------------------------------------------------------- */
/* Public entry point                                                           */
/* -------------------------------------------------------------------------- */

/* Phase 8 stage tracker (defined in inference_device_hailo.c).
 * Inlined here so each sub-translator's entry gets a distinct code,
 * which lets `hailo stage` post-wedge tell activation vs dynamic
 * apart without needing per-step uart_printf. */
extern void cs_load_stage_set_raw(int stage);
#define TRANSLATE_STAGE(n)  cs_load_stage_set_raw(n)

int hailo_cs_translate_contexts(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_context_buffers *out)
{
    if (!info || !cfg || !out) return HAILO_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    struct hailo_cs_builder b;

    /* ACTIVATION */
    TRANSLATE_STAGE(410);
    hailo_cs_builder_init(&b, out->activation, sizeof(out->activation));
    int rc = translate_activation(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->activation_len = hailo_cs_builder_size(&b);
    TRANSLATE_STAGE(411);

    /* BATCH_SWITCHING */
    TRANSLATE_STAGE(412);
    hailo_cs_builder_init(&b, out->batch_switching, sizeof(out->batch_switching));
    rc = translate_batch_switching(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->batch_switching_len = hailo_cs_builder_size(&b);
    TRANSLATE_STAGE(413);

    /* PRELIMINARY */
    TRANSLATE_STAGE(414);
    hailo_cs_builder_init(&b, out->preliminary, sizeof(out->preliminary));
    rc = translate_preliminary(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->preliminary_len = hailo_cs_builder_size(&b);
    TRANSLATE_STAGE(415);

    /* DYNAMIC */
    /* Phase 8 diagnostic: stage after translate_dynamic encodes BOTH
     * the return code and the context_actions_count so a single
     * `hailo stage` post-wedge reveals everything. Layout:
     *   value >= 500_000  → (value - 500000) = context count × 1 +
     *                       500000 * (0 if err -1, 1 if ok)
     * Concretely:
     *   500000 + count       → translate_dynamic returned OK
     *   600000 + count       → translate_dynamic returned ERR_INVAL (-1)
     *   700000 + count       → translate_dynamic returned other rc
     */
    cs_load_stage_set_raw(416);
    hailo_cs_builder_init(&b, out->dynamic, sizeof(out->dynamic));
    uint32_t ctx_count = info->context_actions_count;
    rc = translate_dynamic(info, cfg, &b);
    if (rc == HAILO_OK) {
        cs_load_stage_set_raw(500000 + (int)ctx_count);
    } else if (rc == HAILO_ERR_INVAL) {
        cs_load_stage_set_raw(600000 + (int)ctx_count);
    } else {
        cs_load_stage_set_raw(700000 + (int)ctx_count);
    }
    if (rc != HAILO_OK) return rc;
    out->dynamic_len = hailo_cs_builder_size(&b);
    TRANSLATE_STAGE(417);

    return HAILO_OK;
}
