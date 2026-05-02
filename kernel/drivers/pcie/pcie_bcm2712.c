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
/*
 * EXT_CFG_DATA at 0x8000, NOT 0x9004. The BCM2712 variant table in
 * Linux's pcie-brcmstb.c puts 0x9004 in `reg_offsets[EXT_CFG_DATA]`
 * but that value is vestigial — only the brcm7425 variant's map_bus
 * consults reg_offsets. `brcm_pcie_map_bus` (used for 2712) uses the
 * compile-time constant `PCIE_EXT_CFG_DATA = 0x8000` (pcie-brcmstb.c
 * line 219) unconditionally. Our earlier 0x9004 picked up the
 * vestigial value; reads at 0x9004+0..7 happened to work because the
 * 2712 RC aliases vendor/device/command/status through a fast-path
 * mirror, but reads at 0x9004+8+ returned 0xFFFFFFFF. See the plan
 * doc's Phase 1.5 blocker writeup for the diagnostic trail.
 */
#define PCIE1_EXT_CFG_DATA      0x8000u

/* -------------------------------------------------------------------------- */
/* Link-training registers (Phase 1.5 — firmware doesn't train pcie1).        */
/* All offsets from ../slmos-reference-cache/rpi/rpi-linux-pcie-brcmstb.c unless noted.     */
/* -------------------------------------------------------------------------- */

#define PCIE1_RC_CFG_VENDOR_SPECIFIC_REG1  0x0188u
#define   RC_CFG_VENDOR_ENDIAN_MODE_BAR2_MASK  0xCu
#define PCIE1_RC_CFG_PRIV1_ID_VAL3         0x043Cu
#define   ID_VAL3_CLASS_CODE_MASK              0xFFFFFFu

#define PCIE1_MDIO_ADDR          0x1100u
#define PCIE1_MDIO_WR_DATA       0x1104u
#define PCIE1_MDIO_RD_DATA       0x1108u
#define   MDIO_PORT0_MASK        (0u << 16)     /* port = 0 */
#define   MDIO_CMD_READ          (1u << 20)
#define   MDIO_CMD_WRITE         (0u << 20)
#define   MDIO_DATA_DONE_MASK    0x80000000u
#define   MDIO_SET_ADDR_REGAD    0x1Fu
#define   MDIO_ADDR_BLOCK_PLL    0x1600u

#define PCIE1_RC_PL_PHY_CTL_15   0x184Cu
#define   PHY_CTL_15_PM_CLK_PERIOD_MASK  0xFFu

#define PCIE1_MISC_CTRL          0x4008u
#define   MISC_CTRL_SCB_ACCESS_EN_MASK       (1u << 12)
#define   MISC_CTRL_CFG_READ_UR_MODE_MASK    (1u << 13)
#define   MISC_CTRL_MAX_BURST_SIZE_MASK      (3u << 20)
#define   MISC_CTRL_MAX_BURST_SIZE_128       (1u << 20)  /* 128B on 2712 */
#define   MISC_CTRL_SCB0_SIZE_MASK           0xF8000000u

#define PCIE1_RC_BAR1_CONFIG_LO  0x402Cu
#define PCIE1_RC_BAR2_CONFIG_LO  0x4034u
#define PCIE1_RC_BAR2_CONFIG_HI  0x4038u
#define PCIE1_RC_BAR3_CONFIG_LO  0x403Cu
#define   RC_BAR_CONFIG_LO_SIZE_MASK  0x1Fu

#define PCIE1_RC_CONFIG_RETRY_TIMEOUT  0x405Cu
/* Timeout values match the BRCMSTB reference driver. The encoding
 * stuffs the 32-bit timeout field into bits[31:16] of each register;
 * the low 16 bits are reserved on this controller. The chosen values
 * give ~240 ms RC retry and ~57 ms UBUS-fabric timeout at the
 * 750 MHz UBUS clock, which is the documented BCM2712 default. */
#define   UBUS_TIMEOUT_VAL               0x0B2D0000u
#define   RC_CONFIG_RETRY_TIMEOUT_VAL    0x0ABA0000u
#define PCIE1_MISC_CTRL_REG            0x4064u  /* holds PERSTB at bit 2 */
#define   PCIE_CTRL_PERSTB_MASK        (1u << 2)
#define PCIE1_UBUS_CTRL                0x40A4u
#define   UBUS_CTRL_REPLY_ERR_DIS      (1u << 13)
#define   UBUS_CTRL_REPLY_DECERR_DIS   (1u << 19)
#define PCIE1_UBUS_TIMEOUT             0x40A8u
#define PCIE1_UBUS_BAR2_CONFIG_REMAP   0x40B4u
#define   UBUS_BAR_REMAP_ACCESS_EN     (1u << 0)
#define PCIE1_AXI_READ_ERROR_DATA      0x4170u

/* BCM2712 "chicken bits" for the AXI/PCIe QoS forwarding search.
 * Reference: brcm_pcie_set_tc_qos() @ pcie-brcmstb.c:556-604.
 * Without these, 2712D0 chips have broken QoS forwarding that
 * interacts badly with bridge config-space completions — observed
 * symptom: config reads past offset 0x07 return 0xFFFFFFFF because
 * the CplD never comes back to the requester. */
#define PCIE1_MISC_CTRL_1                     0x40A0u
#define   MISC_CTRL_1_EN_VDM_QOS_CONTROL_MASK (1u << 5)
#define PCIE1_AXI_INTF_CTRL                   0x416Cu
#define   AXI_EN_RCLK_QOS_ARRAY_FIX           (1u << 13)
#define   AXI_EN_QOS_UPDATE_TIMING_FIX        (1u << 12)
#define   AXI_DIS_QOS_GATING_IN_MASTER        (1u << 11)
#define   AXI_REQFIFO_EN_QOS_PROPAGATION      (1u <<  7)
#define   AXI_MASTER_MAX_OUTSTANDING_REQS     0x3Fu

/* HARD_DEBUG offset is variant-specific. For 2712 it's 0x4304 (generic
 * 0x4204). See pcie_offsets_bcm2712[] in the reference driver. */
