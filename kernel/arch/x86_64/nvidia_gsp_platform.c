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
#include "../../gpu/nvidia/nvidia_vbios.h"
#include "pci.h"
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
/* ---- VBIOS loader (E2 / P3-3) ----
 *
 * Read the NVIDIA VBIOS via the PCI Expansion ROM BAR on our GPU,
 * parse the BIT table, and (on Turing+) hand out the FWSEC ucode
 * that GSP boot needs.
 *
 * Buffer: 256 KB static BSS. Current NVIDIA VBIOSes are 128–256 KB;
 * the parser's defensive cap is 1 MB. Living in BSS keeps the
 * boot-time heap allocation out of the path; the copy happens once
 * per boot and the image stays resident for any later GSP re-init.
 */
#define X86_VBIOS_BUF_SIZE  (256 * 1024)
alignas(64) static uint8_t x86_vbios_buf[X86_VBIOS_BUF_SIZE];
static struct nvidia_vbios x86_vbios;
static bool x86_vbios_loaded;

extern int  nvidia_gpu_get_pci_address(uint8_t *bus, uint8_t *dev, uint8_t *func);

/* PCI config-space offsets. */
#define PCI_CFG_COMMAND      0x04
#define PCI_CFG_ROM_BAR      0x30
#define PCI_CMD_MEM_SPACE    0x0002

/*
 * Enable the Expansion ROM BAR on @bus/dev/func, copy up to @max
 * bytes into @dst, then restore the BAR to its original state.
 *
 * Returns copied byte count on success, -1 if no ROM is mapped
 * (firmware never allocated the BAR — unusual, but happens in some
 * QEMU configs and with NVIDIA GPUs in iGPU-primary setups).
 */
static int x86_read_expansion_rom(uint8_t bus, uint8_t dev, uint8_t func,
                                  uint8_t *dst, size_t max)
{
    /* The ROM BAR holds a physical address in its upper bits; bit 0
     * enables ROM decoding. Save + restore the original value so we
     * don't strand any other driver's mapping. */
    uint32_t saved_rom = pci_config_read32(bus, dev, func, PCI_CFG_ROM_BAR);
    uint32_t saved_cmd = pci_config_read32(bus, dev, func, PCI_CFG_COMMAND);

    uint32_t rom_phys = saved_rom & 0xFFFFF800u;
    if (rom_phys == 0 || rom_phys == 0xFFFFF800u)
        return -1;     /* No address assigned — firmware didn't set up */

    /* Ensure memory-space decoding is enabled. */
    pci_config_write32(bus, dev, func, PCI_CFG_COMMAND,
                       saved_cmd | PCI_CMD_MEM_SPACE);

    /* Enable ROM decoding. */
    pci_config_write32(bus, dev, func, PCI_CFG_ROM_BAR, rom_phys | 1u);

    /* The ROM is now mapped at rom_phys in physical memory. Our
     * x86-64 kernel identity-maps the first 4 GB via trampoline32.S,
     * so rom_phys is directly dereferenceable. */
    const volatile uint8_t *rom = (const volatile uint8_t *)(uintptr_t)rom_phys;

    /* PCI expansion ROMs declare their size in byte 2 (units of 512).
     * Read just enough to discover, then copy the claimed size (bounded
     * by our buffer). */
    if (rom[0] != 0x55 || rom[1] != 0xAA) {
        /* Disable ROM, restore command. */
        pci_config_write32(bus, dev, func, PCI_CFG_ROM_BAR, saved_rom);
        pci_config_write32(bus, dev, func, PCI_CFG_COMMAND,  saved_cmd);
        return -1;
    }

    size_t declared = (size_t)rom[2] * 512u;
    size_t copy_len = declared;
    if (copy_len == 0 || copy_len > max) copy_len = max;

    for (size_t i = 0; i < copy_len; i++)
        dst[i] = rom[i];

    /* Restore PCI config — leave the GPU the way we found it. */
    pci_config_write32(bus, dev, func, PCI_CFG_ROM_BAR, saved_rom);
    pci_config_write32(bus, dev, func, PCI_CFG_COMMAND,  saved_cmd);

    return (int)copy_len;
}

/*
 * Shared-layer entry: platform-provided VBIOS loader. Called once
 * by the GSP bringup code before anything that needs FWSEC. Idempotent.
 */
int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    if (x86_vbios_loaded) {
        if (out_data) *out_data = x86_vbios.image;
        if (out_size) *out_size = x86_vbios.image_size;
        return x86_vbios.parsed_ok ? 0 : -1;
    }

    uint8_t bus, dev, func;
    if (nvidia_gpu_get_pci_address(&bus, &dev, &func) < 0)
        return -1;

    int copied = x86_read_expansion_rom(bus, dev, func,
                                        x86_vbios_buf, sizeof(x86_vbios_buf));
    if (copied <= 0) {
        uart_puts("[VBIOS] expansion ROM not readable\n");
        return -1;
    }

    if (nvidia_vbios_parse(x86_vbios_buf, (size_t)copied, &x86_vbios) < 0) {
        uart_printf("[VBIOS] parse failed (copied %d bytes)\n", copied);
        return -1;
    }

    x86_vbios_loaded = true;
    uart_printf("[VBIOS] parsed %d bytes, %u BIT entries\n",
                copied, x86_vbios.num_entries);
    if (out_data) *out_data = x86_vbios.image;
    if (out_size) *out_size = x86_vbios.image_size;
    return 0;
}

static int x86_gsp_vbios_get_fwsec(const void **d, size_t *s)
{
    /* Lazy-load on first request so the GPU probe path doesn't
     * toggle ROM BAR unless GSP is actually being brought up. */
    if (!x86_vbios_loaded) {
        const uint8_t *tmp; size_t tsz;
        if (nvidia_vbios_platform_load(&tmp, &tsz) < 0) {
            *d = NULL; *s = 0;
            return -1;
        }
    }

    const uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    if (nvidia_vbios_get_fwsec(&x86_vbios, &fw, &fw_size) < 0) {
        *d = NULL; *s = 0;
        return -1;
    }
    *d = fw;
    *s = fw_size;
    return 0;
}

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
