/*
 * gpu_nvidia.c - NVIDIA Ampere GPU probe and driver
 *
 * Platform-agnostic GPU identification for NVIDIA Ampere GPUs.
 * Works on both Jetson Orin (integrated) and discrete PCIe GPUs (RTX 3050).
 *
 * Currently implements:
 *   - GPU identification via NV_PMC_BOOT_0 / BOOT_42
 *   - Chip variant decoding (GA10B, GA106, etc.)
 *
 * Future:
 *   - GSP firmware loading
 *   - Compute command submission
 *   - Tensor core operations
 */

#include "gpu_nvidia.h"
#include "gpu.h"
#include "../include/uart.h"
#include "../include/debug.h"
#include <stdbool.h>

/* MMIO register access */
static inline uint32_t gpu_read32(uintptr_t base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

/*
 * Probe an NVIDIA GPU at the given MMIO base address.
 *
 * Reads identification registers and populates the info struct.
 * Returns 0 if a valid GPU is detected, -1 otherwise.
 */
int nv_gpu_probe(uintptr_t mmio_base, struct nv_gpu_info *info)
{
    /* Clear info struct */
    info->present = false;
    info->is_ampere = false;
    info->boot0 = 0;
    info->boot42 = 0;
    info->chip_id = 0;

    /* Read primary identification register */
    info->boot0 = gpu_read32(mmio_base, NV_PMC_BOOT_0);

    /* 0xFFFFFFFF means GPU is not present, in reset, or bus error */
    if (info->boot0 == NV_GPU_NOT_PRESENT) {
        return -1;
    }

    /* Decode BOOT_0 */
    info->architecture = nv_boot0_arch(info->boot0);
    info->implementation = nv_boot0_impl(info->boot0);
    info->major_rev = nv_boot0_major_rev(info->boot0);
    info->minor_rev = nv_boot0_minor_rev(info->boot0);

    /* Read extended identification (cleaner layout) */
    info->boot42 = gpu_read32(mmio_base, NV_PMC_BOOT_42);
    info->chip_id = nv_boot42_chip_id(info->boot42);

    /* Read device enable status */
    info->enable = gpu_read32(mmio_base, NV_PMC_ENABLE);

    info->present = true;
    info->is_ampere = (info->architecture == NV_GPU_ARCHITECTURE_AMPERE);

    return 0;
}

/*
 * Print GPU probe results.
 */
void nv_gpu_print_info(const struct nv_gpu_info *info)
{
    if (!info->present) {
        uart_puts("[GPU-NV] No NVIDIA GPU detected\n");
        return;
    }

    uart_printf("[GPU-NV] NVIDIA GPU detected: %s\n", nv_chip_name(info->chip_id));
    uart_printf("[GPU-NV]   BOOT_0:  0x%08lx\n", (unsigned long)info->boot0);
    uart_printf("[GPU-NV]   BOOT_42: 0x%08lx\n", (unsigned long)info->boot42);
    uart_printf("[GPU-NV]   Chip ID: 0x%03lx (arch=0x%02lx, impl=0x%lx)\n",
                (unsigned long)info->chip_id,
                (unsigned long)info->architecture,
                (unsigned long)info->implementation);
    uart_printf("[GPU-NV]   Silicon: rev %lu.%lu\n",
                (unsigned long)info->major_rev,
                (unsigned long)info->minor_rev);
    uart_printf("[GPU-NV]   Ampere:  %s\n", info->is_ampere ? "yes" : "no");
    uart_printf("[GPU-NV]   Enable:  0x%08lx\n", (unsigned long)info->enable);
}

/* ============================================================================
 * GPU Driver Interface (for gpu.h registration)
 *
 * This implements the gpu_driver struct so the NVIDIA probe can be used
 * through the standard GPU subsystem interface.
 * ============================================================================ */

static uintptr_t nvidia_mmio_base = 0;
static struct nv_gpu_info nvidia_gpu_info;

static int nvidia_init(void)
{
    if (nvidia_mmio_base == 0) {
        WARN("GPU-NV: No MMIO base configured");
        return GPU_ERR_NO_DEVICE;
    }

    INFO("GPU-NV: Probing NVIDIA GPU at 0x%lx...", (unsigned long)nvidia_mmio_base);

    int ret = nv_gpu_probe(nvidia_mmio_base, &nvidia_gpu_info);
    if (ret != 0) {
        WARN("GPU-NV: GPU not detected (BOOT_0 = 0xFFFFFFFF)");
        return GPU_ERR_NO_DEVICE;
    }

    nv_gpu_print_info(&nvidia_gpu_info);

    if (!nvidia_gpu_info.is_ampere) {
        WARN("GPU-NV: Unexpected architecture 0x%lx (expected Ampere 0x17)",
             (unsigned long)nvidia_gpu_info.architecture);
    }

    return GPU_OK;
}

static void nvidia_shutdown(void)
{
    /* Nothing to clean up for probe-only driver */
}

static int nvidia_get_info(struct gpu_info *info)
{
    if (!nvidia_gpu_info.present) {
        return GPU_ERR_NO_DEVICE;
    }

    info->name = nv_chip_name(nvidia_gpu_info.chip_id);
    info->device = "NVIDIA Ampere GPU";

    /* Capabilities based on chip */
    info->capabilities = GPU_CAP_COMPUTE | GPU_CAP_TENSOR_CORES;
    if (nvidia_gpu_info.chip_id == NV_CHIP_ID_GA10B) {
        info->capabilities |= GPU_CAP_UNIFIED_MEMORY;
        info->unified_memory = true;
        info->cuda_cores = 1024;
        info->tensor_cores = 32;
    } else if (nvidia_gpu_info.chip_id == NV_CHIP_ID_GA106) {
        info->unified_memory = false;
        info->cuda_cores = 2560;  /* RTX 3060 variant */
        info->tensor_cores = 80;
    }

    info->memory_size = 0;  /* Reported after GSP init (future) */

    return GPU_OK;
}

const struct gpu_driver gpu_nvidia_driver = {
    .name          = "nvidia",
    .init          = nvidia_init,
    .shutdown      = nvidia_shutdown,
    .get_info      = nvidia_get_info,
    .alloc         = NULL,  /* Future: GPU memory allocation */
    .free          = NULL,
    .sync_for_gpu  = NULL,
    .sync_for_cpu  = NULL,
    .submit        = NULL,
    .wait          = NULL,
};

/*
 * Set the MMIO base address for the NVIDIA GPU.
 * Must be called before gpu_register_driver(&gpu_nvidia_driver).
 */
void nvidia_gpu_set_mmio_base(uintptr_t base)
{
    nvidia_mmio_base = base;
}