#define PCIE1_HARD_DEBUG                 0x4304u
#define   HARD_DEBUG_SERDES_IDDQ_MASK    (1u << 27)
/* CLKREQ# control bits in HARD_DEBUG. Both must be cleared BEFORE
 * PERST# deassert — per Linux commit 1bbe2db (Feb 2026). If a
 * platform has a pull-up on CLKREQ# with no endpoint control
 * (which is the Pi 5 + AI HAT+ case), leaving these bits set in
 * their power-on state causes link init failures in the idle time
 * between PERST# deassertion and the normal post-link-up
 * brcm_config_clkreq() call. Symptom we saw: link trains (PHY +
 * DL both set) but config-space reads past offset 0x07 all return
 * 0xFFFFFFFF. Clearing CLKREQ bits before link-up fixes this. */
#define   HARD_DEBUG_CLKREQ_DEBUG_EN_MASK  (1u <<  1)
#define   HARD_DEBUG_CLKREQ_L1SS_EN_MASK   (1u << 21)
#define   HARD_DEBUG_CLKREQ_MASK \
    (HARD_DEBUG_CLKREQ_DEBUG_EN_MASK | HARD_DEBUG_CLKREQ_L1SS_EN_MASK)
/* Internal PERST# override — when set, forces PERST# low regardless
 * of the PCIE_CTRL.PERSTB bit. Used to extend the PERST# assertion
 * time for slow-to-lock endpoints (tperst_clk_ms sequence). */
#define   HARD_DEBUG_PERST_ASSERT_MASK     (1u <<  3)

/* MDIO PLL programming for 54 MHz xosc refclk (brcm_pcie_munge_pll).
 * Values lifted verbatim from the reference driver — these are
 * empirically-determined SerDes PHY settings for the 2712 variant. */
static const struct { uint8_t regad; uint16_t val; } mdio_pll_tune[] = {
    { 0x16, 0x50b9 }, { 0x17, 0xbda1 }, { 0x18, 0x0094 },
    { 0x19, 0x97b4 }, { 0x1b, 0x5030 }, { 0x1c, 0x5030 },
    { 0x1e, 0x0007 },
};
#define MDIO_PLL_TUNE_COUNT \
    (sizeof(mdio_pll_tune) / sizeof(mdio_pll_tune[0]))

/* Link-up polling budget — same timing as brcm_pcie_start_link in Linux.
 * Set TIMEOUT_US + POLL_INTERVAL_US; the attempt count derives. */
#define LINK_UP_TIMEOUT_US        100000u
#define LINK_UP_POLL_INTERVAL_US  5000u
#define LINK_UP_POLL_ATTEMPTS     (LINK_UP_TIMEOUT_US / LINK_UP_POLL_INTERVAL_US)
#define LINK_UP_TIMEOUT_MS        (LINK_UP_TIMEOUT_US / 1000u)

/* Post-link-up settling delay. 1 ms is plenty on well-behaved hardware;
 * the pi-5-1 + AI HAT+ config-space truncation blocker (tracked in the
 * plan doc, Phase 1.5) is not timing-sensitive — 3 seconds vs. 1 ms
 * showed the same 0xFFFFFFFF reads past offset 0x07. Keep this at 1 ms
 * until the blocker is understood; boot time matters. */
#define LINK_UP_SETTLE_US         1000u

/* BCM reset controller — shared across the SoC.
 * ID 7 and ID 43 are the two reset lines for pcie1 (bcm2712.dtsi:1048).
 * For 2712, bridge_sw_init uses ID 43 (the "bridge reset"). ID 7 is the
 * higher-level PCIe block reset; firmware already deasserts it at boot
 * (pcie2/RP1 works), so we leave it alone and only toggle ID 43. */
#define BCM_RESET_BASE          0x1001504318UL
#define BCM_RESET_BANK_SIZE     0x18u
#define   RESET_SW_INIT_SET     0x00u
#define   RESET_SW_INIT_CLEAR   0x04u
#define BCM_RESET_ID_BRIDGE     43u        /* bank 1, bit 11 */

/* Rescal — shared SATA/PCIe PHY calibration block.
 * Firmware may have already run this (pcie2 works), but running it again
 * is idempotent. */
#define RESCAL_BASE             0x1000119500UL
#define   RESCAL_START          0x00u
#define   RESCAL_START_BIT      (1u << 0)
#define   RESCAL_STATUS         0x08u
#define   RESCAL_STATUS_BIT     (1u << 0)

/*
 * Outbound window — firmware-programmed translation from PCIe-side
 * addresses to CPU physical addresses. These constants come from
 * `bcm2712.dtsi` lines 1053-1060 (cached at
 * ../slmos-reference-cache/rpi/rpi-linux-bcm2712.dtsi).
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

/*
 * Lock ordering: msi_lock → cfg_lock. `bcm2712_alloc_msi` holds
 * msi_lock across `bcm2712_config_read16`, which takes cfg_lock
 * internally. Never the reverse: nothing under cfg_lock touches
 * msi_lock. New code that needs both must respect this order.
 */
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
    /* Reset controller + rescal live in the same 2 MB block already
     * mapped by vmm_setup_platform for pcie1 RC + MIP0/1. Direct
     * access works because the block is Device-nGnRnE identity-mapped. */

    /*
     * Endpoint BAR windows at 0x1b_80000000+ (non-pref) and
     * 0x18_00000000+ (64-bit pref) are NOT mapped by default.
     * vmm_map_region installs Device mappings lazily when map_bar
     * is called on a specific BAR — see bcm2712_map_bar below.
     */
    return PCIE_OK;
}

/* -------------------------------------------------------------------------- */
/* Link training — port of brcm_pcie_setup() for the 2712 variant.             */
/* -------------------------------------------------------------------------- */

/*
 * Busy-wait delay on CNTPCT. Safe pre-scheduler. `us` typical range is
 * 1..1000; larger values are fine, smaller values may round up due to
 * counter granularity (54 MHz on Pi 5 = ~18.5 ns tick).
 */
static void bcm2712_udelay(uint32_t us)
{
    uint64_t freq, ticks, deadline;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    deadline = ticks + (freq * us + 999999u) / 1000000u;
    for (;;) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
        if (ticks >= deadline) break;
    }
}

