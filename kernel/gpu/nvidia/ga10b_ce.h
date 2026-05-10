/*
 * ga10b_ce.h — GA10B (Tegra Ampere) Copy Engine memcpy via pushbuffer.
 *
 * Class binding: AMPERE_DMA_COPY_B = 0xC7B5 on subchannel 4 (NVK
 * convention from `~/slmos-ref/mesa/mesa-nv_push.h:94-95`). Method
 * offsets from `~/slmos-ref/nvidia/nvidia-clc7b5.h`.
 *
 * Architectural role: the GPU weights pool's CPU-writable phys is
 * unreliable on Jetson post-kexec because the channel's GMMU page-
 * table tree gets freed during Linux's kexec teardown — see
 * `docs/design/gpu-weights-pool.md`. The CE memcpy primitive sidesteps
 * the GMMU-walk failure by submitting a pushbuffer that the GPU's CE
 * executes against the channel's existing runlist binding (which
 * survives kexec because the GPU side keeps state). The CE walks the
 * channel's GMMU itself; SLM-OS doesn't have to.
 *
 * Caller pattern (oplib_weights_pool_stage will follow this):
 *   1. CPU writes `bytes` of source data into a helper-staged bounce
 *      buffer with a known phys/gpu_va (e.g. the SASS pool's SCRATCH0
 *      slot at `h->shader_phys + OPLIB_POOL_OFF_SCRATCH0`).
 *   2. `cache_clean_range` the bounce buffer.
 *   3. Call `ga10b_ce_memcpy(b, src_gpu_va=bounce_gva,
 *                            dst_gpu_va=pool_slot_gva, bytes)`.
 *   4. On rc=0 the CE has finished and the destination is valid for
 *      GPU dispatchers reading it via gpu_va.
 */

#ifndef GPU_NVIDIA_GA10B_CE_H
#define GPU_NVIDIA_GA10B_CE_H

#include <stddef.h>
#include <stdint.h>

#include "ga10b_bringup.h"

/* AMPERE_DMA_COPY_B class ID — what SET_OBJECT binds to a subchannel
 * for CE methods. Authoritative per
 * `~/slmos-ref/nvidia/nvidia-clc7b5.h`. Discrete Ampere has both
 * AMPERE_DMA_COPY_A (0xC6B5) for datacenter parts and _B (0xC7B5)
 * for consumer; Tegra GA10B uses the _B variant exclusively (no
 * Tegra-specific divergence found in L4T nvgpu). */
#define GA10B_AMPERE_DMA_COPY_B_CLASS_ID  0xC7B5u

/* Subchannel used for CE class binding. NVK pins copy classes
 * (0x90B5, 0xC1B5, 0xC7B5, ...) to subch 4 in `nv_push.h`. The host
 * channel's PBDMA decodes subchannel routing from the method header
 * bits [15:13], so binding CE to a free subchannel different from the
 * compute path's subch 1 keeps the two engines' method streams
 * separable on the same pushbuffer. */
#define GA10B_CE_SUBCHANNEL  4u

/* CE method byte offsets (class 0xC7B5). Authoritative per
 * `~/slmos-ref/nvidia/nvidia-clc7b5.h`. Pinned by host-side tests in
 * `host-tools/gsp-harness/test_ga10b_ce.c` so a wrong-encoding
 * regression is a build failure, not a hardware mystery. */
#define NVC7B5_LAUNCH_DMA              0x300u
#define NVC7B5_SET_SEMAPHORE_A         0x240u
#define NVC7B5_SET_SEMAPHORE_B         0x244u
#define NVC7B5_SET_SEMAPHORE_PAYLOAD   0x248u
#define NVC7B5_OFFSET_IN_UPPER         0x400u
#define NVC7B5_OFFSET_IN_LOWER         0x404u
#define NVC7B5_OFFSET_OUT_UPPER        0x408u
#define NVC7B5_OFFSET_OUT_LOWER        0x40Cu
#define NVC7B5_PITCH_IN                0x410u
#define NVC7B5_PITCH_OUT               0x414u
#define NVC7B5_LINE_LENGTH_IN          0x418u
#define NVC7B5_LINE_COUNT              0x41Cu

/* LAUNCH_DMA bit fields. */
#define NVC7B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NONE           (0u << 0)
#define NVC7B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_PIPELINED      (1u << 0)
#define NVC7B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED  (2u << 0)
#define NVC7B5_LAUNCH_DMA_FLUSH_ENABLE                      (1u << 2)
#define NVC7B5_LAUNCH_DMA_SEMAPHORE_TYPE_NONE                    (0u << 3)
#define NVC7B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_NO_TIMESTAMP    (1u << 3)
#define NVC7B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH           (1u << 7)
#define NVC7B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH           (1u << 8)
#define NVC7B5_LAUNCH_DMA_SRC_TYPE_VIRTUAL                  (0u << 12)
#define NVC7B5_LAUNCH_DMA_DST_TYPE_VIRTUAL                  (0u << 12)

