/*
 * hailo_infer.c — top-level inference orchestrator.
 *
 * See hailo_infer.h for the per-step sequence and pre-conditions.
 * This file is the "glue" layer — all the heavy lifting lives in
 * hailo_tensor, hailo_vdma, and hailo_control.
 */

#include "hailo_infer.h"
#include "hailo.h"
#include "hailo_internal.h"
#include "hailo_tensor.h"
#include "hailo_vdma.h"
#include "debug.h"
#include <string.h>

/* Descriptor-count policy: round tensor_bytes / page_size up to
 * the next power of 2, clamp to [MIN, MAX]. A real HEF-driven
 * caller would pull this from the stream-config, but for now we
 * pick a conservative power-of-2 that covers the tensor. */
static uint32_t pick_desc_count(uint32_t tensor_bytes, uint16_t page_size)
{
    if (tensor_bytes == 0 || page_size == 0) return 0;
    uint32_t needed = (tensor_bytes + page_size - 1u) / page_size;
    /* Early-reject out-of-range needs BEFORE the shift loop. An
     * unbounded `while (pow2 < needed) pow2 <<= 1` would wrap
     * `pow2` to 0 for `needed > 0x80000000` and loop forever. */
    if (needed > HAILO_VDMA_MAX_DESC_COUNT) return 0;
    /* Round up to power of 2. */
    uint32_t pow2 = 1;
    while (pow2 < needed) pow2 <<= 1;
    if (pow2 < HAILO_VDMA_MIN_DESC_COUNT) pow2 = HAILO_VDMA_MIN_DESC_COUNT;
    return pow2;
}

/* Read CNTPCT via a tiny inline — cheap per-step timestamping on
 * ARM64. On QEMU this ticks at ~62.5 MHz; on Pi 5 at 54 MHz. We
 * don't need absolute-us conversion here since the caller passes
 * timeout in us already and we just compute the elapsed delta as
 * a first-order proxy. */
