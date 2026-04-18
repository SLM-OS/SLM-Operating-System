/*
 * hailo_pi5.c — Raspberry Pi 5 platform shim for the Hailo driver.
 *
 * Finds the Hailo endpoint on the `pcie1` root complex via the
 * Phase 1 PCIe API, maps BAR0 / BAR2 / BAR4 as Device memory, and
 * installs a hailo_platform_ops vtable that the core uses for all
 * hardware access. DMA buffers come from the PMM; cache
 * maintenance goes through kernel/gpu/cache.h (the same helpers
 * the Jetson NVIDIA driver uses).
 *
 * This file is *only* compiled for RASPI5. Other ARM64 platforms
 * link against hailo_stub.c instead.
 */

#include "platform.h"

#if defined(PLATFORM_RASPI5)

#include "hailo.h"
#include "pcie.h"
#include "pmm.h"
#include "gpu.h"          /* cache_clean_range / cache_invalidate_range */
#include "debug.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/* -------------------------------------------------------------------------- */

static const struct pcie_device *hailo_pcidev;

/*
 * Mapped BAR virtual addresses. NULL means that BAR isn't ready yet.
 * BAR0, BAR2, BAR4 per Hailo-8 layout; the other BAR indices are
 * unused on this device.
 */
static volatile uint8_t *bar_map[6];
static uint64_t          bar_size[6];

/* -------------------------------------------------------------------------- */
/* Platform ops                                                                */
/* -------------------------------------------------------------------------- */

static int pi5_init(void)
{
    /*
     * Find the Hailo endpoint on any enumerated PCIe bus. The Pi 5
     * pcie1 backend reports one endpoint (the HAT card) on bus 1,
     * but we don't assume a specific (bus, dev, func) — just match
     * vendor + device.
     */
    hailo_pcidev = pcie_find_device(HAILO_PCI_VENDOR_ID,
                                    HAILO_PCI_DEVICE_HAILO8);
    if (!hailo_pcidev) {
        INFO("hailo: no Hailo-8/8L endpoint on pcie1 "
             "(check AI HAT+ seating and dtparam=pciex1)");
        return HAILO_ERR_NODEV;
    }

    INFO("hailo: endpoint at %02x:%02x.%x",
         hailo_pcidev->bus, hailo_pcidev->dev, hailo_pcidev->func);

    /* Enable memory-space decoding + bus-master. Required before
     * the device responds to MMIO reads or issues DMA. */
    pcie_enable_bus_master(hailo_pcidev);

    /* Map the three BARs we care about. pcie_map_bar calls through
     * to the BCM2712 backend which installs a Device-nGnRnE
     * translation via vmm_map_region. */
    static const uint8_t bars[] = {
        HAILO_BAR_CONFIG, HAILO_BAR_VDMA, HAILO_BAR_FW_ACCESS,
    };
    for (size_t i = 0; i < sizeof(bars); i++) {
        uint8_t  b    = bars[i];
        uint64_t size = 0;
        void    *va   = pcie_map_bar(hailo_pcidev, b, &size);
        if (!va) {
            WARN("hailo: pcie_map_bar(BAR%u) failed — BAR unprogrammed?",
                 b);
            return HAILO_ERR_IO;
        }
        bar_map[b]  = (volatile uint8_t *)va;
        bar_size[b] = size;
    }
    return HAILO_OK;
}

static void pi5_shutdown(void)
{
    /* pcie_map_bar does not currently expose an unmap; the mappings
     * stay live for the kernel's lifetime. Zero the pointers so
     * post-shutdown accesses fault. */
    for (int i = 0; i < 6; i++) {
        bar_map[i]  = NULL;
        bar_size[i] = 0;
    }
    hailo_pcidev = NULL;
}

static inline volatile uint32_t *bar_u32(uint8_t bar, uint32_t offset)
{
    return (volatile uint32_t *)(bar_map[bar] + offset);
}

static uint32_t pi5_read32(uint8_t bar, uint32_t offset)
{
    if (!bar_map[bar]) return 0xFFFFFFFFu;
    return *bar_u32(bar, offset);
}

static void pi5_write32(uint8_t bar, uint32_t offset, uint32_t value)
{
    if (!bar_map[bar]) return;
    *bar_u32(bar, offset) = value;
}

/*
 * BAR4 multi-word copy. Required alignment: source buffer 4 B,
 * target offset 4 B, length 4 B. These hold for every call-site
 * in the Hailo driver (firmware image sections and control
 * messages are all u32-aligned by construction — hailo-fw-validation
 * enforces FW_CODE_SECTION_ALIGNMENT = 4).
 */