/* Completion payload the CE writes to the channel's semaphore when
 * the copy retires. Distinctive value (`0xCAFE`) so a stale read or
 * residue from a prior dispatch never looks like CE success. */
#define GA10B_CE_SEMA_PAYLOAD  0x0000CAFEu

/* Maximum copy size per single LAUNCH_DMA. LINE_LENGTH_IN is the
 * byte count per row (24-bit on Ampere, max ≈16 MB), but the
 * channel's pushbuffer holds at most 65 KB of methods, so the
 * practical per-submit cap is whatever the caller wants to feed at
 * once. The oplib_weights_pool_stage path chunks at the bounce
 * buffer size (OPLIB_POOL_SLOT_BYTES = 64 KB) — see
 * `oplib_weights_pool.c` for the chunking loop. */
#define GA10B_CE_MAX_BYTES_PER_LAUNCH  ((uint32_t)(1u << 24))

/* Pushbuffer dword count for one CE memcpy + terminating semaphore
 * release. Pinned by host tests so a layout change shows up at
 * build time. Layout (header + data pairs):
 *   [0,1]   SET_OBJECT(subch=CE, class=AMPERE_DMA_COPY_B)
 *   [2,3]   SET_SEMAPHORE_A (upper)
 *   [4,5]   SET_SEMAPHORE_B (lower)
 *   [6,7]   SET_SEMAPHORE_PAYLOAD
 *   [8,9]   OFFSET_IN_UPPER
 *   [10,11] OFFSET_IN_LOWER
 *   [12,13] OFFSET_OUT_UPPER
 *   [14,15] OFFSET_OUT_LOWER
 *   [16,17] PITCH_IN  (= LINE_LENGTH_IN for 1D)
 *   [18,19] PITCH_OUT (= LINE_LENGTH_IN for 1D)
 *   [20,21] LINE_LENGTH_IN
 *   [22,23] LINE_COUNT (= 1 for 1D)
 *   [24,25] LAUNCH_DMA */
#define GA10B_CE_MEMCPY_PB_DWORDS  26u

/* Build a CE memcpy pushbuffer encoding a single 1D byte-stream copy
 * from `src_gpu_va` to `dst_gpu_va`. Length `bytes` must be in
 * [1, GA10B_CE_MAX_BYTES_PER_LAUNCH]; the caller chunks larger
 * transfers. The CE's own report-semaphore writes `payload` to
 * `sem_gpu_va` once the copy retires; `ga10b_submit_and_poll` then
 * polls the same address for that payload to confirm completion.
 *
 * Both src and dst are interpreted as VIRTUAL addresses through the
 * channel's GMMU. Both memory layouts are PITCH (1D, not block-
 * linear). DATA_TRANSFER_TYPE is NON_PIPELINED + FLUSH_ENABLE so the
 * subsequent compute dispatch sees the bytes already in L2/DRAM.
 *
 * `pb` must point to at least GA10B_CE_MEMCPY_PB_DWORDS u32 slots.
 * Returns the dword count written (always GA10B_CE_MEMCPY_PB_DWORDS).
 *
 * Pure logic — no MMIO, no globals. Host-testable. */
uint32_t ga10b_build_ce_memcpy_pushbuffer(uint32_t *pb,
                                          uint64_t src_gpu_va,
                                          uint64_t dst_gpu_va,
                                          uint32_t bytes,
                                          uint64_t sem_gpu_va,
                                          uint32_t payload);

/* High-level CE memcpy: build the pushbuffer above, submit through
 * the inherited channel, poll the channel's semaphore for completion.
 *
 *   `b`           - bringup state (channel must be inherited + bound,
 *                   i.e. state >= GA10B_BRINGUP_PMU_UP)
 *   `src_gpu_va`  - source GPU VA in the channel's address space
 *   `dst_gpu_va`  - destination GPU VA in the channel's address space
 *   `bytes`       - byte count, must be in (0, GA10B_CE_MAX_BYTES_PER_LAUNCH]
 *
 * Uses the channel's existing semaphore (`h->semaphore_phys/gpu_va`)
 * as the completion target. Caller does not need to pre-clear the
 * semaphore — `ga10b_submit_and_poll` zeroes it before submit.
 *
 * Returns 0 on success. Negative on:
 *   - null `b`, zero or oversize `bytes`, zero `src_gpu_va` or
 *     `dst_gpu_va`,
 *   - missing handoff or invalid semaphore mapping,
 *   - submit refused (pushbuffer too large — should be impossible
 *     since GA10B_CE_MEMCPY_PB_DWORDS = 26 << 16384),
 *   - dispatch timeout (CE didn't complete in ~2 s, or the channel's
 *     runlist binding doesn't actually include CE on this Tegra GA10B
 *     variant — see implementation comments). */
int ga10b_ce_memcpy(struct ga10b_bringup *b,
                    uint64_t src_gpu_va,
                    uint64_t dst_gpu_va,
                    uint32_t bytes);

#endif /* GPU_NVIDIA_GA10B_CE_H */
