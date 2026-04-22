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
        /* Two distinct quantities:
         *   bpb   = core_bytes_per_buffer — single periph-buffer size
         *           (one 224-pixel row of 224×3 INT8 in our test HEF = 672)
         *   frame = bpb * core_buffers_per_frame — full tensor frame
         *           (224 rows × 672 = 150528 for a 224×224×3 input)
         *
         * Firmware cross-checks:
         *   - bytes_in_pattern  == frame_periph_size      (transfer size)
         *   - periph_bytes_per_buffer * frame/bytes ratio  matches desc list
         *
         * Earlier build collapsed these to one value (frame = bpb) which
         * worked for single-row test HEFs but failed the firmware check
         * on real multi-row tensors with
         * HAILO_DATAFLOW_STATUS_INVALID_RECEIVE_COMMUNICATION. */
        uint32_t bpb   = pad->core_bytes_per_buffer;
        uint32_t bpf   = pad->core_buffers_per_frame
                           ? pad->core_buffers_per_frame : 1u;
        uint32_t frame = bpb * bpf;
        struct hailo_cs_act_open_boundary_input_channel body = {
            .packed_vdma_channel_id = packed_vdma,
            .host_buffer_info = {
                .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
                .dma_address      = cfg->boundary_input_desc_list_iova,
                .desc_page_size   = cfg->boundary_desc_page_size,
                .total_desc_count = cfg->boundary_input_total_desc_count,
                .bytes_in_pattern = frame,
            },
            .stream_index             = stream_index,
            .network_index            = 0,
            .periph_bytes_per_buffer  = (uint16_t)((bpb > 0xFFFFu) ? 0xFFFFu : bpb),
            .frame_periph_size        = frame,
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
        uint32_t obpf = pad->core_buffers_per_frame
                          ? pad->core_buffers_per_frame : 1u;
        uint32_t out_frame = pad->core_bytes_per_buffer * obpf;
        struct hailo_cs_act_open_boundary_output_channel body = {
            .packed_vdma_channel_id = packed_vdma,
            .host_buffer_info = {
                .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
                .dma_address      = cfg->boundary_output_desc_list_iova,
                .desc_page_size   = cfg->boundary_desc_page_size,
                .total_desc_count = cfg->boundary_output_total_desc_count,
                /* See note on the input body above — HailoRT derives
                 * this from the output pad's transfer_size. */
                .bytes_in_pattern = out_frame,
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
    uint8_t stream_index = 0;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *pad = &info->pads[i];
        if (!pad->has_stream_info) continue;
        if (pad->is_input != emit_inputs) continue;
        int rc = translate_open_boundary_for_pad(pad, cfg,
                                                 stream_index, b);
        if (rc != HAILO_OK) return rc;
        stream_index++;
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
static int translate_batch_switching(const struct hef_info *info,
                                     const struct hailo_cs_translate_cfg *cfg,
                                     struct hailo_cs_builder *b)
{
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
static int translate_preliminary(const struct hef_info *info,
                                 const struct hailo_cs_translate_cfg *cfg,
                                 struct hailo_cs_builder *b)
{
    struct hailo_cs_act_activate_cfg_channel act = {
        .packed_vdma_channel_id = cfg->config_vdma_channel,
        .config_stream_index    = cfg->config_stream_index,
        .host_buffer_info = {
            .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
            .dma_address      = cfg->ccw_desc_list_iova,
            .desc_page_size   = cfg->ccw_desc_page_size,
            .total_desc_count = cfg->ccw_total_desc_count,
            .bytes_in_pattern = 0,
        },
    };
    int rc = hailo_cs_builder_append(b, HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL,
                                     &act, sizeof(act));
    if (rc != HAILO_OK) return rc;

    /* One FETCH_CFG_CHANNEL_DESCRIPTORS sub-action wrapped in
     * REPEATED_ACTION. HailoRT v4.23 uses this (not FETCH_CCW_BURSTS)
     * on Hailo-8L because support_pre_fetch=false on that device —
     * wire capture against mobilenet_v1 shows `18 ff ff ff ff NN 00
     * 00` (sub_action_type=0x00 = FETCH_CFG_CHANNEL_DESCRIPTORS)
     * rather than sub_action_type=0x1b (FETCH_CCW_BURSTS). The
     * sub-body carries {descriptors_count, packed_vdma_channel_id}. */
    uint32_t descs = cfg->ccw_total_desc_count;
    if (descs == 0) descs = 1;               /* firmware rejects 0 */
    if (descs > UINT16_MAX) descs = UINT16_MAX;
    (void)info;

    struct hailo_cs_act_fetch_cfg_channel_descriptors sub = {
        .descriptors_count      = (uint16_t)descs,
        .packed_vdma_channel_id = cfg->config_vdma_channel,
    };
    return hailo_cs_builder_append_repeated(
        b, HAILO_CS_ACT_FETCH_CFG_CHANNEL_DESCRIPTORS,
        /*count=*/1, &sub, sizeof(sub));
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
     * #253 fix: frame_periph_size here must match the value
     * OpenBoundaryInputChannel declared in ACTIVATION — i.e. the
     * FULL periph frame (bpb * bpf), not just bpb. Firmware
     * cross-checks the two and silently wedges the inference
     * pipeline (device-side avail stays 0) when they disagree.
     * For our MNIST HEF: bpb=32, bpf=28, frame=896. */
    uint32_t frame_size = 0;
    bool pad_found = false;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        if (info->pads[i].sys_index == a->sys_index) {
            uint32_t bpb = info->pads[i].core_bytes_per_buffer;
            uint32_t bpf = info->pads[i].core_buffers_per_frame
                              ? info->pads[i].core_buffers_per_frame : 1u;
            frame_size = bpb * bpf;
            pad_found = true;
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

    struct hailo_cs_act_fetch_data_from_vdma body = {
        .packed_vdma_channel_id = packed_vdma,
        .stream_index           = 0,
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

/* Fill a stream_reg_info from pad geometry. Matches HailoRT v4.23
 * byte layout exactly (see pios_DYNAMIC.bin for MNIST reference). */
static void fill_stream_reg_info_from_pad(const struct hef_pad_info *p,
                                          struct hailo_cs_stream_reg_info *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t bpb = p->core_bytes_per_buffer;
    uint32_t bpf = p->core_buffers_per_frame ? p->core_buffers_per_frame : 1u;
    out->core_bytes_per_buffer    = (uint16_t)bpb;
    out->core_buffers_per_frame   = (uint16_t)bpf;
    /* periph fields: HailoRT's v4.23 wire capture for MNIST input shows
     * periph_bytes_per_buffer = 784 = HEF tensor h*w (28*28), not bpb.
     * For output the periph side matched bpb (16). Use tensor total as
     * a best approximation — if periph differs from core the HEF's
     * stream_info would expose it but we don't extract those fields. */
    uint32_t tensor_total = p->has_tensor_shape
        ? (uint32_t)p->height * p->width * p->features : bpb * bpf;
    out->periph_bytes_per_buffer  = (uint16_t)(tensor_total / bpf);
    out->periph_buffers_per_frame = 1u;
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

    uint32_t in_bpb  = in_pad->core_bytes_per_buffer;
    uint32_t in_bpf  = in_pad->core_buffers_per_frame
                           ? in_pad->core_buffers_per_frame : 1u;
    uint32_t in_frame  = in_bpb * in_bpf;
    uint32_t out_bpb = out_pad->core_bytes_per_buffer;
    uint32_t out_bpf = out_pad->core_buffers_per_frame
                           ? out_pad->core_buffers_per_frame : 1u;
    uint32_t out_frame = out_bpb * out_bpf;

    /* 1. ACTIVATE_BOUNDARY_OUTPUT (v4.23 emits output first). */
    struct hailo_cs_act_activate_boundary_output out_body;
    memset(&out_body, 0, sizeof(out_body));
    out_body.packed_vdma_channel_id = out_ch;
    out_body.stream_index           = (uint8_t)out_pad->sys_index;
    out_body.network_index          = 0;
    fill_stream_reg_info_from_pad(out_pad, &out_body.stream_reg_info);
    fill_host_buffer_info(cfg->boundary_output_desc_list_iova,
                          cfg->boundary_desc_page_size,
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
     * credit task for this context's boundary channels. Safe to emit
     * unconditionally — empty body, no state pollution if burst
     * credits are already running. */
    int bc_rc = hailo_cs_builder_append(b, HAILO_CS_ACT_BURST_CREDITS_TASK_START,
                                        NULL, 0);
    if (bc_rc != HAILO_OK) return bc_rc;

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