static void pi5_bar4_write(uint32_t offset, const void *src, size_t n)
{
    if (!bar_map[HAILO_BAR_FW_ACCESS]) return;
    const uint32_t *s = (const uint32_t *)src;
    volatile uint32_t *d = bar_u32(HAILO_BAR_FW_ACCESS, offset);
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        d[i] = s[i];
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void pi5_bar4_read(uint32_t offset, void *dst, size_t n)
{
    if (!bar_map[HAILO_BAR_FW_ACCESS]) return;
    uint32_t *d = (uint32_t *)dst;
    volatile uint32_t *s = bar_u32(HAILO_BAR_FW_ACCESS, offset);
    size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        d[i] = s[i];
    }
}

/*
 * DMA allocation. PMM hands out physical pages identity-mapped as
 * cacheable kernel memory. The BCM2712 PCIe inbound window
 * (dma-ranges property) adds 0x10_00000000 so the endpoint's
 * DMA address is phys + PCIE_DMA_OFFSET — see
 * docs/pi5-pcie1-registers.md §2.5.
 */
#define PCIE1_DMA_OFFSET   0x0000001000000000ULL

static void *pi5_dma_alloc(size_t size, size_t align, uint64_t *iova_out)
{
    /* PMM only does page-size alignment; callers that need 64 KB
     * alignment (VDMA descriptor lists) must over-allocate. */
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    (void)align;   /* TODO: honor align by over-allocating when > PAGE_SIZE */

    void *va = pmm_alloc_pages(pages);
    if (!va) return NULL;
    if (iova_out) {
        *iova_out = (uint64_t)(uintptr_t)va + PCIE1_DMA_OFFSET;
    }
    return va;
}

static void pi5_dma_free(void *ptr, size_t size)
{
    if (!ptr) return;
    size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    pmm_free_pages(ptr, pages);
}

static void pi5_cache_clean(const void *addr, size_t size)
{
    cache_clean_range((void *)addr, size);
}

static void pi5_cache_invalidate(void *addr, size_t size)
{
    cache_invalidate_range(addr, size);
}

static void pi5_mb(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static void pi5_udelay(uint32_t usec)
{
    /* CNTPCT-based delay — 54 MHz nominal on Pi 5, so ~54 ticks per
     * microsecond. Read the counter and spin. Matches the pattern
     * in kernel/drivers/timer.c. */
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    uint64_t deadline = ticks + (freq * usec) / 1000000u;
    for (;;) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
        if (ticks >= deadline) break;
    }
}

static void (*user_irq_handler)(void *);
static void  *user_irq_ctx;

static void hailo_msi_trampoline(void *ctx)
{
    (void)ctx;
    if (user_irq_handler) {
        user_irq_handler(user_irq_ctx);
    }
}

static int pi5_register_irq(void (*handler)(void *), void *ctx)
{
    if (!handler || !hailo_pcidev) return HAILO_ERR_INVAL;

    struct pcie_msi_handle msi;
    int rc = pcie_alloc_msi(hailo_pcidev, 1, &msi);
    if (rc != PCIE_OK) {
        WARN("hailo: pcie_alloc_msi failed (%d)", rc);
        return HAILO_ERR_IO;
    }

    user_irq_handler = handler;
    user_irq_ctx     = ctx;

    rc = pcie_bind_irq_handler(&msi, 0, hailo_msi_trampoline, NULL);
    if (rc != PCIE_OK) {
        user_irq_handler = NULL;
        user_irq_ctx     = NULL;
        return HAILO_ERR_IO;
    }
    INFO("hailo: MSI vector %u bound", msi.first_irq);
    return HAILO_OK;
}

/* -------------------------------------------------------------------------- */
/* Vtable + registration                                                       */
/* -------------------------------------------------------------------------- */

static const struct hailo_platform_ops pi5_ops = {
    .name             = "pi5-pcie1",
    .init             = pi5_init,
    .shutdown         = pi5_shutdown,
    .read32           = pi5_read32,
    .write32          = pi5_write32,
    .bar4_write       = pi5_bar4_write,
    .bar4_read        = pi5_bar4_read,
    .dma_alloc        = pi5_dma_alloc,
    .dma_free         = pi5_dma_free,
    .cache_clean      = pi5_cache_clean,
    .cache_invalidate = pi5_cache_invalidate,
    .mb               = pi5_mb,
    .udelay           = pi5_udelay,
    .register_irq     = pi5_register_irq,
};

/*
 * Main-kernel entry point. Called once at boot after pcie_init().
 * Installs the platform vtable and calls hailo_init(), which in
 * turn runs pi5_init() (finds the device, maps BARs).
 *
 * Return codes are informational — a missing HAT+ is not a boot
 * failure; the device just stays at STATE_UNINIT and the `hailo`
 * shell command will report it on demand.
 */
int hailo_platform_install(void)
{
    hailo_platform = &pi5_ops;
    return hailo_init();
}

#endif /* PLATFORM_RASPI5 */
