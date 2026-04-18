/*
 * pcie_bcm2712.c — BCM2712 `pcie1` root-complex backend for the
 * Raspberry Pi 5 AI HAT+ (Hailo-8/8L NPU) and other external
 * PCIe Gen3 x1 endpoints.
 *
 * Scope and responsibilities
 * --------------------------
 *   - Reads `pcie1` link status at register base + 0x4068.
 *   - Accesses endpoint config space through the EXT_CFG index/data
 *     pair at base + 0x9000 / base + 0x9004 (BCM2712 variant offset —
 *     NOT the generic 0x8000 used by most upstream Broadcom parts).
 *   - Translates PCIe-side BAR addresses into CPU VA via the outbound
 *     window the VideoCore firmware programs during boot:
 *         PCIe 0x00_80000000 → CPU phys 0x1b_80000000   (non-pref, 2 GB)
 *         PCIe 0x04_00000000 → CPU phys 0x18_00000000   (64-bit pref, 14 GB)
 *   - Allocates MSI vectors through MIP1 at phys 0x10_00131000. MIP1
 *     exposes 8 vectors mapped to GIC SPIs 247..254 — enough for
 *     Hailo (one MSI) and a handful of other single-endpoint cards.
 *
 * Deliberately left to VideoCore firmware
 * ---------------------------------------
 *   - Root-complex reset, PERST# sequencing, link training, outbound
 *     window programming, and the RC_BAR1 MIP target address — all
 *     touched by `pcie-brcmstb.c` on upstream Linux but owned by the
 *     Pi 5 firmware before SLM-OS runs. See
 *     docs/pi5-pcie1-registers.md §5.
 *
 * Prerequisite: `dtparam=pciex1` must be set in config.txt (or the
 * HAT+ overlay loaded). Without it, VideoCore leaves pcie1 disabled
 * and config-space accesses abort. link_up() catches this at init
 * and returns false — callers then skip enumeration.
 */

#include "platform.h"

#if defined(PLATFORM_RASPI5)

#include "pcie.h"
#include "debug.h"
#include "spinlock.h"
#include "vmm.h"
#include "gic.h"
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Register map — see docs/pi5-pcie1-registers.md §2                          */
/* -------------------------------------------------------------------------- */

#define PCIE1_BASE              0x1000110000UL   /* external RC */
#define PCIE1_SIZE              0x00009400UL    /* 0x9310 rounded up to u32 */

#define PCIE1_MISC_STATUS       0x4068u         /* link status (read) */
#define   STATUS_PHY_LINKUP     (1u << 4)
#define   STATUS_DL_ACTIVE      (1u << 5)

#define PCIE1_EXT_CFG_INDEX     0x9000u
#define PCIE1_EXT_CFG_DATA      0x9004u         /* BCM2712 variant, not 0x8000 */

/*
 * Outbound window — firmware-programmed translation from PCIe-side
 * addresses to CPU physical addresses. These constants come from
 * `bcm2712.dtsi` lines 1053-1060 (cached at
 * docs/reference/rpi-linux-bcm2712.dtsi).
 */
#define PCIE1_NONPREF_PCIE_BASE 0x0000000080000000ULL
#define PCIE1_NONPREF_CPU_BASE  0x0000001b80000000ULL
#define PCIE1_NONPREF_SIZE      0x0000000080000000ULL  /* 2 GB */

#define PCIE1_PREF64_PCIE_BASE  0x0000000400000000ULL
#define PCIE1_PREF64_CPU_BASE   0x0000001800000000ULL
#define PCIE1_PREF64_SIZE       0x0000000380000000ULL  /* 14 GB */

/* MIP1 — MSI peripheral serving pcie1. docs/pi5-pcie1-registers.md §4. */
#define MIP1_BASE               0x1000131000UL
#define MIP1_SIZE               0xC0UL
#define MIP1_NUM_VECTORS        8u
#define MIP1_BASE_SPI           247u       /* from bcm2712.dtsi `brcm,msi-base-spi` */
#define MIP1_MSI_OFFSET         8u         /* `brcm,msi-offset` — skipped vectors */
#define MIP1_MSG_ADDR_LO        0xFFFFE000UL
#define MIP1_MSG_ADDR_HI        0x000000FFUL

