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
 * reset — three actions total, 28+20+8 = 56 bytes of body plus three
 * 8-byte common headers = 80 bytes of ACTIVATION. Well under the
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
        /* frame_periph_size comes from the pad's core_bytes_per_buffer;
         * periph_bytes_per_buffer equals frame size for unpadded
         * single-row tensors (MVP). Multi-row / padded tensors need
         * a proper periph-vs-core split later. */
        uint32_t frame = pad->core_bytes_per_buffer;
        struct hailo_cs_act_open_boundary_input_channel body = {
            .packed_vdma_channel_id = packed_vdma,
            .host_buffer_info = {
                .buffer_type      = HAILO_CS_HOST_BUFFER_EXTERNAL_DESC,
                .dma_address      = cfg->boundary_input_desc_list_iova,
                .desc_page_size   = cfg->boundary_desc_page_size,
                .total_desc_count = cfg->boundary_input_total_desc_count,
                /* HailoRT sets bytes_in_pattern = transfer_size (periph
                 * frame size) for boundary channels — see
                 * vdma_edge_layer.cpp:73 in v4.23.0. For unpadded
                 * single-row tensors (MVP) this equals core_bytes_per_
                 * buffer; multi-row / padded tensors need the full
                 * periph_bytes_per_buffer * periph_buffers_per_frame
                 * product once the translator consumes that split. */
                .bytes_in_pattern = frame,
            },
            .stream_index             = stream_index,
            .network_index            = 0,
            .periph_bytes_per_buffer  = (uint16_t)((frame > 0xFFFFu) ? 0xFFFFu : frame),
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
        uint32_t out_frame = pad->core_bytes_per_buffer;
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

static int translate_activation(const struct hef_info *info,
                                const struct hailo_cs_translate_cfg *cfg,
                                struct hailo_cs_builder *b)
{
    /* Step 1: BURST_CREDITS_TASK_RESET. */
    int rc = hailo_cs_builder_append(b,
                 HAILO_CS_ACT_BURST_CREDITS_TASK_RESET, NULL, 0);
    if (rc != HAILO_OK) return rc;

    /* Steps 2 & 3: walk pads, emit OpenBoundary per boundary edge.
     * HailoRT v4.23 emits OUTPUT actions before INPUT actions in
     * ACTIVATION (resource_manager_builder.cpp:1059-1075 iterates
     * get_output_layer_infos before get_input_layer_infos). Firmware
     * may rely on this ordering — the memory note on
     * INVALID_ENGINE_INDEX lists emission order as a likely cause.
     * Walk outputs first, then inputs, to match HailoRT byte-for-byte.
     *
     * stream_index counts boundary pads per direction. */
    uint8_t input_stream_index  = 0;
    uint8_t output_stream_index = 0;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *pad = &info->pads[i];
        if (!pad->has_stream_info) continue;
        if (pad->is_input) continue;  /* outputs first pass */
        rc = translate_open_boundary_for_pad(pad, cfg,
                                             output_stream_index, b);
        if (rc != HAILO_OK) return rc;
        output_stream_index++;
    }
    for (uint32_t i = 0; i < info->pad_count; i++) {
        const struct hef_pad_info *pad = &info->pads[i];
        if (!pad->has_stream_info) continue;
        if (!pad->is_input) continue;  /* inputs second pass */
        rc = translate_open_boundary_for_pad(pad, cfg,
                                             input_stream_index, b);
        if (rc != HAILO_OK) return rc;
        input_stream_index++;
    }

    return HAILO_OK;
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
 * firmware will DMA-pull CCW payloads through. Followed by one
 * FETCH_CCW_BURSTS that tells firmware how many bursts to pull.
 *
 * For MVP we emit one FETCH_CCW_BURSTS per CCW action captured by
 * the parser; the compiler's chosen burst granularity may differ,
 * but this matches HailoRT's "one burst per write_data_ccw action"
 * convention for simple MLPs. Future multi-burst handling (e.g.
 * repeated-action compression) lives behind a follow-up.
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

    /* If the HEF has CCW actions, emit a FETCH_CCW_BURSTS sized to
     * the count. For a zero-CCW HEF (unusual; some synthetic tests)
     * we still emit a single fetch with count=1 — firmware accepts
     * this, and the subsequent DYNAMIC path tolerates "no weights
     * were actually transferred". */
    uint32_t bursts = info->ccw_action_count;
    if (bursts == 0) bursts = 1;
    if (bursts > UINT16_MAX) bursts = UINT16_MAX;

    struct hailo_cs_act_fetch_ccw_bursts fetch = {
        .ccw_bursts          = (uint16_t)bursts,
        .config_stream_index = cfg->config_stream_index,
    };
    return hailo_cs_builder_append(b, HAILO_CS_ACT_FETCH_CCW_BURSTS,
                                   &fetch, sizeof(fetch));
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
     * the caller surface the missing pad. */
    uint32_t frame_size = 0;
    bool pad_found = false;
    for (uint32_t i = 0; i < info->pad_count; i++) {
        if (info->pads[i].sys_index == a->sys_index) {
            frame_size = info->pads[i].core_bytes_per_buffer;
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

static int translate_dynamic(const struct hef_info *info,
                             const struct hailo_cs_translate_cfg *cfg,
                             struct hailo_cs_builder *b)
{
    const uint32_t target_ctx = 0;
    struct dynamic_cursors cur;
    memset(&cur, 0, sizeof(cur));

    /* If the HEF captured zero contexts, fall through to the tail-
     * only path. Otherwise walk context_actions[0].action_types[]. */
    if (info->context_actions_count == 0) {
        return hailo_cs_builder_append(
            b, HAILO_CS_ACT_APPLICATION_CHANGE_INTERRUPT, NULL, 0);
    }

    /* Multi-context HEFs are not yet supported — we'd silently drop
     * contexts 1..N if we proceeded. Fail loudly instead so the
     * missing dispatch is visible rather than producing a
     * structurally-valid-but-semantically-wrong stream. */
    if (info->context_actions_count > 1) {
        WARN("hailo translator: dynamic_contexts_count=%u but only "
             "ctx0 is currently supported — refusing to translate",
             info->context_actions_count);
        return HAILO_ERR_INVAL;
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

int hailo_cs_translate_contexts(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_context_buffers *out)
{
    if (!info || !cfg || !out) return HAILO_ERR_INVAL;

    memset(out, 0, sizeof(*out));

    struct hailo_cs_builder b;

    /* ACTIVATION */
    hailo_cs_builder_init(&b, out->activation, sizeof(out->activation));
    int rc = translate_activation(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->activation_len = hailo_cs_builder_size(&b);

    /* BATCH_SWITCHING */
    hailo_cs_builder_init(&b, out->batch_switching, sizeof(out->batch_switching));
    rc = translate_batch_switching(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->batch_switching_len = hailo_cs_builder_size(&b);

    /* PRELIMINARY */
    hailo_cs_builder_init(&b, out->preliminary, sizeof(out->preliminary));
    rc = translate_preliminary(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->preliminary_len = hailo_cs_builder_size(&b);

    /* DYNAMIC */
    hailo_cs_builder_init(&b, out->dynamic, sizeof(out->dynamic));
    rc = translate_dynamic(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->dynamic_len = hailo_cs_builder_size(&b);

    return HAILO_OK;
}
