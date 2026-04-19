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
 * action_type is u8 (the enum is packed to u8 in hailort's defs).
 * time_stamp is host-chosen — firmware uses it for tracing only;
 * zero is acceptable. */
struct hailo_cs_common_action_header {
    uint8_t  action_type;
    uint32_t time_stamp;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_common_action_header) == 5,
               "common_action_header must be 5 bytes");

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
    uint32_t periph_buffers_per_frame;
    uint16_t feature_padding_payload;
    uint32_t buffer_padding_payload;
    uint16_t buffer_padding;
    uint8_t  is_core_hw_padding_config_in_dfc;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_stream_reg_info) == 19,
               "stream_reg_info must be 19 bytes");

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
 * chunk of CCW payload bytes. */
struct hailo_cs_act_fetch_ccw_bursts {
    uint16_t ccw_bursts;
    uint8_t  config_stream_index;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_fetch_ccw_bursts) == 3,
               "fetch_ccw_bursts body must be 3 bytes");

/* DEACTIVATE_CFG_CHANNEL: tears down the config stream binding,
 * typically the last action in the preliminary context. */
struct hailo_cs_act_deactivate_cfg_channel {
    uint8_t packed_vdma_channel_id;
    uint8_t config_stream_index;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_cs_act_deactivate_cfg_channel) == 2,
               "deactivate_cfg_channel body must be 2 bytes");

#endif /* AI_ACCEL_HAILO_CS_ACTIONS_H */
