/*
 * pcie_tegra194.c — Tegra234 PCIe C8 root complex bring-up.
 *
 * Drives the BPMP MRQs + APPL wrapper registers + DesignWare DBI + iATU
 * needed to train the PCIe link and reach the RTL8168 endpoint from bare
 * metal at EL2, post-kexec. Layered on top of the BPMP IPC stack at
 * kernel/drivers/bpmp/.
 *
 * Reference files:
 *   docs/reference/linux-pcie-tegra194.c
 *   docs/reference/linux-pcie-designware.c
 *   docs/reference/linux-pcie-designware.h
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "pcie_tegra194.h"
#include "bpmp.h"
#include "timer.h"
#include "debug.h"
#include <stdbool.h>
#include <stdint.h>

/* PCIe C8 addresses (from DT, verified by Linux devmem probing). */
#define TEGRA_PCIE_C8_APPL      0x140a0000UL
#define TEGRA_PCIE_C8_CFG       0x2a000000UL      /* iATU-retargeted config window */
#define TEGRA_PCIE_C8_ATU       0x2a040000UL      /* DW iATU + eDMA registers */
#define TEGRA_PCIE_C8_DBI       0x2a080000UL      /* DBI (RC config space) */

/* Tegra234 controller id for PCIe C8 (DT: nvidia,controller-id = <8>). */
#define TEGRA_PCIE_C8_CID       8

/* APPL register offsets (docs/reference/linux-pcie-tegra194.c:41..) */
#define APPL_PINMUX                     0x000
#define APPL_PINMUX_PEX_RST             (1u << 0)
#define APPL_CTRL                       0x004
#define APPL_CTRL_SYS_PRE_DET_STATE     (1u << 6)
#define APPL_CTRL_LTSSM_EN              (1u << 7)
#define APPL_LINK_STATUS                0x0CC
#define APPL_LINK_STATUS_RDLH_LINK_UP   (1u << 0)
#define APPL_DEBUG                      0x0D0
#define APPL_DEBUG_LTSSM_STATE_SHIFT    3
#define APPL_DEBUG_LTSSM_STATE_MASK     (0x3Fu << 3)
#define APPL_DEBUG_LTSSM_L0             0x11
#define APPL_DM_TYPE                    0x100
#define APPL_DM_TYPE_RP                 0x4
#define APPL_CFG_BASE_ADDR              0x104
#define APPL_CFG_BASE_ADDR_MASK         0xFFFFF000u
#define APPL_CFG_IATU_DMA_BASE_ADDR     0x108
#define APPL_CFG_IATU_DMA_BASE_ADDR_MASK 0xFFFC0000u
#define APPL_CFG_MISC                   0x110
#define APPL_CFG_MISC_ARCACHE_SHIFT     10
#define APPL_CFG_MISC_ARCACHE_VAL       3u
#define APPL_CFG_SLCG_OVERRIDE          0x114

/* iATU outbound region stride (unrolled mapping) and inner offsets. */
#define ATU_REGION_STRIDE               0x200
#define ATU_REGION_DIR_OB_OFFSET        0x0000      /* outbound regions start at ATU base */
#define ATU_CTRL1                       0x000
#define ATU_CTRL2                       0x004
#define ATU_LOWER_BASE                  0x008
#define ATU_UPPER_BASE                  0x00C
#define ATU_LIMIT                       0x010
#define ATU_LOWER_TARGET                0x014
#define ATU_UPPER_TARGET                0x018
#define ATU_CTRL1_TYPE_CFG1             0x5
#define ATU_CTRL2_ENABLE                (1u << 31)

static bool g_host_inited;

