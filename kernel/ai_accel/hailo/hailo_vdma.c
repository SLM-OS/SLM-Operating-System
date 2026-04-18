/*
 * hailo_vdma.c — Hailo VDMA descriptor-list allocator.
 *
 * See hailo_vdma.h for the wire-format rationale and sizing rules.
 * This file owns just the list allocator; descriptor programming
 * and channel start/stop land in follow-up commits on the Phase 5.4
 * stack.
 */

#include "hailo_vdma.h"
#include "hailo.h"
#include "hailo_internal.h"
#include "debug.h"
#include <string.h>

/* Power-of-2 check: n is a power of 2 iff n != 0 and n & (n-1) == 0. */
static inline bool is_power_of_two(uint32_t n)
{
    return (n != 0) && ((n & (n - 1u)) == 0);
}

uint32_t hailo_vdma_desc_list_alloc_size(uint32_t desc_count)
{
    /* Raw buffer = desc_count * 16. Round up to the 64 KB DMA
     * alignment so the VDMA engine's HOST_DESC_BASE_ADDR truncation
     * doesn't land in the middle of a descriptor. For typical
     * desc_count values (64..4096) the rounded size dominates;
     * the trade-off is acceptable given the 64 KB align is
     * firmware-mandated. */
    uint32_t raw = desc_count * (uint32_t)sizeof(struct hailo_vdma_descriptor);
    uint32_t aligned = (raw + HAILO_VDMA_DESC_LIST_ALIGN - 1u)
                     & ~(HAILO_VDMA_DESC_LIST_ALIGN - 1u);
    return aligned;
}

int hailo_vdma_desc_list_alloc(uint32_t desc_count,
                               uint16_t desc_page_size,
                               bool is_circular,
                               struct hailo_vdma_desc_list *out)
{
    if (!out) return HAILO_ERR_INVAL;
    memset(out, 0, sizeof(*out));

    /* Caller contract: desc_count is power-of-2 in [MIN, MAX].
     * HW-fixed — the VDMA engine uses (index & mask) to walk the
     * ring and a non-power-of-2 list would corrupt on wraparound.
     * MIN=2 because a list of one is meaningless (just single-shot).
     * MAX=65536 because num_avail/num_proc are 16-bit counters. */
    if (desc_count < HAILO_VDMA_MIN_DESC_COUNT
     || desc_count > HAILO_VDMA_MAX_DESC_COUNT
     || !is_power_of_two(desc_count)) {
        return HAILO_ERR_INVAL;
    }
    if (desc_page_size == 0) return HAILO_ERR_INVAL;

    if (!hailo_platform || !hailo_platform->dma_alloc
                        || !hailo_platform->dma_free) {
        return HAILO_ERR_NODEV;
    }

    uint32_t alloc_size = hailo_vdma_desc_list_alloc_size(desc_count);

    uint64_t iova = 0;
    void *cpu = hailo_platform->dma_alloc((size_t)alloc_size,
                                          (size_t)HAILO_VDMA_DESC_LIST_ALIGN,
                                          &iova);
    if (!cpu) return HAILO_ERR_NOMEM;

    /* Zero-init the whole buffer. A descriptor with all-zero fields
     * is inert (page size 0 → no transfer), which is what we want
     * until the caller programs real contents. */
    memset(cpu, 0, alloc_size);

    out->descs            = (struct hailo_vdma_descriptor *)cpu;
    out->iova             = iova;
    out->desc_count       = desc_count;
    out->desc_count_mask  = desc_count - 1u;
    out->desc_page_size   = desc_page_size;
    out->is_circular      = is_circular;
    return HAILO_OK;
}

void hailo_vdma_desc_list_free(struct hailo_vdma_desc_list *list)
{
    if (!list || !list->descs) return;
    if (hailo_platform && hailo_platform->dma_free) {
        uint32_t alloc_size = hailo_vdma_desc_list_alloc_size(list->desc_count);
        hailo_platform->dma_free(list->descs,
                                 (size_t)alloc_size,
                                 (size_t)HAILO_VDMA_DESC_LIST_ALIGN);
    }
    memset(list, 0, sizeof(*list));
}
