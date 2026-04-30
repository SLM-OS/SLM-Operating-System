/*
 * hailo_vdma.h — Hailo VDMA descriptor-list allocator (Phase 5.4 foundation).
 *
 * The VDMA engine on Hailo-8 is a PLDA "descriptor-DMA" controller that
 * reads a host-prepared list of descriptors and DMAs each page of data
 * named by them between host memory and on-chip SRAM. A single
 * inference submits 1+ descriptors per input/output tensor; the list
 * itself lives in host DRAM at a 64 KB-aligned address that the VDMA
 * engine's per-channel registers point at.
 *
 * This file is just the list allocator — it does NOT program descriptor
 * contents, start channels, or wait for completion. Those land in the
 * follow-up Phase 5.4 commits on top of this foundation.
 *
 * Descriptor layout mirrors hailo_vdma_descriptor in
 * `../slmos-reference-cache/hailo/hailo-vdma-common.h:35`:
 *
 *   struct hailo_vdma_descriptor {
 *       u32 PageSize_DescControl;    // page size (low bits) + control flags
 *       u32 AddrL_rsvd_DataID;       // bus address low + data-ID byte
 *       u32 AddrH;                   // bus address high
 *       u32 RemainingPageSize_Status;// remaining-bytes + status bits
 *   };
 *
 * Total: 16 bytes per descriptor. Fixed by firmware — don't rearrange.
 *
 * Capacity policy:
 * - desc_count MUST be a power of 2 (VDMA engine uses (index & mask) in
 *   place of modulo for the ring-buffer walk).
 * - Reasonable sizes: 64 (small tensors), 256 (typical), 4096 (large).
 *   We allow 2..65536 inclusive. Larger than 65536 descriptors per
 *   channel is beyond what any current Hailo-8 model uses and would
 *   overflow the 16-bit num_avail/num_proc counters.
 */

#ifndef AI_ACCEL_HAILO_VDMA_H
#define AI_ACCEL_HAILO_VDMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hailo.h"

/* 64 KB alignment — matches VDMA_DESCRIPTOR_LIST_ALIGN in
 * hailo-vdma-common.h:16. The VDMA engine's per-channel register
 * HOST_DESC_BASE_ADDR truncates the low 16 bits of the list
 * address, so misalignment is silently catastrophic. */
#define HAILO_VDMA_DESC_LIST_ALIGN 65536u

/* One descriptor on the wire. Packed to make sure the compiler
 * doesn't insert padding between fields — firmware expects a
 * contiguous 16-byte stride. */
struct hailo_vdma_descriptor {
    uint32_t page_size_desc_control;
    uint32_t addr_l_rsvd_data_id;
    uint32_t addr_h;
    uint32_t remaining_page_size_status;
} __attribute__((packed));

_Static_assert(sizeof(struct hailo_vdma_descriptor) == 16,
               "VDMA descriptor must be exactly 16 bytes on the wire");

/*
 * A host-resident, power-of-2-sized descriptor list backed by a
 * single contiguous 64 KB-aligned DMA buffer. The VDMA engine
 * reads from `iova`; we populate through `descs`. `desc_count_mask`
 * is `desc_count - 1` — used for `(idx & mask)` ring-buffer walks.
 *
 * Cache ownership contract (audit F-02, 2026-04-24):
 *
 * Each descriptor is a coherent object with both host-written fields
 * (PageSize_DescControl, AddrL_rsvd_DataID, AddrH) and a device-
 * written field (RemainingPageSize_Status). The list lives in
 * cacheable PMM memory; manual maintenance bridges the host/device
 * split:
 *
 *   1. CPU writes descriptor fields via `descs[i] = ...` or
 *      hailo_vdma_program_descriptor / hailo_vdma_program_buffer.
 *   2. CPU runs `cache_clean(&descs[i..i+n], n*sizeof(descriptor))`
 *      to push the new bytes through to PoC. After this point the
 *      CPU MUST NOT write the same descriptor without a paired
 *      device-known-done synchronization (otherwise a dirty L2 line
 *      can later be flushed and stomp a device write).
 *   3. CPU publishes by ringing the channel doorbell (writing
 *      num_avail). Device reads the descriptor.
 *   4. Device writes RemainingPageSize_Status when it completes
 *      (or errors).
 *   5. CPU runs `cache_invalidate(&descs[i..i+n], ...)` before
 *      reading status. Skipping this invalidate causes stale L1/L2
 *      contents (e.g. the zero we wrote at allocation) to mask the
 *      device's status update.
 *
 * Future migration: if a coherent or non-cacheable mapping becomes
 * available for descriptor lists (Pi 5 has ncmem_alloc but it's a
 * 2 MB bump-allocator with no free), the manual clean/invalidate
 * dance can be eliminated. Tensors stay cacheable either way —
 * they're one-way buffers and the existing prepare_for_device /
 * prepare_for_host pattern is sufficient.
 */
