/*
 * pcie_tegra194.c — Tegra234 PCIe C8 root complex bring-up.
 *
 * Drives the BPMP MRQs + APPL wrapper registers + DesignWare DBI + iATU
 * needed to train the PCIe link and reach the RTL8168 endpoint from bare
 * metal at EL2, post-kexec. Layered on top of the BPMP IPC stack at
 * kernel/drivers/bpmp/.
 *
 * Reference files:
 *   ~/slmos-ref/linux/linux-pcie-tegra194.c
 *   ~/slmos-ref/linux/linux-pcie-designware.c
 *   ~/slmos-ref/linux/linux-pcie-designware.h
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

/* APPL register offsets (~/slmos-ref/linux/linux-pcie-tegra194.c:41..) */
#define APPL_PINMUX                     0x000
#define APPL_PINMUX_PEX_RST             (1u << 0)
#define APPL_PINMUX_CLKREQ_OVERRIDE_EN  (1u << 2)
#define APPL_PINMUX_CLKREQ_OVERRIDE     (1u << 3)
#define APPL_PINMUX_CLKREQ_DEFAULT_VALUE (1u << 13)
#define APPL_CTRL                       0x004
#define APPL_CTRL_SYS_PRE_DET_STATE     (1u << 6)
#define APPL_CTRL_LTSSM_EN              (1u << 7)
#define APPL_CTRL_HW_HOT_RST_EN         (1u << 20)
#define APPL_CTRL_HW_HOT_RST_MODE_MASK  (0x3u << 22)
#define APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST_LTSSM_EN (0x2u << 22)
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

/* Tegra P2U (PIPE-to-UPHY) bases for PCIe C8 (two lanes).
 * From the jetson-nano-1 Linux DT: pcie@140a0000 phys = <p2u_0, p2u_1>
 * where p2u_0 = phy@3f40000 and p2u_1 = phy@3f50000. */
#define TEGRA_P2U_C8_LANE0              0x03F40000UL
#define TEGRA_P2U_C8_LANE1              0x03F50000UL

/* P2U register offsets (~/slmos-ref/linux/linux-phy-tegra194-p2u.c:17-32). */
#define P2U_CONTROL_CMN                             0x74
#define P2U_CONTROL_CMN_ENABLE_L2_EXIT_RATE_CHANGE  (1u << 13)
#define P2U_PERIODIC_EQ_CTRL_GEN3                   0xC0
#define P2U_PERIODIC_EQ_CTRL_GEN3_PERIODIC_EQ_EN          (1u << 0)
#define P2U_PERIODIC_EQ_CTRL_GEN3_INIT_PRESET_EQ_TRAIN_EN (1u << 1)
#define P2U_PERIODIC_EQ_CTRL_GEN4                   0xC4
#define P2U_PERIODIC_EQ_CTRL_GEN4_INIT_PRESET_EQ_TRAIN_EN (1u << 1)
#define P2U_RX_DEBOUNCE_TIME                        0xA4
#define P2U_RX_DEBOUNCE_TIME_MASK                   0x0000FFFFu
#define P2U_RX_DEBOUNCE_TIME_VAL                    160u
#define P2U_DIR_SEARCH_CTRL                         0xD4
#define P2U_DIR_SEARCH_CTRL_GEN4_FINE_GRAIN_SEARCH_TWICE  (1u << 18)

