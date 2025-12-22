/*
 * GPU Driver Core
 *
 * Platform-agnostic GPU driver infrastructure. Manages driver registration
 * and provides the global API that delegates to the active driver.
 */

#include "gpu.h"
#include "../include/uart.h"
#include <stddef.h>

/* Currently registered GPU driver */
static const struct gpu_driver *gpu_drv = NULL;
static bool gpu_initialized = false;

/*
 * Register a GPU driver.
 *
 * Called by platform initialization code to register the appropriate
 * driver for the current hardware (stub for QEMU, tegra for Jetson).
 */
void gpu_register_driver(const struct gpu_driver *drv)
{
    if (drv) {
        gpu_drv = drv;
        uart_printf("[GPU] Registered driver: %s\n", drv->name);
    }
}

/*
 * Initialize the GPU subsystem.
 *
 * Calls the registered driver's init function. Must be called after
 * gpu_register_driver() and before any other GPU operations.
 */
int gpu_init(void)
{
    if (!gpu_drv) {
        uart_puts("[GPU] ERROR: No driver registered\n");
        return GPU_ERR_NO_DEVICE;
    }

    if (gpu_initialized) {
        return GPU_OK;  /* Already initialized */
    }

    uart_printf("[GPU] Initializing %s driver...\n", gpu_drv->name);

    int ret = GPU_OK;
    if (gpu_drv->init) {
        ret = gpu_drv->init();
    }

    if (ret == GPU_OK) {
        gpu_initialized = true;
        uart_puts("[GPU] Initialization complete\n");
    } else {
        uart_printf("[GPU] Initialization failed: %d\n", ret);
    }

    return ret;
}

/*
 * Shutdown the GPU subsystem.
 */
void gpu_shutdown(void)
{
    if (!gpu_initialized || !gpu_drv) {
        return;
    }

    if (gpu_drv->shutdown) {
        gpu_drv->shutdown();
    }

    gpu_initialized = false;
    uart_puts("[GPU] Shutdown complete\n");
}

/*
 * Get GPU information.
 */
int gpu_get_info(gpu_info_t *info)
{
    if (!info) {
        return GPU_ERR_INVALID_PARAM;
    }

    if (!gpu_drv) {
        return GPU_ERR_NO_DEVICE;
    }

    if (gpu_drv->get_info) {
        return gpu_drv->get_info(info);
    }

    return GPU_ERR_NOT_SUPPORTED;
}

/*
 * Allocate GPU-accessible memory.
 *
 * Returns a buffer that can be accessed by both CPU and GPU.
 * The caller must use gpu_sync_for_gpu() before GPU access and
 * gpu_sync_for_cpu() after GPU writes.
 */
int gpu_alloc(size_t size, uint32_t flags, gpu_buffer_t *buf)
{
    if (!buf || size == 0) {
        return GPU_ERR_INVALID_PARAM;
    }

    if (!gpu_initialized || !gpu_drv) {
        return GPU_ERR_NOT_INIT;
    }

    if (gpu_drv->alloc) {
        return gpu_drv->alloc(size, flags, buf);
    }

    return GPU_ERR_NOT_SUPPORTED;
}

/*
 * Free GPU-accessible memory.
 */
void gpu_free(gpu_buffer_t *buf)
{
    if (!buf || !gpu_initialized || !gpu_drv) {
        return;
    }

    if (gpu_drv->free) {
        gpu_drv->free(buf);
    }
}

/*
 * Sync buffer for GPU access.
 *
 * Call this after CPU writes to the buffer, before GPU reads.
 * Cleans (writes back) any dirty cache lines.
 */
void gpu_sync_for_gpu(gpu_buffer_t *buf)
{
    if (!buf || !gpu_drv) {
        return;
    }

    if (gpu_drv->sync_for_gpu) {
        gpu_drv->sync_for_gpu(buf);
    }
}

/*
 * Sync buffer for CPU access.
 *
 * Call this after GPU writes to the buffer, before CPU reads.
 * Invalidates cache lines so CPU sees GPU's writes.
 */
void gpu_sync_for_cpu(gpu_buffer_t *buf)
{
    if (!buf || !gpu_drv) {
        return;
    }

    if (gpu_drv->sync_for_cpu) {
        gpu_drv->sync_for_cpu(buf);
    }
}

/*
 * Check if GPU is available.
 *
 * Returns true if a GPU driver is registered and initialized.
 * Note: The stub driver returns true but has no compute capability.
 */
bool gpu_available(void)
{
    return gpu_initialized && gpu_drv != NULL;
}