static inline uint32_t reset_r32(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(BCM_RESET_BASE + off);
}
static inline void reset_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(BCM_RESET_BASE + off) = val;
}
static inline uint32_t rescal_r32(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(RESCAL_BASE + off);
}
static inline void rescal_w32(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(RESCAL_BASE + off) = val;
}

/*
 * Toggle a reset line managed by the brcmstb-reset controller.
 * Each bank is 0x18 bytes; bit = ID & 0x1F.
 * See ../slmos-reference-cache/rpi/rpi-linux-reset-brcmstb.c for the reference logic.
 */
static void bcm_reset_assert(uint32_t id)
{
    uint32_t bank = id >> 5;
    uint32_t bit  = 1u << (id & 0x1Fu);
    reset_w32(bank * BCM_RESET_BANK_SIZE + RESET_SW_INIT_SET, bit);
}
static void bcm_reset_deassert(uint32_t id)
{
    uint32_t bank = id >> 5;
    uint32_t bit  = 1u << (id & 0x1Fu);
    reset_w32(bank * BCM_RESET_BANK_SIZE + RESET_SW_INIT_CLEAR, bit);
    bcm2712_udelay(200);
}

/*
 * Run the shared PCIe/SATA rescal. Idempotent — if firmware already
 * ran it for pcie2, running again doesn't break anything. Per
 * ../slmos-reference-cache/rpi/rpi-linux-reset-brcmstb-rescal.c.
 */
static int rescal_bring_up(void)
{
    uint32_t reg = rescal_r32(RESCAL_START);
    rescal_w32(RESCAL_START, reg | RESCAL_START_BIT);
    if ((rescal_r32(RESCAL_START) & RESCAL_START_BIT) == 0) {
        ERROR("pcie1: rescal did not start");
        return PCIE_ERR_IO;
    }
    /* Poll STATUS, 100 us interval, 1 ms total. */
    for (int i = 0; i < 10; i++) {
        if (rescal_r32(RESCAL_STATUS) & RESCAL_STATUS_BIT) {
            reg = rescal_r32(RESCAL_START);
            rescal_w32(RESCAL_START, reg & ~RESCAL_START_BIT);
            return PCIE_OK;
        }
        bcm2712_udelay(100);
    }
    ERROR("pcie1: rescal timeout waiting for STATUS bit");
    return PCIE_ERR_TIMEOUT;
}

/*
 * MDIO write over the RC's internal PCIe PHY bus. Ports 0, used for
 * the PLL block on this SoC. Poll for DONE bit, 10 us interval, 100 us
 * total. Port / cmd encoding matches brcm_pcie_mdio_form_pkt().
 */
static int mdio_wait_done(uint32_t off, bool want_done_low)
{
    for (int i = 0; i < 10; i++) {
        uint32_t v = pcie1_r32(off);
        bool done_set = (v & MDIO_DATA_DONE_MASK) != 0;
        if (want_done_low ? !done_set : done_set) return PCIE_OK;
        bcm2712_udelay(10);
    }
    (void)off;
    return PCIE_ERR_TIMEOUT;
}

static int pcie1_mdio_write(uint8_t regad, uint16_t val)
{
    uint32_t pkt = MDIO_PORT0_MASK
                 | ((uint32_t)regad & 0xFFFFu)
                 | MDIO_CMD_WRITE;
    pcie1_w32(PCIE1_MDIO_ADDR, pkt);
    (void)pcie1_r32(PCIE1_MDIO_ADDR);          /* read-back fence */
    pcie1_w32(PCIE1_MDIO_WR_DATA, MDIO_DATA_DONE_MASK | val);
    /* brcm_pcie_mdio_write waits for DONE bit LOW (cleared by HW). */
    return mdio_wait_done(PCIE1_MDIO_WR_DATA, /*want_done_low=*/true);
}

static int munge_pll_54mhz(void)
{
    /* Set block-address register so subsequent writes land at 0x1600. */
    int rc = pcie1_mdio_write(MDIO_SET_ADDR_REGAD, MDIO_ADDR_BLOCK_PLL);
    if (rc != PCIE_OK) {
        ERROR("pcie1: MDIO block-address write failed (%d)", rc);
        return rc;
    }
    for (size_t i = 0; i < MDIO_PLL_TUNE_COUNT; i++) {
        rc = pcie1_mdio_write(mdio_pll_tune[i].regad, mdio_pll_tune[i].val);
        if (rc != PCIE_OK) {
            ERROR("pcie1: MDIO PLL write [%u] regad=0x%x failed (%d)",
                  (unsigned)i, mdio_pll_tune[i].regad, rc);
            return rc;
        }
    }
    bcm2712_udelay(200);
    return PCIE_OK;
}

/*
 * Program a single outbound window (CPU MMIO → PCIe memory-space
 * address). Reference: brcm_pcie_set_outbound_win().
 */
static void set_outbound_win(unsigned win,
                             uint64_t cpu_addr, uint64_t pcie_addr, uint64_t size)
{
    /* Window base/hi registers — PCIe-side low/high 32. */
    pcie1_w32(0x400Cu + win * 8, (uint32_t)(pcie_addr & 0xFFFFFFFFu));
    pcie1_w32(0x4010u + win * 8, (uint32_t)(pcie_addr >> 32));

    uint64_t cpu_mb   = cpu_addr / (1024ull * 1024ull);
    uint64_t limit_mb = (cpu_addr + size - 1ull) / (1024ull * 1024ull);

    /* BASE_LIMIT packs low 12 bits each: base in bits[15:4], limit in
     * bits[31:20]. High bits go in separate BASE_HI / LIMIT_HI regs. */
    uint32_t bl = ((uint32_t)(cpu_mb   & 0xFFFu) << 4)
                | ((uint32_t)(limit_mb & 0xFFFu) << 20);
    pcie1_w32(0x4070u + win * 4, bl);
    /* High 8 bits of each address (4096..1M GB worth). */
    pcie1_w32(0x4080u + win * 8, (uint32_t)(cpu_mb   >> 12) & 0xFFu);
    pcie1_w32(0x4084u + win * 8, (uint32_t)(limit_mb >> 12) & 0xFFu);
}