static inline uint32_t mmio_read32(uintptr_t addr)
{
    uint32_t v = *(volatile uint32_t *)addr;
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static inline void mmio_write32(uintptr_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t appl_read(uint32_t off)
{
    return mmio_read32(TEGRA_PCIE_C8_APPL + off);
}

static inline void appl_write(uint32_t off, uint32_t val)
{
    mmio_write32(TEGRA_PCIE_C8_APPL + off, val);
}

static inline void atu_ob_write(uint32_t region, uint32_t reg, uint32_t val)
{
    uintptr_t addr = TEGRA_PCIE_C8_ATU + ATU_REGION_DIR_OB_OFFSET
                     + region * ATU_REGION_STRIDE + reg;
    mmio_write32(addr, val);
}

static inline uint32_t atu_ob_read(uint32_t region, uint32_t reg)
{
    uintptr_t addr = TEGRA_PCIE_C8_ATU + ATU_REGION_DIR_OB_OFFSET
                     + region * ATU_REGION_STRIDE + reg;
    return mmio_read32(addr);
}

/* Busy-wait in microseconds using CNTPCT_EL0 (works at EL2 on all
 * Jetson boards). timer_busy_wait_us is available from timer.h. */
static inline void pcie_udelay(uint32_t us)
{
    timer_busy_wait_us(us);
}

int pcie_tegra_host_init(void)
{
    INFO("pcie-tegra: configuring PCIe C8 root complex");

    /* Step 1: UPHY controller state — BPMP powers up the PHY brick. */
    int rc = bpmp_uphy_pcie_controller_state(TEGRA_PCIE_C8_CID, true);
    if (rc != 0) {
        WARN("pcie-tegra: UPHY_PCIE_CONTROLLER_STATE(8, enable) rc=%d", rc);
        /* Not strictly fatal — on a live system BPMP may have already
         * powered the UPHY up and returns rc=1 (EINVAL) for "already
         * enabled". Proceed and let the link-training step judge. */
    }

    /* Step 2: Core clock + APB reset + core reset. These must succeed
     * or the APPL wrapper itself is dead. */
    rc = bpmp_clk_enable(TEGRA234_CLK_PEX2_C8_CORE);
    if (rc != 0) {
        WARN("pcie-tegra: CLK_ENABLE(PEX2_C8_CORE) rc=%d", rc);
        return -1;
    }
    rc = bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8_APB);
    if (rc != 0) {
        WARN("pcie-tegra: RESET_DEASSERT(PEX2_CORE_8_APB) rc=%d", rc);
        return -1;
    }

    /* Step 3: APPL programming (matches linux-pcie-tegra194.c
     * tegra_pcie_config_controller lines 1425-1465). Ordering here is
     * important — DM_TYPE and SYS_PRE_DET_STATE must be set before
     * LTSSM is enabled in start_link(). */

    /* CFG base address. Must be 4 KB-aligned and placed in the masked
     * bits [31:12]. */
    appl_write(APPL_CFG_BASE_ADDR,
               TEGRA_PCIE_C8_DBI & APPL_CFG_BASE_ADDR_MASK);

    /* RP mode, not EP. */
    appl_write(APPL_DM_TYPE, APPL_DM_TYPE_RP);

    /* Clear SLCG (2nd-level clock gating) override. */
    appl_write(APPL_CFG_SLCG_OVERRIDE, 0);

    /* Set SYS_PRE_DET_STATE — latched internal state the controller
     * checks during training. */
    uint32_t ctrl = appl_read(APPL_CTRL);
    ctrl |= APPL_CTRL_SYS_PRE_DET_STATE;
    appl_write(APPL_CTRL, ctrl);

    /* Program ARCACHE attribute in APPL_CFG_MISC. Linux uses value 3
     * which corresponds to "cacheable, bufferable" for outbound TLPs. */
    uint32_t misc = appl_read(APPL_CFG_MISC);
    misc |= (APPL_CFG_MISC_ARCACHE_VAL << APPL_CFG_MISC_ARCACHE_SHIFT);
    appl_write(APPL_CFG_MISC, misc);

    /* iATU/DMA base address. Linux masks [31:18] (256 KB granularity). */
    appl_write(APPL_CFG_IATU_DMA_BASE_ADDR,
               TEGRA_PCIE_C8_ATU & APPL_CFG_IATU_DMA_BASE_ADDR_MASK);

    /* Step 4: deassert the main core reset. */
    rc = bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8);
    if (rc != 0) {
        WARN("pcie-tegra: RESET_DEASSERT(PEX2_CORE_8) rc=%d", rc);
        return -1;
    }

    g_host_inited = true;
    INFO("pcie-tegra: host init OK (APPL_CTRL=0x%08lx CFG_MISC=0x%08lx)",
         (unsigned long)appl_read(APPL_CTRL),
         (unsigned long)appl_read(APPL_CFG_MISC));
    return 0;
}