/* MIP register offsets are declared in platform.h (MIP_INT_CLEARED,
 * MIP_INT_CFGL_HOST, MIP_INT_MASKL_HOST, MIP_INT_MASKL_VPU) — shared
 * between MIP0 (RP1's MSI controller, already used for uart_rp1 RX)
 * and MIP1. No local redefinition. */

/* MSI capability register layout (PCI spec §7.7). */
#define MSI_MSG_CTRL_OFFSET     0x02u
#define MSI_MSG_ADDR_LO_OFFSET  0x04u
#define MSI_MSG_ADDR_HI_OFFSET  0x08u
#define MSI_MSG_DATA_OFFSET_32  0x08u   /* when ADDR_HI absent */
#define MSI_MSG_DATA_OFFSET_64  0x0Cu

#define MSI_CTRL_ENABLE         (1u <<  0)
#define MSI_CTRL_MMC_MASK       0x000Eu /* multiple-message capable (RO) */
#define MSI_CTRL_MME_SHIFT      4       /* multiple-message enable */
#define MSI_CTRL_ADDR_64        (1u <<  7)

/* -------------------------------------------------------------------------- */
/* Driver state                                                                */
/* -------------------------------------------------------------------------- */

static volatile uint8_t *pcie1_regs;   /* PCIe RC MMIO VA */
static volatile uint8_t *mip1_regs;    /* MIP1 MMIO VA */
static spinlock_t cfg_lock;            /* guards EXT_CFG_INDEX/DATA pair */

/*
 * MIP1 vector allocation bitmap: bit N set = vector N in use.
 * Guarded by `msi_lock` — alloc/bind can race with other CPUs
 * touching the bitmap or the per-slot handler table. In practice
 * allocation is init-time on CPU 0 today, but there's no reason
 * to require that at the API level; locking keeps the code safe
 * against a future caller doing runtime MSI alloc.
 */
static uint8_t mip1_vec_inuse;
static spinlock_t msi_lock = SPINLOCK_INIT;

static inline uint32_t pcie1_r32(uint32_t off)
{
    return *(volatile uint32_t *)(pcie1_regs + off);
}

static inline void pcie1_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(pcie1_regs + off) = val;
}

static inline uint32_t mip1_r32(uint32_t off)
{
    return *(volatile uint32_t *)(mip1_regs + off);
}

static inline void mip1_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(mip1_regs + off) = val;
}

/*
 * Compose the ECAM-style (bus, devfn, offset) selector written to
 * EXT_CFG_INDEX. Format lifted from `PCIE_ECAM_OFFSET()` in the
 * upstream driver (`rpi-linux-pcie-brcmstb.c`): bus in [27:20],
 * devfn in [19:12].
 */
static inline uint32_t ecam_idx(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t devfn = (((uint32_t)dev & 0x1F) << 3) | ((uint32_t)func & 0x7);
    return ((uint32_t)bus << 20) | (devfn << 12);
}

/* -------------------------------------------------------------------------- */
/* MMIO region setup                                                           */
/* -------------------------------------------------------------------------- */

/*
 * Resolves the pcie1 RC + MIP1 register blocks to VA pointers.
 * Does NOT install any mapping — vmm_setup_platform() in
 * kernel/mm/vmm.c already installs a 2 MB block covering
 * 0x1000000000..0x10001FFFFF as Device memory, which includes
 * both pcie1 (0x10_00110000) and MIP1 (0x10_00131000). The VA
 * matches the PA because the mapping is installed via TTBR0's
 * identity region.
 */
static int resolve_mmio_regions(void)
{
    pcie1_regs = (volatile uint8_t *)(uintptr_t)PCIE1_BASE;
    mip1_regs  = (volatile uint8_t *)(uintptr_t)MIP1_BASE;

    /*
     * Endpoint BAR windows at 0x1b_80000000+ (non-pref) and
     * 0x18_00000000+ (64-bit pref) are NOT mapped by default.
     * vmm_map_region installs Device mappings lazily when map_bar
     * is called on a specific BAR — see bcm2712_map_bar below.
     */
    return PCIE_OK;
}