/*
 * Wait for link training to complete. Same timing as the Linux
 * brcm_pcie_start_link path (LINK_UP_TIMEOUT_MS ms total).
 */
static int wait_link_up(void)
{
    for (unsigned i = 0; i < LINK_UP_POLL_ATTEMPTS; i++) {
        uint32_t status = pcie1_r32(PCIE1_MISC_STATUS);
        if ((status & (STATUS_PHY_LINKUP | STATUS_DL_ACTIVE))
            == (STATUS_PHY_LINKUP | STATUS_DL_ACTIVE)) {
            INFO("pcie1: link up (status=0x%x) after %u ms",
                 status, i * LINK_UP_POLL_INTERVAL_US / 1000u);
            return PCIE_OK;
        }
        bcm2712_udelay(LINK_UP_POLL_INTERVAL_US);
    }
    uint32_t status = pcie1_r32(PCIE1_MISC_STATUS);
    ERROR("pcie1: link training timeout after %u ms (status=0x%x)",
          LINK_UP_TIMEOUT_MS, status);
    return PCIE_ERR_NOLINK;
}

/*
 * Steps 1-5 of the train_link sequence: reset the bridge, power up
 * the PHY, run MDIO PLL tuning, and apply the L1SS PM clock errata.
 * All MMIO after the bridge deassert goes to pcie1_regs
 * (PCIE1_BASE); the reset writes go to BCM_RESET_BASE, a different
 * Device-nGnRnE region. ARM Device-nGnRnE is strongly ordered per
 * location but only weakly ordered across locations, so a `dsb sy`
 * bridges the two register windows — the udelay loop alone reads
 * CNTPCT and provides no MMIO ordering guarantee.
 */
static int bcm2712_phy_bringup(void)
{
    int rc = rescal_bring_up();
    if (rc != PCIE_OK) return rc;

    bcm_reset_assert(BCM_RESET_ID_BRIDGE);
    bcm2712_udelay(200);
    bcm_reset_deassert(BCM_RESET_ID_BRIDGE);

    /* Bridge (BCM_RESET_BASE) → PHY (PCIE1_BASE) crossover. */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Clear SERDES_IDDQ — power on the PHY. */
    uint32_t tmp = pcie1_r32(PCIE1_HARD_DEBUG);
    tmp &= ~HARD_DEBUG_SERDES_IDDQ_MASK;
    pcie1_w32(PCIE1_HARD_DEBUG, tmp);
    bcm2712_udelay(200);

    /* MDIO PLL tuning for 54 MHz xosc refclk (2712-specific). */
    rc = munge_pll_54mhz();
    if (rc != PCIE_OK) return rc;

    /* L1SS errata — PM clock period = 18.52 ns (encoded 0x12). */
    tmp = pcie1_r32(PCIE1_RC_PL_PHY_CTL_15);
    tmp &= ~PHY_CTL_15_PM_CLK_PERIOD_MASK;
    tmp |= 0x12;
    pcie1_w32(PCIE1_RC_PL_PHY_CTL_15, tmp);

    return PCIE_OK;
}

/*
 * Steps 6 + 6b of the train_link sequence, extracted so train_link
 * reads as a sequence of named phases rather than a wall of register
 * pokes. MISC_CTRL bits + BCM2712-specific AXI QoS chicken bits are
 * closely related (the AXI path arbitrates the bus transactions that
 * MISC_CTRL gates), so they live together.
 *
 * Observed consequence on 2712D0: without the QoS chicken bits,
 * config-space reads past offset 0x07 return 0xFFFFFFFF. The
 * TIMING_FIX bit is Reserved-0 on 2712C1 — detect readback and fall
 * back to throttling the AXI master's outstanding-request count.
 *
 * CFG_READ_UR_MODE is SET here to match Linux's brcm_pcie_setup
 * (pcie-brcmstb.c:1223). This tells the RC to convert an endpoint
 * UR (Unsupported Request) into the standard 0xFFFFFFFF read-back,
 * rather than propagating the UR upstream as an AXI abort. The
 * RC_CONFIG_RETRY_TIMEOUT (~240 ms, set below) still governs how
 * long the RC waits on CRS completions before giving up.
 */
static void bcm2712_misc_and_axi_qos(void)
{
    uint32_t tmp = pcie1_r32(PCIE1_MISC_CTRL);
    tmp |= MISC_CTRL_SCB_ACCESS_EN_MASK;
    tmp |= MISC_CTRL_CFG_READ_UR_MODE_MASK;
    tmp &= ~MISC_CTRL_MAX_BURST_SIZE_MASK;
    tmp |=  MISC_CTRL_MAX_BURST_SIZE_128;
    pcie1_w32(PCIE1_MISC_CTRL, tmp);

    uint32_t axi = pcie1_r32(PCIE1_AXI_INTF_CTRL);
    axi &= ~AXI_REQFIFO_EN_QOS_PROPAGATION;
    axi |=  AXI_EN_RCLK_QOS_ARRAY_FIX
         |  AXI_EN_QOS_UPDATE_TIMING_FIX
         |  AXI_DIS_QOS_GATING_IN_MASTER;
    pcie1_w32(PCIE1_AXI_INTF_CTRL, axi);
    axi = pcie1_r32(PCIE1_AXI_INTF_CTRL);
    if (!(axi & AXI_EN_QOS_UPDATE_TIMING_FIX)) {
        /* 2712C1 — TIMING_FIX is Reserved-0. Throttle AXI master to
         * 15 outstanding requests as a best-effort mitigation. */
        axi &= ~AXI_MASTER_MAX_OUTSTANDING_REQS;
        axi |= 15u;
        pcie1_w32(PCIE1_AXI_INTF_CTRL, axi);
    }
    tmp = pcie1_r32(PCIE1_MISC_CTRL_1);
    tmp &= ~MISC_CTRL_1_EN_VDM_QOS_CONTROL_MASK;
    pcie1_w32(PCIE1_MISC_CTRL_1, tmp);
}

