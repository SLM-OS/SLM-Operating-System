/*
 * hailo_infer.h — top-level inference orchestrator (Phase 5.4).
 *
 * Ties hailo_tensor (DMA buffers), hailo_vdma (descriptor lists +
 * channel reg writes), and the platform cache-maintenance ops
 * into a single per-inference sequence:
 *
 *   1. Allocate input + output tensor buffers.
 *   2. Allocate + program input descriptor list; allocate output.
 *   3. memcpy user data into the input tensor; cache-clean it.
 *   4. Start input channel + output channel.
 *   5. Submit + wait on the OUTPUT channel (device DMA fires when
 *      the firmware's inference pipeline produces output, which
 *      happens after it has consumed the input via the input
 *      channel).
 *   6. Cache-invalidate output tensor; memcpy to user buffer.
 *   7. Stop both channels. Free buffers.
 *
 * Parameters that a real HEF-driven caller would source from
 * CONFIG_STREAM responses (channel index, data_id, desc_page_size)
 * are passed in explicitly. Once a `.hef` is in the lab and the
 * CONFIG_STREAM round-trip produces real ids, a higher-level
 * wrapper can parse the HEF and fill this struct.
 *
 * Pre-conditions: firmware already booted (hailo_boot) and in
 * HAILO_STATE_RUNNING; CONFIG_STREAM has established the data_id
 * for each channel; BAR2 is mapped by the platform (Pi 5: pcie1
 * is trained — still future work).
 */

#ifndef AI_ACCEL_HAILO_INFER_H
#define AI_ACCEL_HAILO_INFER_H

#include <stdint.h>
#include <stddef.h>

#include "hailo.h"

struct hailo_infer_config {
    /* Host-side input / output sizes in bytes. */
    uint32_t input_bytes;
    uint32_t output_bytes;

    /* VDMA channel assignments. Hailo-8 fw v4.23 splits the 32 VDMA
     * channels into H2D [0, 15] (host→device, used for input) and
     * D2H [16, 31] (device→host, used for output). `input_channel`
     * MUST land in H2D and `output_channel` in D2H; mismatched
     * directions silently wedge the engine (#682). Callers should
     * use the translator's HAILO_CS_DEFAULT_CONFIG_VDMA_CHANNEL +
     * HAILO_CS_BOUNDARY_{INPUT,OUTPUT}_CHANNEL_OFFSET so the host
     * MMIO and the CS OPEN_BOUNDARY RPCs reference the same
     * channels by construction. */
    uint8_t  input_channel;
    uint8_t  output_channel;

    /* data_id identifies which on-chip data source/sink this
     * channel feeds. Set up earlier via CONFIG_STREAM. */
    uint8_t  input_data_id;
    uint8_t  output_data_id;

    /* Per-descriptor page size, per-direction. From the HEF's
     * nn_stream_config fields (core_bytes_per_buffer for input,
     * periph_bytes_per_buffer for output) in the general case. */
    uint16_t input_page_size;
    uint16_t output_page_size;

    /* Per-direction timeout for the submit-and-wait poll. Input
     * typically completes in microseconds; output depends on
     * model runtime. A generous 500 ms default lets small
     * classification models run without hitting the cap. */
    uint32_t timeout_us;
};

/*
 * Run one inference. Returns HAILO_OK on success with `output`
 * populated, or a negative error code. On error, all partial
 * state (tensors, desc lists, channels) is cleaned up before
 * return — the caller doesn't have to free anything.
 *
 * `*out_elapsed_us` (if non-NULL) carries the poll-loop-measured
 * latency from "submit output channel" to "num_proc matched" —
 * a first-order proxy for inference latency until a real MSI
 * completion path lands.
 */
int hailo_infer_run(const struct hailo_infer_config *cfg,
                    const void *input,
                    void *output,
                    uint64_t *out_elapsed_us);

#endif /* AI_ACCEL_HAILO_INFER_H */
