/*
 * GPU Stub Driver
 *
 * No-op GPU driver for platforms without GPU hardware (QEMU virt).
 * Provides the full GPU API but all operations are simulated:
 * - Memory allocation uses regular PMM pages
 * - Cache operations are performed (useful for testing coherency code)
 * - No actual GPU compute capability
 *
 * This allows SLM-OS code to use the GPU API uniformly, with the
 * stub silently succeeding on non-GPU platforms.
 */

#include "gpu.h"
#include "../include/pmm.h"
#include "../include/uart.h"
#include <stddef.h>

/*
 * Stub driver state
 */
static bool stub_initialized = false;

/*
 * Stub initialization.
 * Always succeeds - there's no hardware to initialize.
 */
static int stub_init(void)
{
    uart_puts("[GPU-STUB] Initializing stub driver (no GPU hardware)\n");
    stub_initialized = true;
    return GPU_OK;
}

/*
 * Stub shutdown.
 */
static void stub_shutdown(void)
{
    uart_puts("[GPU-STUB] Shutting down stub driver\n");
    stub_initialized = false;
}

/*
 * Get stub driver info.
 */
static int stub_get_info(gpu_info_t *info)
{
    if (!info) {
        return GPU_ERR_INVALID_PARAM;
    }

    info->name = "stub";
    info->device = "No GPU (QEMU virt)";
    info->capabilities = GPU_CAP_NONE;
    info->cuda_cores = 0;
    info->tensor_cores = 0;
    info->memory_size = 0;
    info->unified_memory = true;  /* CPU memory is "GPU" memory */

    return GPU_OK;
}

/*
 * Allocate "GPU-accessible" memory.
 *
 * Since there's no real GPU, we just allocate regular pages from PMM.
 * The cpu_addr and gpu_addr are the same (identity mapped).
 * Cache operations still work for testing the coherency code path.
 *
 * For 2MB alignment, we over-allocate and return an aligned subset.
 * This is wasteful but acceptable for the stub driver (test/QEMU only).
 */
static int stub_alloc(size_t size, uint32_t flags, gpu_buffer_t *buf)
{
    if (!buf || size == 0) {
        return GPU_ERR_INVALID_PARAM;
    }

    /* Calculate pages needed */
    size_t page_size = 4096;
    size_t pages = (size + page_size - 1) / page_size;
    void *addr;
    size_t actual_pages;

    /* Use 2MB alignment if requested (for huge page compatibility) */
    if (flags & GPU_MEM_ALIGN_2MB) {
        /*
         * Over-allocate to guarantee 2MB alignment.
         * We need size + 2MB padding to ensure we can find an aligned address.
         */
        size_t align_2mb = 2 * 1024 * 1024;
        size_t align_pages = align_2mb / page_size;  /* 512 pages */

        /* Round size up to 2MB */
        size_t aligned_size = ((size + align_2mb - 1) / align_2mb) * align_2mb;
        size_t aligned_pages = aligned_size / page_size;

        /* Allocate extra pages for alignment padding */
        actual_pages = aligned_pages + align_pages;
        addr = pmm_alloc_pages(actual_pages);
        if (!addr) {
            uart_puts("[GPU-STUB] Failed to allocate aligned memory\n");
            return GPU_ERR_NO_MEMORY;
        }

        /* Find 2MB aligned address within allocated region */
        uintptr_t raw_addr = (uintptr_t)addr;
        uintptr_t aligned_addr = (raw_addr + align_2mb - 1) & ~(align_2mb - 1);

        buf->cpu_addr = (void *)aligned_addr;
        buf->gpu_addr = aligned_addr;
        buf->size = aligned_size;
        buf->flags = flags;

        /*
         * Note: We lose track of the original allocation address.
         * For a real driver, we'd store it. For the stub, we accept
         * that gpu_free won't reclaim the padding pages properly.
         * This is fine for testing purposes.
         */
    } else {
        /* Standard allocation without alignment */
        actual_pages = pages;
        addr = pmm_alloc_pages(actual_pages);
        if (!addr) {
            uart_puts("[GPU-STUB] Failed to allocate memory\n");
            return GPU_ERR_NO_MEMORY;
        }

        buf->cpu_addr = addr;
        buf->gpu_addr = (uint64_t)(uintptr_t)addr;
        buf->size = pages * page_size;
        buf->flags = flags;
    }

    return GPU_OK;
}

/*
 * Free "GPU-accessible" memory.
 */
static void stub_free(gpu_buffer_t *buf)
{
    if (!buf || !buf->cpu_addr) {
        return;
    }

    size_t page_size = 4096;
    size_t pages = buf->size / page_size;

    pmm_free_pages(buf->cpu_addr, pages);

    buf->cpu_addr = NULL;
    buf->gpu_addr = 0;
    buf->size = 0;
}

/*
 * Sync buffer for GPU access.
 *
 * Even though there's no GPU, we perform the cache clean.
 * This exercises the cache coherency code path for testing.
 */
static void stub_sync_for_gpu(gpu_buffer_t *buf)
{
    if (!buf || !buf->cpu_addr) {
        return;
    }

    /* Clean cache - write back dirty lines */
    cache_clean_range(buf->cpu_addr, buf->size);
}

/*
 * Sync buffer for CPU access.
 *
 * Invalidate cache lines so CPU sees "GPU" (memory) state.
 */
static void stub_sync_for_cpu(gpu_buffer_t *buf)
{
    if (!buf || !buf->cpu_addr) {
        return;
    }

    /* Invalidate cache - discard cached data */
    cache_invalidate_range(buf->cpu_addr, buf->size);
}

/*
 * Stub GPU driver structure
 */
const struct gpu_driver gpu_stub_driver = {
    .name          = "stub",
    .init          = stub_init,
    .shutdown      = stub_shutdown,
    .get_info      = stub_get_info,
    .alloc         = stub_alloc,
    .free          = stub_free,
    .sync_for_gpu  = stub_sync_for_gpu,
    .sync_for_cpu  = stub_sync_for_cpu,
    .submit        = NULL,  /* Not supported */
    .wait          = NULL,  /* Not supported */
};