/* -------------------------------------------------------------------------- */
/* Host ops                                                                    */
/* -------------------------------------------------------------------------- */

static int bcm2712_init(void)
{
    spin_init(&cfg_lock);

    int rc = resolve_mmio_regions();
    if (rc != PCIE_OK) return rc;

    /* Per docs/pi5-pcie1-registers.md §4.5: unmask all 8 host
     * vectors on MIP1 and mask the VPU-side bits so VideoCore
     * doesn't see them. Only touch the LOW 32-bit registers —
     * MIP1 has at most 8 vectors, all in the low half. */
    mip1_w32(MIP_INT_MASKL_HOST, 0);
    mip1_w32(MIP_INT_MASKL_VPU, 0xFFFFFFFFu);
    mip1_w32(MIP_INT_CFGL_HOST, 0xFFFFFFFFu);  /* edge-triggered */
    mip1_w32(MIP_INT_CLEARED,   0xFFFFFFFFu);  /* clear any pending */

    return PCIE_OK;
}

static bool bcm2712_link_up(void)
{
    uint32_t status = pcie1_r32(PCIE1_MISC_STATUS);
    bool ok = (status & (STATUS_PHY_LINKUP | STATUS_DL_ACTIVE))
           == (STATUS_PHY_LINKUP | STATUS_DL_ACTIVE);
    if (!ok) {
        INFO("pcie1: link not trained (status=0x%x) — "
             "check `dtparam=pciex1` in config.txt", status);
    }
    return ok;
}

/*
 * Bus 0 / devfn 0 is the root complex itself — brcm_pcie_map_bus()
 * short-circuits it to read from `pcie1_regs + offset` rather than
 * through EXT_CFG_INDEX. Mirrors that behaviour here; our
 * enumeration never addresses bus 0 beyond devfn 0, so the only
 * other caller is the AI HAT+ endpoint which lives on bus 1.
 */
static uint32_t cfg_read32_locked(uint8_t bus, uint8_t dev, uint8_t func,
                                  uint16_t offset)
{
    if (bus == 0 && dev == 0 && func == 0) {
        return *(volatile uint32_t *)(pcie1_regs + (offset & 0xFFCu));
    }
    pcie1_w32(PCIE1_EXT_CFG_INDEX, ecam_idx(bus, dev, func));
    return *(volatile uint32_t *)(pcie1_regs + PCIE1_EXT_CFG_DATA
                                  + (offset & 0xFFCu));
}

static void cfg_write32_locked(uint8_t bus, uint8_t dev, uint8_t func,
                               uint16_t offset, uint32_t value)
{
    if (bus == 0 && dev == 0 && func == 0) {
        *(volatile uint32_t *)(pcie1_regs + (offset & 0xFFCu)) = value;
        return;
    }
    pcie1_w32(PCIE1_EXT_CFG_INDEX, ecam_idx(bus, dev, func));
    *(volatile uint32_t *)(pcie1_regs + PCIE1_EXT_CFG_DATA
                           + (offset & 0xFFCu)) = value;
}

static uint32_t bcm2712_config_read32(uint8_t bus, uint8_t dev, uint8_t func,
                                      uint16_t offset)
{
    irq_flags_t flags = spin_lock_irqsave(&cfg_lock);
    uint32_t val = cfg_read32_locked(bus, dev, func, offset);
    spin_unlock_irqrestore(&cfg_lock, flags);
    return val;
}

static uint16_t bcm2712_config_read16(uint8_t bus, uint8_t dev, uint8_t func,
                                      uint16_t offset)
{
    uint32_t val = bcm2712_config_read32(bus, dev, func,
                                         (uint16_t)(offset & ~1u));
    return (uint16_t)(val >> ((offset & 2u) * 8u));
}

static uint8_t bcm2712_config_read8(uint8_t bus, uint8_t dev, uint8_t func,
                                    uint16_t offset)
{
    uint32_t val = bcm2712_config_read32(bus, dev, func,
                                         (uint16_t)(offset & ~3u));
    return (uint8_t)(val >> ((offset & 3u) * 8u));
}