static inline uint64_t cntpct_read(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

static inline uint64_t cntfrq_read(void)
{
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
#else
    return 1;
#endif
}

/*
 * TODO(phase-5.4+): no cross-caller serialization. The current
 * sole caller is the shell on CPU 0; a future AI-scheduler policy
 * submitting concurrent inferences would race on the shared VDMA
 * channel state (channels 0 and 1 are hardcoded via the shell
 * command and the static per-channel register banks live in BAR2).
 * When a second caller arrives, wrap the body in a file-scope
 * spin_lock(&infer_lock) — plain, not irqsave, because the poll
 * waits can run for milliseconds.
 */
int hailo_infer_run(const struct hailo_infer_config *cfg,
                    const void *input,
                    void *output,
                    uint64_t *out_elapsed_us)
{
    if (out_elapsed_us) *out_elapsed_us = 0;
    if (!cfg || !input || !output) return HAILO_ERR_INVAL;
    if (cfg->input_bytes == 0 || cfg->output_bytes == 0) return HAILO_ERR_INVAL;
    if (cfg->input_channel >= HAILO_VDMA_MAX_CHANNELS
     || cfg->output_channel >= HAILO_VDMA_MAX_CHANNELS) {
        return HAILO_ERR_INVAL;
    }
    if (cfg->input_channel == cfg->output_channel) return HAILO_ERR_INVAL;
    if (cfg->input_page_size == 0 || cfg->output_page_size == 0) {
        return HAILO_ERR_INVAL;
    }
    if (!hailo_platform || hailo_get_state() != HAILO_STATE_RUNNING) {
        return HAILO_ERR_NODEV;
    }

    uint32_t in_descs  = pick_desc_count(cfg->input_bytes,
                                         cfg->input_page_size);
    uint32_t out_descs = pick_desc_count(cfg->output_bytes,
                                         cfg->output_page_size);
    if (in_descs == 0 || out_descs == 0) return HAILO_ERR_INVAL;

    /* Allocate everything up front so error paths can unconditionally
     * clean up via a single label. All handles are zero-initialized
     * so the cleanup code can check for NULL / zero cheaply. */
    struct hailo_tensor        in_tensor  = {0};
    struct hailo_tensor        out_tensor = {0};
    struct hailo_vdma_desc_list in_list   = {0};
    struct hailo_vdma_desc_list out_list  = {0};
    int  rc;
    bool in_started  = false;
    bool out_started = false;

    rc = hailo_tensor_alloc(cfg->input_bytes, &in_tensor);
    if (rc != HAILO_OK) goto out;
    rc = hailo_tensor_alloc(cfg->output_bytes, &out_tensor);
    if (rc != HAILO_OK) goto out;

    rc = hailo_vdma_desc_list_alloc(in_descs, cfg->input_page_size,
                                    /*circular=*/false, &in_list);
    if (rc != HAILO_OK) goto out;
    rc = hailo_vdma_desc_list_alloc(out_descs, cfg->output_page_size,
                                    /*circular=*/false, &out_list);
    if (rc != HAILO_OK) goto out;

    /* Copy user input into the DMA buffer. Cache_clean ensures the
     * device sees the freshly-written bytes (no-op on NC memory). */
    memcpy(in_tensor.cpu_addr, input, cfg->input_bytes);
    hailo_tensor_prepare_for_device(&in_tensor);

    int programmed = hailo_vdma_program_buffer(&in_list, 0,
                                               in_tensor.iova,
                                               cfg->input_bytes,
                                               cfg->input_data_id);
    if (programmed < 0) { rc = programmed; goto out; }
    uint16_t in_num_avail = (uint16_t)programmed;

    programmed = hailo_vdma_program_buffer(&out_list, 0,
                                           out_tensor.iova,
                                           cfg->output_bytes,
                                           cfg->output_data_id);
    if (programmed < 0) { rc = programmed; goto out; }
    uint16_t out_num_avail = (uint16_t)programmed;

    rc = hailo_vdma_channel_start(cfg->input_channel, &in_list,
                                  cfg->input_data_id);
    if (rc != HAILO_OK) goto out;
    in_started = true;

    rc = hailo_vdma_channel_start(cfg->output_channel, &out_list,
                                  cfg->output_data_id);
    if (rc != HAILO_OK) goto out;
    out_started = true;

    /* Kick the input DMA — firmware's NN engine consumes the
     * pages as they arrive. */
    rc = hailo_vdma_submit_and_wait(cfg->input_channel, in_num_avail,
                                    cfg->timeout_us);
    if (rc != HAILO_OK) goto out;

    /* Kick the output DMA — blocks until the NN engine has produced
     * all output pages. Time this loop as the latency estimate. */
    uint64_t t_start = cntpct_read();
    rc = hailo_vdma_submit_and_wait(cfg->output_channel, out_num_avail,
                                    cfg->timeout_us);
    uint64_t t_end = cntpct_read();
    if (rc != HAILO_OK) goto out;

    /* Invalidate the output tensor's cache so the CPU picks up
     * device-written bytes, then copy to the caller's buffer. */
    hailo_tensor_prepare_for_host(&out_tensor);
    memcpy(output, out_tensor.cpu_addr, cfg->output_bytes);

    if (out_elapsed_us) {
        uint64_t cycles = t_end - t_start;
        uint64_t freq   = cntfrq_read();
        if (freq > 0) *out_elapsed_us = (cycles * 1000000ULL) / freq;
    }
    rc = HAILO_OK;

out:
    /* Cleanup ordering is load-bearing:
     *   1. Stop channels FIRST so the VDMA engine stops fetching
     *      from the descriptor lists and writing to the tensor
     *      buffers. Freeing these buffers underneath a live engine
     *      would hand device-visible addresses back to the page
     *      allocator and allow the DMA to corrupt whatever
     *      reclaims them.
     *   2. Free descriptor lists next — they own the addresses the
     *      channel regs were pointed at; the engine must be idle
     *      before release is safe.
     *   3. Free tensors last. Their IOVAs are embedded in the
     *      descriptors we just freed.
     * hailo_vdma_channel_stop issues ABORT_PAUSE which the
     * reference (hailo-vdma-common.c:937) treats as sufficient
     * for the engine to stop fetching. */
    if (in_started)  hailo_vdma_channel_stop(cfg->input_channel);
    if (out_started) hailo_vdma_channel_stop(cfg->output_channel);
    hailo_vdma_desc_list_free(&in_list);
    hailo_vdma_desc_list_free(&out_list);
    hailo_tensor_free(&in_tensor);
    hailo_tensor_free(&out_tensor);
    return rc;
}
