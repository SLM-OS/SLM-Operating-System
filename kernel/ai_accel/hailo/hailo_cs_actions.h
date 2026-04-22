/*
 * hailo_cs_actions.h — wire-format context-switch action structs.
 *
 * These mirror the CONTEXT_SWITCH_DEFS__* typedefs in hailort's
 * firmware-facing header at docs/reference/hailort-context_switch_defs.h.
 * Firmware parses the context_network_data blob of a SET_CONTEXT_INFO
 * RPC as a concatenation of [common_action_header_t][per-type body]
 * tuples; each action_type encodes which body type follows.
 *
 * Packing: HailoRT uses #pragma pack(push, 1) for the whole region.
 * We use __attribute__((packed)) per struct to match. Every struct
 * has an explicit _Static_assert on its size so a missed field or
 * unexpected padding breaks the build rather than producing a
 * silently-malformed action.
 *
 * Subset shipped in Phase 6.4: the actions needed for a simple MLP
 * inference — CCW upload (preliminary context) + compute (dynamic
 * context). Additional action types (NMS, DDR-buffer, cache, etc.)
 * land as the feature set grows.
 */
#ifndef AI_ACCEL_HAILO_CS_ACTIONS_H
#define AI_ACCEL_HAILO_CS_ACTIONS_H

#include <stdbool.h>
#include <stdint.h>

/* Hailo firmware v4.23 expects wire scalars in native little-endian
 * (confirmed cross-checking hailort's pack/unpack macros — no
 * byteswap on the data-plane RPC path). These struct layouts rely on
 * host endianness matching, so reject a BE-host build loudly rather
 * than producing silent field corruption on the wire. */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "Hailo context-switch wire format assumes LE host");

/* Mirrors CONTEXT_SWITCH_DEFS__ACTION_TYPE_t values from v4.23 firmware.
 * Positions in the enum ARE the wire values — do not reorder. */
enum hailo_cs_action_type {
    HAILO_CS_ACT_FETCH_CFG_CHANNEL_DESCRIPTORS                 = 0,
    HAILO_CS_ACT_TRIGGER_SEQUENCER                             = 1,
    HAILO_CS_ACT_FETCH_DATA_FROM_VDMA_CHANNEL                  = 2,
    HAILO_CS_ACT_ENABLE_LCU_DEFAULT                            = 3,
    HAILO_CS_ACT_ENABLE_LCU_NON_DEFAULT                        = 4,
    HAILO_CS_ACT_DISABLE_LCU                                   = 5,
    HAILO_CS_ACT_ACTIVATE_BOUNDARY_INPUT                       = 6,
    HAILO_CS_ACT_ACTIVATE_BOUNDARY_OUTPUT                      = 7,
    HAILO_CS_ACT_ACTIVATE_INTER_CONTEXT_INPUT                  = 8,
    HAILO_CS_ACT_ACTIVATE_INTER_CONTEXT_OUTPUT                 = 9,
    HAILO_CS_ACT_ACTIVATE_DDR_BUFFER_INPUT                     = 10,
    HAILO_CS_ACT_ACTIVATE_DDR_BUFFER_OUTPUT                    = 11,
    HAILO_CS_ACT_DEACTIVATE_VDMA_CHANNEL                       = 12,
    HAILO_CS_ACT_CHANGE_VDMA_TO_STREAM_MAPPING                 = 13,
    HAILO_CS_ACT_ADD_DDR_PAIR_INFO                             = 14,
    HAILO_CS_ACT_DDR_BUFFERING_START                           = 15,
    HAILO_CS_ACT_LCU_INTERRUPT                                 = 16,
    HAILO_CS_ACT_SEQUENCER_DONE_INTERRUPT                      = 17,
    HAILO_CS_ACT_INPUT_CHANNEL_TRANSFER_DONE_INTERRUPT         = 18,
    HAILO_CS_ACT_OUTPUT_CHANNEL_TRANSFER_DONE_INTERRUPT        = 19,
    HAILO_CS_ACT_MODULE_CONFIG_DONE_INTERRUPT                  = 20,
    HAILO_CS_ACT_APPLICATION_CHANGE_INTERRUPT                  = 21,
    HAILO_CS_ACT_ACTIVATE_CFG_CHANNEL                          = 22,
    HAILO_CS_ACT_DEACTIVATE_CFG_CHANNEL                        = 23,
    HAILO_CS_ACT_REPEATED_ACTION                               = 24,
    HAILO_CS_ACT_WAIT_FOR_DMA_IDLE_ACTION                      = 25,
    HAILO_CS_ACT_WAIT_FOR_NMS                                  = 26,
    HAILO_CS_ACT_FETCH_CCW_BURSTS                              = 27,
    HAILO_CS_ACT_VALIDATE_VDMA_CHANNEL                         = 28,
    HAILO_CS_ACT_BURST_CREDITS_TASK_START                      = 29,
    HAILO_CS_ACT_BURST_CREDITS_TASK_RESET                      = 30,
    HAILO_CS_ACT_DDR_BUFFERING_RESET                           = 31,
    HAILO_CS_ACT_OPEN_BOUNDARY_INPUT_CHANNEL                   = 32,
    HAILO_CS_ACT_OPEN_BOUNDARY_OUTPUT_CHANNEL                  = 33,
    HAILO_CS_ACT_ENABLE_NMS                                    = 34,
    HAILO_CS_ACT_WRITE_DATA_BY_TYPE                            = 35,
    HAILO_CS_ACT_SWITCH_LCU_BATCH                              = 36,
    HAILO_CS_ACT_CHANGE_BOUNDARY_INPUT_BATCH                   = 37,
    HAILO_CS_ACT_PAUSE_VDMA_CHANNEL                            = 38,
    HAILO_CS_ACT_RESUME_VDMA_CHANNEL                           = 39,
    HAILO_CS_ACT_ACTIVATE_CACHE_INPUT                          = 40,
    HAILO_CS_ACT_ACTIVATE_CACHE_OUTPUT                         = 41,
    HAILO_CS_ACT_WAIT_FOR_CACHE_UPDATED                        = 42,
    HAILO_CS_ACT_SLEEP                                         = 43,
    HAILO_CS_ACT_HALT                                          = 44,
};

