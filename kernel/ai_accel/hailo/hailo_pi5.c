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
#include "spinlock.h"
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
#define HAILO_PI5_NUM_BARS 6
static volatile uint8_t *bar_map[HAILO_PI5_NUM_BARS];
static uint64_t          bar_size[HAILO_PI5_NUM_BARS];

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

    /* Disable ASPM L0s on the endpoint. Reference hailo_pcie.c:155-260
     * (hailo_pcie_disable_aspm) does this unconditionally at probe,
     * with a comment citing "Some devices *must* have certain ASPM
     * states disabled per hardware errata". ASPM L0s transitions can
     * silently drop PCIe completion transactions; for a design that
     * polls num_proc and relies on in-order completions to advance
     * VDMA state, a dropped completion looks exactly like "fw never
     * advanced num_proc" — which is the Phase 8 boundary-submit
     * symptom.
     *
     * PCI Express capability ID 0x10; LNKCTL is at cap_base + 0x10;
     * ASPM_L0S is bit 0 of LNKCTL. Cleared here on the endpoint; the
     * parent pcie1 RC side should also be cleared but is accessed
     * via BCM2712-specific regs, tracked separately. */
    uint8_t exp_cap = pcie_find_capability(hailo_pcidev, 0x10);
    if (exp_cap != 0) {
        uint16_t lnkctl = pcie_config_read16(hailo_pcidev,
                                             (uint16_t)(exp_cap + 0x10));
        if (lnkctl & 0x0001u) {
            INFO("hailo: endpoint LNKCTL 0x%04x had ASPM_L0S set; "
                 "clearing", lnkctl);
            pcie_config_write16(hailo_pcidev,
                                (uint16_t)(exp_cap + 0x10),
                                (uint16_t)(lnkctl & ~0x0001u));
            uint16_t verify = pcie_config_read16(hailo_pcidev,
                                                 (uint16_t)(exp_cap + 0x10));
            INFO("hailo: endpoint LNKCTL after clear: 0x%04x", verify);
        } else {
            INFO("hailo: endpoint LNKCTL 0x%04x ASPM_L0S already off",
                 lnkctl);
        }

        /* Audit F-03 (2026-04-24): log negotiated PCIe geometry so we
         * can compare against HailoRT-on-Pi-OS and against our chosen
         * descriptor page sizes. The Hailo PCIe driver has a documented
         * workaround that drops desc_max_page_size if MaxReadReq < 512;
         * if MRRS comes up at 128/256 here, our 512-byte input desc
         * page geometry is at odds with the actual link configuration.
         *
         * PCI Express Cap layout:
         *   +0x08 DEVCTL: bits 7:5 = MPS, bits 14:12 = MRRS
         *                 (encoded value v means 128 * 2^v bytes)
         *   +0x12 LNKSTA: bits 3:0 = current link speed (1=Gen1,
         *                 2=Gen2, 3=Gen3), bits 9:4 = link width. */
        uint16_t devctl = pcie_config_read16(hailo_pcidev,
                                             (uint16_t)(exp_cap + 0x08));
        uint16_t lnksta = pcie_config_read16(hailo_pcidev,
                                             (uint16_t)(exp_cap + 0x12));
        unsigned mps_enc  = (devctl >> 5)  & 0x7u;
        unsigned mrrs_enc = (devctl >> 12) & 0x7u;
        unsigned spd      = lnksta & 0xFu;
        unsigned wid      = (lnksta >> 4) & 0x3Fu;
        /* PCIe spec defines MPS/MRRS encodings 0..5 (128..4096 B);
         * 6 and 7 are reserved. Print "reserved" instead of a bogus
         * 8192/16384 figure if a malformed device returns them. */
        if (mps_enc <= 5 && mrrs_enc <= 5) {
            unsigned mps_b  = 128u << mps_enc;
            unsigned mrrs_b = 128u << mrrs_enc;
            INFO("hailo: endpoint DEVCTL=0x%04x MPS=%u MRRS=%u "
                 "LNKSTA=0x%04x speed=Gen%u width=x%u",
                 devctl, mps_b, mrrs_b, lnksta, spd, wid);
            if (mrrs_b < 512u) {
                WARN("hailo: MRRS=%u (<512) — Hailo driver normally "
                     "caps desc_max_page_size to MRRS in this regime; "
                     "our fixed 512-byte input desc page may not "
                     "match the actual link", mrrs_b);
            }
        } else {
            WARN("hailo: endpoint DEVCTL=0x%04x has reserved MPS/MRRS "
                 "encoding (mps_enc=%u mrrs_enc=%u); LNKSTA=0x%04x "
                 "speed=Gen%u width=x%u — device may be reporting "
                 "malformed PCIe config", devctl, mps_enc, mrrs_enc,
                 lnksta, spd, wid);
        }
    } else {
        WARN("hailo: no PCI Express capability on endpoint — "
             "cannot gate ASPM");
    }

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
    for (int i = 0; i < HAILO_PI5_NUM_BARS; i++) {
        bar_map[i]  = NULL;
        bar_size[i] = 0;
    }
    hailo_pcidev = NULL;
}