/*
 * Steps 13b + 14 + 15: deassert PERST# in the order HATs with
 * brcm,tperst-clk-ms need. CLKREQ# disabled first so a platform
 * pull-up on an endpoint without CLKREQ# control (the AI HAT+ case)
 * can't confuse the link state machine during the idle gap. Then
 * the two-phase PERST# release with 100 ms of stable refclk in
 * between, followed by the CEM §6.6.1 100 ms settle.
 */
static void bcm2712_perst_tperst_clk_ms(void)
{
    uint32_t tmp = pcie1_r32(PCIE1_HARD_DEBUG);
    tmp &= ~HARD_DEBUG_CLKREQ_MASK;
    pcie1_w32(PCIE1_HARD_DEBUG, tmp);

    /* Force PERST# low via the internal bit. */
    tmp = pcie1_r32(PCIE1_HARD_DEBUG);
    tmp |= HARD_DEBUG_PERST_ASSERT_MASK;
    pcie1_w32(PCIE1_HARD_DEBUG, tmp);

    /* Deassert main PERST# (bit 2 of PCIE_CTRL). Refclk now stable
     * while endpoint still sees PERST# asserted externally. */
    tmp = pcie1_r32(PCIE1_MISC_CTRL_REG);
    tmp |= PCIE_CTRL_PERSTB_MASK;
    pcie1_w32(PCIE1_MISC_CTRL_REG, tmp);
    bcm2712_udelay(100000);

    /* Release internal PERST# — endpoint actually sees deassertion here. */
    tmp = pcie1_r32(PCIE1_HARD_DEBUG);
    tmp &= ~HARD_DEBUG_PERST_ASSERT_MASK;
    pcie1_w32(PCIE1_HARD_DEBUG, tmp);

    /* PCIe CEM §6.6.1 — 100 ms from PERST# deassertion to first
     * config-space access. */
    bcm2712_udelay(100000);
}

/*
 * Step 17 + 17b: program bus numbers on the RC bridge so downstream
 * config cycles (bus 1) are forwarded, and enable the standard
 * BUS_MASTER + MEM_SPACE bits on the bridge's command register.
 * Without these, the bridge silently limits config-cycle forwarding
 * to the first two dwords on Pi 5 — reads past offset 0x07 on the
 * endpoint return 0xFFFFFFFF.
 *
 * MEM_BASE / MEM_LIMIT cover the non-prefetchable outbound window.
 * Encoding is NONSTANDARD on BCM2712's internal RC bridge: the
 * 16-bit fields at config 0x20 encode bits [39:24] of the 40-bit
 * CPU address rather than standard PCI's bits [31:20]. With
 * PCIE1_NONPREF_CPU_BASE = 0x1b_8000_0000 and 2 GB size, this
 * produces base=0x1b80, limit=0x1bff (packed as 0x1bff_1b80).
 *
 * RC config space is directly mapped at pcie1_regs — the Linux
 * driver confirms this pattern (brcm_pcie_map_bus at
 * ../slmos-reference-cache/rpi/rpi-linux-pcie-brcmstb.c:940-941: RC access goes
 * through `base + offset` without EXT_CFG_INDEX).
 */
#define BCM2712_RC_CFG_COMMAND         0x04u  /* PCI COMMAND/STATUS dword */
#define BCM2712_RC_CFG_BUS_NUMBERS     0x18u  /* primary/sec/subord */
#define BCM2712_RC_CFG_MEM_BASE_LIMIT  0x20u  /* nonstandard 40-bit encoding */

/* bcm2712_program_rc_bridge indexes pcie1_regs as uint32_t[], so each
 * offset must be a multiple of 4. RC_CFG_IDX reads as "index into the
 * uint32_t view" at call sites; _Static_assert catches typos like
 * 0x05 at compile time rather than silently writing to (offset/4)*4. */
#define RC_CFG_IDX(off) ((off) / 4u)
_Static_assert(BCM2712_RC_CFG_COMMAND        % 4 == 0, "RC CMD offset must be 4-aligned");
_Static_assert(BCM2712_RC_CFG_BUS_NUMBERS    % 4 == 0, "RC BUS offset must be 4-aligned");
_Static_assert(BCM2712_RC_CFG_MEM_BASE_LIMIT % 4 == 0, "RC MEM_BASE_LIMIT offset must be 4-aligned");

#define BCM2712_RC_CMD_BITS            0x0007u   /* IO + MEM + BUS_MASTER */
#define BCM2712_RC_BUS_LAYOUT          0x00010100u /* primary=0, secondary=1, subordinate=1 */

static void bcm2712_program_rc_bridge(void)
{
    volatile uint32_t *rc_cfg = (volatile uint32_t *)pcie1_regs;

    rc_cfg[RC_CFG_IDX(BCM2712_RC_CFG_BUS_NUMBERS)] = BCM2712_RC_BUS_LAYOUT;

    uint32_t rc_cmd = rc_cfg[RC_CFG_IDX(BCM2712_RC_CFG_COMMAND)];
    /* Preserve STATUS (bits 16..31) and set IO+MEM+BUS_MASTER in the
     * COMMAND half. STATUS is RW1C, so writing back what was read is
     * semantically equivalent to not touching it for zero bits. */
    rc_cmd = (rc_cmd & 0xFFFF0000u) | BCM2712_RC_CMD_BITS;
    rc_cfg[RC_CFG_IDX(BCM2712_RC_CFG_COMMAND)] = rc_cmd;

    /* MEM_BASE / MEM_LIMIT derived from the outbound window constants.
     * See comment above this function for the encoding. */
    uint32_t base_field  = (uint32_t)(PCIE1_NONPREF_CPU_BASE >> 24) & 0xFFFFu;
    uint32_t limit_field = (uint32_t)(
        (PCIE1_NONPREF_CPU_BASE + PCIE1_NONPREF_SIZE - 1ULL) >> 24) & 0xFFFFu;
    rc_cfg[RC_CFG_IDX(BCM2712_RC_CFG_MEM_BASE_LIMIT)] =
        (limit_field << 16) | base_field;
}

