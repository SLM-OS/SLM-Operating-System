/*
 * nvidia_gsp_platform.c — Jetson Orin Nano (GA10B) implementation of
 * `struct gsp_platform_ops` for the integrated Ampere GPU.
 *
 * Vtable contract: docs/nvidia-gsp.md §"Platform Shim Contract".
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../../gpu/nvidia/gsp.h"
#include "../../gpu/nvidia/nvidia_vbios.h"
#include "../../include/uart.h"
#include "../../include/pmm.h"
#include "../../include/cache.h"

/* ---- GPU MMIO base ----
 *
 * GA10B on Tegra234 is memory-mapped at 0x17000000 (GPU_BASE from
 * platform.h). Confirmed accessible from EL2+VHE — NV_PMC_BOOT_0
 * reads 0xB7B000A1. */
#define JETSON_GPU_BAR0_BASE  GPU_BASE

/* ---- BAR1 (unified memory) ----
 *
 * GA10B has no discrete VRAM — the GPU's framebuffer is a carveout
 * in system DRAM. BAR1 offsets in the GSP protocol are relative to
 * this carveout base. The actual base address is determined by the
 * GPU memory controller after GSP-RM brings up the memory subsystem.
 *
 * For Phase 0 (firmware load) and initial bringup, bar1_read/write
 * are not called. The base will be populated when compute submission
 * (E5) lands. For now, accesses log a warning and no-op. */
static uintptr_t g_bar1_base;

/* ---- Firmware symbols from nvidia_gsp_firmware.S ----
 *
 * Guarded by ENABLE_GSP_FIRMWARE — when off, the build skips
 * extraction and these symbols don't exist. */
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

/* ---- BAR0 register access ----
 *
 * Volatile read/write at (GPU_BASE + offset). DSB SY after each
 * write ensures the store is observable by the GPU before the CPU
 * proceeds — ARM64 device memory ordering requires explicit barriers
 * between successive MMIO writes to distinct registers. */

static uint32_t jetson_gsp_read32(uint32_t offset)
{
    volatile uint32_t *reg =
        (volatile uint32_t *)(JETSON_GPU_BAR0_BASE + offset);
    return *reg;
}