static inline volatile uint32_t *bar_u32(uint8_t bar, uint32_t offset)
{
    return (volatile uint32_t *)(bar_map[bar] + offset);
}

/* bar is uint8_t (0..255) from the vtable contract. Bound to the
 * bar_map[] array size before indexing — a caller that picks up a
 * bogus BAR index (defensive coding, not expected in practice) must
 * not read OOB memory. */
static uint32_t pi5_read32(uint8_t bar, uint32_t offset)
{
    if (bar >= HAILO_PI5_NUM_BARS || !bar_map[bar]) return 0xFFFFFFFFu;
    return *bar_u32(bar, offset);
}

static void pi5_write32(uint8_t bar, uint32_t offset, uint32_t value)
{
    if (bar >= HAILO_PI5_NUM_BARS || !bar_map[bar]) return;
    *bar_u32(bar, offset) = value;
}

/*
 * BAR4 multi-word copy. Required alignment: `offset`, `n`, and the
 * source/destination pointer all 4 B. These hold for every
 * call-site in the Hailo driver (firmware image sections and
 * control messages are u32-aligned by construction —
 * hailo-fw-validation enforces FW_CODE_SECTION_ALIGNMENT = 4).
 *
 * Unaligned inputs are a programming error — ARM64 Device-nGnRnE
 * memory faults on unaligned access, so a silent truncation would
 * hide the bug under hardware abort. Reject instead.
 */
