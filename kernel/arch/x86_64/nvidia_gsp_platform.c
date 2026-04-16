/*
 * nvidia_gsp_platform.c — x86-64 implementation of
 * `struct gsp_platform_ops` for discrete PCIe GPUs.
 *
 * See docs/nvidia-gsp.md §"Platform Shim Contract" for the full
 * vtable specification and per-platform implementation notes.
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
#include "pmm.h"
#include "string.h"

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

/* ---- BAR0/BAR1 accessors from nvidia_gpu.c ---- */
extern volatile uint32_t *nvidia_gpu_get_bar0(void);
extern uint32_t           nvidia_gpu_get_bar0_size(void);
extern volatile uint8_t  *nvidia_gpu_get_bar1(void);
extern uint64_t           nvidia_gpu_get_bar1_size(void);

/* ---- BAR0 register access ----
 *
 * BAR0 is the GPU's 16 MB MMIO register space. Offsets passed by the
 * shared GSP code are relative to BAR0 base. The volatile qualifier
 * on the pointer from nvidia_gpu.c prevents read coalescing — see
 * docs/nvidia-gsp.md §"Platform Shim Contract" and #163. */
static uint32_t x86_gsp_bar0_read32(uint32_t offset)
{
    volatile uint32_t *bar0 = nvidia_gpu_get_bar0();
    uint32_t size = nvidia_gpu_get_bar0_size();
    if (!bar0 || offset + 4 > size)
        return 0xBADF5040u;
    return bar0[offset / 4];
}

static void x86_gsp_bar0_write32(uint32_t offset, uint32_t value)
{
    volatile uint32_t *bar0 = nvidia_gpu_get_bar0();
    uint32_t size = nvidia_gpu_get_bar0_size();
    if (!bar0 || offset + 4 > size)
        return;
    bar0[offset / 4] = value;
}

/* ---- BAR1 (VRAM) byte access ----
 *
 * BAR1 is the VRAM aperture. On x86-64, MMIO reads/writes through
 * the identity-mapped BAR1 address are strongly ordered (UC/WC
 * depending on MTRR), so no extra fencing is needed. */
static void x86_gsp_bar1_read(uint32_t offset, void *dst, size_t n)
{
    volatile uint8_t *bar1 = nvidia_gpu_get_bar1();
    uint64_t size = nvidia_gpu_get_bar1_size();
    if (!bar1 || (uint64_t)offset + n > size)
        return;
    /* Byte-by-byte from volatile MMIO — memcpy is not safe on
     * volatile pointers (compiler may optimize to non-volatile). */
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++)
        d[i] = bar1[offset + i];
}

static void x86_gsp_bar1_write(uint32_t offset, const void *src, size_t n)
{
    volatile uint8_t *bar1 = nvidia_gpu_get_bar1();
    uint64_t size = nvidia_gpu_get_bar1_size();
    if (!bar1 || (uint64_t)offset + n > size)
        return;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++)
        bar1[offset + i] = s[i];
}

/* ---- DMA allocation via PMM ----
 *
 * On x86-64 bare-metal the first 4 GB is identity-mapped
 * (trampoline32.S), so VA == PA — no IOMMU translation needed.
 * The GPU does direct DMA to physical addresses.
 *
 * PMM returns page-aligned (4 KB) memory, which satisfies every
 * alignment the GSP boot sequence actually requests (Falcon DMA
 * needs 256-byte alignment for DMATRFBASE). */
static void *x86_gsp_dma_alloc(size_t size, size_t align, uint64_t *out_dma)
{
    if (size == 0) {
        if (out_dma) *out_dma = 0;
        return NULL;
    }

    /* PMM always returns page-aligned. Reject over-page alignment
     * that the buddy allocator can't guarantee (no current caller
     * needs > 4 KB alignment). */
    if (align > PAGE_SIZE) {
        uart_printf("[GSP-DMA] unsupported alignment 0x%lx > PAGE_SIZE\n",
                    (unsigned long)align);
        if (out_dma) *out_dma = 0;
        return NULL;
    }

    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    void *ptr = pmm_alloc_pages(pages);
    if (!ptr) {
        if (out_dma) *out_dma = 0;
        return NULL;
    }

    /* Zero the DMA buffer — GPU expects clean memory for command
     * rings, status pages, and ucode staging areas. */
    memset(ptr, 0, pages * PAGE_SIZE);

    /* Identity-mapped: VA == PA. */
    if (out_dma) *out_dma = (uint64_t)(uintptr_t)ptr;
    return ptr;
}

static void x86_gsp_dma_free(void *ptr, size_t size)
{
    if (!ptr) return;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(ptr, pages);
}

/* ---- Cache / barrier ----
 *
 * x86-64 has coherent DMA — no cache maintenance needed.
 * mfence serializes all loads/stores (full barrier). */
