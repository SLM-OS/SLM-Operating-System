/*
 * hailo_tensor.c — DMA tensor buffer allocator.
 *
 * Thin wrapper over hailo_platform->dma_alloc / dma_free plus the
 * cache_clean / cache_invalidate hooks. See the header for
 * per-function contracts and the host-vs-device prepare semantics.
 */

#include "hailo_tensor.h"
#include "hailo.h"
#include "hailo_internal.h"
#include "debug.h"
#include <string.h>

uint32_t hailo_tensor_size_from_shape(uint32_t padded_height,
                                      uint32_t padded_width,
                                      uint32_t padded_features,
                                      uint32_t data_bytes)
{
    if (padded_height == 0 || padded_width == 0
     || padded_features == 0 || data_bytes == 0) {
        return 0;
    }
    /* Overflow-safe multiply. We need height * width * features *
     * data_bytes to fit in uint32_t — any path that overflows
     * returns 0 so the caller can reject. Carrying intermediates
     * in uint64_t and checking each step keeps the guard simple. */
    uint64_t acc = (uint64_t)padded_height * (uint64_t)padded_width;
    if (acc > UINT32_MAX) return 0;
    acc *= (uint64_t)padded_features;
    if (acc > UINT32_MAX) return 0;
    acc *= (uint64_t)data_bytes;
    if (acc > UINT32_MAX) return 0;
    return (uint32_t)acc;
}

/* Shared body of hailo_tensor_alloc and hailo_tensor_alloc_low. The
 * two only differ in which platform allocator they invoke; everything
 * else (size rounding, alignment validation, zero-init) is identical. */
static int tensor_alloc_inner(uint32_t tensor_bytes, struct hailo_tensor *out,
                              bool prefer_low)
{
    if (!out) return HAILO_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    if (tensor_bytes == 0) return HAILO_ERR_INVAL;
    if (!hailo_platform || !hailo_platform->dma_alloc
                        || !hailo_platform->dma_free) {
        return HAILO_ERR_NODEV;
    }

    /* Round the allocation up to HAILO_TENSOR_DMA_ALIGN so the
     * buffer is sized to the VDMA page granularity. Descriptor
     * rings work in multiples of this on Hailo-8. */
    uint32_t alloc_size = (tensor_bytes + HAILO_TENSOR_DMA_ALIGN - 1u)
                         & ~(HAILO_TENSOR_DMA_ALIGN - 1u);
    /* Wrap guard. HAILO_TENSOR_DMA_ALIGN is 4096, so wrap happens
     * only for tensor_bytes > UINT32_MAX - 4095 — unrealistic but
     * worth a single branch. */
    if (alloc_size < tensor_bytes) return HAILO_ERR_INVAL;

    uint64_t iova = 0;
    /* Pick allocator: low-bias variant if requested AND available,
     * else fall back to default. Platforms without dma_alloc_low
     * (e.g. coherent IOMMU systems) silently use dma_alloc — caller
     * preference is best-effort, not a hard requirement. */
    void *(*alloc_fn)(size_t, size_t, uint64_t *) = hailo_platform->dma_alloc;
    if (prefer_low && hailo_platform->dma_alloc_low) {
        alloc_fn = hailo_platform->dma_alloc_low;
    }
    void *cpu = alloc_fn((size_t)alloc_size,
                         (size_t)HAILO_TENSOR_DMA_ALIGN,
                         &iova);
    if (!cpu) return HAILO_ERR_NOMEM;
    /* Defensive: catch a mis-behaving platform allocator that
     * ignored the alignment hint. VDMA descriptors assume every
     * tensor page is HAILO_TENSOR_DMA_ALIGN-aligned; a less-
     * aligned return would corrupt descriptor addresses silently.
     * Free and fail rather than hand the caller a bad buffer. */
    if ((uintptr_t)cpu & (HAILO_TENSOR_DMA_ALIGN - 1u)) {
        WARN("hailo: tensor allocator returned unaligned ptr %p "
             "(need %u)", cpu, HAILO_TENSOR_DMA_ALIGN);
        hailo_platform->dma_free(cpu, (size_t)alloc_size,
                                 (size_t)HAILO_TENSOR_DMA_ALIGN);
        return HAILO_ERR_NOMEM;
    }

    /* Zero-init. A freshly-allocated output tensor should read as
     * zeros before the first inference. Also avoids leaking prior
     * contents on NC memory (which we allocate from a bump pool
     * on Pi 5 — no reuse, but cheap belt-and-suspenders). */
    memset(cpu, 0, alloc_size);

    out->cpu_addr     = cpu;
    out->iova         = iova;
    out->tensor_bytes = tensor_bytes;
    out->alloc_size   = alloc_size;
    out->align        = HAILO_TENSOR_DMA_ALIGN;
    return HAILO_OK;
}

int hailo_tensor_alloc(uint32_t tensor_bytes, struct hailo_tensor *out)
{
    return tensor_alloc_inner(tensor_bytes, out, /*prefer_low=*/false);
}

int hailo_tensor_alloc_low(uint32_t tensor_bytes, struct hailo_tensor *out)
{
    return tensor_alloc_inner(tensor_bytes, out, /*prefer_low=*/true);
}

void hailo_tensor_free(struct hailo_tensor *t)
{
    if (!t || !t->cpu_addr) return;
    if (hailo_platform && hailo_platform->dma_free) {
        hailo_platform->dma_free(t->cpu_addr,
                                 (size_t)t->alloc_size,
                                 (size_t)t->align);
    }
    memset(t, 0, sizeof(*t));
}

void hailo_tensor_prepare_for_device(const struct hailo_tensor *t)
{
    if (!t || !t->cpu_addr) return;
    if (hailo_platform && hailo_platform->cache_clean) {
        hailo_platform->cache_clean(t->cpu_addr, (size_t)t->tensor_bytes);
    }
}

void hailo_tensor_prepare_for_host(struct hailo_tensor *t)
{
    if (!t || !t->cpu_addr) return;
    if (hailo_platform && hailo_platform->cache_invalidate) {
        hailo_platform->cache_invalidate(t->cpu_addr, (size_t)t->tensor_bytes);
    }
}