static void pi5_bar4_write(uint32_t offset, const void *src, size_t n)
{
    if (!bar_map[HAILO_BAR_FW_ACCESS]) return;
    if ((n & 3u) || (offset & 3u) || ((uintptr_t)src & 3u)) {
        WARN("hailo: pi5_bar4_write unaligned (off=0x%x src=%p n=%lu)",
             offset, src, (unsigned long)n);
        return;
    }
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
    if ((n & 3u) || (offset & 3u) || ((uintptr_t)dst & 3u)) {
        WARN("hailo: pi5_bar4_read unaligned (off=0x%x dst=%p n=%lu)",
             offset, dst, (unsigned long)n);
        return;
    }
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

/*
 * PMM is a buddy allocator: pmm_alloc_pages(N) rounds N up to the
 * next power of 2 and returns a block aligned to that size (pmm.c
 * enforces this via is_aligned_to_order on free). So asking for
 * max(size, align) pages gives us a block whose natural alignment
 * meets `align` without any manual within-block alignment — a
 * mid-block pointer would fail pmm_free_pages's alignment check
 * anyway (pmm.c:560).
 *
 * alloc and free must therefore agree on the "effective size"
 * max(size, align). The vtable passes both values to free so the
 * order computation matches.
 */
static inline size_t pi5_dma_pages(size_t size, size_t align)
{
    if (align < PAGE_SIZE) align = PAGE_SIZE;
    size_t request = size > align ? size : align;
    return (request + PAGE_SIZE - 1) / PAGE_SIZE;
}

/* Hard ceiling on `low_bias` allocations. Audit F-01 (2026-04-24):
 * pmm_alloc_pages_low returns the LOWEST currently-free block, which
 * silently degrades to high memory once low memory is fragmented.
 * For Hailo DMA we need a hard contract instead — if we can't satisfy
 * the request below the ceiling, fail loudly so the caller (and the
 * boot log) sees it instead of attributing the resulting "fw never
 * fetched the descriptor" symptom to something else.
 *
 * 1 GB = 0x40000000 is conservative for the BCM2712 inbound window
 * (which spans the full 4 GB on Pi 5 per dma-ranges) but matches the
 * audit's recommended starting point for bisecting upward later. */
#define HAILO_DMA_LOW_CEILING_PHYS  0x40000000ULL

/* Common alloc helper: validates + allocates via the chosen PMM
 * function, then validates alignment and produces the IOVA.
 * `low_bias` selects pmm_alloc_pages_low (DMA buffers that must
 * land in low physical memory) over pmm_alloc_pages (everything
 * else). No global state — bias is a per-call argument so
 * concurrent allocations from different threads each request
 * independently. */
static void *pi5_dma_alloc_common(size_t size, size_t align,
                                  uint64_t *iova_out, bool low_bias)
{
    if (size == 0) return NULL;
    size_t pages = pi5_dma_pages(size, align);
    void *va = low_bias ? pmm_alloc_pages_low(pages)
                        : pmm_alloc_pages(pages);
    if (!va) return NULL;

    /* Defensive: pmm buddy alignment should already satisfy `align`.
     * A mismatch would be a PMM bug or a non-power-of-2 `align`. */
    if (align > PAGE_SIZE && ((uintptr_t)va & (align - 1)) != 0) {
        WARN("hailo: pi5_dma_alloc got misaligned VA %p (align=%lu)",
             va, (unsigned long)align);
        pmm_free_pages(va, pages);
        return NULL;
    }

    /* Hard ceiling enforcement for low_bias requests (F-01).
     * Explicit wide-multiply ((uint64_t)pages * PAGE_SIZE) to keep
     * the size computation in 64-bit even if PMM_MAX_ORDER ever
     * grows past today's 1 GB limit. */
    uintptr_t phys = (uintptr_t)va;
    if (low_bias && (uint64_t)phys + ((uint64_t)pages * PAGE_SIZE)
                        > HAILO_DMA_LOW_CEILING_PHYS) {
        WARN("hailo: dma_alloc_low returned phys=0x%lx pages=%lu — "
             "above ceiling 0x%llx; rejecting (low memory likely "
             "fragmented; raise ceiling or run earlier in boot)",
             (unsigned long)phys, (unsigned long)pages,
             (unsigned long long)HAILO_DMA_LOW_CEILING_PHYS);
        pmm_free_pages(va, pages);
        return NULL;
    }

    uint64_t iova = (uint64_t)phys + PCIE1_DMA_OFFSET;
    if (iova_out) *iova_out = iova;

    /* Per-allocation address trace (F-01): every Hailo DMA buffer's
     * (phys, iova, tag) is logged so a hardware capture can show
     * exactly which DMA objects landed where, and whether the
     * "low" tag is being honored end-to-end. */
    INFO("hailo: dma_alloc%s phys=0x%lx iova=0x%llx pages=%lu align=%lu",
         low_bias ? "_low" : "", (unsigned long)phys,
         (unsigned long long)iova,
         (unsigned long)pages, (unsigned long)align);
    return va;
}

static void *pi5_dma_alloc(size_t size, size_t align, uint64_t *iova_out)
{
    return pi5_dma_alloc_common(size, align, iova_out, /*low_bias=*/false);
}

static void *pi5_dma_alloc_low(size_t size, size_t align, uint64_t *iova_out)
{
    return pi5_dma_alloc_common(size, align, iova_out, /*low_bias=*/true);
}

static void pi5_dma_free(void *ptr, size_t size, size_t align)
{
    if (!ptr) return;
    pmm_free_pages(ptr, pi5_dma_pages(size, align));
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

/*
 * Hailo-8 uses a single MSI vector (see hailo-driver-notes.md §9.6),
 * so this shim intentionally keeps only one handler slot. Callers
 * must not register twice — the second call is rejected to make
 * the limit loud rather than silently clobbering state the IRQ
 * trampoline still sees.
 *
 * Synchronization: `irq_lock` serializes pi5_register_irq against
 * concurrent callers and, more importantly, closes the window where
 * two CPUs could both observe `user_irq_handler == NULL` in the
 * bind-once check and both proceed to register. The trampoline
 * itself reads the globals unlocked (spinlocks aren't safe from
 * ISR context on this platform — see `UART Lock on Pi 5 / Jetson`
 * note in kernel/CLAUDE.md). The ordering that makes the unlocked
 * read safe is: (1) writers hold irq_lock; (2) writers DSB SY
 * before pcie_bind_irq_handler enables the GIC line; (3) GIC enable
 * is the edge that lets the IRQ fire. The trampoline therefore only
 * runs *after* the publish has been observed.
 */
static spinlock_t irq_lock = SPINLOCK_INIT;
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

    irq_flags_t flags = spin_lock_irqsave(&irq_lock);
    if (user_irq_handler) {
        spin_unlock_irqrestore(&irq_lock, flags);
        WARN("hailo: MSI handler already registered — rejecting second call");
        return HAILO_ERR_INVAL;
    }

    struct pcie_msi_handle msi;
    int rc = pcie_alloc_msi(hailo_pcidev, 1, &msi);
    if (rc != PCIE_OK) {
        spin_unlock_irqrestore(&irq_lock, flags);
        WARN("hailo: pcie_alloc_msi failed (%d)", rc);
        return HAILO_ERR_IO;
    }

    /* Publish handler + ctx before binding. DSB SY pushes the writes
     * to the point of coherence so the trampoline (which may run on
     * any CPU) observes them by the time the GIC line is enabled. */
    user_irq_handler = handler;
    user_irq_ctx     = ctx;
    __asm__ volatile("dsb sy" ::: "memory");

    rc = pcie_bind_irq_handler(&msi, 0, hailo_msi_trampoline, NULL);
    if (rc != PCIE_OK) {
        user_irq_handler = NULL;
        user_irq_ctx     = NULL;
        __asm__ volatile("dsb sy" ::: "memory");
        spin_unlock_irqrestore(&irq_lock, flags);
        return HAILO_ERR_IO;
    }
    spin_unlock_irqrestore(&irq_lock, flags);
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
    .dma_alloc_low    = pi5_dma_alloc_low,
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
