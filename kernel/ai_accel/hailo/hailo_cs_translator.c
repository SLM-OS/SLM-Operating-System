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

    /* boundary_channels_bitmap: bit per VDMA channel used for input
     * or output dataflow. For single-CCW loads (no boundary I/O
     * yet) only the config channel counts. Engine 0 is index 0
     * of the bitmap array; packed_vdma_channel_id's low nibble is
     * the channel number. */
    uint8_t chan = cfg->config_vdma_channel & 0x0Fu;
    out->boundary_channels_bitmap[0] = 1u << chan;

    (void)info;   /* Not yet consumed — reserved for future expansion. */
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* ACTIVATION context                                                           */
/* -------------------------------------------------------------------------- */

/* Minimum legal ACTIVATION per HailoRT's fill_activation_config_recepies:
 * BURST_CREDITS_TASK_RESET (zero body). Real MLP ACTIVATION contexts
 * also carry OpenBoundaryInput/Output per boundary edge layer; those
 * land once the parser surfaces edge_layer → VDMA channel mapping. */
static int translate_activation(struct hailo_cs_builder *b)
{
    return hailo_cs_builder_append(b, HAILO_CS_ACT_BURST_CREDITS_TASK_RESET,
                                   NULL, 0);
}

/* -------------------------------------------------------------------------- */
/* BATCH_SWITCHING context                                                      */
/* -------------------------------------------------------------------------- */

/* Minimum BATCH_SWITCHING per HailoRT's fill_batch_switching_context:
 * DDR_BUFFERING_RESET + BURST_CREDITS_TASK_START (both zero body).
 * Real MLP BATCH_SWITCHING contexts with DDR buffers or LCU batch
 * switching add actions here; zero-boundary single-context MLPs
 * don't need them. */
static int translate_batch_switching(struct hailo_cs_builder *b)
{
    int rc = hailo_cs_builder_append(b, HAILO_CS_ACT_DDR_BUFFERING_RESET,
                                     NULL, 0);
    if (rc != HAILO_OK) return rc;
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

static int translate_dynamic(const struct hef_info *info,
                             struct hailo_cs_builder *b)
{
    /* Emit captured EnableLcu actions that belong to the (only)
     * dynamic context we currently support — context_index == 0.
     * Entries tagged with any other context_index are skipped so
     * a future multi-dynamic-context HEF (dynamic_contexts_count > 1)
     * doesn't silently splat context-1+ actions into context 0's
     * byte stream. Those will need per-context-index dispatch when
     * multi-context translation lands. */
    uint32_t scanned = (info->enable_lcu_count > HEF_PARSER_MAX_ENABLE_LCU_ACTIONS)
                          ? HEF_PARSER_MAX_ENABLE_LCU_ACTIONS
                          : info->enable_lcu_count;
    for (uint32_t i = 0; i < scanned; i++) {
        if (info->enable_lcu_actions[i].context_index != 0) continue;
        int rc = translate_enable_lcu(&info->enable_lcu_actions[i], b);
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
    int rc = translate_activation(&b);
    if (rc != HAILO_OK) return rc;
    out->activation_len = hailo_cs_builder_size(&b);

    /* BATCH_SWITCHING */
    hailo_cs_builder_init(&b, out->batch_switching, sizeof(out->batch_switching));
    rc = translate_batch_switching(&b);
    if (rc != HAILO_OK) return rc;
    out->batch_switching_len = hailo_cs_builder_size(&b);

    /* PRELIMINARY */
    hailo_cs_builder_init(&b, out->preliminary, sizeof(out->preliminary));
    rc = translate_preliminary(info, cfg, &b);
    if (rc != HAILO_OK) return rc;
    out->preliminary_len = hailo_cs_builder_size(&b);

    /* DYNAMIC */
    hailo_cs_builder_init(&b, out->dynamic, sizeof(out->dynamic));
    rc = translate_dynamic(info, &b);
    if (rc != HAILO_OK) return rc;
    out->dynamic_len = hailo_cs_builder_size(&b);

    return HAILO_OK;
}
