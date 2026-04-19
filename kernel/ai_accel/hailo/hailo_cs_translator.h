/*
 * hailo_cs_translator.h — HEF → context-switch wire-action translator.
 *
 * Consumes a parsed `hef_info` (from hef_parser.c) and emits the four
 * per-context action byte streams that `hailo_control_set_context_info`
 * then ships to firmware. This is the integration point that turns a
 * static HEF blob into the dynamic RPC sequence firmware expects.
 *
 * Firmware-enforced constraints this module satisfies (see
 * docs/pi5-ai-hat-plan.md §6.3 and the memory notes for full decodes):
 *
 *   1. The 4-context sequence ACTIVATION → BATCH_SWITCHING →
 *      PRELIMINARY → DYNAMIC must be emitted in that exact order.
 *      Firmware returns 0x4013006e (UNEXPECTED_CONTEXT_ORDER) if
 *      anything else is sent first.
 *
 *   2. Each context must contain ≥1 well-formed action. Zero-length
 *      contexts return 0x40130004; action walker misalignment returns
 *      0x40130016.
 *
 *   3. Per-type minimum actions per context (per HailoRT's
 *      fill_*_context_recipes functions, v4.23 source):
 *        ACTIVATION:      BURST_CREDITS_TASK_RESET (zero body)
 *        BATCH_SWITCHING: DDR_BUFFERING_RESET + BURST_CREDITS_TASK_START
 *        PRELIMINARY:     ACTIVATE_CFG_CHANNEL + FETCH_CCW_BURSTS per
 *                         distinct config channel
 *        DYNAMIC:         APPLICATION_CHANGE_INTERRUPT tail marker
 *                         (for single-dynamic-context loads)
 *
 *   4. Common action header is **8 bytes** on the wire despite the
 *      reference header's #pragma pack(1). Handled transparently by
 *      hailo_cs_builder.
 *
 * Current scope (Phase 6.4f minimum viable):
 *  - ACTIVATION, BATCH_SWITCHING: fixed stub sequences, no HEF input.
 *  - PRELIMINARY: derived from hef_info.ccw_action_count; one
 *    ACTIVATE_CFG_CHANNEL per config channel, one FETCH_CCW_BURSTS
 *    sized from ccw_action_count.
 *  - DYNAMIC: minimum tail-only marker. The full
 *    operations[].actions[] translation lives behind a follow-up;
 *    the current stub is enough to pass firmware's context-present
 *    check (empty DYNAMIC is rejected; one action is accepted).
 *
 * Per-HEF-action translation (EnableLcu → ENABLE_LCU_DEFAULT,
 * TriggerSequencer → TRIGGER_SEQUENCER with sequencer_config body,
 * etc.) requires per-action parameter extraction in hef_parser and
 * will land incrementally.
 */
#ifndef AI_ACCEL_HAILO_CS_TRANSLATOR_H
#define AI_ACCEL_HAILO_CS_TRANSLATOR_H

#include <stddef.h>
#include <stdint.h>

#include "hailo.h"
#include "hailo_control.h"
#include "hailo_cs_actions.h"
#include "hef_parser.h"

/*
 * Runtime configuration the translator needs that isn't in the HEF
 * itself — host-chosen VDMA channel assignments, the PCIe bus
 * address of the CCW DMA buffer the caller has already allocated
 * and populated, and similar "wiring" info.
 *
 * The caller (inference_device_hailo::load_model) is responsible
 * for allocating the CCW DMA buffer via hailo_vdma_desc_list_alloc
 * + hailo_vdma_program_buffer before calling the translator.
 */

/* Boundary VDMA channel offsets relative to config_vdma_channel.
 * The HEF identifies boundary streams by sys_index; the host picks
 * which VDMA channel each sys_index maps to. Today the convention
 * is fixed: input boundary on (config + 1), output boundary on
 * (config + 2). When #178 lands real edge_layer → channel mapping,
 * these become defaults with explicit cfg override. ACTIVATION's
 * OpenBoundaryInput/Output emitter (future) and translate_allow_
 * input_dataflow (current) both use these so the two ends agree. */
#define HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET   1u
#define HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET  2u

struct hailo_cs_translate_cfg {
    /* packed_vdma_channel_id for the config stream (the channel the
     * firmware DMA-pulls CCW payloads through). Typical choice on
     * Hailo-8 with 1 engine is channel 1 (packed_id = 0x01). */
    uint8_t config_vdma_channel;

    /* config_stream_index matching the HEF's cfg_channel_index.
     * For single-config-channel loads this is 0. */
    uint8_t config_stream_index;