static void x86_gsp_cache_clean(const void *a, size_t s) { (void)a; (void)s; }
static void x86_gsp_cache_invalidate(void *a, size_t s)  { (void)a; (void)s; }
static void x86_gsp_mb(void) { __asm__ volatile("mfence" ::: "memory"); }
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
/*
 * Sanity bounds for the ROM BAR physical address. Modern PCIe GPUs
 * have their BARs allocated above 4 GB by UEFI (x86_64-class firmware)
 * OR in the "PCI memory hole" at 0x80000000..0xFFFFF800. Anything
 * below 0x80000000 would overlap with OS-critical physical memory
 * (RAM, ACPI tables, legacy BIOS regions) and is a hard error. We
 * also cap the upper bound at the 4 GB identity-map limit since
 * trampoline32.S only maps the first 4 GB. If that changes, this
 * check must too.
 *
 * ROM size cap of 2 MB: modern NVIDIA VBIOSes are 128–512 KB; 2 MB
 * is well past every known real shipping VBIOS. Larger claims are
 * almost certainly a misread BAR or attacker input.
 */
#define PCI_ROM_PHYS_MIN    0x80000000u
#define PCI_ROM_PHYS_MAX    0xFFFFF800u
#define PCI_ROM_SIZE_MAX    (2u * 1024u * 1024u)

static int x86_read_expansion_rom(uint8_t bus, uint8_t dev, uint8_t func,
                                  uint8_t *dst, size_t max)
{
    /* Caller must give us room for at least the 3-byte ROM header
     * we probe (PCI sig + size byte). Defense-in-depth — the only
     * in-tree caller passes a 256 KB buffer. */
    if (!dst || max < 3) return -1;

    /* The ROM BAR holds a physical address in its upper bits; bit 0
     * enables ROM decoding. Save + restore the original value so we
     * don't strand any other driver's mapping. */
    uint32_t saved_rom = pci_config_read32(bus, dev, func, PCI_CFG_ROM_BAR);
    uint32_t saved_cmd = pci_config_read32(bus, dev, func, PCI_CFG_COMMAND);

    uint32_t rom_phys = saved_rom & 0xFFFFF800u;

    /* Physical-address range validation. Reject:
     *   - unassigned (0 or all-1s from a disabled BAR)
     *   - anything below PCI_ROM_PHYS_MIN (would overlap OS memory)
     *   - anything at or above our identity-map ceiling
     * A hostile or buggy firmware that parked the BAR at e.g. 0x1000
     * would otherwise have us reading RAM as if it were VBIOS. */
    if (rom_phys == 0 || rom_phys == 0xFFFFF800u ||
        rom_phys < PCI_ROM_PHYS_MIN ||
        rom_phys >= PCI_ROM_PHYS_MAX)
        return -1;

    /* Ensure memory-space decoding is enabled. */
    pci_config_write32(bus, dev, func, PCI_CFG_COMMAND,
                       saved_cmd | PCI_CMD_MEM_SPACE);

    /* Enable ROM decoding. */
    pci_config_write32(bus, dev, func, PCI_CFG_ROM_BAR, rom_phys | 1u);

    /* The ROM is now mapped at rom_phys in physical memory. Our
     * x86-64 kernel identity-maps the first 4 GB via trampoline32.S,
     * so rom_phys is directly dereferenceable. The range check above
     * keeps us inside that window. */
    const volatile uint8_t *rom = (const volatile uint8_t *)(uintptr_t)rom_phys;

    /* PCI expansion ROMs declare their size in byte 2 (units of 512).
     * Read just enough to discover, then copy the claimed size (bounded
     * by our buffer and the hard 2 MB cap). */
    if (rom[0] != 0x55 || rom[1] != 0xAA) {
        /* Disable ROM, restore command. */
        pci_config_write32(bus, dev, func, PCI_CFG_ROM_BAR, saved_rom);
        pci_config_write32(bus, dev, func, PCI_CFG_COMMAND,  saved_cmd);
        return -1;
    }

    /* rom[2] is a byte (0..255), so declared fits in 17 bits — no
     * overflow on 32-bit arithmetic even on 32-bit size_t platforms. */
    size_t declared = (size_t)rom[2] * 512u;
    size_t copy_len = declared;
    if (copy_len == 0 || copy_len > max) copy_len = max;
    if (copy_len > PCI_ROM_SIZE_MAX)     copy_len = PCI_ROM_SIZE_MAX;

    /* Invariant check (#149): the clamp chain above keeps us inside
     * the caller's buffer AND the hard 2 MB cap. A future edit that
     * reordered, removed, or misordered one of those clamps would
     * let us read past `rom[]` or write past `dst[]`. Fail closed if
     * the invariant is ever violated. */
    if (copy_len > max || copy_len > PCI_ROM_SIZE_MAX) {
        pci_config_write32(bus, dev, func, PCI_CFG_ROM_BAR, saved_rom);
        pci_config_write32(bus, dev, func, PCI_CFG_COMMAND,  saved_cmd);
        return -1;
    }

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