/* Host-buffer type values for host_buffer_info_t.buffer_type.
 * Mirrors CONTROL_PROTOCOL__HOST_BUFFER_TYPE_t. */
enum hailo_cs_host_buffer_type {
    HAILO_CS_HOST_BUFFER_EXTERNAL_DESC                 = 0,
    HAILO_CS_HOST_BUFFER_CCB                           = 1,
    HAILO_CS_HOST_BUFFER_HOST_MANAGED_EXTERNAL_DESC    = 2,  /* DEPRECATED */
};

/* Edge layer direction values for wire action fields (not the proto
 * one in hef.proto — the firmware-facing one). */
enum hailo_cs_edge_direction {
    HAILO_CS_DIR_UNINITIALIZED   = 0,
    HAILO_CS_DIR_HOST_TO_DEVICE  = 1,
    HAILO_CS_DIR_DEVICE_TO_HOST  = 2,
};

/* Common header that precedes every action body on the wire.
 *
 * IMPORTANT: 5 bytes total. action_type (u8) + time_stamp (u32 LE)
 * with NO padding. The host struct must be __attribute__((packed)).
 *
 * A prior session's memory note claimed this was 8 bytes (with 3
 * pad bytes for natural alignment of time_stamp). That was wrong —
 * confirmed by capturing HailoRT v4.23 wire bytes on pi-5-1 against
 * a real Hailo-8L Model Zoo HEF (mobilenet_v1) on 2026-04-20.
 * HailoRT emits BURST_CREDITS_TASK_RESET as exactly 5 bytes
 * (1e ff ff ff ff) before the next action.
 *
 * time_stamp is set to CONTEXT_SWITCH_DEFS__TIMESTAMP_INIT_VALUE
 * (0xFFFFFFFF), not 0. Setting 0 may have firmware interpret it as
 * a real timestamp and reject the action stream as out-of-order.
 *
 * Layout on the wire:
 *   [0]       action_type (u8)
 *   [1..4]    time_stamp (u32 LE; 0xFFFFFFFF = INIT)
 */
struct hailo_cs_common_action_header {
    uint8_t  action_type;
    uint32_t time_stamp;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_common_action_header) == 5,
               "common_action_header must be 5 bytes on the wire");

#define HAILO_CS_TIMESTAMP_INIT_VALUE 0xFFFFFFFFu