/* DesignWare DBI register offsets (~/slmos-ref/linux/linux-pcie-designware.h). */
#define DBI_PCI_COMMAND                 0x004
#define   DBI_PCI_CMD_IO_EN             (1u << 0)
#define   DBI_PCI_CMD_MEM_EN            (1u << 1)
#define   DBI_PCI_CMD_MASTER_EN         (1u << 2)
#define   DBI_PCI_CMD_SERR_EN           (1u << 8)
#define DBI_PCI_CLASS_DEVICE            0x00A   /* 16-bit */
#define   DBI_CLASS_BRIDGE_PCI          0x0604
#define DBI_PCI_BASE_ADDRESS_0          0x010
#define DBI_PCI_BASE_ADDRESS_1          0x014
#define DBI_PCI_PRIMARY_BUS             0x018   /* 32-bit: [7:0] primary, [15:8] sec, [23:16] sub */
#define DBI_PCI_IO_BASE                 0x01C   /* [7:0] IO base, [15:8] IO limit */
#define   IO_BASE_IO_DECODE             (1u << 0)
#define   IO_BASE_IO_DECODE_BIT8        (1u << 8)
#define DBI_PCI_PREF_MEMORY_BASE        0x024
#define   CFG_PREF_MEM_LIMIT_BASE_MEM_DECODE        (1u << 0)
#define   CFG_PREF_MEM_LIMIT_BASE_MEM_LIMIT_DECODE  (1u << 16)
#define DBI_PCIE_PORT_AFR               0x70C
#define DBI_PCIE_PORT_LINK_CONTROL      0x710
#define   PORT_LINK_DLL_LINK_EN         (1u << 5)
#define   PORT_LINK_FAST_LINK_MODE      (1u << 7)
#define   PORT_LINK_MODE_MASK           (0x3Fu << 16)
#define   PORT_LINK_MODE_1_LANES        (0x01u << 16)
#define   PORT_LINK_MODE_2_LANES        (0x03u << 16)
#define   PORT_LINK_MODE_4_LANES        (0x07u << 16)
#define DBI_PCIE_LINK_WIDTH_SPEED_CONTROL 0x80C   /* AKA PORT_LOGIC_GEN2_CTRL */
#define   PORT_LOGIC_LINK_WIDTH_MASK    (0x1Fu << 8)
#define   PORT_LOGIC_LINK_WIDTH_1_LANES (0x01u << 8)
#define   PORT_LOGIC_SPEED_CHANGE       (1u << 17)   /* edk2-nvidia DIRECT_SPEED_CHANGE */
#define DBI_PCIE_MISC_CONTROL_1_OFF     0x8BC
#define   PCIE_DBI_RO_WR_EN             (1u << 0)

/*
 * PCIe standard capability base on DW PCIe (Tegra234): 0x70.
 * LNKCTL2 at cap_base + 0x30 holds the target link speed in bits [3:0].
 *   1 = 2.5 GT/s (Gen1)
 *   2 = 5   GT/s (Gen2)
 *   3 = 8   GT/s (Gen3)
 * Forcing Gen1 here short-circuits the Gen3-attempt-then-fall-back
 * path — the RTL8168 is Gen1 only, and our RC otherwise polls at Gen3
 * then hangs in POLLING.COMPLIANCE instead of training to Gen1.
 *
 * Verified on jetson-nano-1: std cap walk 0x34→0x40(id 01)→0x50(05)→
 * 0x70(10 = PCIe)→0xB0(11), so LNKCTL2 is at 0xA0.
 */
#define DBI_PCIE_CAP_BASE               0x70
#define DBI_PCIE_LNKCTL2                (DBI_PCIE_CAP_BASE + 0x30)
#define   PCI_EXP_LNKCTL2_TLS_MASK      0xFu
#define   PCI_EXP_LNKCTL2_TLS_2_5GT     0x1u

/*
 * Data Link Feature (DLF) extended capability at DBI offset 0x2F4 on
 * Tegra234 (from ext-cap walk: 0x100→0x148→0x168→0x18C→0x1AC→0x1BC→
 * 0x2BC→0x2F4 = DLF cap). PCI_DLF_CAP is at DLF_offset + 0x04, so
 * 0x2F8. Bit 31 = Local DLF Supported / Exchange Enable. Clearing it
 * tells the RC not to attempt DLF negotiation — the RTL8168 is a
 * PCIe 2.1-era endpoint that doesn't implement DLF.
 */
#define DBI_DLF_CAP                     0x2F8
#define   DLF_EXCHANGE_ENABLE           (1u << 31)

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

/* iATU target-address BDF encoding for CFG transactions (mirrors
 * Linux's PCIE_ATU_{BUS,DEV,FUNC} in linux-pcie-designware.h:190-192). */
#define ATU_TARGET_BUS(b)   (((uint32_t)(b) & 0xFFu) << 24)
#define ATU_TARGET_DEV(d)   (((uint32_t)(d) & 0x1Fu) << 19)
#define ATU_TARGET_FUNC(f)  (((uint32_t)(f) & 0x07u) << 16)

static bool g_host_inited;

/*
 * Tegra MMIO accessors. Pre-read DSB matches the convention established
 * by uart_tegra.c (see "UART LSR Read After Kexec" in the root
 * CLAUDE.md): without it, a speculatively-issued earlier load can be
 * satisfied from a stale buffer, returning the wrong value. Critical
 * here for APPL_DEBUG / APPL_LINK_STATUS / ATU_CTRL2 polling.
 */
