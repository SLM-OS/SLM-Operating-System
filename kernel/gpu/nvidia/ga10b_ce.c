/*
 * ga10b_ce.c — GA10B Copy Engine memcpy via pushbuffer. See header
 * for design notes.
 */

#include "ga10b_ce.h"

#include "ga10b_bringup.h"
#include "ga10b_channel_handoff.h"

#include <stdbool.h>
#include <stdint.h>

/* SET_OBJECT is method byte offset 0 on every channel class — the
 * host-family PBDMA decodes it before routing to the subchannel-bound
 * engine. Identical value as in ga10b_bringup.c (NVC56F_SET_OBJECT);
 * redefined here to keep ga10b_ce.c self-contained against future
 * include-order shuffles. */
#define NVC7B5_SET_OBJECT  0x00u

/* Channel host-family method-header encoding for an "incrementing"
 * method burst — one header followed by `count` consecutive data
 * dwords, each landing at successive 4-byte method offsets starting
 * at `byte_off`. Same encoding as `NVC56F_METHOD_HEADER_INC` in
 * ga10b_bringup.c; redefined here so this file doesn't depend on the
 * bringup TU's static macros.
 *
 * Encoding (Volta+): bit 29 = INC opcode, bits [25:16] = count
 * (10 bits), bits [15:13] = subchannel (3 bits), bits [12:0] =
 * method id = byte_off / 4 (13 bits — widened from Kepler's 11 bits
 * to cover Ampere's > 0x1FFC method-space classes). */
static inline uint32_t ce_hdr_inc(uint32_t count, uint32_t subch,
                                  uint32_t byte_off)
{
    return (1u << 29) | ((count & 0x3FFu) << 16) |
           ((subch & 0x7u) << 13) | (((byte_off) >> 2) & 0x1FFFu);
}

uint32_t ga10b_build_ce_memcpy_pushbuffer(uint32_t *pb,
                                          uint64_t src_gpu_va,
                                          uint64_t dst_gpu_va,
                                          uint32_t bytes,
                                          uint64_t sem_gpu_va,
                                          uint32_t payload)
{
    const uint32_t sc = GA10B_CE_SUBCHANNEL;
    uint32_t i = 0;

    /* SET_OBJECT — bind AMPERE_DMA_COPY_B to the CE subchannel. */
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_SET_OBJECT);
    pb[i++] = GA10B_AMPERE_DMA_COPY_B_CLASS_ID;

    /* Semaphore target — A is the upper 17 bits, B is the lower 32.
     * Three consecutive methods (0x240, 0x244, 0x248) emitted as
     * separate header+data pairs to mirror the host-family builder's
     * style; PBDMA decodes a count=3 burst identically, but keeping
     * the pairs explicit makes the encoding easier to step through
     * in a kernel trace. */
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_SET_SEMAPHORE_A);
    pb[i++] = (uint32_t)((sem_gpu_va >> 32) & 0x1FFFFu);
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_SET_SEMAPHORE_B);
    pb[i++] = (uint32_t)(sem_gpu_va & 0xFFFFFFFFu);
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_SET_SEMAPHORE_PAYLOAD);
    pb[i++] = payload;

    /* Source GPU VA. UPPER carries bits [48:32] of the address as a
     * 17-bit field. Both UPPER/LOWER methods exist as independent
     * registers — the encoding writes only the high 17 bits to
     * UPPER and the low 32 to LOWER. */
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_OFFSET_IN_UPPER);
    pb[i++] = (uint32_t)((src_gpu_va >> 32) & 0x1FFFFu);
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_OFFSET_IN_LOWER);
    pb[i++] = (uint32_t)(src_gpu_va & 0xFFFFFFFFu);

    /* Destination GPU VA. Same shape. */
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_OFFSET_OUT_UPPER);
    pb[i++] = (uint32_t)((dst_gpu_va >> 32) & 0x1FFFFu);
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_OFFSET_OUT_LOWER);
    pb[i++] = (uint32_t)(dst_gpu_va & 0xFFFFFFFFu);

    /* Pitch in/out are byte strides between rows. For a 1D copy
     * (LINE_COUNT=1) pitch equals line-length; both are the same
     * value. */
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_PITCH_IN);
    pb[i++] = bytes;
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_PITCH_OUT);
    pb[i++] = bytes;
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_LINE_LENGTH_IN);
    pb[i++] = bytes;
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_LINE_COUNT);
    pb[i++] = 1u;

    /* LAUNCH_DMA — the engine kicks here. NON_PIPELINED means the CE
     * drains any prior work on the same channel before starting this
     * copy. FLUSH_ENABLE writes back the dest through L2 to PoC so
     * the subsequent compute dispatch sees the bytes from DRAM (the
     * weights pool's destination is in sysmem on Tegra Orin Nano).
     * SEMAPHORE_TYPE=RELEASE_NO_TIMESTAMP fires the report-semaphore
     * after the copy completes; ga10b_submit_and_poll's caller polls
     * that semaphore. Both source and destination are VIRTUAL —
     * resolved through the channel's GMMU which the helper set up
     * pre-kexec. */
    uint32_t launch =
        NVC7B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED |
        NVC7B5_LAUNCH_DMA_FLUSH_ENABLE |
        NVC7B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_NO_TIMESTAMP |
        NVC7B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH |
        NVC7B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH |
        NVC7B5_LAUNCH_DMA_SRC_TYPE_VIRTUAL |
        NVC7B5_LAUNCH_DMA_DST_TYPE_VIRTUAL;
    pb[i++] = ce_hdr_inc(1, sc, NVC7B5_LAUNCH_DMA);
    pb[i++] = launch;

    return i;
}

int ga10b_ce_memcpy(struct ga10b_bringup *b,
                    uint64_t src_gpu_va,
                    uint64_t dst_gpu_va,
                    uint32_t bytes)
{
    if (b == NULL ||
        bytes == 0 || bytes > GA10B_CE_MAX_BYTES_PER_LAUNCH ||
        src_gpu_va == 0 || dst_gpu_va == 0) {
        return -1;
    }
    const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
    if (h == NULL || h->semaphore_phys == 0 || h->semaphore_gpu_va == 0) {
        return -1;
    }

    /* Use a distinctive payload so a stale read of the semaphore
     * doesn't look like success. Defined in our header rather than
     * shared with the compute path's smoke payload — keeps the two
     * engines' completion sentinels independently auditable. */
    const uint32_t payload = GA10B_CE_SEMA_PAYLOAD;

    uint32_t pb[GA10B_CE_MEMCPY_PB_DWORDS];
    uint32_t dwords = ga10b_build_ce_memcpy_pushbuffer(
        pb, src_gpu_va, dst_gpu_va, bytes,
        h->semaphore_gpu_va, payload);

    /* The channel's semaphore page is mapped both at the channel's
     * `semaphore_gpu_va` (used by the GPU) and at `semaphore_phys`
     * (identity-mapped for CPU polling). `ga10b_submit_and_poll`
     * polls the phys-side address. */
    return ga10b_submit_and_poll(b, pb, dwords,
                                  h->semaphore_phys, payload,
                                  /*error_phase=*/-1, "ce-memcpy");
}