/* CONTROL_PROTOCOL__host_buffer_info_t. Embedded inside ACTIVATE_*
 * actions so firmware can DMA-pull data from our host-side
 * descriptor list. dma_address is the PCIe bus address (IOVA);
 * desc_page_size must match the descriptor list's page size. */
struct hailo_cs_host_buffer_info {
    uint8_t  buffer_type;          /* enum hailo_cs_host_buffer_type */
    uint64_t dma_address;
    uint16_t desc_page_size;
    uint32_t total_desc_count;
    uint32_t bytes_in_pattern;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_host_buffer_info) == 19,
               "host_buffer_info must be 19 bytes");

/* CONTEXT_SWITCH_DEFS__stream_reg_info_t. Carried inside every
 * ACTIVATE_BOUNDARY_X / ACTIVATE_INTER_CONTEXT_X action and
 * describes the NN-stream buffer geometry. Values come from the
 * HEF's nn_stream_config (same fields as the CONFIG_STREAM opcode
 * body). */
struct hailo_cs_stream_reg_info {
    uint16_t core_bytes_per_buffer;
    uint16_t core_buffers_per_frame;
    uint16_t periph_bytes_per_buffer;
    uint16_t periph_buffers_per_frame;       /* #253: was uint32_t; reference
                                              * uses u16, confirmed via
                                              * byte-for-byte wire capture on
                                              * pi-5-1 (pios_DYNAMIC.bin). */
    uint16_t feature_padding_payload;
    uint32_t buffer_padding_payload;
    uint16_t buffer_padding;
    uint8_t  is_core_hw_padding_config_in_dfc;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_stream_reg_info) == 17,
               "stream_reg_info must be 17 bytes per v4.23 wire format");

/* -------------------------------------------------------------------------- */
/* Preliminary-context actions (CCW upload)                                    */
/* -------------------------------------------------------------------------- */

/* ACTIVATE_CFG_CHANNEL: binds a config stream to a VDMA channel and
 * tells firmware where to DMA-pull CCW payloads from. Followed by
 * one or more FETCH_CCW_BURSTS actions. */
struct hailo_cs_act_activate_cfg_channel {
    uint8_t                           packed_vdma_channel_id;
    uint8_t                           config_stream_index;
    struct hailo_cs_host_buffer_info  host_buffer_info;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_activate_cfg_channel) == 21,
               "activate_cfg_channel body must be 21 bytes");

/* FETCH_CCW_BURSTS: tells firmware to pull `ccw_bursts` bursts from
 * the config stream. Each burst is a fixed-size (compiler-chosen)
 * chunk of CCW payload bytes. Used by HailoRT on Hailo-8 (with
 * support_pre_fetch). On Hailo-8L support_pre_fetch=false, so
 * FETCH_CFG_CHANNEL_DESCRIPTORS is used instead — see below. */
struct hailo_cs_act_fetch_ccw_bursts {
    uint16_t ccw_bursts;
    uint8_t  config_stream_index;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_fetch_ccw_bursts) == 3,
               "fetch_ccw_bursts body must be 3 bytes");

/* FETCH_CFG_CHANNEL_DESCRIPTORS (action_type 0): tells firmware to
 * program `descriptors_count` VDMA descriptors on the config channel
 * for upcoming CCW DMA-pulls. HailoRT's fallback (non-pre-fetch)
 * path used on Hailo-8L. Normally wrapped in REPEATED_ACTION so
 * the firmware's CONFIG_MANAGER_WRAPPER dispatches correctly — the
 * bare action_type is rejected in PRELIMINARY as
 * ACTION_TYPE_NOT_SUPPORTED. Per v4.23 context_switch_defs.h:187-191. */
struct hailo_cs_act_fetch_cfg_channel_descriptors {
    uint16_t descriptors_count;
    uint8_t  packed_vdma_channel_id;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_fetch_cfg_channel_descriptors) == 3,
               "fetch_cfg_channel_descriptors body must be 3 bytes");

/* DEACTIVATE_CFG_CHANNEL: tears down the config stream binding,
 * typically the last action in the preliminary context. */
struct hailo_cs_act_deactivate_cfg_channel {
    uint8_t packed_vdma_channel_id;
    uint8_t config_stream_index;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_deactivate_cfg_channel) == 2,
               "deactivate_cfg_channel body must be 2 bytes");