static void jetson_gsp_write32(uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg =
        (volatile uint32_t *)(JETSON_GPU_BAR0_BASE + offset);
    *reg = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- BAR1 (unified memory) byte-level access ----
 *
 * On GA10B, BAR1 offsets map into the unified framebuffer carveout
 * in system DRAM. Cache maintenance is required after CPU writes
 * (so the GPU sees the data) and before CPU reads (so stale
 * cachelines don't mask GPU writes). */

static void jetson_gsp_bar1_read(uint32_t offset, void *dst, size_t n)
{
    if (!g_bar1_base) {
        uart_puts("[GSP-JETSON] bar1_read: base not configured\n");
        return;
    }
    volatile uint8_t *src = (volatile uint8_t *)(g_bar1_base + offset);
    cache_invalidate_range((void *)src, n);
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = src[i];
}

static void jetson_gsp_bar1_write(uint32_t offset, const void *src, size_t n)
{
    if (!g_bar1_base) {
        uart_puts("[GSP-JETSON] bar1_write: base not configured\n");
        return;
    }
    volatile uint8_t *d = (volatile uint8_t *)(g_bar1_base + offset);
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    cache_clean_range((void *)(g_bar1_base + offset), n);
}

/* ---- DMA allocation ----
 *
 * Allocate from the buddy PMM. On GA10B with SMMU passthrough,
 * dma_addr == phys_addr (identity mapping). The PMM returns
 * page-aligned (4 KB) addresses; for larger alignments, we
 * over-allocate and align manually.
 *
 * The GPU's Falcon DMA engine silently truncates DMATRFBASE when
 * low bits are non-zero, so alignment correctness is critical —
 * callers should use gsp_dma_alloc_checked() for the assertion. */

static void *jetson_gsp_dma_alloc(size_t size, size_t align,
                                  uint64_t *out_dma_addr)
{
    if (size == 0) {
        if (out_dma_addr) *out_dma_addr = 0;
        return (void *)0;
    }

    /* PMM allocates in page granularity (4 KB). If alignment is
     * within a page, a simple allocation suffices. For larger
     * alignments, over-allocate to guarantee we can find an aligned
     * block within the allocation. */
    size_t page_size = 4096;
    if (align < page_size)
        align = page_size;

    size_t alloc_size = size;
    if (align > page_size) {
        /* Over-allocate by (align - page_size) to guarantee an
         * aligned address exists within the allocation. */
        alloc_size = size + align - page_size;
    }

    size_t pages = (alloc_size + page_size - 1) / page_size;
    void *raw = pmm_alloc_pages(pages);
    if (!raw) {
        uart_printf("[GSP-JETSON] dma_alloc: PMM failed (%lu pages)\n",
                    (unsigned long)pages);
        if (out_dma_addr) *out_dma_addr = 0;
        return (void *)0;
    }

    /* Align within the allocation. */
    uintptr_t addr = (uintptr_t)raw;
    uintptr_t aligned = (addr + align - 1) & ~(align - 1);

    /* Identity mapping: DMA address == physical address. */
    if (out_dma_addr)
        *out_dma_addr = (uint64_t)aligned;

    return (void *)aligned;
}

static void jetson_gsp_dma_free(void *ptr, size_t size)
{
    if (!ptr) return;

    /* PMM free expects the original allocation address and page
     * count. Since we may have aligned the pointer in dma_alloc,
     * we free from the aligned address with the requested size
     * rounded up to pages. This leaks the alignment padding — an
     * acceptable trade-off for the boot-time-only GSP allocations
     * (typically < 10 buffers total). A more precise solution
     * would track the original raw pointer, but GSP bringup is a
     * one-shot path. */
    size_t page_size = 4096;
    size_t pages = (size + page_size - 1) / page_size;
    pmm_free_pages(ptr, pages);
}

/* ---- Cache maintenance ---- */

static void jetson_gsp_cache_clean(const void *addr, size_t size)
{
    cache_clean_range(addr, size);
}

static void jetson_gsp_cache_invalidate(void *addr, size_t size)
{
    cache_invalidate_range(addr, size);
}

/* ---- Memory barrier ---- */

static void jetson_gsp_mb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

/* ---- Firmware accessor ----
 *
 * ⚠ GA10B diverges from discrete Ampere: there is no GSP-RM firmware.
 *
 * Discovered during hardware bringup (jetson-nano-2, 2026-04-15):
 * `/lib/firmware/nvidia/ga10b/` on L4T R36.4.7 does NOT ship the
 * 535.113.01 GSP-RM stack (gsp.bin / bootloader / booter_load /
 * booter_unload) that discrete GA10x uses. Instead, GA10B uses the
 * **nvgpu-native** firmware layout: acr-gsp.* encrypted ucode,
 * fecs/gpccs_encrypt_prod.bin, gpmu_ucode_next_prod_image.bin,
 * NET{A,B,C,D}_img_prod_encrypted.bin, safety-scheduler.*, etc.
 *
 * This is a fundamental architectural difference:
 *   - Discrete Ampere (GA102/GA107): GSP-RM runs a full resource
 *     manager on the RISC-V core. nouveau/open-RM driver pattern.
 *   - Integrated Ampere (GA10B): legacy nvgpu-style bringup with
 *     FECS/GPCCS ucode loaded via ACR (Access Controlled Region);
 *     no GSP-RM image.
 *
 * Consequence: the 7-phase GSP boot sequence in kernel/gpu/nvidia/
 * does not apply to Jetson compute. A Jetson compute path needs a
 * separate nvgpu-style ACR loader. This file's firmware_get
 * currently still refers to the discrete-GPU blob names because
 * that is what `struct gsp_firmware_blob` expresses; on Jetson the
 * build always disables ENABLE_GSP_FIRMWARE and these return
 * {NULL, 0, NULL}, which causes gsp_init() to abort cleanly in
 * Phase 0. This is the right behavior until an nvgpu-style
 * bringup lands alongside the GSP path. */

static void jetson_gsp_firmware_get(enum gsp_firmware_kind kind,
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
    out->data = (void *)0;
    out->size = 0;
    out->version = (void *)0;
}

/* ---- VBIOS ----
 *
 * GA10B is an integrated GPU — no VBIOS. Firmware runtime services
 * (FWSEC equivalent) come from the pre-boot firmware via QSPI.
 * Return success with NULL data; the bringup code has a Jetson path
 * that sources FWSEC-equivalent setup from the bootloader instead. */

static int jetson_gsp_vbios_get_fwsec(const void **out_data,
                                      size_t *out_size)
{
    *out_data = (void *)0;
    *out_size = 0;
    return 0;   /* Success — VBIOS simply doesn't exist on Jetson. */
}

/*
 * nvidia_vbios_platform_load — shared-layer entry point called by
 * bringup.c before FWSEC phases. On Jetson there is no VBIOS to
 * load, so return -1. The GSP bringup sequence will need a
 * Jetson-specific path for FWSEC (sourced from QSPI/bootloader)
 * when phases 3+ are implemented.
 */
int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    (void)out_data;
    (void)out_size;
    return -1;
}

/* ---- Vtable ---- */

static const struct gsp_platform_ops jetson_gsp_ops = {
    .read32           = jetson_gsp_read32,
    .write32          = jetson_gsp_write32,
    .bar1_read        = jetson_gsp_bar1_read,
    .bar1_write       = jetson_gsp_bar1_write,
    .dma_alloc        = jetson_gsp_dma_alloc,
    .dma_free         = jetson_gsp_dma_free,
    .cache_clean      = jetson_gsp_cache_clean,
    .cache_invalidate = jetson_gsp_cache_invalidate,
    .mb               = jetson_gsp_mb,
    .firmware_get     = jetson_gsp_firmware_get,
    .vbios_get_fwsec  = jetson_gsp_vbios_get_fwsec,
};

/*
 * Installer. Called from main.c after GPU probe succeeds, before
 * gsp_init() is invoked.
 */
void jetson_gsp_platform_install(void)
{
    extern const struct gsp_platform_ops *gsp_platform;
    gsp_platform = &jetson_gsp_ops;
    uart_puts("[GSP-JETSON] platform shim installed\n");
}

/*
 * Set the BAR1 base address for unified memory access. Called when
 * the GPU memory controller configuration is known (post-GSP-init).
 */
void jetson_gsp_set_bar1_base(uintptr_t base)
{
    g_bar1_base = base;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