static void bcm2712_config_write32(uint8_t bus, uint8_t dev, uint8_t func,
                                   uint16_t offset, uint32_t value)
{
    irq_flags_t flags = spin_lock_irqsave(&cfg_lock);
    cfg_write32_locked(bus, dev, func, offset, value);
    spin_unlock_irqrestore(&cfg_lock, flags);
}

/* 16-bit config write — RMW on the enclosing dword while holding
 * cfg_lock so the index/data pair doesn't get clobbered between
 * the read and write halves. */
static void bcm2712_config_write16(uint8_t bus, uint8_t dev, uint8_t func,
                                   uint16_t offset, uint16_t value)
{
    irq_flags_t flags = spin_lock_irqsave(&cfg_lock);
    uint16_t aligned = (uint16_t)(offset & ~3u);
    uint32_t dword = cfg_read32_locked(bus, dev, func, aligned);
    unsigned shift = (offset & 2u) * 8u;
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    cfg_write32_locked(bus, dev, func, aligned, dword);
    spin_unlock_irqrestore(&cfg_lock, flags);
}

/* -------------------------------------------------------------------------- */
/* BAR mapping                                                                 */
/* -------------------------------------------------------------------------- */

static void *bcm2712_map_bar(uint64_t pcie_addr, uint64_t size)
{
    uint64_t cpu_phys;

    if (pcie_addr >= PCIE1_NONPREF_PCIE_BASE
     && pcie_addr <  PCIE1_NONPREF_PCIE_BASE + PCIE1_NONPREF_SIZE) {
        cpu_phys = pcie_addr - PCIE1_NONPREF_PCIE_BASE + PCIE1_NONPREF_CPU_BASE;
    } else if (pcie_addr >= PCIE1_PREF64_PCIE_BASE
            && pcie_addr <  PCIE1_PREF64_PCIE_BASE + PCIE1_PREF64_SIZE) {
        cpu_phys = pcie_addr - PCIE1_PREF64_PCIE_BASE + PCIE1_PREF64_CPU_BASE;
    } else {
        WARN("pcie1: BAR addr 0x%llx outside outbound windows",
             (unsigned long long)pcie_addr);
        return NULL;
    }

    /* Round the region to a 2 MB boundary for vmm_map_region. */
    uint64_t phys_aligned = cpu_phys & ~(BLOCK_SIZE - 1ull);
    uint64_t region_end   = cpu_phys + size;
    uint64_t size_aligned = ((region_end + BLOCK_SIZE - 1ull)
                            & ~(BLOCK_SIZE - 1ull)) - phys_aligned;

    /* Identity-map the region as Device memory. pcie1 endpoints'
     * BARs live in the 0x18-0x1b_xxxxxxxx range; these physical
     * addresses are not pre-mapped by vmm_setup_platform. */
    int rc = vmm_map_region(phys_aligned, phys_aligned, size_aligned,
                            VMM_FLAGS_DEVICE);
    if (rc != 0) {
        ERROR("pcie1: vmm_map_region(0x%llx, 0x%llx) failed",
              (unsigned long long)phys_aligned,
              (unsigned long long)size_aligned);
        return NULL;
    }

    return (void *)(uintptr_t)cpu_phys;
}

/* -------------------------------------------------------------------------- */
/* MSI allocation (via MIP1)                                                   */
/* -------------------------------------------------------------------------- */

/* Per-vector handler slot. Indexed by MIP1 vector number 0..7. */
struct mip1_slot {
    void (*handler)(void *);
    void *ctx;
};
static struct mip1_slot mip1_slots[MIP1_NUM_VECTORS];

/* GIC handlers for SPIs 247..254 fan out through these trampolines. */
#define DEFINE_MIP1_TRAMPOLINE(n) \
    static void mip1_trampoline_##n(void) { \
        /* ACK the MIP1 latch before invoking the driver handler — \
         * otherwise a second edge during handler run is lost. */ \
        mip1_w32(MIP_INT_CLEARED, 1u << (n)); \
        if (mip1_slots[n].handler) \
            mip1_slots[n].handler(mip1_slots[n].ctx); \
    }