/* REPEATED_ACTION header — the 3-byte block that follows the 5-byte
 * common_action_header when an action is of type REPEATED_ACTION.
 * Contents:
 *   count: how many consecutive sub-action bodies follow (1..255).
 *   last_executed: firmware-tracked progress counter; set to 0 on
 *     emission (firmware overwrites as it processes each sub-body).
 *   sub_action_type: action_type of the sub-bodies, with the bodies
 *     laid out back-to-back with NO interleaved common_action_headers.
 *
 * Layout on the wire (per v4.23 context_switch_defs.h:146-187):
 *   [0] common_action_header (5 B, action_type = REPEATED_ACTION)
 *   [5] repeated_action_header {
 *         count (u8), last_executed (u8), sub_action_type (u8)
 *       }
 *   [8..] N × <sub-action body> (each sized to its action_type's
 *         body struct; no per-body headers)
 *
 * HailoRT uses REPEATED_ACTION in PRELIMINARY to wrap the CCW-load
 * sub-action, with the sub_action_type picked by
 * ChannelAllocator::support_pre_fetch:
 *
 *   Hailo-8  (support_pre_fetch = true):
 *     sub_action_type = FETCH_CCW_BURSTS  (0x1b)
 *     body            = hailo_cs_act_fetch_ccw_bursts (3 B)
 *
 *   Hailo-8L (support_pre_fetch = false):
 *     sub_action_type = FETCH_CFG_CHANNEL_DESCRIPTORS  (0x00)
 *     body            = hailo_cs_act_fetch_cfg_channel_descriptors (3 B)
 *
 * SLM-OS targets Hailo-8L (AI HAT+) today, so translate_preliminary
 * emits the FETCH_CFG_CHANNEL_DESCRIPTORS variant. Either sub-type
 * emitted WITHOUT the REPEATED_ACTION wrapper is rejected with
 * CONFIG_MANAGER_WRAPPER_STATUS_ACTION_TYPE_NOT_SUPPORTED. */
struct hailo_cs_repeated_action_header {
    uint8_t count;
    uint8_t last_executed;
    uint8_t sub_action_type;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_repeated_action_header) == 3,
               "repeated_action_header must be 3 bytes");

/* -------------------------------------------------------------------------- */
/* Compute-context actions (DYNAMIC)                                            */
/* -------------------------------------------------------------------------- */

/* ENABLE_LCU_DEFAULT: turns on an LCU with firmware-default
 * kernel_done_address / kernel_done_count. The smaller of the two
 * ENABLE_LCU variants, used when the HEF didn't override those
 * fields. packed_lcu_id encoding: (cluster_index << 4) | (lcu_index
 * & 0xF) per HailoRT convention — single u8 carries both. */
struct hailo_cs_act_enable_lcu_default {
    uint8_t packed_lcu_id;
    uint8_t network_index;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_enable_lcu_default) == 2,
               "enable_lcu_default body must be 2 bytes");

/* ENABLE_LCU_NON_DEFAULT: same as default plus kernel_done_address
 * + kernel_done_count — used when the HEF carries non-zero values
 * for those fields. */
struct hailo_cs_act_enable_lcu_non_default {
    uint8_t  packed_lcu_id;
    uint8_t  network_index;
    uint16_t kernel_done_address;
    uint32_t kernel_done_count;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_enable_lcu_non_default) == 8,
               "enable_lcu_non_default body must be 8 bytes");

/* Encoding helper: pack cluster_index + lcu_index into a single u8
 * for the wire structs above. High nibble = cluster, low nibble =
 * lcu. Hailo-8 caps both at 15 so the 4-bit split is lossless for
 * conforming HEFs. Out-of-range inputs (corrupt HEF, future
 * architecture, test mistake) would otherwise silently wrap —
 * hailo_cs_pack_lcu_id_checked surfaces that with a WARN. */
static inline uint8_t hailo_cs_pack_lcu_id(uint32_t cluster_index,
                                           uint32_t lcu_index)
{
    return (uint8_t)(((cluster_index & 0x0Fu) << 4) | (lcu_index & 0x0Fu));
}

/* Wider-contract variant: validates cluster/lcu both fit in 4 bits.
 * Returns 0 (not a valid packed id on Hailo-8 with cluster=0,lcu=0)
 * and sets *clamped=true if either field was out of range. Callers
 * that can't reasonably handle a bad HEF just use the truncating
 * variant above; translator code paths use this + WARN-log so a
 * field bug doesn't silently produce a bogus wire action. */
