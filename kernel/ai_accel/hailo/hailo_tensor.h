/*
 * hailo_tensor.h — DMA-addressable tensor buffer API for the Hailo
 * inference path.
 *
 * Hailo's VDMA engine consumes physically-contiguous buffers
 * addressed by IOVA (the device-side address). For a Hailo-8 model,
 * the caller needs one input buffer per input pad and one output
 * buffer per output pad, each sized to the pad's padded dims.
 * This file wraps the platform's dma_alloc / dma_free / cache
 * hooks into a typed `hailo_tensor` handle so:
 *
 *   - Input-tensor prep (host → device): write data, cache_clean
 *     before the descriptor is posted. Firmware reads device-side
 *     through the VDMA engine; our CPU's dirty cache lines must be
 *     written out to DRAM first.
 *   - Output-tensor read (device → host): firmware writes via
 *     VDMA, we cache_invalidate before reading so the CPU doesn't
 *     pick up stale cache lines from before the transfer.
 *
 * Allocation goes through hailo_platform->dma_alloc. On Pi 5 that's
 * the NC (non-cacheable) bump allocator — the cache-maintenance
 * hooks compile to no-ops there because non-cacheable memory
 * doesn't need flushing. On a future system-with-coherent-IOMMU
 * platform, dma_alloc could return cacheable-with-CSID memory and
 * the cache hooks would do real work.
 *
 * Thread-safety contract: a tensor is owned by exactly one caller.
 * The driver doesn't serialize access; if two tasks need to share
 * a tensor, they coordinate above this layer.
 */

#ifndef AI_ACCEL_HAILO_TENSOR_H
#define AI_ACCEL_HAILO_TENSOR_H

#include <stddef.h>
#include <stdint.h>

#include "hailo.h"

/*
 * Opaque tensor handle. Fields are visible for test inspection
 * (the test harness asserts on iova and size directly) but callers
 * should treat the struct as opaque and go through the API.
 *
 * `cpu_addr` is what the host writes/reads. `iova` is what the
 * VDMA engine's descriptor targets — on Pi 5 today both are the
 * same physical address (identity mapping) but the distinction
 * matters for future platforms behind an IOMMU.
 *
 * `alloc_size` captures the rounded-up size actually allocated
 * so hailo_tensor_free can hand it back to the platform
 * allocator. `tensor_bytes` is the logical payload size; anything
 * in `[tensor_bytes, alloc_size)` is padding the caller shouldn't
 * touch.
 */
struct hailo_tensor {
    void    *cpu_addr;
    uint64_t iova;
    uint32_t tensor_bytes;
    uint32_t alloc_size;
    uint32_t align;
};

/* Minimum alignment for DMA tensor buffers. HailoRT's VDMA engine
 * requires page-aligned descriptor addresses (Hailo-8 uses 4 KB
 * pages for DMA); smaller alignments work for intermediate copies
 * but not for direct DMA. Match the page size so the same buffer
 * can be used for either purpose. */
#define HAILO_TENSOR_DMA_ALIGN 4096u

/*
 * Compute the byte size of a tensor from a pad's padded dims.
 * Matches what HailoRT's inference engine submits to the device:
 * padded_height * padded_width * padded_features * data_bytes.
 * `data_bytes` defaults to 1 for Hailo-8 INT8 models (caller
 * passes the value from the hef_pad_info / ProtoHEFEdgeLayerBase
 * data_bytes field when it's available; for now the caller
 * typically passes 1).
 *
 * Returns 0 if any dim is zero OR the multiplication would
 * overflow uint32_t — the caller should refuse to allocate a
 * zero/overflowing tensor.
 */
uint32_t hailo_tensor_size_from_shape(uint32_t padded_height,
                                      uint32_t padded_width,
                                      uint32_t padded_features,
                                      uint32_t data_bytes);

/*
 * Allocate a tensor buffer of `tensor_bytes` through the platform
 * DMA allocator. Rounds the allocation up to a 4 KB page. Zeros
 * the buffer on success (so a freshly-allocated output tensor
 * reads as zeros before any inference).
 *
 * Returns HAILO_OK / HAILO_ERR_INVAL (bad args) / HAILO_ERR_NODEV
 * (platform not installed) / HAILO_ERR_NOMEM (allocator returned
 * NULL). On success `*out` is fully populated.
 */
int hailo_tensor_alloc(uint32_t tensor_bytes, struct hailo_tensor *out);

/*
 * Like hailo_tensor_alloc but biases toward LOW physical addresses
 * via the platform's optional dma_alloc_low op. Falls back to the
 * default allocator on platforms that don't expose a low-bias variant
 * — the bias is best-effort, not a hard requirement.
 *
 * Use this for boundary I/O tensors when the platform's PCIe inbound
 * translation only reaches the bottom of physical RAM (Pi 5).
 * Per-call: no global state, safe under concurrent loads.
 */
int hailo_tensor_alloc_low(uint32_t tensor_bytes, struct hailo_tensor *out);

/*
 * Release a tensor previously returned by hailo_tensor_alloc. The
 * handle is zeroed so stale uses fail loudly. Safe to call on an
 * already-zero handle (no-op).
 */
void hailo_tensor_free(struct hailo_tensor *t);

/*
 * Flush CPU writes to DRAM so the device sees them. Call before
 * posting a descriptor that reads from the tensor (input tensor
 * before an inference submit). No-op on non-cacheable NC memory.
 */
void hailo_tensor_prepare_for_device(const struct hailo_tensor *t);

/*
 * Invalidate the CPU cache lines covering the tensor so a
 * subsequent host read picks up what the device just wrote. Call
 * after an output-tensor VDMA completion. No-op on NC memory.
 */
void hailo_tensor_prepare_for_host(struct hailo_tensor *t);

#endif /* AI_ACCEL_HAILO_TENSOR_H */