int pcie_tegra_start_link(uint32_t timeout_ms, uint32_t *ltssm_out)
{
    if (!g_host_inited) {
        return -2;
    }

    /* Step 5: PEX_RST assert (logical low — the bit CLEARED means reset
     * asserted, per the "PEX_RST" signal's active-low semantics). */
    uint32_t pinmux = appl_read(APPL_PINMUX);
    pinmux &= ~APPL_PINMUX_PEX_RST;
    appl_write(APPL_PINMUX, pinmux);

    pcie_udelay(200);

    /* Step 6: enable LTSSM. */
    uint32_t ctrl = appl_read(APPL_CTRL);
    ctrl |= APPL_CTRL_LTSSM_EN;
    appl_write(APPL_CTRL, ctrl);

    /* Step 7: PEX_RST de-assert (release endpoint from reset). */
    pinmux = appl_read(APPL_PINMUX);
    pinmux |= APPL_PINMUX_PEX_RST;
    appl_write(APPL_PINMUX, pinmux);

    /* Step 8: poll APPL_DEBUG for LTSSM state = L0. */
    uint32_t waited_ms = 0;
    uint32_t ltssm = 0;
    const uint32_t poll_step_ms = 5;

    while (waited_ms < timeout_ms) {
        uint32_t dbg = appl_read(APPL_DEBUG);
        ltssm = (dbg & APPL_DEBUG_LTSSM_STATE_MASK)
                 >> APPL_DEBUG_LTSSM_STATE_SHIFT;

        if (ltssm == APPL_DEBUG_LTSSM_L0) {
            if (ltssm_out) *ltssm_out = ltssm;
            INFO("pcie-tegra: link UP (LTSSM=L0) after %u ms", waited_ms);
            return 0;
        }

        pcie_udelay(poll_step_ms * 1000u);
        waited_ms += poll_step_ms;
    }

    if (ltssm_out) *ltssm_out = ltssm;
    WARN("pcie-tegra: link NOT up after %u ms (last LTSSM=0x%02x)",
         timeout_ms, (unsigned)ltssm);
    return -1;
}

int pcie_tegra_probe_endpoint(uint32_t *vid_did_out)
{
    if (!g_host_inited) {
        return -2;
    }

    /*
     * iATU region 0 outbound:
     *   LOWER_BASE   = CPU CFG window base
     *   LIMIT        = base + size - 1  (use a 4 KB window — one BDF worth)
     *   TARGET       = BDF encoded (bus 1 dev 0 func 0 → 0x01000000)
     *   CTRL1        = type CFG1
     *   CTRL2        = ENABLE
     */
    const uint64_t cfg_base  = TEGRA_PCIE_C8_CFG;
    const uint64_t cfg_limit = cfg_base + 0x1000 - 1;
    const uint32_t target_bdf = (1u << 24);     /* bus 1, dev 0, func 0 */

    atu_ob_write(0, ATU_LOWER_BASE,   (uint32_t)(cfg_base  & 0xFFFFFFFFu));
    atu_ob_write(0, ATU_UPPER_BASE,   (uint32_t)(cfg_base  >> 32));
    atu_ob_write(0, ATU_LIMIT,        (uint32_t)(cfg_limit & 0xFFFFFFFFu));
    atu_ob_write(0, ATU_LOWER_TARGET, target_bdf);
    atu_ob_write(0, ATU_UPPER_TARGET, 0);
    atu_ob_write(0, ATU_CTRL1,        ATU_CTRL1_TYPE_CFG1);
    atu_ob_write(0, ATU_CTRL2,        ATU_CTRL2_ENABLE);

    /* Poll for ENABLE to take effect (dw_pcie_prog_outbound_atu does
     * this; max 9 retries in Linux with ~1 ms step). */
    for (int retries = 0; retries < 10; retries++) {
        uint32_t c2 = atu_ob_read(0, ATU_CTRL2);
        if (c2 & ATU_CTRL2_ENABLE) {
            break;
        }
        pcie_udelay(1000);
    }

    /* Read VID/DID at offset 0 of the CFG window. */
    uint32_t vid_did = mmio_read32(TEGRA_PCIE_C8_CFG + 0);
    if (vid_did_out) *vid_did_out = vid_did;

    INFO("pcie-tegra: bus 1 dev 0 fn 0: VID:DID = 0x%08lx "
         "(expect 0x8168_10EC for RTL8168)",
         (unsigned long)vid_did);

    return 0;
}

void pcie_tegra_read_snapshot(struct pcie_tegra_snapshot *out)
{
    if (!out) return;

    out->appl_ctrl        = appl_read(APPL_CTRL);
    out->appl_debug       = appl_read(APPL_DEBUG);
    out->appl_pinmux      = appl_read(APPL_PINMUX);
    out->appl_link_status = appl_read(APPL_LINK_STATUS);
    out->ltssm_state      = (out->appl_debug & APPL_DEBUG_LTSSM_STATE_MASK)
                             >> APPL_DEBUG_LTSSM_STATE_SHIFT;
    out->ltssm_en         = (out->appl_ctrl & APPL_CTRL_LTSSM_EN) != 0;

    /* DBI bus-0 vendor/device ID. Valid only if RC is alive. */
    uint32_t vid_did = mmio_read32(TEGRA_PCIE_C8_DBI + 0);
    out->dbi_bus0_vid_did = vid_did;
    out->rc_alive         = ((vid_did & 0xFFFF) != 0xFFFF) &&
                             ((vid_did & 0xFFFF) != 0x0000);
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