static inline uint8_t hailo_cs_pack_lcu_id_checked(uint32_t cluster_index,
                                                   uint32_t lcu_index,
                                                   bool    *clamped)
{
    if (cluster_index > 0x0Fu || lcu_index > 0x0Fu) {
        *clamped = true;
    }
    return hailo_cs_pack_lcu_id(cluster_index, lcu_index);
}

/* DISABLE_LCU: turns off an LCU previously enabled with
 * ENABLE_LCU_*. 1-byte body — just the packed_lcu_id. */
struct hailo_cs_act_disable_lcu {
    uint8_t packed_lcu_id;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_disable_lcu) == 1,
               "disable_lcu body must be 1 byte");

/* SEQUENCER_DONE_INTERRUPT: firmware waits on a sequencer's done
 * interrupt. 1-byte body — sequencer_index (== cluster_index on
 * Hailo-8). */
struct hailo_cs_act_sequencer_interrupt {
    uint8_t sequencer_index;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_sequencer_interrupt) == 1,
               "sequencer_interrupt body must be 1 byte");

/* CONTEXT_SWITCH_DEFS__sequencer_config_t. Embedded inside
 * TRIGGER_SEQUENCER's action body. Captures enough register-image
 * state for firmware to program the sequencer. Packed to 43 bytes
 * (1+2+4+4+8+8+8+8).
 *
 * ⚠ Hardware verification pending (#180 blocker). The common_action_
 * header case (memory: hailo_cs_common_header_8_bytes) showed fw
 * v4.23 reads some packed structs with NATURAL alignment even when
 * the host spec packs them — a 5-byte packed common_header was
 * rejected with 0x40130016 (MISALIGNMENT_ERROR). If fw v4.23 reads
 * sequencer_config with natural alignment it would expect 48 bytes
 * (1 + 1 pad + 2 + 4 + 4 + 8 + 8 + 8 + 8). Cross-check against a
 * running firmware trace before wiring TRIGGER_SEQUENCER into the
 * real load path (#179). The boundary-channel structs below carry
 * the same risk. */
struct hailo_cs_sequencer_config {
    uint8_t  initial_l3_cut;
    uint16_t initial_l3_offset;
    uint32_t active_apu;
    uint32_t active_ia;
    uint64_t active_sc;
    uint64_t active_l2;
    uint64_t l2_offset_0;
    uint64_t l2_offset_1;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_sequencer_config) == 43,
               "sequencer_config must be 43 bytes "
               "(1+2+4+4+8+8+8+8)");

/* TRIGGER_SEQUENCER: kicks a cluster's sequencer. Body is
 * cluster_index + full sequencer_config (44 bytes total). */
struct hailo_cs_act_trigger_sequencer {
    uint8_t                          cluster_index;
    struct hailo_cs_sequencer_config sequencer_config;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_trigger_sequencer) == 44,
               "trigger_sequencer body must be 44 bytes");

/* FETCH_DATA_FROM_VDMA_CHANNEL: firmware pulls N frames of data
 * from the host-side VDMA channel into on-chip buffers. Used for
 * boundary-input flow. Body layout per hailort's
 * fetch_data_action_data_t. */
struct hailo_cs_act_fetch_data_from_vdma {
    uint8_t  packed_vdma_channel_id;
    uint8_t  stream_index;
    uint8_t  network_index;
    uint32_t frame_periph_size;
    uint8_t  credit_type;     /* CREDIT_IN_BYTES=1, CREDIT_IN_DESCRIPTORS=2 */
    uint8_t  host_buffer_type; /* HOST_BUFFER_* enum above */
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_fetch_data_from_vdma) == 9,
               "fetch_data_from_vdma body must be 9 bytes");

/* CHANGE_BOUNDARY_INPUT_BATCH: BATCH_SWITCHING context action. Tells
 * firmware "this boundary H2D channel has a new batch size this cycle"
 * — programmed per boundary input channel between
 * DDR_BUFFERING_RESET and BURST_CREDITS_TASK_START. HailoRT emits one
 * per boundary H2D layer; skipping it causes firmware's
 * BURST_CREDITS_TASK_START to read uninitialized batch state on the
 * boundary channel and crash the control CPU (observed on pi-5-1 fw
 * v4.23 as BAR4 going dark + PCIe link drop — issue #180). Body is
 * just the packed VDMA channel id. */
