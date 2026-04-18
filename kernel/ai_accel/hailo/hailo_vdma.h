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
 * `docs/reference/hailo-vdma-common.h:35`:
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

#endif /* AI_ACCEL_HAILO_VDMA_H */