DEFINE_MIP1_TRAMPOLINE(0)
DEFINE_MIP1_TRAMPOLINE(1)
DEFINE_MIP1_TRAMPOLINE(2)
DEFINE_MIP1_TRAMPOLINE(3)
DEFINE_MIP1_TRAMPOLINE(4)
DEFINE_MIP1_TRAMPOLINE(5)
DEFINE_MIP1_TRAMPOLINE(6)
DEFINE_MIP1_TRAMPOLINE(7)
#undef DEFINE_MIP1_TRAMPOLINE

static const gic_handler_fn mip1_trampolines[MIP1_NUM_VECTORS] = {
    mip1_trampoline_0, mip1_trampoline_1, mip1_trampoline_2, mip1_trampoline_3,
    mip1_trampoline_4, mip1_trampoline_5, mip1_trampoline_6, mip1_trampoline_7,
};

/*
 * Find `count` consecutive free bits in mip1_vec_inuse. count must be
 * 1, 2, 4, or 8 (MSI multi-vector rule). Returns the starting vector
 * index or -1 if no aligned block is free.
 */
/* Caller must hold msi_lock. */
static int mip1_alloc_block_locked(int count)
{
    /* MSI multi-vector requires the base to be aligned to `count`. */
    int align = count;

    for (int base = 0; base + count <= (int)MIP1_NUM_VECTORS; base += align) {
        uint8_t mask = (uint8_t)(((1u << count) - 1u) << base);
        if ((mip1_vec_inuse & mask) == 0) {
            mip1_vec_inuse |= mask;
            return base;
        }
    }
    return -1;
}

static int bcm2712_alloc_msi(const struct pcie_device *dev, uint8_t cap_ptr,
                             bool is_msix, int count,
                             struct pcie_msi_handle *out)
{
    if (is_msix) {
        /* MSI-X table setup goes through an endpoint BAR; we'd still
         * point entries at MIP1_MSG_ADDR_LO/HI but the first Phase 1
         * consumer (Hailo) uses plain MSI, so leave this for a
         * follow-on. */
        return PCIE_ERR_UNSUPPORTED;
    }

    /* MSI multi-message: count must be a power of two ≤ 8 on MIP1. */
    if (count < 1 || count > 8 || (count & (count - 1)) != 0) {
        return PCIE_ERR_INVAL;
    }

    irq_flags_t msi_flags = spin_lock_irqsave(&msi_lock);
    int base_vec = mip1_alloc_block_locked(count);
    spin_unlock_irqrestore(&msi_lock, msi_flags);
    if (base_vec < 0) return PCIE_ERR_NOVEC;

    /* Check the endpoint's MSI_CTRL — it advertises how many
     * messages the device is capable of via MMC[3:1]. A device with
     * MMC=0 can only take 1 vector; MMC=1 → up to 2; MMC=2 → 4; etc. */
    uint16_t ctrl = bcm2712_config_read16(dev->bus, dev->dev, dev->func,
                                          (uint16_t)(cap_ptr + MSI_MSG_CTRL_OFFSET));
    uint16_t mmc = (ctrl & MSI_CTRL_MMC_MASK) >> 1;
    uint32_t max_msgs = 1u << mmc;
    if ((uint32_t)count > max_msgs) {
        /* Give back what we reserved. */
        msi_flags = spin_lock_irqsave(&msi_lock);
        uint8_t mask = (uint8_t)(((1u << count) - 1u) << base_vec);
        mip1_vec_inuse &= ~mask;
        spin_unlock_irqrestore(&msi_lock, msi_flags);
        return PCIE_ERR_NOVEC;
    }

