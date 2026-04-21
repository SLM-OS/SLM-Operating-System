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
 *      fill_*_context_recipes functions, v4.23 source, plus the
 *      hardware bisect from #180):
 *        ACTIVATION:      BURST_CREDITS_TASK_RESET (zero body) +
 *                         OpenBoundary OUT/IN per boundary edge
 *        BATCH_SWITCHING: DDR_BUFFERING_RESET + BURST_CREDITS_TASK_START
 *        PRELIMINARY:     ACTIVATE_CFG_CHANNEL per distinct config
 *                         channel. (HailoRT also wraps AddCcwBurst
 *                         sub-actions inside REPEATED_ACTION here on
 *                         Hailo-8L; that path is Phase 6.10. Direct
 *                         FETCH_CCW_BURSTS is rejected with
 *                         CONFIG_MANAGER_WRAPPER_STATUS_ACTION_TYPE_
 *                         NOT_SUPPORTED — see #180.)
 *        DYNAMIC:         APPLICATION_CHANGE_INTERRUPT tail marker
 *                         (for single-dynamic-context loads)
 *
 *   4. Common action header is **5 bytes** on the wire (1-byte
 *      action_type + 4-byte time_stamp, packed). time_stamp must be
 *      HAILO_CS_TIMESTAMP_INIT_VALUE (0xFFFFFFFF), not 0. Handled
 *      transparently by hailo_cs_builder.
 *
 * Current scope (post-#180 minimum viable):
 *  - ACTIVATION: BURST_CREDITS_TASK_RESET + OpenBoundary OUT before IN
 *    (HailoRT emission order).
 *  - BATCH_SWITCHING: fixed stub sequence + ChangeBoundaryInputBatch
 *    per input pad.
 *  - PRELIMINARY: ACTIVATE_CFG_CHANNEL only. CCW weight loading via
 *    REPEATED_ACTION + AddCcwBurst is Phase 6.10.
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
 * which VDMA channel each sys_index maps to. When #178 lands real
 * edge_layer → channel mapping, these become defaults with explicit
 * cfg override. ACTIVATION's OpenBoundaryInput/Output emitter and
 * translate_allow_input_dataflow both use these so the two ends agree.
 *
 * On Hailo-8 PCIe, firmware v4.23 enforces distinct H2D and D2H
 * channel-index ranges: H2D ∈ [0, 15], D2H ∈ [16, 31]. HailoRT's
 * ChannelAllocator::get_available_channel_id branches on DmaDirection
 * and picks from the appropriate half (see
 * hailort-v4.23.0-channel_allocator.cpp and -hailort_driver.hpp:
 * MIN_H2D_CHANNEL_INDEX=0 / MAX_H2D=15 / MIN_D2H=16 / MAX_D2H=31).
 * Sending a D2H OpenBoundary with a channel in [0,15] causes firmware
 * to reject ACTIVATION with VDMA_SERVICE_STATUS_INVALID_ENGINE_INDEX
 * (observed 2026-04-20 on pi-5-1) — the status code name is
 * misleading; the actual failure is "channel doesn't match direction".
 *
 * INPUT_OFFSET=1  → channel config+1   (H2D slot; with config=1 → 2)
 * OUTPUT_OFFSET=15 → channel config+15 (D2H slot; with config=1 → 16,
 *                    the first valid D2H index). */
#define HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET   1u
#define HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET  15u

/* Default config-stream VDMA channel. The MVP hardcodes this to 1
 * (first non-zero H2D slot) so a single load can use channel 1 for
 * config, channel 2 (= config+INPUT_OFFSET) for boundary input, and
 * channel 16 (= config+OUTPUT_OFFSET) for boundary output. Lives
 * here, not in inference_device_hailo.c, so the static_asserts
 * below reference the same value the inference path uses. */
#define HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL  0x01u

/* Compile-time guards on the offsets above. Firmware enforces
 *   H2D channels ∈ [HAILO_CS_PCIE_MIN_H2D, HAILO_CS_PCIE_MAX_H2D] = [0, 15]
 *   D2H channels ∈ [HAILO_CS_PCIE_MIN_D2H, HAILO_CS_PCIE_MAX_D2H] = [16, 31]
 * The asserts fire if anyone (a) bumps an offset past its
 * direction's range or (b) raises the default config channel
 * without updating both offsets. */
#define HAILO_CS_PCIE_MIN_H2D       0u
#define HAILO_CS_PCIE_MAX_H2D       15u
#define HAILO_CS_PCIE_MIN_D2H       16u
#define HAILO_CS_PCIE_MAX_D2H       31u

_Static_assert(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                   + HAILO_CS_BOUNDARY_INPUT_CHANNEL_OFFSET
               <= HAILO_CS_PCIE_MAX_H2D,
               "boundary INPUT channel must land in H2D range [0,15]");
_Static_assert(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                   + HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET
               >= HAILO_CS_PCIE_MIN_D2H,
               "boundary OUTPUT channel must land in D2H range [16,31]");
_Static_assert(HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL
                   + HAILO_CS_BOUNDARY_OUTPUT_CHANNEL_OFFSET
               <= HAILO_CS_PCIE_MAX_D2H,
               "boundary OUTPUT channel must not exceed D2H upper bound");

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
