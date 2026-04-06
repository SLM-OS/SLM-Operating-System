/*
 * gpu_nvidia.h - NVIDIA GPU register definitions (Ampere architecture)
 *
 * Shared definitions for NVIDIA Ampere GPUs:
 *   - Jetson Orin Nano (GA10B, integrated, MMIO at 0x17000000)
 *   - RTX 3050/3060 (GA106, discrete, PCIe BAR0)
 *
 * Register definitions from NVIDIA/open-gpu-kernel-modules:
 *   src/common/inc/swref/published/nv_ref.h
 *   src/common/inc/swref/published/nv_arch.h
 *   src/common/inc/swref/published/ampere/ga100/dev_boot.h
 */

#ifndef GPU_NVIDIA_H
#define GPU_NVIDIA_H

#include <stdint.h>

/* ============================================================================
 * PMC (Power Management Controller) Identification Registers
 *
 * These are read-only and safe to access without side effects.
 * ============================================================================ */

/* NV_PMC_BOOT_0 — Primary chip identification (offset 0x000)
 *
 * Bit layout:
 *   [3:0]   MINOR_REVISION
 *   [7:4]   MAJOR_REVISION
 *   [8:8]   ARCHITECTURE_1 (bit 5 of 6-bit architecture)
 *   [23:20] IMPLEMENTATION (chip variant)
 *   [28:24] ARCHITECTURE_0 (bits 4:0 of architecture)
 */
#define NV_PMC_BOOT_0                   0x000

/* NV_PMC_BOOT_42 — Extended chip identification (offset 0xA00)
 *
 * Cleaner layout (available since G94):
 *   [11:8]  MINOR_EXTENDED_REVISION
 *   [15:12] MINOR_REVISION
 *   [19:16] MAJOR_REVISION
 *   [23:20] IMPLEMENTATION
 *   [29:24] ARCHITECTURE (6 bits, no split)
 *   [29:20] CHIP_ID (combined architecture + implementation)
 */
#define NV_PMC_BOOT_42                  0xA00

/* NV_PMC_ENABLE — Device enable bits (offset 0x200, read-only for probe) */
#define NV_PMC_ENABLE                   0x200

/* ============================================================================
 * Architecture Constants
 * ============================================================================ */

#define NV_GPU_ARCHITECTURE_TURING      0x16
#define NV_GPU_ARCHITECTURE_AMPERE      0x17
#define NV_GPU_ARCHITECTURE_HOPPER      0x18
#define NV_GPU_ARCHITECTURE_ADA         0x19

/* Ampere implementation variants */
#define NV_GPU_IMPL_GA100               0x0     /* A100 (data center) */
#define NV_GPU_IMPL_GA102               0x2     /* RTX 3080, 3090 */
#define NV_GPU_IMPL_GA104               0x4     /* RTX 3060 Ti, 3070 */
#define NV_GPU_IMPL_GA106               0x6     /* RTX 3050, 3060 */
#define NV_GPU_IMPL_GA107               0x7     /* RTX 3050 (lower-end) */
#define NV_GPU_IMPL_GA10B               0xB     /* Jetson Orin iGPU */

/* Combined chip IDs (architecture << 4 | implementation) */
#define NV_CHIP_ID_GA10B                0x17B
#define NV_CHIP_ID_GA106                0x176
#define NV_CHIP_ID_GA100                0x170

/* GPU not present / in reset sentinel */
#define NV_GPU_NOT_PRESENT              0xFFFFFFFF

/* ============================================================================
 * Register Decode Helpers
 * ============================================================================ */

/* Decode BOOT_0 fields */
static inline uint32_t nv_boot0_arch(uint32_t boot0)
{
    /* 6-bit architecture: {bit 8, bits 28:24} */
    return ((boot0 >> 24) & 0x1F) | ((boot0 >> 3) & 0x20);
}

static inline uint32_t nv_boot0_impl(uint32_t boot0)
{
    return (boot0 >> 20) & 0xF;
}

static inline uint32_t nv_boot0_major_rev(uint32_t boot0)
{
    return (boot0 >> 4) & 0xF;
}

static inline uint32_t nv_boot0_minor_rev(uint32_t boot0)
{
    return boot0 & 0xF;
}

/* Decode BOOT_42 fields (cleaner layout) */
static inline uint32_t nv_boot42_chip_id(uint32_t boot42)
{
    return (boot42 >> 20) & 0x3FF;  /* 10-bit combined arch+impl */
}

static inline uint32_t nv_boot42_arch(uint32_t boot42)
{
    return (boot42 >> 24) & 0x3F;
}

static inline uint32_t nv_boot42_impl(uint32_t boot42)
{
    return (boot42 >> 20) & 0xF;
}

/* ============================================================================
 * Chip Name Lookup
 * ============================================================================ */

static inline const char *nv_chip_name(uint32_t chip_id)
{
    switch (chip_id) {
    case NV_CHIP_ID_GA100: return "GA100 (A100)";
    case 0x172:            return "GA102 (RTX 3080/3090)";
    case 0x174:            return "GA104 (RTX 3070)";
    case NV_CHIP_ID_GA106: return "GA106 (RTX 3050/3060)";
    case 0x177:            return "GA107 (RTX 3050)";
    case NV_CHIP_ID_GA10B: return "GA10B (Jetson Orin)";
    default:               return "Unknown";
    }
}

/* ============================================================================
 * Probe Function (implemented in gpu_nvidia.c)
 * ============================================================================ */

struct nv_gpu_info {
    uint32_t boot0;         /* Raw NV_PMC_BOOT_0 value */
    uint32_t boot42;        /* Raw NV_PMC_BOOT_42 value */
    uint32_t chip_id;       /* Combined arch+impl from BOOT_42 */
    uint32_t architecture;  /* 6-bit architecture */
    uint32_t implementation;/* 4-bit implementation */
    uint32_t major_rev;     /* Silicon major revision */
    uint32_t minor_rev;     /* Silicon minor revision */
    uint32_t enable;        /* NV_PMC_ENABLE value */
    bool     present;       /* GPU detected and accessible */
    bool     is_ampere;     /* Architecture == 0x17 */
};

/*
 * Probe an NVIDIA GPU at the given MMIO base address.
 * Returns 0 on success (GPU found), non-zero on failure.
 * Fills info struct with identification data.
 *
 * WARNING: If the address is protected by a bus firewall (e.g., CBB on
 * Tegra234), the read may trigger a fatal bus error. This function should
 * only be called after confirming the address is mapped and likely accessible.
 */
int nv_gpu_probe(uintptr_t mmio_base, struct nv_gpu_info *info);

/*
 * Print GPU probe results to UART.
 */
void nv_gpu_print_info(const struct nv_gpu_info *info);

#endif /* GPU_NVIDIA_H */