    /* Program the endpoint's MSI capability to target MIP1. */
    bool is_64 = (ctrl & MSI_CTRL_ADDR_64) != 0;
    bcm2712_config_write32(dev->bus, dev->dev, dev->func,
                           (uint16_t)(cap_ptr + MSI_MSG_ADDR_LO_OFFSET),
                           MIP1_MSG_ADDR_LO);
    if (is_64) {
        bcm2712_config_write32(dev->bus, dev->dev, dev->func,
                               (uint16_t)(cap_ptr + MSI_MSG_ADDR_HI_OFFSET),
                               MIP1_MSG_ADDR_HI);
    }
    uint16_t data_off = is_64 ? MSI_MSG_DATA_OFFSET_64 : MSI_MSG_DATA_OFFSET_32;
    bcm2712_config_write16(dev->bus, dev->dev, dev->func,
                           (uint16_t)(cap_ptr + data_off),
                           (uint16_t)base_vec);
    /*
     * MSI-CTRL write: enable + set MME to log2(count). Must preserve
     * RO bits (MMC, ADDR_64, per-vector mask support).
     */
    uint16_t mme = 0;
    for (int c = count; c > 1; c >>= 1) mme++;
    uint16_t new_ctrl = (uint16_t)((ctrl & ~(uint16_t)(0x7 << 4))
                                 | ((mme & 0x7u) << 4)
                                 | MSI_CTRL_ENABLE);
    bcm2712_config_write16(dev->bus, dev->dev, dev->func,
                           (uint16_t)(cap_ptr + MSI_MSG_CTRL_OFFSET),
                           new_ctrl);

    /* GIC SPI = 247 + msi-offset(8) + vector. Add 32 to convert to
     * the logical IRQ number (SPI numbers are 0-based; our gic code
     * uses raw IRQ = SPI + 32). */
    uint32_t first_irq = (uint32_t)(MIP1_BASE_SPI + MIP1_MSI_OFFSET + base_vec) + 32u;

    out->dev       = dev;
    out->first_irq = first_irq;
    out->count     = (uint16_t)count;
    out->is_msix   = 0;
    out->cap_ptr   = cap_ptr;
    return PCIE_OK;
}

static int bcm2712_bind_irq_handler(const struct pcie_msi_handle *h,
                                    int vec_idx,
                                    void (*handler)(void *), void *ctx)
{
    /* first_irq = 32 + MIP1_BASE_SPI + MIP1_MSI_OFFSET + base_vec */
    uint32_t irq = h->first_irq + (uint32_t)vec_idx;
    int vec = (int)(irq - 32u - MIP1_BASE_SPI - MIP1_MSI_OFFSET);
    if (vec < 0 || vec >= (int)MIP1_NUM_VECTORS) {
        return PCIE_ERR_INVAL;
    }

    /* Publish the handler + ctx under msi_lock, and DSB SY after
     * so the trampoline (which may run on another CPU) sees a
     * fully-initialised slot before we enable the GIC line. */
    irq_flags_t msi_flags = spin_lock_irqsave(&msi_lock);
    mip1_slots[vec].handler = handler;
    mip1_slots[vec].ctx = ctx;
    __asm__ volatile("dsb sy" ::: "memory");
    spin_unlock_irqrestore(&msi_lock, msi_flags);

    int rc = gic_register_handler(irq, mip1_trampolines[vec]);
    if (rc != 0) {
        msi_flags = spin_lock_irqsave(&msi_lock);
        mip1_slots[vec].handler = NULL;
        mip1_slots[vec].ctx = NULL;
        spin_unlock_irqrestore(&msi_lock, msi_flags);
        return PCIE_ERR_INVAL;
    }
    gic_enable_irq(irq);
    return PCIE_OK;
}

/* -------------------------------------------------------------------------- */
/* host_ops                                                                    */
/* -------------------------------------------------------------------------- */

static const struct pcie_host_ops bcm2712_ops = {
    .name             = "bcm2712-pcie1",
    .init             = bcm2712_init,
    .link_up          = bcm2712_link_up,
    .config_read8     = bcm2712_config_read8,
    .config_read16    = bcm2712_config_read16,
    .config_read32    = bcm2712_config_read32,
    .config_write32   = bcm2712_config_write32,
    .map_bar          = bcm2712_map_bar,
    .alloc_msi        = bcm2712_alloc_msi,
    .bind_irq_handler = bcm2712_bind_irq_handler,
};

int pcie_backend_register(void)
{
    return pcie_core_register_host(&bcm2712_ops);
}

#endif /* PLATFORM_RASPI5 */
