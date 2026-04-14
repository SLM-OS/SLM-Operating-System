/*
 * nvidia_gsp_platform.c — x86-64 platform shim for the shared
 * NVIDIA GSP-RM bringup code in kernel/gpu/nvidia/.
 *
 * This file provides the `struct gsp_platform_ops` vtable that the
 * shared GSP code calls into. It hides everything about how the GPU
 * is reached on a discrete PCIe system:
 *   - BAR0 / BAR1 are ioremap'd out of ECAM (handled by nvidia_gpu.c)
 *   - DMA is direct (no IOMMU translation)
 *   - Firmware lives in the kernel binary via .incbin (see nvidia_gsp_firmware.S)
 *   - VBIOS is read via the PCI expansion ROM (E2 — NOT yet implemented)
 *
 * Jetson has a sibling file at kernel/arch/arm64/nvidia_gsp_platform.c
 * (future, when Jetson GSP bringup lands) that implements the same
 * vtable differently: direct MMIO at 0x17000000, SMMU-mapped DMA,
 * firmware from VFS, no VBIOS.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../gpu/nvidia/gsp.h"
#include "uart.h"

/* ---- Firmware symbols from nvidia_gsp_firmware.S ----
 *
 * Guarded by ENABLE_GSP_FIRMWARE — when off, the build skips
 * extraction and these symbols don't exist. We weak-import them so
 * the firmware_get callback can report "not available" at runtime
 * without a link failure. */
#if defined(ENABLE_GSP_FIRMWARE)
extern const uint8_t gsp_fw_gsp_start[];
extern const uint8_t gsp_fw_gsp_end[];
extern const uint8_t gsp_fw_bootloader_start[];
extern const uint8_t gsp_fw_bootloader_end[];
extern const uint8_t gsp_fw_booter_load_start[];
extern const uint8_t gsp_fw_booter_load_end[];
extern const uint8_t gsp_fw_booter_unload_start[];
extern const uint8_t gsp_fw_booter_unload_end[];
#endif

static void x86_gsp_firmware_get(enum gsp_firmware_kind kind,
                                 struct gsp_firmware_blob *out)
{
#if defined(ENABLE_GSP_FIRMWARE)
    static const char VERSION[] = "535.113.01";
    switch (kind) {
    case GSP_FW_GSP:
        out->data = gsp_fw_gsp_start;
        out->size = (size_t)(gsp_fw_gsp_end - gsp_fw_gsp_start);
        out->version = VERSION;
        return;
    case GSP_FW_BOOTLOADER:
        out->data = gsp_fw_bootloader_start;
        out->size = (size_t)(gsp_fw_bootloader_end - gsp_fw_bootloader_start);
        out->version = VERSION;
        return;
    case GSP_FW_BOOTER_LOAD:
        out->data = gsp_fw_booter_load_start;
        out->size = (size_t)(gsp_fw_booter_load_end - gsp_fw_booter_load_start);
        out->version = VERSION;
        return;
    case GSP_FW_BOOTER_UNLOAD:
        out->data = gsp_fw_booter_unload_start;
        out->size = (size_t)(gsp_fw_booter_unload_end - gsp_fw_booter_unload_start);
        out->version = VERSION;
        return;
    default: break;
    }
#else
    (void)kind;
#endif
    out->data = NULL;
    out->size = 0;
    out->version = NULL;
}

/* ---- BAR0 / BAR1 / DMA / cache / barrier ----
 *
 * Stubbed for now: E2+ fills these in. Making the vtable load
 * clean first lets the firmware accessor be tested in isolation,
 * which is what the E1 regression tests exercise.
 */
static uint32_t x86_gsp_bar0_read32(uint32_t offset) { (void)offset; return 0xBADF5040; }
static void     x86_gsp_bar0_write32(uint32_t offset, uint32_t v) { (void)offset; (void)v; }
static void     x86_gsp_bar1_read(uint32_t o, void *d, size_t n) { (void)o; (void)d; (void)n; }
static void     x86_gsp_bar1_write(uint32_t o, const void *s, size_t n) { (void)o; (void)s; (void)n; }
static void    *x86_gsp_dma_alloc(size_t s, size_t a, uint64_t *p) { (void)s; (void)a; if (p) *p = 0; return NULL; }
static void     x86_gsp_dma_free(void *p, size_t s) { (void)p; (void)s; }
static void     x86_gsp_cache_clean(const void *a, size_t s) { (void)a; (void)s; }
static void     x86_gsp_cache_invalidate(void *a, size_t s) { (void)a; (void)s; }
static void     x86_gsp_mb(void) { __asm__ volatile("mfence" ::: "memory"); }
static int      x86_gsp_vbios_get_fwsec(const void **d, size_t *s) { *d = NULL; *s = 0; return -1; }

static const struct gsp_platform_ops x86_gsp_ops = {
    .read32           = x86_gsp_bar0_read32,
    .write32          = x86_gsp_bar0_write32,
    .bar1_read        = x86_gsp_bar1_read,
    .bar1_write       = x86_gsp_bar1_write,
    .dma_alloc        = x86_gsp_dma_alloc,
    .dma_free         = x86_gsp_dma_free,
    .cache_clean      = x86_gsp_cache_clean,
    .cache_invalidate = x86_gsp_cache_invalidate,
    .mb               = x86_gsp_mb,
    .firmware_get     = x86_gsp_firmware_get,
    .vbios_get_fwsec  = x86_gsp_vbios_get_fwsec,
};

/*
 * Installer. Called from nvidia_gpu.c:nvidia_gpu_init() after BAR
 * discovery succeeds, before gsp_init() is invoked. No-ops if the
 * GPU isn't an Ampere or if firmware isn't embedded.
 */
void x86_gsp_platform_install(void)
{
    extern const struct gsp_platform_ops *gsp_platform;
    gsp_platform = &x86_gsp_ops;
}

#endif /* PLATFORM_X86_64 */