struct hailo_vdma_desc_list {
    struct hailo_vdma_descriptor *descs;   /* host-cpu pointer */
    uint64_t                      iova;    /* device-side bus address */
    uint32_t                      desc_count;        /* power of 2 */
    uint32_t                      desc_count_mask;   /* desc_count - 1 */
    uint16_t                      desc_page_size;    /* bytes per descriptor */
    bool                          is_circular;       /* ring vs. one-shot */
};

/* Lower + upper bounds on `desc_count`. Lower = 2 (a list of one
 * descriptor makes no sense — you'd just do a single-shot DMA);
 * upper = 65536 (16-bit num_avail/num_proc counters wrap). */
#define HAILO_VDMA_MIN_DESC_COUNT    2u
#define HAILO_VDMA_MAX_DESC_COUNT    65536u

/* data_id written into each descriptor's AddrL_rsvd_DataID field AND
 * into the channel's BASE_DWORD at start time. Reference hw-ops pin
 * this at 0 for PCIe (`HAILO_PCIE_HOST_DMA_DATA_ID` in
 * ../slmos-reference-cache/hailo/hailo-pcie-common.h:35, used as `vdma_hw->ddr_data_id`
 * in hailo-vdma-common.c:294 and hailo-pcie.c:670). The `sys_index`
 * from the HEF is a stream identifier, NOT the descriptor's data_id —
 * mixing them up causes channel-vs-descriptor data_id divergence,
 * which the VDMA engine rejects silently (num_proc never advances). */
#define HAILO_VDMA_HOST_DMA_DATA_ID  0u

/*
 * Allocate a descriptor list. `desc_count` must be a power of 2
 * in [2, 65536]. `desc_page_size` is the number of bytes each
 * descriptor carries — typically matches the stream's
 * desc_page_size (set by CONFIG_STREAM for output or host-side
 * discretion for input). `is_circular` selects between ring-buffer
 * (desc_count_mask can modulo) and one-shot (list is walked once).
 *
 * On success, `*out` is fully populated and the backing buffer is
 * zero-initialized (safe to hand to the VDMA engine even before
 * the caller programs individual descriptors).
 *
 * Returns HAILO_OK / HAILO_ERR_INVAL (bad args or desc_count not
 * power of 2) / HAILO_ERR_NODEV (platform not installed) /
 * HAILO_ERR_NOMEM (platform allocator returned NULL).
 */
int hailo_vdma_desc_list_alloc(uint32_t desc_count,
                               uint16_t desc_page_size,
                               bool is_circular,
                               struct hailo_vdma_desc_list *out);

/*
 * Like hailo_vdma_desc_list_alloc but biases toward LOW physical
 * addresses via the platform's optional dma_alloc_low op. Falls back
 * to the default allocator on platforms without the low-bias variant.
 * Use for boundary-channel desc lists when the platform's inbound
 * translation window only reaches low physical RAM (Pi 5).
 */
int hailo_vdma_desc_list_alloc_low(uint32_t desc_count,
                                   uint16_t desc_page_size,
                                   bool is_circular,
                                   struct hailo_vdma_desc_list *out);

/*
 * Release a descriptor list previously returned by
 * hailo_vdma_desc_list_alloc. The handle is zeroed so stale uses
 * fault loudly. Safe to call on an already-zero handle (no-op).
 */
void hailo_vdma_desc_list_free(struct hailo_vdma_desc_list *list);

/*
 * Compute the backing-buffer size in bytes for a given desc_count.
 * Exposed so callers can sanity-check / log the allocation without
 * reaching inside the opaque handle. sizeof(descriptor) * desc_count,
 * rounded up to HAILO_VDMA_DESC_LIST_ALIGN for the DMA alloc.
 */
uint32_t hailo_vdma_desc_list_alloc_size(uint32_t desc_count);