struct hailo_cs_act_change_boundary_input_batch {
    uint8_t packed_vdma_channel_id;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_change_boundary_input_batch) == 1,
               "change_boundary_input_batch body must be 1 byte");

/* -------------------------------------------------------------------------- */
/* Boundary-channel open/activate actions (ACTIVATION context)                  */
/* -------------------------------------------------------------------------- */

/* Scaffolding only — declared + static_assert-sized here so the wire
 * layouts are pinned against CONTEXT_SWITCH_DEFS, but NO translator
 * emits these bodies yet. Wiring lands with #178 (boundary-channel
 * mapping + OpenBoundary actions). Do not assume dead code: the
 * structs are load-bearing contracts the #178 implementation will
 * populate. */

/* OPEN_BOUNDARY_INPUT_CHANNEL: binds a host→device VDMA channel
 * for boundary input (the stream a host-produced tensor flows
 * through). Body carries the packed channel id + host_buffer_info
 * for the descriptor list + stream geometry.
 */
struct hailo_cs_act_open_boundary_input_channel {
    uint8_t                          packed_vdma_channel_id;
    struct hailo_cs_host_buffer_info host_buffer_info;
    uint8_t                          stream_index;
    uint8_t                          network_index;
    uint16_t                         periph_bytes_per_buffer;
    uint32_t                         frame_periph_size;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_open_boundary_input_channel) == 28,
               "open_boundary_input_channel body must be 28 bytes "
               "(1 + 19 host_buffer_info + 1 + 1 + 2 + 4)");

/* OPEN_BOUNDARY_OUTPUT_CHANNEL: mirror for device→host. Smaller
 * body — firmware derives output geometry from CONFIG_STREAM state,
 * so we only supply the channel id + host_buffer_info. */
struct hailo_cs_act_open_boundary_output_channel {
    uint8_t                          packed_vdma_channel_id;
    struct hailo_cs_host_buffer_info host_buffer_info;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_open_boundary_output_channel) == 20,
               "open_boundary_output_channel body must be 20 bytes");

/* ACTIVATE_BOUNDARY_INPUT / ACTIVATE_BOUNDARY_OUTPUT: firmware
 * activates the boundary streams for inference. Input carries
 * stream_reg_info + host_buffer_info + initial_credit_size;
 * output is similar plus a network_index. */
struct hailo_cs_act_activate_boundary_input {
    uint8_t                          packed_vdma_channel_id;
    uint8_t                          stream_index;
    struct hailo_cs_stream_reg_info  stream_reg_info;
    struct hailo_cs_host_buffer_info host_buffer_info;
    uint32_t                         initial_credit_size;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_activate_boundary_input) == 42,
               "activate_boundary_input body must be 42 bytes per v4.23 wire");

struct hailo_cs_act_activate_boundary_output {
    uint8_t                          packed_vdma_channel_id;
    uint8_t                          stream_index;
    uint8_t                          network_index;
    struct hailo_cs_stream_reg_info  stream_reg_info;
    struct hailo_cs_host_buffer_info host_buffer_info;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_activate_boundary_output) == 39,
               "activate_boundary_output body must be 39 bytes per v4.23 wire");

/* Edge layer direction enum used by (de)activate/pause/resume actions.
 * Reference: hailort-v4.23.0-context_switch_defs.h (near the
 * deactivate_vdma_channel / resume_vdma_channel structs). */
enum hailo_cs_edge_layer_direction {
    HAILO_CS_EDGE_DIR_H2D = 0,
    HAILO_CS_EDGE_DIR_D2H = 1,
};

/* PAUSE_VDMA_CHANNEL / RESUME_VDMA_CHANNEL share the same 2-byte body:
 * just the packed channel id and the direction. Fw uses these to
 * freeze/unfreeze a boundary channel between context switches;
 * RESUME must be emitted in DYNAMIC before the first FETCH_DATA
 * on that channel. */
struct hailo_cs_act_resume_vdma_channel {
    uint8_t packed_vdma_channel_id;
    uint8_t edge_layer_direction;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_resume_vdma_channel) == 2,
               "resume_vdma_channel body must be 2 bytes");

#endif /* AI_ACCEL_HAILO_CS_ACTIONS_H */