static inline uint32_t mmio_read32(uintptr_t addr)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return *(volatile uint32_t *)addr;
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

static inline uint32_t dbi_read32(uint32_t off)
{
    return mmio_read32(TEGRA_PCIE_C8_DBI + off);
}

static inline void dbi_write32(uint32_t off, uint32_t val)
{
    mmio_write32(TEGRA_PCIE_C8_DBI + off, val);
}

static inline uint16_t dbi_read16(uint32_t off)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return *(volatile uint16_t *)(TEGRA_PCIE_C8_DBI + off);
}

static inline void dbi_write16(uint32_t off, uint16_t val)
{
    *(volatile uint16_t *)(TEGRA_PCIE_C8_DBI + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Enable writes to DBI registers that are hardware-readonly in config
 * space but RW from the DBI port. Needed for class-code, LNKCAP, etc.
 * Mirrors dw_pcie_dbi_ro_wr_en.
 */
static void dbi_ro_wr_enable(bool enable)
{
    uint32_t v = dbi_read32(DBI_PCIE_MISC_CONTROL_1_OFF);
    if (enable) {
        v |= PCIE_DBI_RO_WR_EN;
    } else {
        v &= ~PCIE_DBI_RO_WR_EN;
    }
    dbi_write32(DBI_PCIE_MISC_CONTROL_1_OFF, v);
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
    /* Single-init only. The APPL/DBI/iATU MMIO programming below is
     * not idempotent (some registers latch on first write, others
     * accumulate state) — a second concurrent or sequential call
     * would corrupt the controller state. There's no per-controller
     * spinlock here because the contract is one-time bring-up from
     * boot init on CPU 0; if a future hot-replug path needs this,
     * it must add proper teardown + re-init sequencing first. */
    if (g_host_inited) {
        WARN("pcie-tegra: pcie_tegra_host_init called twice — ignoring");
        return 0;
    }

    INFO("pcie-tegra: configuring PCIe C8 root complex");

    /* Step 0: Ensure the BPMP power-domain PCIEX4CA is ON. Linux
     * handles this implicitly via the power-domains DT property and
     * runtime_pm framework; edk2-nvidia's AssertPgNodes(false) does
     * it explicitly. Without it, the APPL register block might be
     * readable (other masters keep the partition alive) but the
     * controller's own power islands may not be fully up. */
    int rc = bpmp_pg_set_state(TEGRA234_POWER_DOMAIN_PCIEX4CA, true);
    INFO("pcie-tegra: PG_SET_STATE(PCIEX4CA, on) rc=%d", rc);
    /* Not fatal if rc != 0 — the domain is often already on. */

    /* Step 1: UPHY controller state — BPMP powers up the PHY brick. */
    rc = bpmp_uphy_pcie_controller_state(TEGRA_PCIE_C8_CID, true);
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

    /*
     * Fully reset the core so its internal LTSSM state machine starts
     * from DETECT.QUIET — matches Linux's retry path in
     * tegra_pcie_dw_start_link:1020-1021 which assert+deassert
     * pcie->core_rst when the first training attempt fails. Without
     * this, the core inherits whatever LTSSM state Linux left behind
     * pre-kexec, and our PEX_RST pulse + LTSSM_EN set isn't enough
     * to unstick it from POLLING.COMPLIANCE.
     */
    (void)bpmp_reset_assert(TEGRA234_RESET_PEX2_CORE_8);
    pcie_udelay(1000);
    rc = bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8);
    if (rc != 0) {
        WARN("pcie-tegra: (re-)RESET_DEASSERT(PEX2_CORE_8) rc=%d", rc);
        return -1;
    }
    /* Second APB reset to sync — idempotent, keeps APB alive. */
    (void)bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8_APB);

    /* Step 3: APPL programming (matches linux-pcie-tegra194.c
     * tegra_pcie_config_controller lines 1425-1465). Ordering here is
     * important — DM_TYPE and SYS_PRE_DET_STATE must be set before
     * LTSSM is enabled in start_link(). */

    /* Enable HW_HOT_RST in IMDT_RST_LTSSM_EN mode. edk2-nvidia's
     * InitializeController does this unconditionally on T234 (its
     * "IsT234" path). Linux does it conditionally on
     * `has_sbr_reset_fix`, which is true for T234. The IMDT_RST_LTSSM_EN
     * mode means "on hot reset, LTSSM is re-enabled automatically" —
     * without this, some RC configurations refuse to complete POLLING
     * after the endpoint exits its own reset. */
    {
        uint32_t c = appl_read(APPL_CTRL);
        c &= ~APPL_CTRL_HW_HOT_RST_MODE_MASK;
        c |= APPL_CTRL_HW_HOT_RST_MODE_IMDT_RST_LTSSM_EN;
        c |= APPL_CTRL_HW_HOT_RST_EN;
        appl_write(APPL_CTRL, c);
    }

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

    /* Step 3.5: P2U PHY init (matches linux-phy-tegra194-p2u.c
     * tegra_p2u_power_on). Tegra234's "one_dir_search" variant clears
     * the GEN4 fine-grain search-twice bit; we apply that to both
     * P2U lanes used by PCIe C8 (DT confirmed: p2u-0 @ 0x3F40000 and
     * p2u-1 @ 0x3F50000). Without this the UPHY never exits
     * POLLING.COMPLIANCE and the link stays at LTSSM=0x03. */
    {
        const uintptr_t p2u_bases[] = { TEGRA_P2U_C8_LANE0, TEGRA_P2U_C8_LANE1 };
        for (unsigned i = 0; i < 2; i++) {
            uintptr_t b = p2u_bases[i];
            uint32_t val;

            val = mmio_read32(b + P2U_PERIODIC_EQ_CTRL_GEN3);
            val &= ~P2U_PERIODIC_EQ_CTRL_GEN3_PERIODIC_EQ_EN;
            val |=  P2U_PERIODIC_EQ_CTRL_GEN3_INIT_PRESET_EQ_TRAIN_EN;
            mmio_write32(b + P2U_PERIODIC_EQ_CTRL_GEN3, val);

            val = mmio_read32(b + P2U_PERIODIC_EQ_CTRL_GEN4);
            val |= P2U_PERIODIC_EQ_CTRL_GEN4_INIT_PRESET_EQ_TRAIN_EN;
            mmio_write32(b + P2U_PERIODIC_EQ_CTRL_GEN4, val);

            val = mmio_read32(b + P2U_RX_DEBOUNCE_TIME);
            val &= ~P2U_RX_DEBOUNCE_TIME_MASK;
            val |= P2U_RX_DEBOUNCE_TIME_VAL;
            mmio_write32(b + P2U_RX_DEBOUNCE_TIME, val);

            /* Tegra234 one_dir_search = true — clear the GEN4 fine-grain
             * search-twice bit. See linux-phy-tegra194-p2u.c:tegra234_p2u_of_data. */
            val = mmio_read32(b + P2U_DIR_SEARCH_CTRL);
            val &= ~P2U_DIR_SEARCH_CTRL_GEN4_FINE_GRAIN_SEARCH_TWICE;
            mmio_write32(b + P2U_DIR_SEARCH_CTRL, val);
        }
        INFO("pcie-tegra: P2U lane 0 + lane 1 init OK");
    }

    /* Step 4: (the main core reset was already cycled earlier so the
     * LTSSM state machine starts clean — redundant deassert here is
     * idempotent and acts as a barrier.) */
    (void)bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8);

    /* Step 5: APPL_PINMUX setup.
     *
     *   - PEX_RST bit 0 → drive HIGH (de-asserted = endpoint running).
     *     After core-reset deassertion APPL_PINMUX bit 0 defaults to 0
     *     (PERST# asserted = endpoint held in reset); if we leave it
     *     there, pcie_tegra_start_link's "assert" step is a no-op
     *     (bit already 0) and no PEX_RST edge is generated.
     *
     *   - CLKREQ override → force CCPLEX to supply RefClk unconditionally
     *     (CLKREQ_OVERRIDE_EN=1, CLKREQ_OVERRIDE=0, CLKREQ_DEFAULT_VALUE=0).
     *     Without this, the CCPLEX gates the endpoint's reference clock
     *     whenever the endpoint de-asserts CLKREQ#, and the endpoint
     *     can't drive CLKREQ# until it has a clock — chicken/egg.
     *     Linux's tegra_pcie_config_controller applies the same override
     *     when !supports_clkreq.
     */
    {
        uint32_t pinmux = appl_read(APPL_PINMUX);
        pinmux |= APPL_PINMUX_PEX_RST;
        pinmux |= APPL_PINMUX_CLKREQ_OVERRIDE_EN;
        pinmux &= ~APPL_PINMUX_CLKREQ_OVERRIDE;
        pinmux &= ~APPL_PINMUX_CLKREQ_DEFAULT_VALUE;
        appl_write(APPL_PINMUX, pinmux);
    }

    /* Step 6: DBI RC setup (mirrors linux-pcie-designware-host.c
     * dw_pcie_setup_rc + dw_pcie_setup). Programs the DW PCIe core's
     * DBI Type-1 header and link-capable configuration so LTSSM can
     * actually complete training. Without this the RC stalls in
     * POLLING.COMPLIANCE (LTSSM=0x03) because its internal link
     * parameters are reset defaults. */
    {
        /* Enable writes to RO DBI registers for the class code + LNKCAP
         * tweaks further down. */
        dbi_ro_wr_enable(true);

        /* Disable bridge I/O decode — edk2-nvidia PrepareHost, first
         * step. Without this the bridge may try to forward I/O
         * transactions through a window that hasn't been programmed,
         * causing CRS/abort responses that parent timing out. */
        {
            uint32_t iob = dbi_read32(DBI_PCI_IO_BASE);
            iob &= ~(IO_BASE_IO_DECODE | IO_BASE_IO_DECODE_BIT8);
            dbi_write32(DBI_PCI_IO_BASE, iob);
        }

        /* Enable prefetchable memory decode — edk2-nvidia PrepareHost
         * step 2. Sets the decode-enable bits so the bridge's memory
         * window is consulted during config cycles. */
        {
            uint32_t pmb = dbi_read32(DBI_PCI_PREF_MEMORY_BASE);
            pmb |= CFG_PREF_MEM_LIMIT_BASE_MEM_DECODE;
            pmb |= CFG_PREF_MEM_LIMIT_BASE_MEM_LIMIT_DECODE;
            dbi_write32(DBI_PCI_PREF_MEMORY_BASE, pmb);
        }

        /* PORT_LINK_CONTROL: clear FAST_LINK_MODE, set DLL_LINK_EN,
         * advertise max lane capability. num-lanes=4 on Tegra234 C8
         * (per DT), but the endpoint (RTL8168) is x1 — training auto-
         * negotiates to x1. */
        uint32_t plc = dbi_read32(DBI_PCIE_PORT_LINK_CONTROL);
        plc &= ~PORT_LINK_FAST_LINK_MODE;
        plc |= PORT_LINK_DLL_LINK_EN;
        plc &= ~PORT_LINK_MODE_MASK;
        plc |= PORT_LINK_MODE_4_LANES;
        dbi_write32(DBI_PCIE_PORT_LINK_CONTROL, plc);

        /* LINK_WIDTH_SPEED_CONTROL: initial training width = 1 lane
         * (mandatory per DesignWare reference — x1 is the starting
         * width; auto-negotiation widens after link-up). */
        uint32_t lwsc = dbi_read32(DBI_PCIE_LINK_WIDTH_SPEED_CONTROL);
        lwsc &= ~PORT_LOGIC_LINK_WIDTH_MASK;
        lwsc |= PORT_LOGIC_LINK_WIDTH_1_LANES;
        dbi_write32(DBI_PCIE_LINK_WIDTH_SPEED_CONTROL, lwsc);

        /* Type-1 header: BAR0=0 (initial — rewritten to 0 again later
         * per dw_pcie_setup_rc), BAR1=0. */
        dbi_write32(DBI_PCI_BASE_ADDRESS_0, 0);
        dbi_write32(DBI_PCI_BASE_ADDRESS_1, 0);

        /* Bus numbers: primary=0, secondary=1, subordinate=0xff.
         * Keep upper 8 bits (type 1 latency timer etc.) intact. */
        uint32_t pb = dbi_read32(DBI_PCI_PRIMARY_BUS);
        pb &= 0xFF000000u;
        pb |= 0x00FF0100u;
        dbi_write32(DBI_PCI_PRIMARY_BUS, pb);

        /* Command register: enable IO, Memory, Bus Master, SERR.
         * Keep upper 16 bits (Status register) intact. */
        uint32_t cmd = dbi_read32(DBI_PCI_COMMAND);
        cmd &= 0xFFFF0000u;
        cmd |= DBI_PCI_CMD_IO_EN | DBI_PCI_CMD_MEM_EN |
               DBI_PCI_CMD_MASTER_EN | DBI_PCI_CMD_SERR_EN;
        dbi_write32(DBI_PCI_COMMAND, cmd);

        /* Class code: PCI-to-PCI bridge (0x0604). Required so Linux
         * and any enumerator recognise this as a Root Port. */
        dbi_write16(DBI_PCI_CLASS_DEVICE, DBI_CLASS_BRIDGE_PCI);

        /* Initiate speed change after link comes up (harmless if left
         * set; dw_pcie_setup_rc does this unconditionally). */
        lwsc = dbi_read32(DBI_PCIE_LINK_WIDTH_SPEED_CONTROL);
        lwsc |= PORT_LOGIC_SPEED_CHANGE;
        dbi_write32(DBI_PCIE_LINK_WIDTH_SPEED_CONTROL, lwsc);

        /* Force LNKCTL2 target speed = Gen1 (2.5 GT/s). RTL8168 is a
         * Gen1-only endpoint; the RC trying Gen3 first then failing
         * over to Gen1 is what leaves us stuck in POLLING.COMPLIANCE.
         * LNKCTL2 is 16-bit; TLS bits [3:0]. */
        uint16_t lnkctl2 = dbi_read16(DBI_PCIE_LNKCTL2);
        lnkctl2 = (lnkctl2 & ~(uint16_t)PCI_EXP_LNKCTL2_TLS_MASK) |
                  (uint16_t)PCI_EXP_LNKCTL2_TLS_2_5GT;
        dbi_write16(DBI_PCIE_LNKCTL2, lnkctl2);

        /* Disable DLF exchange. Older endpoints (RTL8168 era) don't
         * implement Data Link Feature; the RC attempts DLF negotiation
         * as part of training, the endpoint doesn't respond, and
         * POLLING.COMPLIANCE is where things park. Clear bit 31 of
         * DLF_CAP (DBI+0x2F8). Matches Linux's retry path in
         * tegra_pcie_dw_start_link:1024-1026. */
        {
            uint32_t dlf = dbi_read32(DBI_DLF_CAP);
            dlf &= ~DLF_EXCHANGE_ENABLE;
            dbi_write32(DBI_DLF_CAP, dlf);
            INFO("pcie-tegra: DLF exchange disabled (DLF_CAP=0x%08lx)",
                 (unsigned long)dbi_read32(DBI_DLF_CAP));
        }

        dbi_ro_wr_enable(false);

        INFO("pcie-tegra: DBI RC setup done (PORT_LINK_CTRL=0x%08lx "
             "LWSC=0x%08lx PCI_CMD=0x%08lx)",
             (unsigned long)dbi_read32(DBI_PCIE_PORT_LINK_CONTROL),
             (unsigned long)dbi_read32(DBI_PCIE_LINK_WIDTH_SPEED_CONTROL),
             (unsigned long)dbi_read32(DBI_PCI_COMMAND));
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
     * asserted, per the "PEX_RST" signal's active-low semantics).
     * Linux holds this for 100-200 us but that assumes the endpoint was
     * already observing PERST# from a prior boot. Coming out of a kexec
     * the endpoint has been running at Linux-era LTSSM state; a 100 ms
     * hold matches the PCIe CEM spec minimum and reliably forces the
     * endpoint back to its reset sequence. */
    uint32_t pinmux = appl_read(APPL_PINMUX);
    pinmux &= ~APPL_PINMUX_PEX_RST;
    appl_write(APPL_PINMUX, pinmux);

    pcie_udelay(100 * 1000);      /* 100 ms */

    /* Step 6: enable LTSSM. */
    uint32_t ctrl = appl_read(APPL_CTRL);
    ctrl |= APPL_CTRL_LTSSM_EN;
    appl_write(APPL_CTRL, ctrl);

    /* Step 7: PEX_RST de-assert (release endpoint from reset). */
    pinmux = appl_read(APPL_PINMUX);
    pinmux |= APPL_PINMUX_PEX_RST;
    appl_write(APPL_PINMUX, pinmux);

    /* PCIe spec: T_PVPERL = 100 ms power-stable to PERST# de-assertion.
     * The endpoint then needs additional time to enter DETECT.QUIET
     * before LTSSM training can begin. edk2-nvidia's PrepareHost waits
     * 200 ms after de-assert; Linux waits 100 ms msleep. We err on the
     * longer side. */
    pcie_udelay(200 * 1000);

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
    const uint32_t target_bdf = ATU_TARGET_BUS(1) | ATU_TARGET_DEV(0) |
                                ATU_TARGET_FUNC(0);

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