/* -------------------------------------------------------------------------- */
/* Descriptor programming                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Program a single descriptor with (dma_address, page_size, data_id).
 * Bit layout mirrors hailo_vdma_program_descriptor in the reference
 * driver (hailo-vdma-common.c:139):
 *
 *   PageSize_DescControl     = (page_size << 8) | 0x02
 *   AddrL_rsvd_DataID        = (dma_address & 0xFFFFFFC0) | data_id
 *   AddrH                    = dma_address >> 32
 *   RemainingPageSize_Status = 0
 *
 * The low 6 bits of AddrL are masked off by hardware — descriptor
 * addresses must be 64-byte aligned. The `data_id` byte identifies
 * which on-chip data-source/sink this descriptor belongs to (set
 * up earlier via CONFIG_STREAM).
 *
 * No return value — the function is a pure field assignment; the
 * caller controls which descriptor slot is targeted.
 */
void hailo_vdma_program_descriptor(struct hailo_vdma_descriptor *desc,
                                   uint64_t dma_address,
                                   uint16_t page_size,
                                   uint8_t  data_id);

/*
 * Program a run of descriptors to cover a single contiguous DMA
 * buffer. Slices the buffer into chunks of `list->desc_page_size`
 * bytes; the final descriptor carries any residue (buffer_size %
 * page_size). Each descriptor's DMA address advances by
 * page_size from the previous.
 *
 * `starting_desc` is the index into list->descs[] at which to begin.
 * For a circular list, subsequent indices wrap via desc_count_mask.
 * For a non-circular list, the end of the list is a hard stop —
 * the function returns HAILO_ERR_INVAL if the buffer would overrun.
 *
 * Returns the number of descriptors programmed on success, or a
 * negative HAILO_ERR_* on failure. Caller decodes by comparing to
 * zero: anything < 0 is an error, >= 0 is a count.
 */
int hailo_vdma_program_buffer(struct hailo_vdma_desc_list *list,
                              uint32_t starting_desc,
                              uint64_t buffer_iova,
                              uint32_t buffer_size,
                              uint8_t  data_id);

/* -------------------------------------------------------------------------- */
/* Channel start / stop / submit                                               */
/* -------------------------------------------------------------------------- */

/* Per-channel register-block size within BAR2. Each channel owns
 * 32 contiguous bytes (reference CHANNEL_BASE_OFFSET macro). */
#define HAILO_VDMA_CHANNEL_STRIDE   32u

/* Max VDMA channels per engine on Hailo-8.
 * MAX_VDMA_CHANNELS_PER_ENGINE = 32 in the reference driver
 * (hailo-ioctl-common.h:20). Channel index space is a shared 0..31
 * pool — both H2D and D2H draw from it; direction is conveyed by
 * the action type (OPEN_BOUNDARY_INPUT_CHANNEL vs OUTPUT_CHANNEL),
 * not by the index range. `HAILO_PCIE_DMA_SRC_CHANNELS_BITMASK =
 * 0x0000FFFF` in the reference driver only controls per-channel
 * register layout within each channel's 32-byte window (host-side
 * regs first for channels 0..15, device-side first for 16..31 —
 * see get_channel_regs in hailo-vdma-common.c:576). It is NOT a
 * per-direction channel reservation. */
#define HAILO_VDMA_MAX_CHANNELS     32u

/* Sub-offsets within a channel's register block (host side).
 * Mirror the constants in `hailo-vdma-common.c:32-37` and
 * `hailo-vdma-common.h:21-26`:
 *
 *   0x00  BASE_DWORD   [CONTROL:8][data_id:3 at shift 8][depth:4 at shift 11][num_avail:16 at shift 16]
 *   0x04  NUM_PROC_DWORD  [num_proc:16]
 *   0x08  ALIGNED_ADDR_L_DWORD  [reserved:16][address_l:16 at shift 16]
 *   0x0C  ADDR_H_DWORD          [address_h:32]
 *
 * CONTROL lives in bits [7:0] of BASE_DWORD. DEPTH and DATA_ID also
 * live in BASE_DWORD but in different bit ranges — the reference
 * writes `(depth << 11) | (data_id << 8)` to it (which clears
 * CONTROL), then follows with a separate CONTROL-start RMW.
 * NUM_AVAIL lives in bits [31:16] of BASE_DWORD and is written
 * via RMW to preserve CONTROL + DEPTH + DATA_ID. */
#define HAILO_VDMA_CHANNEL_BASE_DWORD        0x00u
#define HAILO_VDMA_CHANNEL_NUM_PROC_DWORD    0x04u
#define HAILO_VDMA_CHANNEL_ALIGNED_ADDR_L    0x08u
#define HAILO_VDMA_CHANNEL_ADDR_H            0x0Cu