/*
 * Full link-training sequence for pcie1. Called from bcm2712_init
 * before host_ops signals link_up. See plan §2.3 / Phase 1.5 for
 * context on why SLM-OS has to do this.
 *
 * Step numbering below is 1-18, matching brcm_pcie_setup() in the
 * upstream Linux driver so the reference doc reads 1:1 against this
 * function. Steps 1-5 moved into bcm2712_phy_bringup(); 6+6b into
 * bcm2712_misc_and_axi_qos(); 13b+14+15 into
 * bcm2712_perst_tperst_clk_ms(); 17+17b into bcm2712_program_rc_bridge().
 * The remaining in-line steps (7-13, 16, 18) live here.
 *
 * Idempotent once link is up: if the link is already trained, return
 * PCIE_OK without touching resets.
 */
static int bcm2712_train_link(void)
{
    /* If firmware (or a prior SLM-OS boot) already trained the link,
     * don't re-reset — that'd disconnect the HAT. */
    uint32_t status = pcie1_r32(PCIE1_MISC_STATUS);
    if ((status & (STATUS_PHY_LINKUP | STATUS_DL_ACTIVE))
        == (STATUS_PHY_LINKUP | STATUS_DL_ACTIVE)) {
        INFO("pcie1: link already up (status=0x%x) — skipping training",
             status);
        return PCIE_OK;
    }

    INFO("pcie1: training link (status=0x%x before reset)", status);

    /* 1-5. rescal, bridge reset, PHY power, PLL tune, L1SS errata. */
    int rc = bcm2712_phy_bringup();
    if (rc != PCIE_OK) return rc;

    /* 6 + 6b. MISC_CTRL + BCM2712 AXI QoS chicken bits. Deliberately
     *         leaves CFG_READ_UR_MODE cleared so the RC retries on
     *         CRS rather than folding it into 0xFFFFFFFF reads. */
    bcm2712_misc_and_axi_qos();

    /*
     * 7. Inbound window (RC_BAR2). DT says
     *    dma-ranges = 0x10_00000000 PCIe → 0x0 CPU, 64 GB.
     *    Encoded-size = log2(64GB) - 15 = 36 - 15 = 21 (0x15).
     */
    pcie1_w32(PCIE1_RC_BAR2_CONFIG_LO,
              (0u /* cpu_phys low */ & 0xFFFFFFE0u) | 0x15u);
    pcie1_w32(PCIE1_RC_BAR2_CONFIG_HI, 0x10u /* high 32 of 0x10_00000000 */);

    uint32_t tmp = pcie1_r32(PCIE1_UBUS_BAR2_CONFIG_REMAP);
    tmp |= UBUS_BAR_REMAP_ACCESS_EN;
    pcie1_w32(PCIE1_UBUS_BAR2_CONFIG_REMAP, tmp);

    /* SCB0 size: on Pi 5 with 4 GB RAM, log2(4GB) - 15 = 17 (0x11).
     * Bits 27-31 of MISC_CTRL. */
    tmp = pcie1_r32(PCIE1_MISC_CTRL);
    tmp = (tmp & ~MISC_CTRL_SCB0_SIZE_MASK) | ((17u & 0x1Fu) << 27);
    pcie1_w32(PCIE1_MISC_CTRL, tmp);

    /* 8. Suppress AXI error responses on unreachable endpoints
     *    (2712-specific — prevents AER aborts during enumeration). */
    tmp = pcie1_r32(PCIE1_UBUS_CTRL);
    tmp |= UBUS_CTRL_REPLY_ERR_DIS | UBUS_CTRL_REPLY_DECERR_DIS;
    pcie1_w32(PCIE1_UBUS_CTRL, tmp);
    pcie1_w32(PCIE1_AXI_READ_ERROR_DATA, 0xFFFFFFFFu);

    /* 9. Timeouts (2712-specific; values from reference driver). */
    pcie1_w32(PCIE1_UBUS_TIMEOUT, UBUS_TIMEOUT_VAL);
    pcie1_w32(PCIE1_RC_CONFIG_RETRY_TIMEOUT, RC_CONFIG_RETRY_TIMEOUT_VAL);

    /* 10. Disable RC_BAR1 and RC_BAR3 (clear size field). */
    tmp = pcie1_r32(PCIE1_RC_BAR1_CONFIG_LO);
    tmp &= ~RC_BAR_CONFIG_LO_SIZE_MASK;
    pcie1_w32(PCIE1_RC_BAR1_CONFIG_LO, tmp);
    tmp = pcie1_r32(PCIE1_RC_BAR3_CONFIG_LO);
    tmp &= ~RC_BAR_CONFIG_LO_SIZE_MASK;
    pcie1_w32(PCIE1_RC_BAR3_CONFIG_LO, tmp);

    /* 11. Set class code to PCI-PCI bridge (0x060400). */
    tmp = pcie1_r32(PCIE1_RC_CFG_PRIV1_ID_VAL3);
    tmp = (tmp & ~ID_VAL3_CLASS_CODE_MASK) | 0x060400u;
    pcie1_w32(PCIE1_RC_CFG_PRIV1_ID_VAL3, tmp);

    /* 12. Outbound windows — match dma-ranges from bcm2712.dtsi:
     *     win 0: PCIe 0x00_80000000 → CPU 0x1b_80000000, 2 GB
     *     win 1: PCIe 0x04_00000000 → CPU 0x18_00000000, 14 GB */
    set_outbound_win(0, PCIE1_NONPREF_CPU_BASE, PCIE1_NONPREF_PCIE_BASE,
                     PCIE1_NONPREF_SIZE);
    set_outbound_win(1, PCIE1_PREF64_CPU_BASE, PCIE1_PREF64_PCIE_BASE,
                     PCIE1_PREF64_SIZE);

    /* 13. Endian mode = little-endian for BAR2 (bits 2-3 = 0). */
    tmp = pcie1_r32(PCIE1_RC_CFG_VENDOR_SPECIFIC_REG1);
    tmp &= ~RC_CFG_VENDOR_ENDIAN_MODE_BAR2_MASK;
    pcie1_w32(PCIE1_RC_CFG_VENDOR_SPECIFIC_REG1, tmp);

    /* 13b + 14 + 15. CLKREQ# disable, tperst_clk_ms dance, CEM settle. */
    bcm2712_perst_tperst_clk_ms();

    /* 16. Wait for link-up (PHY + DL bits). */
    rc = wait_link_up();
    if (rc != PCIE_OK) return rc;

    /* 17 + 17b. RC bridge bus numbers, command register, MEM_BASE/LIMIT. */
    bcm2712_program_rc_bridge();

    /* 17c (Phase 8 #253). Disable ASPM L0s on the pcie1 RC side.
     * Linux's hailo_pci clears LNKCTL.ASPM_L0S on BOTH the endpoint
     * and the parent RC — endpoint already checks clean when SLM-OS
     * probes (pi5_init logs "already off"), but the RC side is ours
     * to own. ASPM L0s transitions have a history of silently
     * dropping PCIe completion packets; leaving it enabled on the
     * RC could plausibly explain the Phase 8 boundary-submit stall
     * where fw's num_proc never advances.
     *
     * BCM2712 RC config is directly mapped at pcie1_regs (no
     * EXT_CFG_INDEX indirection — see bcm2712_program_rc_bridge).
     * Walk the cap list, find PCIe cap ID 0x10, clear bit 0 of
     * LNKCTL at cap_base + 0x10. Cap list entries are dword-aligned
     * per PCI spec so every config access here is a single 32-bit
     * MMIO read/write with shifts for sub-dword extraction. */
    {
        volatile uint32_t *rc_cfg = (volatile uint32_t *)pcie1_regs;
        /* Cap list lives in standard config space (256 B); 0xFC is
         * the last dword-aligned offset that can host a 4-byte cap
         * header. Anything beyond that (or unaligned) is a malformed
         * pointer and we abort the walk. PCI spec also caps the chain
         * at 48 entries (one per nibble of next-pointer space), used
         * here as a belt-and-braces bound against a circular cap
         * list. */
        const uint8_t cap_offset_max = 0xFCu;
        const int     cap_walk_max   = 48;

        uint32_t status_dw = rc_cfg[0x04 / 4];     /* cmd(low) + status(high) */
        uint16_t cfg_status = (uint16_t)(status_dw >> 16);
        if (cfg_status & 0x0010u) {                /* PCI_STATUS_CAP_LIST */
            uint32_t capptr_dw = rc_cfg[0x34 / 4]; /* CAP_POINTER at 0x34 */
            uint8_t  ptr = (uint8_t)(capptr_dw & 0xFCu);
            int      iter = 0;
            bool     found = false;
            while (ptr != 0 && ptr <= cap_offset_max && iter < cap_walk_max) {
                uint32_t cap_dw = rc_cfg[ptr / 4u];
                uint8_t  cap_id = (uint8_t)(cap_dw & 0xFFu);
                uint8_t  next   = (uint8_t)((cap_dw >> 8) & 0xFFu);
                if (cap_id == 0x10) {              /* PCI Express cap */
                    found = true;
                    uint16_t lnkctl_off = (uint16_t)(ptr + 0x10u);
                    uint32_t lnkctl_dw  = rc_cfg[lnkctl_off / 4u];
                    uint16_t lnkctl     = (uint16_t)(lnkctl_dw & 0xFFFFu);
                    if (lnkctl & 0x0001u) {
                        INFO("pcie1: RC LNKCTL 0x%04x had ASPM_L0S set; "
                             "clearing", lnkctl);
                        /* Dword-wide RMW. The upper 16 bits are
                         * LNKSTA, which is RO/W1C — writing back
                         * the previously-read value is a no-op for
                         * its status bits, so this RMW does not
                         * clobber link status accidentally. */
                        rc_cfg[lnkctl_off / 4u] =
                            (lnkctl_dw & 0xFFFF0000u) |
                            (uint32_t)(lnkctl & ~0x0001u);
                        uint32_t verify_dw = rc_cfg[lnkctl_off / 4u];
                        INFO("pcie1: RC LNKCTL after clear: 0x%04x",
                             (unsigned)(verify_dw & 0xFFFFu));
                    } else {
                        INFO("pcie1: RC LNKCTL 0x%04x ASPM_L0S already off",
                             lnkctl);
                    }
                    break;
                }
                ptr = (uint8_t)(next & 0xFCu);
                iter++;
            }
            if (!found) {
                INFO("pcie1: RC cap list ended without finding PCIe cap "
                     "(0x10) — last ptr=0x%02x iter=%d (max=%d)",
                     (unsigned)ptr, iter, cap_walk_max);
            }
        } else {
            INFO("pcie1: RC STATUS 0x%04x has no CAP_LIST bit — "
                 "cannot gate ASPM", cfg_status);
        }
    }

    /*
     * 18. Post-link-up settling delay (see LINK_UP_SETTLE_US — 1 ms,
     *     kept short so boot time doesn't suffer while the
     *     config-space-read blocker is investigated in Phase 1.5).
     */
    bcm2712_udelay(LINK_UP_SETTLE_US);

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

    /* Train pcie1 link before the rest of the PCIe core tries to scan.
     * Link-down is non-fatal — the core will log and skip enumeration,
     * which is the same behaviour as "no HAT+ plugged in". */
    rc = bcm2712_train_link();
    if (rc != PCIE_OK) {
        INFO("pcie1: link training failed (%d) — pcie1 will stay down", rc);
        /* Don't return the error: an AI-HAT-less boot should still
         * succeed, and pcie_core's own link_up() check will gate
         * enumeration. */
    }

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
    /*
     * Read-back of INDEX after the write acts as a barrier and gives
     * the bridge time to set up the TLP. Without this, back-to-back
     * accesses on the 2712 bridge sometimes return stale data for
     * offsets past the first dword. Matches the pattern Linux uses
     * (readl after write).
     */
    (void)pcie1_r32(PCIE1_EXT_CFG_INDEX);
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
    /* Same read-back-as-barrier pattern as cfg_read32_locked above:
     * back-to-back ECAM accesses on the BCM2712 bridge can otherwise
     * land before the INDEX write has set up the TLP, causing the
     * subsequent DATA write to target the previous bus/dev/func.
     * Matches the Linux brcmstb pattern. */
    (void)pcie1_r32(PCIE1_EXT_CFG_INDEX);
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

/*
 * Report the non-prefetchable outbound window so pcie_core's BAR
 * allocator can assign endpoint BAR addresses. pcie1's 2 GB
 * PCIe-side range starts at 0x80000000 (maps to CPU 0x1b_80000000).
 */
static int bcm2712_get_mmio_window(uint64_t *base_out, uint64_t *size_out)
{
    *base_out = PCIE1_NONPREF_PCIE_BASE;
    *size_out = PCIE1_NONPREF_SIZE;
    return PCIE_OK;
}

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

    /* Round the region to a 2 MB boundary for vmm_map_region. Map
     * 2 MB blocks one at a time and tolerate already-mapped blocks
     * (a second BAR sharing the first BAR's 2 MB block — common on
     * Hailo-8 where BAR0/2/4 are all inside the first ~32 KB of the
     * outbound window). Blocks pre-installed by vmm_setup_platform
     * or by earlier pcie_map_bar calls are accepted as-is.
     *
     * Invariant this relies on: every PA inside the pcie1 outbound
     * window is identity-mapped (CPU VA == PA). Both the prefetchable
     * (0x18_0000_0000..0x1b_7FFF_FFFF) and non-prefetchable
     * (0x1b_8000_0000..0x1b_FFFF_FFFF) regions follow this rule —
     * pcie_map_bar's only caller is the device enumeration path,
     * which always asks for the CPU-side outbound-translated
     * address. If a future caller ever installs a non-identity
     * mapping in these L1 entries, the vmm_is_mapped skip would
     * silently return a mis-mapped region; add a PA readback or a
     * vmm_lookup_phys() check if that becomes possible. */
    uint64_t phys_aligned = cpu_phys & ~(BLOCK_SIZE - 1ull);
    uint64_t region_end   = cpu_phys + size;
    uint64_t size_aligned = ((region_end + BLOCK_SIZE - 1ull)
                            & ~(BLOCK_SIZE - 1ull)) - phys_aligned;

    for (uint64_t off = 0; off < size_aligned; off += BLOCK_SIZE) {
        uint64_t block_pa = phys_aligned + off;
        if (vmm_is_mapped(block_pa)) continue;
        int rc = vmm_map_block(block_pa, block_pa, VMM_FLAGS_DEVICE);
        if (rc != 0) {
            ERROR("pcie1: vmm_map_block(0x%llx) failed",
                  (unsigned long long)block_pa);
            return NULL;
        }
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

    /*
     * Hold msi_lock across the whole allocate-then-validate
     * sequence. Earlier revision released the lock between the
     * bitmap reservation and the MMC validation; the brief window
     * wasn't exploitable (reserved vectors can't be re-allocated
     * while their bit is set), but holding the lock across the
     * whole decision is simpler to reason about. Safe against
     * cfg_lock recursion because bcm2712_config_read16 acquires
     * cfg_lock and nothing under cfg_lock touches msi_lock.
     */
    irq_flags_t msi_flags = spin_lock_irqsave(&msi_lock);
    int base_vec = mip1_alloc_block_locked(count);
    if (base_vec < 0) {
        spin_unlock_irqrestore(&msi_lock, msi_flags);
        return PCIE_ERR_NOVEC;
    }

    /* Check the endpoint's MSI_CTRL — it advertises how many
     * messages the device is capable of via MMC[3:1]. A device with
     * MMC=0 can only take 1 vector; MMC=1 → up to 2; MMC=2 → 4; etc. */
    uint16_t ctrl = bcm2712_config_read16(dev->bus, dev->dev, dev->func,
                                          (uint16_t)(cap_ptr + MSI_MSG_CTRL_OFFSET));
    uint16_t mmc = (ctrl & MSI_CTRL_MMC_MASK) >> 1;
    uint32_t max_msgs = 1u << mmc;
    if ((uint32_t)count > max_msgs) {
        /* Give back what we reserved. */
        uint8_t mask = (uint8_t)(((1u << count) - 1u) << base_vec);
        mip1_vec_inuse &= ~mask;
        spin_unlock_irqrestore(&msi_lock, msi_flags);
        return PCIE_ERR_NOVEC;
    }
    spin_unlock_irqrestore(&msi_lock, msi_flags);

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

    /*
     * Bind-once contract: reject a second bind to an already-bound
     * vector. `mip1_slots[]` is read by the IRQ trampoline outside
     * any lock — the DSB SY below publishes the write before
     * gic_enable_irq, which works for the first delivery but does
     * not handle a cross-CPU rebind cleanly (the trampoline could
     * read a half-updated slot). Enforcing bind-once eliminates
     * that race entirely. A consumer that needs rebinding must
     * first unregister the vector.
     */
    irq_flags_t msi_flags = spin_lock_irqsave(&msi_lock);
    if (mip1_slots[vec].handler) {
        spin_unlock_irqrestore(&msi_lock, msi_flags);
        WARN("pcie1: MSI vector %d already bound — rejecting rebind", vec);
        return PCIE_ERR_INVAL;
    }
    mip1_slots[vec].handler = handler;
    mip1_slots[vec].ctx = ctx;
    /* DSB SY publishes the slot to DRAM before we enable the GIC
     * line. Required on Pi 5 (BCM2712 has no SMPEN, per-core L2
     * caches are incoherent — see kernel/CLAUDE.md). */
    __asm__ volatile("dsb sy" ::: "memory");
    spin_unlock_irqrestore(&msi_lock, msi_flags);

    int rc = gic_register_handler(irq, mip1_trampolines[vec]);
    if (rc != 0) {
        msi_flags = spin_lock_irqsave(&msi_lock);
        mip1_slots[vec].handler = NULL;
        mip1_slots[vec].ctx = NULL;
        /* Symmetric with the publish path: push the cleared slot
         * out to DRAM so a later rebind sees a real NULL, not a
         * stale cached handler pointer from this CPU's L2. */
        __asm__ volatile("dsb sy" ::: "memory");
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
    .get_mmio_window  = bcm2712_get_mmio_window,
    .alloc_msi        = bcm2712_alloc_msi,
    .bind_irq_handler = bcm2712_bind_irq_handler,
};

int pcie_backend_register(void)
{
    return pcie_core_register_host(&bcm2712_ops);
}

#endif /* PLATFORM_RASPI5 */