    /* PCIe bus address (IOVA) of the CCW VDMA descriptor list the
     * host has programmed. Goes straight into
     * host_buffer_info.dma_address of the ACTIVATE_CFG_CHANNEL body. */
    uint64_t ccw_desc_list_iova;

    /* Descriptor page size of the CCW buffer, in bytes. Must match
     * the VDMA list's desc_page_size. Firmware also validates this
     * against csm_buffer_size in the network group header. */
    uint16_t ccw_desc_page_size;

    /* Number of descriptors in the CCW list. */
    uint32_t ccw_total_desc_count;

    /* ------------------------------------------------------------ *
     * Boundary-channel descriptor lists (one for input, one for
     * output). Populated by the caller (inference_device_hailo::
     * load_model) with host-allocated VDMA descriptor lists, one
     * per direction. The translator emits OPEN_BOUNDARY_INPUT_CHANNEL
     * (for each boundary pad with is_input=true) and
     * OPEN_BOUNDARY_OUTPUT_CHANNEL (is_input=false) in ACTIVATION,
     * binding the descriptor lists to the VDMA channels firmware
     * uses for the dataflow.
     *
     * Single-input / single-output MVP: only one of each direction
     * is supported. Multi-stream HEFs will need this to become a
     * small array indexed by stream_index.
     *
     * Zero-IOVA is treated as "no boundary of this direction in the
     * HEF" — the translator skips emission. This lets a HEF with
     * only an input boundary (or synthetic tests that skip the
     * boundary path) work without requiring fake values.
     * ------------------------------------------------------------ */
    uint64_t boundary_input_desc_list_iova;
    uint32_t boundary_input_total_desc_count;

    uint64_t boundary_output_desc_list_iova;
    uint32_t boundary_output_total_desc_count;

    /* Boundary descriptor page size (shared by input + output today).
     * Must match the programmed VDMA descriptor list's page size. */
    uint16_t boundary_desc_page_size;
};

/*
 * Per-context output buffers. Each context's serialized action bytes
 * go into its own fixed-size buffer here. Buffer sizes are generous
 * enough for a simple-MLP load (the largest context is typically the
 * DYNAMIC compute context with a few dozen actions at 8-40 B each).
 *
 * Total footprint: ~2 KB. Callers typically declare one on the stack
 * of the load_model path (16 KB kernel stack has room).
 */
/* Per-context serialized action buffer cap. Sized for worst-case
 * ACTIVATION with HEF_PARSER_MAX_PADS (16) all as boundary inputs:
 * 16 × (8 header + 28 body) + 8 BURST_CREDITS = 584 B. 1024 leaves
 * comfortable headroom for future ACTIVATION additions. Four copies
 * × 1024 = 4 KB per hailo_cs_context_buffers — still stack-friendly. */
#define HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES  1024u

struct hailo_cs_context_buffers {
    uint8_t activation      [HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES];
    uint8_t batch_switching [HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES];
    uint8_t preliminary     [HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES];
    uint8_t dynamic         [HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES];
    size_t  activation_len;
    size_t  batch_switching_len;
    size_t  preliminary_len;
    size_t  dynamic_len;
};

/*
 * Produce the application header firmware expects in
 * SET_NETWORK_GROUP_HEADER. Derived from hef_info + cfg.
 *
 * On v4.23 firmware the wire size is 32 bytes (3 INFER bools +
 * 4 config channels); see hailo_control.c for the static_assert.
 *
 * Returns HAILO_OK, or HAILO_ERR_INVAL on null args.
 */
int hailo_cs_translate_application_header(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_application_header *out);

/*
 * Build the four per-context action byte streams.
 *
 * On success each out->*_len is set to the serialized length of
 * that context's action bytes (≤ HAILO_CS_TRANSLATE_MAX_CONTEXT_BYTES).
 * The caller then passes each to hailo_control_set_context_info in
 * the fixed ACTIVATION → BATCH_SWITCHING → PRELIMINARY → DYNAMIC
 * order firmware enforces.
 *
 * Returns HAILO_OK, HAILO_ERR_INVAL (null args), or HAILO_ERR_NOMEM
 * if any context's byte stream would exceed its buffer.
 */
int hailo_cs_translate_contexts(
    const struct hef_info *info,
    const struct hailo_cs_translate_cfg *cfg,
    struct hailo_cs_context_buffers *out);

#endif /* AI_ACCEL_HAILO_CS_TRANSLATOR_H */