/* H2D / D2H split point. Channels [0, HAILO_VDMA_H2D_CHANNEL_COUNT)
 * are host-to-device (input boundary); channels
 * [HAILO_VDMA_H2D_CHANNEL_COUNT, HAILO_VDMA_MAX_CHANNELS) are
 * device-to-host. Mirrors HAILO_PCIE_DMA_SRC_CHANNELS_BITMASK =
 * 0x0000FFFF in the reference driver — the low 16 channel ids are
 * the H2D bank. */
#define HAILO_VDMA_H2D_CHANNEL_COUNT         16u

/* Each channel's 32-byte register window contains a {host, device}
 * sub-block pair. The HOST-side regs sit at +0x00 for H2D channels
 * and at +0x10 for D2H channels (the two halves swap order — see
 * get_channel_regs in hailo-vdma-common.c:576). All host-side
 * accesses (CONTROL, num_avail, num_proc, ALIGNED_ADDR_L, ADDR_H)
 * must add this offset to channel_base; missing it silently writes
 * the device-side mirror and the transfer never starts. */
#define HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_H2D  0x00u
#define HAILO_VDMA_CHANNEL_HOST_REGS_OFFSET_D2H  0x10u

/* Bit shifts within BASE_DWORD. */
#define HAILO_VDMA_CHANNEL_DATA_ID_SHIFT     8u     /* data_id bits */
#define HAILO_VDMA_CHANNEL_DESC_DEPTH_SHIFT  11u    /* depth bits */
#define HAILO_VDMA_CHANNEL_NUM_AVAIL_SHIFT   16u    /* num_avail u16 */

/*
 * Start a VDMA channel on a previously-programmed descriptor list.
 * Writes the list's IOVA (low 16 + high 32 bits), depth (log2 of
 * desc_count), data_id, then the CONTROL "start" bit.
 *
 * `channel_index` must be in [0, HAILO_VDMA_MAX_CHANNELS). `list`
 * must be a fully-populated list whose IOVA is 64 KB-aligned.
 * `data_id` is the per-channel identifier that CONFIG_STREAM
 * assigned.
 *
 * Returns HAILO_OK / HAILO_ERR_INVAL / HAILO_ERR_NODEV.
 */
int hailo_vdma_channel_start(uint8_t channel_index,
                             const struct hailo_vdma_desc_list *list,
                             uint8_t data_id);

/*
 * Stop a VDMA channel. Issues the pause-then-abort sequence from
 * the reference driver (hailo-vdma-common.c:932). Safe to call
 * even if the channel was never started — the stop sequence
 * tolerates an idle channel.
 */
void hailo_vdma_channel_stop(uint8_t channel_index);

/*
 * Publish `new_num_avail` to the VDMA engine and wait for
 * `num_proc` to reach it (completion) or `timeout_us` to elapse.
 * Polls via the platform's udelay between reads.
 *
 * Returns HAILO_OK on completion, HAILO_ERR_TIMEOUT if num_proc
 * didn't catch up, or HAILO_ERR_INVAL on bad args.
 *
 * Audit F-06 (2026-04-24) — known limitation: this API operates on
 * ABSOLUTE `num_avail` / `num_proc` values, not on per-channel
 * cursor deltas. That's correct for first-inference bring-up and
 * for any sequence whose lifetime cumulative descriptor count stays
 * below 65536 (the 16-bit register width). For long-lived steady-
 * state inference where counters wrap, a software cursor that
 * tracks (producer_idx, expected_completion_idx, wrap_count) is
 * needed instead. Adding that cursor while #253 is unresolved would
 * change the variable under test; once #253 lifts and steady-state
 * throughput becomes the target, this absolute-counter API should
 * be replaced by a cursor-aware path. Single-shot bring-up callers
 * keep using the current API.
 */
int hailo_vdma_submit_and_wait(uint8_t channel_index,
                               uint16_t new_num_avail,
                               uint32_t timeout_us);

/*
 * Diagnostic: dump the first `max_descs` entries of `list` plus the
 * `channel_index` host-register block to UART. `label` is a short
 * tag ("IN"/"OUT") embedded in each line to disambiguate channels
 * in mixed log output. Decodes PageSize_DescControl into page_size
 * and control byte (with named IRQ bits where set) so a byte-for-
 * byte comparison against hailo-vdma-common.c is possible without
 * post-processing. Intended for the Phase 8 inference-submit
 * investigation (issue #253) — strip once the blocker lifts.
 */
void hailo_vdma_dump_desc_list(const struct hailo_vdma_desc_list *list,
                               const char *label,
                               uint32_t max_descs);
void hailo_vdma_dump_channel_regs(uint8_t channel_index, const char *label);

/*
 * Diagnostic: read back per-descriptor status fields from DRAM after
 * a stuck submit, so we can tell whether fw fetched our descriptors.
 *
 * VDMA hardware writes the per-descriptor status into bits [7:0] of
 * `remaining_page_size_status` when it processes a descriptor (per
 * hailo-vdma-common.c:121-137 in the reference driver):
 *   bit 0 — DESC_DONE   (HW finished processing this descriptor)
 *   bit 1 — DESC_ERROR  (HW tried but got a DMA error)
 *
 * Reading the field after a num_proc-stuck submit answers a key
 * diagnostic question: did fw ever even try to fetch desc[0]?
 *   - status == 0 → fw never touched it (problem is upstream:
 *                    channel arming, num_avail latch, scheduler
 *                    not assigning credits)
 *   - DONE set    → fw fetched and processed it (problem is
 *                    downstream — periph engine accepting data)
 *   - ERROR set   → fw fetched but got a DMA fault (points at
 *                    IOVA / inbound-window translation)
 *
 * Issues a cache_invalidate over the desc range first because
 * descriptor write-backs come from device DMA and bypass host cache.
 */
void hailo_vdma_dump_desc_status(const struct hailo_vdma_desc_list *list,
                                 const char *label,
                                 uint32_t max_descs);

/*
 * Poll `channel_index` until its CONTROL byte reads START (0x01), or
 * `timeout_us` elapses. Used by the context-switch load path to know
 * when fw has finished processing ACTIVATION/PRELIMINARY and armed
 * the channel — before that, MMIO writes to the channel's regs don't
 * stick (base_dword reads back as 0). Returns HAILO_OK on arm,
 * HAILO_ERR_TIMEOUT on expiry, HAILO_ERR_INVAL for bad args.
 */
int hailo_vdma_channel_wait_armed(uint8_t channel_index, uint32_t timeout_us);

/*
 * RMW the NUM_AVAIL field (bits 31:16 of CHANNEL_BASE_DWORD) without
 * polling for num_proc afterwards. Reference
 * `hailo_vdma_set_num_avail` in hailo-vdma-common.c:426 does the same
 * read-modify-write; the difference vs hailo_vdma_submit_and_wait is
 * that this helper returns immediately, leaving it to the caller to
 * poll num_proc on its own schedule. Used to pre-arm the OUTPUT
 * boundary channel before submitting INPUT, matching HailoRT's
 * observed host-side order (see ../slmos-reference-cache/derivatives/hailort-traces/hailort-v4.23.0-vdma-
 * mnist-pi5.txt). Returns HAILO_OK / HAILO_ERR_INVAL.
 */
int hailo_vdma_write_num_avail(uint8_t channel_index, uint16_t num_avail);

/*
 * Poll `channel_index`'s num_proc until it advances by at least
 * `target_num_proc` descriptors relative to the count observed when
 * the function was entered, or `timeout_us` elapses. Unlike
 * hailo_vdma_submit_and_wait this does NOT write num_avail — use
 * when fw is driving DMA internally (e.g. via PRELIMINARY's
 * FETCH_CFG_CHANNEL_DESCRIPTORS actions) and the host just needs to
 * observe completion. Returns HAILO_OK on reach, HAILO_ERR_TIMEOUT
 * otherwise.
 */
int hailo_vdma_channel_wait_proc(uint8_t channel_index,
                                 uint16_t target_num_proc,
                                 uint32_t timeout_us);

/*
 * OR `ctrl_mask` (lower 8 bits of page_size_desc_control) into the
 * first descriptor of `list`. Mirrors the reference driver's pattern
 * in hailo_vdma_launch_transfer (hailo-vdma-common.c:505-506):
 *
 *   desc_list->desc_list[first_desc].PageSize_DescControl |=
 *       get_interrupts_bitmask(vdma_hw, first_interrupts_domain, ...);
 *
 * For boundary transfers the typical choice is the DEVICE-side IRQ
 * bitmask (0x10 | 0x04 | 0x08 = 0x1C) so the NPU's DMA engine sees
 * "new transfer starting here". Pair with the LAST descriptor's
 * HOST-side bits set at program time to close the round-trip.
 *
 * Cache-flushes the descriptor after the OR so fw reads the new
 * control byte. Safe to call multiple times; idempotent on already-
 * set bits.
 *
 * Returns HAILO_OK / HAILO_ERR_INVAL.
 */
int hailo_vdma_arm_first_desc_irq(struct hailo_vdma_desc_list *list,
                                  uint32_t starting_desc,
                                  uint32_t ctrl_mask);

#endif /* AI_ACCEL_HAILO_VDMA_H */
