/*
 * ga10b_cold.c - GA10B bring-up from cold, without Linux (#1088).
 *
 * See ga10b_cold.h for the overview. Each step mirrors one or more
 * entries of nvgpu's init tables (L4T R36.4.x,
 * drivers/gpu/nvgpu/common/init/nvgpu_init.c) and is annotated with
 * the nvgpu HAL function it reproduces. Register values were checked
 * against an MMIO trace of nvgpu's first power-on on jetson-nano-2
 * (UEFI 36.4.7 / L4T R36.4.7), captured with an instrumented
 * nvgpu_writel/nvgpu_readl:
 *   ~/slmos-ref/derivatives/nvgpu-traces/ga10b-r36.4.7-cold-poweron-2026-11-03.trace
 *
 * Clock-gating "prod" writes (SLCG/BLCG/ELCG) are skipped: they only
 * save power, and the reset values leave every clock running.
 */

#include "platform.h"

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "ga10b_cold.h"
#include "bpmp.h"
#include "tegra234_clocks.h"
#include "debug.h"
#include "timer.h"
#include "pmm.h"
#include "ga10b_bringup.h"
#include "ga10b_netlist.h"
#include "ga10b_ctxsw_ucode.h"
#include "ga10b_acr_blob.h"
#include "ga10b_pmu.h"
#include "ga10b_chan.h"
#include "ga10b_grctx.h"
#include "ga10b_channel_handoff.h"
#include "ga10b_mnist_pipeline.h"
#include "cache.h"
#include "string.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ---- BAR0 register offsets (GA10B, from nvgpu hw_*_ga10b.h) ---- */
#define NV_PMC_BOOT_0                    0x000000u
#define NV_PMC_ENABLE                    0x000200u
#define NV_PBUS_INTR_EN_0                0x001140u
#define NV_PPRIV_MASTER_RING_COMMAND     0x12004cu  /* pri_ringmaster_command_r */
#define NV_PPRIV_MASTER_RING_START_RESULTS 0x120050u
#define NV_PPRIV_MASTER_RING_INTR_STATUS0 0x120058u
#define NV_PPRIV_MASTER_RING_ENUMERATE_RESULTS_FBP 0x120074u
#define NV_PPRIV_MASTER_RING_ENUMERATE_RESULTS_GPC 0x120078u
#define NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX0 0x409800u
#define NV_FUSE_STATUS_OPT_GPC           0x820c1cu
#define NV_PMC_DEVICE_ENABLE0            0x000600u  /* mc_device_enable_r(0) */
#define NV_PTOP_DEVICE_INFO2(i)          (0x022800u + (i) * 4u)
#define NV_PGRAPH_PRI_GPCS_GPCCS_ENGINE_RESET_CTL 0x41a610u
#define NV_PGRAPH_PRI_GPC0_GPCCS_ENGINE_RESET_CTL 0x502610u

/* PMC_ENABLE unit bits reset by gr_reset_engine (NVGPU_UNIT_PERFMON |
 * NVGPU_UNIT_BLG): mc_enable_perfmon_m() = bit 28; BLG has no PMC_ENABLE
 * bit on GA10B (ga10b_mc_get_unit_reset_mask returns 0 for it). */
#define NV_PFB_NISO_FLUSH_SYSMEM_ADDR    0x100c10u
#define NV_PFB_PRI_MMU_DEBUG_WR          0x100cc8u
#define NV_PFB_PRI_MMU_DEBUG_RD          0x100cccu

/* GSP falcon (NV_PGSP base 0x110000) and its RISC-V core. */
#define NV_PGSP_FALCON_MAILBOX0          0x110040u
#define NV_PGSP_FALCON_MAILBOX1          0x110044u
#define NV_PGSP_FALCON_HWCFG2            0x1100f4u
#define NV_PGSP_FALCON_IMEMC0            0x110180u
#define NV_PGSP_FALCON_IMEMD0            0x110184u
#define NV_PGSP_FALCON_DMEMC0            0x1101c0u
#define NV_PGSP_FALCON_DMEMD0            0x1101c4u
#define NV_PGSP_FALCON_ENGINE            0x1103c0u
#define NV_PGSP_FALCON_HWCFG             0x110108u
#define NV_PGSP_RISCV_CPUCTL             0x111388u
#define NV_PGSP_RISCV_BR_RETCODE         0x11165cu
#define NV_PGSP_RISCV_BCR_CTRL           0x111668u

#define FALCON_HWCFG2_MEM_SCRUBBING      (1u << 12)
#define FALCON_MEMC_AINCW                (1u << 24)
#define FALCON_IMEMC_SECURE              (1u << 28)
#define RISCV_CPUCTL_STARTCPU            (1u << 0)
#define RISCV_CPUCTL_HALTED              (1u << 4)
/* BCR_CTRL: CORE_SELECT=RISCV (bit 4) | VALID (bit 0): boot from the
 * preloaded IMEM/DMEM (riscv_bcr_ctrl in ga10b_falcon_set_bcr). */
#define RISCV_BCR_CTRL_PRELOADED         0x11u
#define RISCV_BR_RETCODE_RESULT(v)       ((v) & 0x3u)
#define RISCV_BR_RETCODE_PASS            3u
#define RISCV_BR_RETCODE_FAIL            2u

/* GR (hw_gr_ga10b.h). */
#define NV_PGRAPH_STATUS                 0x400700u
#define NV_PGRAPH_PRI_FE_FIFO_ACCESS     0x400500u  /* gr_gpfifo_ctl */
#define GR_GPFIFO_CTL_ACCESS_SEMAPHORE   0x00010001u /* access + semaphore_access enabled */
#define NV_PGRAPH_PRI_FECS_DMACTL        0x40910cu
#define NV_PGRAPH_PRI_GPCS_GPCCS_DMACTL  0x41a10cu
#define GR_DMACTL_SCRUBBING              0x6u       /* imem (bit 2) | dmem (bit 1) */
#define NV_PGRAPH_PRI_FECS_CPUCTL        0x409100u
#define NV_PGRAPH_PRI_FECS_CPUCTL_ALIAS  0x409130u
#define NV_PGRAPH_PRI_GPCS_GPCCS_CPUCTL_ALIAS 0x41a130u
#define GR_CPUCTL_STARTCPU               0x2u
#define NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(i) (0x409800u + (i) * 4u)
#define NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX_CLEAR0 0x409840u
#define NV_PGRAPH_PRI_FECS_CURRENT_CTX   0x409b00u
#define NV_PGRAPH_PRI_FECS_METHOD_DATA   0x409500u
#define NV_PGRAPH_PRI_FECS_METHOD_PUSH   0x409504u
#define FECS_MAILBOX0_PASS               1u
#define NV_PGRAPH_PRI_GPC0_GPM_PD_LIVE_PHYSICAL_PES 0x500c50u
#define NV_PGRAPH_PRI_GPC0_SWDX_PES_MASK 0x5001d0u
#define NV_PGRAPH_PRI_GPC0_GPM_PD_SM_ID(tpc) (0x500c10u + (tpc) * 4u)
#define NV_PGRAPH_PRI_GPC0_TPC_SM_CFG(tpc)   (0x504608u + (tpc) * 0x800u)

/* FECS methods (gr_fecs_method_push_adr_*). */
#define FECS_METHOD_SET_WATCHDOG_TIMEOUT 0x21u
#define FECS_METHOD_DISCOVER_IMAGE_SIZE  0x10u

/* FB / LTC / GPC MMU (hw_fb_ga10b.h, hw_ltc_ga10b.h, hw_gr_ga10b.h). */
#define NV_PFB_PRI_MMU_CTRL              0x100c80u
#define NV_PFB_PRI_MMU_DEBUG_CTRL        0x100cc4u
#define NV_PFB_FBHUB_NUM_ACTIVE_LTCS     0x100800u
#define NV_PFB_HSHUB0_NUM_ACTIVE_LTCS    0x004c20u
#define NV_PFB_FBHUB_NUM_ACTIVE_LTCS_MMU 0x100ec0u   /* fb_mmu_num_active_ltcs */
#define FB_MMU_CTRL_ATOMIC_CAP_MODE_M    (0x3u << 24)
#define FB_MMU_CTRL_ATOMIC_CAP_MODE_RMW  (0x2u << 24)
#define FB_MMU_CTRL_ATOMIC_SYS_NCOH_M    (0x1u << 27)  /* 0 = L2 */
#define FB_HUB_SYS_ATOMIC_MODE_M         (0x1u << 25)
#define FB_HUB_SYS_ATOMIC_MODE_USE_RMW   (0x1u << 25)
#define NV_PLTCG_LTCS_LTSS_TSTG_SET_MGMT_3 0x17e2b4u
#define LTC_SET_MGMT_3_DISALLOW_CLEAN_IMM (0x5u << 21) /* ce_imm | fclr_imm */
#define NV_PGRAPH_PRI_GPCS_MMU_CTRL      0x418880u
#define NV_PGRAPH_PRI_GPCS_MMU_PM_UNIT_MASK 0x418890u
#define NV_PGRAPH_PRI_GPCS_MMU_PM_REQ_MASK  0x418894u
#define NV_PGRAPH_PRI_GPCS_MMU_DEBUG_CTRL 0x4188b0u
#define NV_PGRAPH_PRI_GPCS_MMU_DEBUG_WR  0x4188b4u
#define NV_PGRAPH_PRI_GPCS_MMU_DEBUG_RD  0x4188b8u
#define NV_PGRAPH_PRI_GPCS_MMU_NUM_ACTIVE_LTCS 0x4188acu
/* gr_gpcs_pri_mmu_ctrl fields ga10b_gr_init_gpc_mmu copies from FB:
 * vm_pg_size, use_pdb_big_page_size, comp_fault, miss_gran,
 * cache_mode, atomic_capability_mode, atomic_capability_sys_ncoh. */
#define GPC_MMU_CTRL_COPY_MASK           (0x1u | (0x1u << 11) | (0x1u << 2) | \
                                          (0x3u << 3) | (0x3u << 5) | \
                                          (0x3u << 24) | (0x1u << 27))
#define NV_PGRAPH_PRI_GPC0_GPCCS_FS_GPC  0x502608u   /* tpc count bits 4:0 */
#define GA10B_COLD_TPC_COUNT             4u

/* Copy engines: device enable for the LCE reset ids (2..5 per
 * device info), as nvgpu's ce_init_support does. */
#define GA10B_LCE_DEVICE_ENABLE_MASK     0x3cu

#define PMC_ENABLE_PERFMON               (1u << 28)

/* Device-info layout on GA10B (read back at step 3 and checked):
 * entry 0 (rows 0..2) is GR instance 0, reset_id 12; entry 1 (rows
 * 3..5) carries reset_id 1, which nvgpu enables during early init
 * (trace line 147) before any GR programming. */
#define GA10B_GR_DEVINFO_ROW1            1u
#define GA10B_GR_RESET_ID                12u
#define GA10B_EARLY_RESET_ID             1u

/* pri_ringmaster_command_cmd_enumerate_and_start_ring_f() */
#define PRI_RING_CMD_ENUMERATE_AND_START 0x4u

/* ga10b_bus_init_hw: pri_fecserr | pri_timeout | fb_{req,ack}_timeout |
 * fb_ack_extra | fb_rdata_{timeout,extra} | sw | posted_deadlock_timeout
 * | mpmu | access_timeout. The trace shows the same value. */
#define PBUS_INTR_EN_0_MASK              0x9c0001fcu

/* Poll budgets, microseconds. ACR's BROM check and run time follow
 * nvgpu's silicon timeouts (NVRISCV_BR_COMPLETION / ACR_COMPLETION),
 * rounded up; the GR/FECS ones are generous bounds for waits that
 * finish in microseconds on hardware. */
#define COLD_TIMEOUT_ACR_BROM_US        5000000u
#define COLD_TIMEOUT_ACR_RUN_US        10000000u
#define COLD_TIMEOUT_GR_IDLE_US         2000000u
#define COLD_TIMEOUT_FECS_US            2000000u
#define COLD_TIMEOUT_SCRUB_US           1000000u
#define COLD_TIMEOUT_PRI_RING_US          10000u

/* FECS watchdog timeout nvgpu sends with SET_WATCHDOG_TIMEOUT
 * (nvgpu_gr_falcon_init_ctx_state, trace line 11550). */
#define FECS_WATCHDOG_TIMEOUT           0x3fffffffu

/* Floorsweeping the constant GR init table encodes: GPC1 swept
 * (NV_FUSE_STATUS_OPT_GPC = 0x2), 4 TPCs in GPC0. */
#define GA10B_COLD_OPT_GPC_EXPECTED     0x2u
#define GA10B_COLD_MAX_GPCS             2u

/* BAR0 aperture size; netlist addresses must fall inside it. */
#define GA10B_BAR0_SIZE                 0x01000000u

static enum ga10b_cold_step cold_done = GA10B_COLD_NONE;

static const char *const step_names[GA10B_COLD_STEP_COUNT] = {
    [GA10B_COLD_NONE]  = "none",
    [GA10B_COLD_POWER] = "power",
    [GA10B_COLD_PRI]   = "pri",
    [GA10B_COLD_ENGINES] = "engines",
    [GA10B_COLD_FB]      = "fb",
    [GA10B_COLD_GR_HW]   = "gr_hw",
    [GA10B_COLD_ACR]     = "acr",
    [GA10B_COLD_PMU]     = "pmu",
    [GA10B_COLD_FALCONS] = "falcons",
    [GA10B_COLD_GR_INIT] = "gr_init",
    [GA10B_COLD_GOLDEN]  = "golden",
    [GA10B_COLD_CHANNEL] = "channel",
    [GA10B_COLD_MNIST]   = "mnist",
};

const char *ga10b_cold_step_name(enum ga10b_cold_step step)
{
    if ((unsigned)step >= GA10B_COLD_STEP_COUNT) return "?";
    return step_names[step];
}

enum ga10b_cold_step ga10b_cold_progress(void)
{
    return cold_done;
}

static inline uint32_t bar0_rd(uint32_t off)
{
    uint32_t v = *(volatile uint32_t *)(GPU_BASE + off);
    __asm__ volatile("dsb sy" ::: "memory");
    return v;
}

static inline void bar0_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(GPU_BASE + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void udelay(uint32_t us)
{
    uint64_t freq = timer_get_frequency();
    uint64_t end = timer_get_count() + (freq * us) / 1000000u;
    while (timer_get_count() < end) { }
}

/* Poll until (reg & mask) == 0 or `timeout_us` passes; re-checks once
 * after the deadline so a value that settled during the last delay is
 * not reported as a timeout. */
static int wait_bits_clear(uint32_t reg, uint32_t mask, uint32_t timeout_us)
{
    for (uint32_t t = 0; t < timeout_us; t += 10) {
        if ((bar0_rd(reg) & mask) == 0) {
            return 0;
        }
        udelay(10);
    }
    return (bar0_rd(reg) & mask) == 0 ? 0 : -1;
}

/* ---- Step 1: power ----
 *
 * Linux: genpd powers TEGRA234_POWER_DOMAIN_GPU through BPMP MRQ_PG
 * when runtime PM resumes the device, then ga10b_tegra_unrailgate
 * enables the DT clocks sysclk, gpc0clk, gpc1clk
 * (platform_ga10b_tegra.c tegra_ga10b_clocks[]), skipping the gpcclk
 * of a floorswept GPC (ga10b_tegra_is_clock_available; the Orin Nano
 * ships with GPC1 swept and BPMP rejects gpc1clk with -EINVAL). BPMP
 * sequences the partition reset with the power-up. MRQ_STRAP
 * floorsweeping masks are left at their fused defaults. */
static int clk_on(uint32_t id, const char *name)
{
    int rc = bpmp_clk_enable(id);
    if (rc != 0) {
        WARN("ga10b-cold: clk %s (%u) enable rc=%d", name, (unsigned)id, rc);
    }
    return rc;
}

static int step_power(void)
{
    int rc = bpmp_pg_set_state(TEGRA234_POWER_DOMAIN_GPU, true);
    if (rc != 0) {
        WARN("ga10b-cold: GPU partition power-on rc=%d", rc);
        return -1;
    }
    if (clk_on(TEGRA234_CLK_GPUSYS, "gpusysclk") != 0) return -1;

    uint32_t boot0 = bar0_rd(NV_PMC_BOOT_0);
    if (boot0 == 0xFFFFFFFFu || ga10b_cold_is_pri_poison(boot0)) {
        WARN("ga10b-cold: BOOT_0=0x%08x unreadable after power-on",
             (unsigned)boot0);
        return -1;
    }

    uint32_t gpc_fuse = bar0_rd(NV_FUSE_STATUS_OPT_GPC);
    static const struct { uint32_t id; const char *name; } gpc_clk[] = {
        { TEGRA234_CLK_GPC0CLK, "gpc0clk" },
        { TEGRA234_CLK_GPC1CLK, "gpc1clk" },
    };
    for (unsigned g = 0; g < GA10B_COLD_MAX_GPCS; g++) {
        if (!ga10b_cold_gpc_present(gpc_fuse, g)) continue;
        if (clk_on(gpc_clk[g].id, gpc_clk[g].name) != 0) return -1;
    }
    INFO("ga10b-cold: power on, BOOT_0=0x%08x PMC_ENABLE=0x%08x "
         "opt_gpc=0x%x", (unsigned)boot0,
         (unsigned)bar0_rd(NV_PMC_ENABLE), (unsigned)gpc_fuse);
    return 0;
}

/* ---- Step 2: PRI ring + PBUS ----
 *
 * gm20b_priv_ring_enable: write ENUMERATE_AND_START, wait for the
 * command register to read 0, check START_RESULTS.connectivity.
 * ga10b_bus_init_hw: enable PBUS error interrupts (SLM-OS polls; the
 * enables only latch status). Until the ring is up, every register
 * behind it (FECS, GPCs, FBPs) reads 0xbadfXXXX. */
static int step_pri(void)
{
    bar0_wr(NV_PPRIV_MASTER_RING_COMMAND, PRI_RING_CMD_ENUMERATE_AND_START);

    if (wait_bits_clear(NV_PPRIV_MASTER_RING_COMMAND, 0xFFFFFFFFu, COLD_TIMEOUT_PRI_RING_US) != 0) {
        WARN("ga10b-cold: PRI ring enumeration timed out");
        return -1;
    }
    uint32_t start = bar0_rd(NV_PPRIV_MASTER_RING_START_RESULTS);
    if (!ga10b_cold_pri_start_passed(start)) {
        WARN("ga10b-cold: PRI ring enumeration failed start=0x%08x "
             "intr0=0x%08x", (unsigned)start,
             (unsigned)bar0_rd(NV_PPRIV_MASTER_RING_INTR_STATUS0));
        return -1;
    }

    bar0_wr(NV_PBUS_INTR_EN_0, PBUS_INTR_EN_0_MASK);

    uint32_t fbp = bar0_rd(NV_PPRIV_MASTER_RING_ENUMERATE_RESULTS_FBP);
    uint32_t gpc = bar0_rd(NV_PPRIV_MASTER_RING_ENUMERATE_RESULTS_GPC);
    INFO("ga10b-cold: PRI ring up (fbp=0x%x gpc=0x%x)",
         (unsigned)fbp, (unsigned)gpc);
    return 0;
}

/* ga10b_mc_enable_engine for PMC_DEVICE_ENABLE(0): read-modify-write,
 * then poll until the register reads back the written value. */
static int device_enable(uint32_t mask, bool enable)
{
    uint32_t v = bar0_rd(NV_PMC_DEVICE_ENABLE0);
    v = enable ? (v | mask) : (v & ~mask);
    bar0_wr(NV_PMC_DEVICE_ENABLE0, v);
    for (unsigned i = 0; i < 1000u; i++) {
        if (bar0_rd(NV_PMC_DEVICE_ENABLE0) == v) return 0;
        udelay(10);
    }
    WARN("ga10b-cold: PMC_DEVICE_ENABLE stuck at 0x%08x (want 0x%08x)",
         (unsigned)bar0_rd(NV_PMC_DEVICE_ENABLE0), (unsigned)v);
    return -1;
}

/* ga10b_mc_enable_units: RMW of PMC_ENABLE, read back to confirm. */
static int units_enable(uint32_t mask, bool enable)
{
    if (enable) udelay(20);                 /* MC_RESET_DELAY_US */
    uint32_t v = bar0_rd(NV_PMC_ENABLE);
    v = enable ? (v | mask) : (v & ~mask);
    bar0_wr(NV_PMC_ENABLE, v);
    uint32_t back = bar0_rd(NV_PMC_ENABLE);
    udelay(20);                             /* MC_ENABLE_DELAY_US */
    if (back != v) {
        WARN("ga10b-cold: PMC_ENABLE 0x%08x != 0x%08x", (unsigned)back,
             (unsigned)v);
        return -1;
    }
    return 0;
}

/* ---- Step 3: engines ----
 *
 * Early init enables the device with reset_id 1. nvgpu_gr_enable_hw ->
 * gr_reset_engine then cycles GR: device disable, ga10b_gr_init_reset_gpcs
 * (pulse GPCCS_ENGINE_RESET_CTL on the GPC broadcast, check GPC0 came
 * out of reset), device enable, then a PERFMON unit reset. After this
 * FECS registers answer instead of returning PRI poison. */
/* Defined with the other GPU memory ops further down. */
static const struct ga10b_gpu_ops cold_gpu_ops;

static int step_engines(void)
{
    /* nvgpu programs FIFO/runlist/PBDMA state (doorbell enable,
     * runlist interrupt vectors, ctxsw timeouts, PBDMA interrupt
     * enables, USERD writeback timer) before the GR reset (trace
     * lines 244-368). */
    ga10b_chan_fifo_init(&cold_gpu_ops);
    uint32_t row1 = bar0_rd(NV_PTOP_DEVICE_INFO2(GA10B_GR_DEVINFO_ROW1));
    if ((row1 & 0xFFu) != GA10B_GR_RESET_ID) {
        WARN("ga10b-cold: device_info2[1]=0x%08x, GR reset_id %u expected",
             (unsigned)row1, (unsigned)GA10B_GR_RESET_ID);
        return -1;
    }
    if (device_enable(ga10b_cold_reset_id_mask(GA10B_EARLY_RESET_ID), true) != 0) {
        return -1;
    }

    uint32_t gr_mask = ga10b_cold_reset_id_mask(GA10B_GR_RESET_ID);
    if (device_enable(gr_mask, false) != 0) return -1;
    bar0_wr(NV_PGRAPH_PRI_GPCS_GPCCS_ENGINE_RESET_CTL, 0u);
    udelay(10);                             /* NVGPU_GR_GPCS_RESET_DELAY_US */
    bar0_wr(NV_PGRAPH_PRI_GPCS_GPCCS_ENGINE_RESET_CTL, 1u);
    uint32_t gpc0 = bar0_rd(NV_PGRAPH_PRI_GPC0_GPCCS_ENGINE_RESET_CTL);
    if ((gpc0 & 1u) == 0u) {
        WARN("ga10b-cold: GPC0 not out of reset (0x%08x)", (unsigned)gpc0);
        return -1;
    }
    if (device_enable(gr_mask, true) != 0) return -1;
    if (units_enable(PMC_ENABLE_PERFMON, false) != 0) return -1;
    if (units_enable(PMC_ENABLE_PERFMON, true) != 0) return -1;

    uint32_t fecs = bar0_rd(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX0);
    INFO("ga10b-cold: engines up: DEVICE_ENABLE=0x%08x PMC_ENABLE=0x%08x "
         "FECS_MAILBOX0=0x%08x", (unsigned)bar0_rd(NV_PMC_DEVICE_ENABLE0),
         (unsigned)bar0_rd(NV_PMC_ENABLE), (unsigned)fecs);
    if (ga10b_cold_is_pri_poison(fecs)) {
        WARN("ga10b-cold: FECS unreachable after GR reset");
        return -1;
    }
    return 0;
}

/* One zeroed, cache-clean 4 KB page for the GPU (non-coherent sysmem
 * aperture, so the CPU's view must be in DRAM). Never freed: these
 * pages back hardware state for the life of the GPU. */
static uint64_t gpu_page_alloc(void)
{
    void *p = pmm_alloc_page();
    if (!p) return 0;
    memset(p, 0, 4096);
    cache_clean_range(p, 4096);
    return (uint64_t)(uintptr_t)p;
}

/* ---- Step 4: FB / MMU ----
 *
 * gm20b_fb_init_hw (called by ga10b_fb_init_hw): point NISO flush at a
 * sysmem page (L2 flushes post there) and give the MMU its debug
 * write/read pages. ga10b_fb_init_fs_state writes nothing on a
 * priv-secure part, ga10b_fb_intr_vectorid_init only reads. BAR1/BAR2
 * are not bound and no MMU fault buffers are set up: compute work
 * runs in channel address spaces, and SLM-OS reads fault state from
 * the FAULT_STATUS registers. */
static uint64_t fb_flush_page, fb_dbg_wr_page, fb_dbg_rd_page;

static int step_fb(void)
{
    if (!fb_flush_page) fb_flush_page = gpu_page_alloc();
    if (!fb_dbg_wr_page) fb_dbg_wr_page = gpu_page_alloc();
    if (!fb_dbg_rd_page) fb_dbg_rd_page = gpu_page_alloc();
    if (!fb_flush_page || !fb_dbg_wr_page || !fb_dbg_rd_page) {
        WARN("ga10b-cold: out of memory for FB pages");
        return -1;
    }
    bar0_wr(NV_PFB_NISO_FLUSH_SYSMEM_ADDR, (uint32_t)(fb_flush_page >> 8));
    bar0_wr(NV_PFB_PRI_MMU_DEBUG_WR, ga10b_cold_mmu_debug_val(fb_dbg_wr_page));
    bar0_wr(NV_PFB_PRI_MMU_DEBUG_RD, ga10b_cold_mmu_debug_val(fb_dbg_rd_page));
    INFO("ga10b-cold: FB flush page 0x%lx, MMU debug wr 0x%lx rd 0x%lx",
         (unsigned long)fb_flush_page, (unsigned long)fb_dbg_wr_page,
         (unsigned long)fb_dbg_rd_page);
    return 0;
}

/* ---- GSP RISC-V ACR boot ----
 *
 * nvgpu_riscv_hs_ucode_load_bootstrap + nvgpu_falcon_wait_for_nvriscv_
 * brom_completion + nvgpu_acr_wait_for_completion, register for
 * register as in the trace: engine reset and scrub wait, descriptor
 * address into MAILBOX0/1, BCR_CTRL, code into IMEM at 0 (secure,
 * auto-increment, no per-page tags on the RISC-V path), data into DMEM
 * at 0, manifest at the top of DMEM, STARTCPU, then BR_RETCODE PASS and
 * a halt with MAILBOX0 == 0. */
static void pio_write(uint32_t ctl_reg, uint32_t data_reg, uint32_t ctl,
                      const uint8_t *src, uint32_t len)
{
    bar0_wr(ctl_reg, ctl);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4 && i + b < len; b++) {
            w |= (uint32_t)src[i + b] << (8 * b);
        }
        bar0_wr(data_reg, w);
    }
}


/* Boot the ACR HS ucode (acr-gsp text/data/manifest) on the GSP RISC-V
 * core. `desc_phys` is the physical address of the flcn2_acr_desc the
 * ucode reads through MAILBOX0/1. Returns 0 when the BROM accepted the
 * image and ACR halted with MAILBOX0 == 0. */
static int ga10b_cold_gsp_boot_acr_blobs(const uint8_t *code, uint32_t code_len,
                                  const uint8_t *data, uint32_t data_len,
                                  const uint8_t *manifest, uint32_t manifest_len,
                                  uint64_t desc_phys)
{
    /* ga10b_gsp_engine_reset: assert, deassert, wait for scrubbing. */
    bar0_wr(NV_PGSP_FALCON_ENGINE, 1u);
    udelay(10);
    bar0_wr(NV_PGSP_FALCON_ENGINE, 0u);
    if (wait_bits_clear(NV_PGSP_FALCON_HWCFG2, FALCON_HWCFG2_MEM_SCRUBBING,
                        100000u) != 0) {
        WARN("ga10b-cold: GSP memory scrub did not finish");
        return -1;
    }

    /* IMEM/DMEM sizes: HWCFG bits 8:0 / 17:9 in 256-byte blocks
     * (falcon_hwcfg_imem_size / _dmem_size). */
    uint32_t hwcfg = bar0_rd(NV_PGSP_FALCON_HWCFG);
    uint32_t imem_size = (hwcfg & 0x1FFu) * 256u;
    uint32_t dmem_size = ((hwcfg >> 9) & 0x1FFu) * 256u;
    if (!ga10b_cold_acr_image_fits(code_len, data_len, manifest_len,
                                   imem_size, dmem_size)) {
        WARN("ga10b-cold: ACR image does not fit (code %u / IMEM %u, data %u "
             "+ manifest %u / DMEM %u)", (unsigned)code_len, (unsigned)imem_size,
             (unsigned)data_len, (unsigned)manifest_len, (unsigned)dmem_size);
        return -1;
    }

    bar0_wr(NV_PGSP_FALCON_MAILBOX0, (uint32_t)desc_phys);
    bar0_wr(NV_PGSP_FALCON_MAILBOX1, (uint32_t)(desc_phys >> 32));
    bar0_wr(NV_PGSP_RISCV_BCR_CTRL, RISCV_BCR_CTRL_PRELOADED);

    pio_write(NV_PGSP_FALCON_IMEMC0, NV_PGSP_FALCON_IMEMD0,
              FALCON_IMEMC_SECURE | FALCON_MEMC_AINCW, code, code_len);
    pio_write(NV_PGSP_FALCON_DMEMC0, NV_PGSP_FALCON_DMEMD0,
              FALCON_MEMC_AINCW, data, data_len);
    pio_write(NV_PGSP_FALCON_DMEMC0, NV_PGSP_FALCON_DMEMD0,
              FALCON_MEMC_AINCW | (dmem_size - manifest_len),
              manifest, manifest_len);

    bar0_wr(NV_PGSP_RISCV_CPUCTL, RISCV_CPUCTL_STARTCPU);

    uint32_t ret = 0;
    for (uint32_t t = 0; t < COLD_TIMEOUT_ACR_BROM_US; t += 10) {
        ret = bar0_rd(NV_PGSP_RISCV_BR_RETCODE);
        if (RISCV_BR_RETCODE_RESULT(ret) == RISCV_BR_RETCODE_PASS ||
            RISCV_BR_RETCODE_RESULT(ret) == RISCV_BR_RETCODE_FAIL) {
            break;
        }
        udelay(10);
    }
    if (RISCV_BR_RETCODE_RESULT(ret) != RISCV_BR_RETCODE_PASS) {
        WARN("ga10b-cold: GSP BROM rejected ACR (BR_RETCODE=0x%08x)",
             (unsigned)ret);
        return -1;
    }

    uint32_t ctl = 0;
    for (uint32_t t = 0; t < COLD_TIMEOUT_ACR_RUN_US; t += 10) {
        ctl = bar0_rd(NV_PGSP_RISCV_CPUCTL);
        if (ctl & RISCV_CPUCTL_HALTED) break;
        udelay(10);
    }
    uint32_t mb0 = bar0_rd(NV_PGSP_FALCON_MAILBOX0);
    uint32_t mb1 = bar0_rd(NV_PGSP_FALCON_MAILBOX1);
    INFO("ga10b-cold: ACR done CPUCTL=0x%08x MAILBOX0=0x%08x MAILBOX1=0x%08x",
         (unsigned)ctl, (unsigned)mb0, (unsigned)mb1);
    if (!(ctl & RISCV_CPUCTL_HALTED)) {
        WARN("ga10b-cold: ACR did not halt");
        return -1;
    }
    if (mb0 != 0u) {
        WARN("ga10b-cold: ACR reported error 0x%08x (MAILBOX1=0x%08x)",
             (unsigned)mb0, (unsigned)mb1);
        return -1;
    }
    return 0;
}

/* ---- Netlist (NETC: the image nvgpu loads on silicon, see
 * ga10b_netlist.h) ---- */
static struct ga10b_netlist cold_netlist;

static int netlist_open(void)
{
    if (cold_netlist.data) return 0;
    struct ga10b_firmware_blob net;
    if (ga10b_firmware_get(GA10B_FW_NET_C, &net) < 0) {
        WARN("ga10b-cold: NETC netlist not embedded");
        return -1;
    }
    int rc = ga10b_netlist_open(&cold_netlist, net.data, net.size);
    if (rc != GA10B_NETLIST_OK) {
        WARN("ga10b-cold: NETC netlist rejected rc=%d", rc);
        return -1;
    }
    return 0;
}

static int load_av_region(uint32_t region_id)
{
    /* A region the netlist does not carry is an empty list, as in
     * nvgpu (the list stays zero-length when netlist.c never sees it). */
    struct ga10b_netlist_av_view v;
    int rc = ga10b_netlist_get_av_list(&cold_netlist, region_id, &v);
    if (rc == GA10B_NETLIST_ERR_NOT_FOUND) return 0;
    if (rc != GA10B_NETLIST_OK) {
        WARN("ga10b-cold: netlist region %s unreadable rc=%d",
             ga10b_netlist_region_name(region_id), rc);
        return -1;
    }
    for (uint32_t i = 0; i < v.count; i++) {
        if (v.l[i].addr >= GA10B_BAR0_SIZE || (v.l[i].addr & 3u)) {
            WARN("ga10b-cold: netlist region %s entry %u: bad address 0x%x",
                 ga10b_netlist_region_name(region_id), (unsigned)i,
                 (unsigned)v.l[i].addr);
            return -1;
        }
    }
    for (uint32_t i = 0; i < v.count; i++) {
        bar0_wr(v.l[i].addr, v.l[i].value);
    }
    return (int)v.count;
}

static int wait_gr_idle(void)
{
    /* ga10b_gr_init_wait_idle: GR_STATUS.busy clear. */
    if (wait_bits_clear(NV_PGRAPH_STATUS, 0x1u, COLD_TIMEOUT_GR_IDLE_US) != 0) {
        WARN("ga10b-cold: GR busy (STATUS=0x%08x)",
             (unsigned)bar0_rd(NV_PGRAPH_STATUS));
        return -1;
    }
    return 0;
}

/* ---- Step 5: GR hardware (gr_init_prepare_hw_impl) ----
 *
 * FIFO access on, the netlist's non-context register loads (sw_non_ctx
 * plus the compute and graphics local/global lists, as the L4T build
 * with CONFIG_NVGPU_GRAPHICS loads them), then wait for FECS/GPCCS
 * memory scrubbing and GR idle. GR interrupt enables are skipped: SLM-OS
 * polls. */
static int step_gr_hw(void)
{
    if (netlist_open() != 0) return -1;
    bar0_wr(NV_PGRAPH_PRI_FE_FIFO_ACCESS, GR_GPFIFO_CTL_ACCESS_SEMAPHORE);
    static const uint32_t lists[] = {
        GA10B_NETLIST_REGIONID_SW_NON_CTX_LOAD,
        GA10B_NETLIST_REGIONID_SW_NON_CTX_LOCAL_COMPUTE_LOAD,
        GA10B_NETLIST_REGIONID_SW_NON_CTX_LOCAL_GFX_LOAD,
        GA10B_NETLIST_REGIONID_SW_NON_CTX_GLOBAL_COMPUTE_LOAD,
        GA10B_NETLIST_REGIONID_SW_NON_CTX_GLOBAL_GFX_LOAD,
    };
    int total = 0;
    for (unsigned i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        int n = load_av_region(lists[i]);
        if (n < 0) return -1;
        total += n;
    }
    if (wait_bits_clear(NV_PGRAPH_PRI_FECS_DMACTL, GR_DMACTL_SCRUBBING, COLD_TIMEOUT_SCRUB_US) ||
        wait_bits_clear(NV_PGRAPH_PRI_GPCS_GPCCS_DMACTL, GR_DMACTL_SCRUBBING, COLD_TIMEOUT_SCRUB_US)) {
        WARN("ga10b-cold: FECS/GPCCS memory scrub did not finish");
        return -1;
    }
    if (wait_gr_idle() != 0) return -1;
    INFO("ga10b-cold: GR non-ctx loads done (%d writes)", total);
    return 0;
}

/* ---- Step 6: ACR ----
 *
 * Build the ctxsw ucode surface (FECS/GPCCS boot + netlist code/data),
 * the non-WPR LS blob for FECS and GPCCS and the flcn2_acr_desc that
 * points at it, then boot ACR on the GSP. ACR authenticates and
 * decrypts both images into the WPR carveout (programmed by MB2 for
 * every boot path) and loads them into the GR falcons. The surface is
 * CPU-only scratch; the blob and descriptor are read by the GSP over a
 * non-coherent path, so both are cleaned to DRAM. */
static int step_acr(void)
{
    struct ga10b_firmware_blob fecs, gpccs, fecs_sig, gpccs_sig, t, d, m;
    struct ga10b_firmware_blob pmu_img, pmu_desc, pmu_sig;
    if (netlist_open() != 0 ||
        ga10b_firmware_get(GA10B_FW_PMU_IMAGE, &pmu_img) < 0 ||
        ga10b_firmware_get(GA10B_FW_PMU_DESC, &pmu_desc) < 0 ||
        ga10b_firmware_get(GA10B_FW_PMU_SIG, &pmu_sig) < 0 ||
        ga10b_firmware_get(GA10B_FW_FECS, &fecs) < 0 ||
        ga10b_firmware_get(GA10B_FW_GPCCS, &gpccs) < 0 ||
        ga10b_firmware_get(GA10B_FW_FECS_SIG, &fecs_sig) < 0 ||
        ga10b_firmware_get(GA10B_FW_GPCCS_SIG, &gpccs_sig) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_TEXT, &t) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_DATA, &d) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_MANIFEST, &m) < 0) {
        WARN("ga10b-cold: GA10B firmware not embedded");
        return -1;
    }

    struct ga10b_ctxsw_ucode_inputs uin = {
        .fecs_fw = fecs.data, .fecs_fw_size = fecs.size,
        .gpccs_fw = gpccs.data, .gpccs_fw_size = gpccs.size,
    };
    struct ga10b_ctxsw_ucode_info uinfo;
    if (ga10b_ctxsw_ucode_inputs_from_netlist(&cold_netlist, &uin) != GA10B_CTXSW_OK ||
        ga10b_ctxsw_ucode_layout(&uin, &uinfo) != GA10B_CTXSW_OK) {
        WARN("ga10b-cold: ctxsw ucode layout failed");
        return -1;
    }
    size_t surf_pages = (uinfo.surface_size + 4095u) / 4096u;
    void *surface = pmm_alloc_pages(surf_pages);
    if (!surface ||
        ga10b_ctxsw_ucode_build(&uin, surface, surf_pages * 4096u, &uinfo) != GA10B_CTXSW_OK) {
        WARN("ga10b-cold: ctxsw ucode surface build failed");
        if (surface) pmm_free_pages(surface, surf_pages);
        return -1;
    }

    struct ga10b_wpr_info wpr;
    if (!ga10b_acr_wpr_info_from_regs(bar0_rd(GA10B_FB_MMU_WPR1_ADDR_LO),
                                      bar0_rd(GA10B_FB_MMU_WPR1_ADDR_HI), &wpr)) {
        WARN("ga10b-cold: WPR1 carveout not programmed");
        pmm_free_pages(surface, surf_pages);
        return -1;
    }

    struct ga10b_acr_blob_inputs bin = {
        .surface = surface, .surface_size = uinfo.surface_size, .ucode = &uinfo,
        .fecs_sig = fecs_sig.data, .fecs_sig_size = fecs_sig.size,
        .gpccs_sig = gpccs_sig.data, .gpccs_sig_size = gpccs_sig.size,
        .wpr_base = wpr.wpr_base,
        /* L4T's configuration: the LS PMU is in the blob and bootstraps
         * FECS/GPCCS lazily. ACR-only bootstrap is not usable with this
         * firmware: the ctxsw ucode halts with mailbox6 = 0x98, and
         * nvgpu with support_ls_pmu=false fails the same way. */
        .include_pmu = true,
        .pmu_image = pmu_img.data, .pmu_image_size = pmu_img.size,
        .pmu_desc = pmu_desc.data, .pmu_desc_size = pmu_desc.size,
        .pmu_sig = pmu_sig.data, .pmu_sig_size = pmu_sig.size,
    };
    static struct ga10b_acr_blob_layout lay;
    if (ga10b_acr_blob_layout(&bin, &lay) != GA10B_ACR_OK || lay.wpr_size > wpr.size) {
        WARN("ga10b-cold: LS blob layout failed (size 0x%x, WPR 0x%lx)",
             (unsigned)lay.wpr_size, (unsigned long)wpr.size);
        pmm_free_pages(surface, surf_pages);
        return -1;
    }
    /* Blob, descriptor and ls_pmu_desc stay allocated on success and on
     * failure alike: ACR and the PMU read them by physical address, and
     * after a failed or timed-out boot SLM-OS cannot tell whether the
     * GSP is still reading them. nvgpu keeps them for the GPU's
     * lifetime too. A retried step allocates fresh copies. */
    size_t blob_pages = (lay.wpr_size + 4095u) / 4096u;
    void *blob = pmm_alloc_pages(blob_pages);
    struct ga10b_flcn2_acr_desc *desc = pmm_alloc_page();
    void *ls_pmu_desc = pmm_alloc_page();
    int rc = -1;
    if (blob && desc && ls_pmu_desc &&
        ga10b_acr_blob_build(&bin, blob, blob_pages * 4096u, &lay) == GA10B_ACR_OK &&
        ga10b_acr_copy_ls_pmu_desc(ls_pmu_desc, 4096, pmu_desc.data,
                                   pmu_desc.size) == GA10B_ACR_OK) {
        cache_clean_range(blob, lay.wpr_size);
        cache_clean_range(ls_pmu_desc, 4096);
        memset(desc, 0, 4096);
        ga10b_acr_build_flcn2_desc_ls_pmu(desc, (uint64_t)(uintptr_t)blob,
                                          lay.wpr_size,
                                          (uint64_t)(uintptr_t)ls_pmu_desc);
        cache_clean_range(desc, sizeof(*desc));
        INFO("ga10b-cold: LS blob 0x%lx (%u bytes), desc 0x%lx, WPR 0x%lx+0x%lx",
             (unsigned long)(uintptr_t)blob, (unsigned)lay.wpr_size,
             (unsigned long)(uintptr_t)desc, (unsigned long)wpr.wpr_base,
             (unsigned long)wpr.size);
        rc = ga10b_cold_gsp_boot_acr_blobs(t.data, (uint32_t)t.size,
                                           d.data, (uint32_t)d.size,
                                           m.data, (uint32_t)m.size,
                                           (uint64_t)(uintptr_t)desc);
    } else {
        WARN("ga10b-cold: LS blob build failed");
    }
    pmm_free_pages(surface, surf_pages);
    return rc;
}

/* FECS method interface (gm20b_gr_falcon_submit_fecs_method_op):
 * clear mailbox 0, write data then method, poll mailbox 0. */
static int fecs_method(uint32_t method, uint32_t data, uint32_t *ret)
{
    bar0_wr(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(0), 0u);
    bar0_wr(NV_PGRAPH_PRI_FECS_METHOD_DATA, data);
    bar0_wr(NV_PGRAPH_PRI_FECS_METHOD_PUSH, method);
    for (uint32_t t = 0; t < COLD_TIMEOUT_FECS_US; t += 10) {
        uint32_t v = bar0_rd(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(0));
        if (v != 0u) {
            if (ret) *ret = v;
            return 0;
        }
        udelay(10);
    }
    return -1;
}

/* LS PMU driver state (step_pmu boots it, step_falcons uses it). */
static struct ga10b_pmu cold_pmu;

/* Print what the driver parsed from the PMU INIT message and the raw
 * first message-queue element in the super surface. */
static void pmu_dump_init(void)
{
    const struct ga10b_pmu_init_msg *im = &cold_pmu.init;
    INFO("ga10b-cold: PMU init: unit %u func %u status %u queues_start 0x%x "
         "sw_area 0x%x+0x%x heap 0x%x/%u blks cmdq %u",
         (unsigned)cold_pmu.init_unit_id, (unsigned)im->hdr.function,
         (unsigned)im->status, (unsigned)im->queues_start,
         (unsigned)im->sw_managed_area_offset, (unsigned)im->sw_managed_area_size,
         (unsigned)cold_pmu.heap_base, (unsigned)cold_pmu.heap_blks,
         (unsigned)cold_pmu.cmdq_index);
    const uint8_t *ss = cold_pmu.cfg.super_surface.cpu;
    if (!ss) return;
    cache_invalidate_range(ss + GA10B_SS_FBQ_MSG_OFFSET, 64);
    const uint32_t *w = (const uint32_t *)(ss + GA10B_SS_FBQ_MSG_OFFSET);
    for (unsigned i = 0; i < 16u; i += 4u) {
        INFO("ga10b-cold: msgq[0]+%02x: %08x %08x %08x %08x", i * 4u,
             (unsigned)w[i], (unsigned)w[i + 1], (unsigned)w[i + 2],
             (unsigned)w[i + 3]);
    }
}

/* ---- Step 8: start FECS/GPCCS ----
 *
 * nvgpu_gr_falcon_load_secure_ctxsw_ucode with ACR-loaded falcons:
 * start GPCCS, clear FECS mailbox 0, set mailbox 1, start FECS
 * (gm20b_gr_falcon_start_fecs), wait for the ctxsw ucode to post PASS
 * in mailbox 0 (gm20b_gr_falcon_wait_ctxsw_ready), then the first
 * methods nvgpu sends: watchdog timeout and the context image size. */
static uint32_t gr_ctx_image_size;

/* SM numbering nvgpu computes for this part (1 GPC, 4 TPCs on 2 PES,
 * nvgpu_gr_config SM ordering), from the trace: SM n -> TPC
 * ga10b_sm_to_tpc[n]. ga10b_gr_init_sm_id_numbering writes TPC.SM_CFG
 * = smid and GPM_PD_SM_ID(tpc) = smid. */
static const uint8_t ga10b_sm_to_tpc[4] = { 3, 1, 2, 0 };

static void gr_config_sm_ids(void)
{
    /* gv11b_gr_config_set_live_pes_mask: SWDX PES mask = live PES. */
    bar0_wr(NV_PGRAPH_PRI_GPC0_SWDX_PES_MASK,
            bar0_rd(NV_PGRAPH_PRI_GPC0_GPM_PD_LIVE_PHYSICAL_PES));
    for (uint32_t sm = 0; sm < 4u; sm++) {
        uint32_t tpc = ga10b_sm_to_tpc[sm];
        bar0_wr(NV_PGRAPH_PRI_GPC0_TPC_SM_CFG(tpc), sm);
        bar0_wr(NV_PGRAPH_PRI_GPC0_GPM_PD_SM_ID(tpc), sm);
    }
}

static int step_falcons(void)
{
    gr_config_sm_ids();

    /* gr_falcon_sec2_or_ls_pmu_coldboot_bootstrap: the PMU loads FECS
     * and GPCCS from WPR (ACR INIT_WPR_REGION + BOOTSTRAP_GR_FALCONS). */
    int prc = ga10b_pmu_bootstrap_ctxsw_falcons(&cold_pmu, 3000);
    ga10b_pmu_dbg_drain(&cold_pmu);
    if (prc != GA10B_PMU_OK) {
        WARN("ga10b-cold: PMU did not bootstrap FECS/GPCCS rc=%d last_error=%d",
             prc, cold_pmu.last_error);
        pmu_dump_init();
        return -1;
    }
    bar0_wr(NV_PGRAPH_PRI_GPCS_GPCCS_CPUCTL_ALIAS, GR_CPUCTL_STARTCPU);
    bar0_wr(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(0), 0u);
    bar0_wr(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(1), 1u);
    bar0_wr(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(6), 0u);
    bar0_wr(NV_PGRAPH_PRI_FECS_CPUCTL_ALIAS, GR_CPUCTL_STARTCPU);

    uint32_t mb0 = 0;
    for (uint32_t t = 0; t < COLD_TIMEOUT_FECS_US; t += 10) {
        mb0 = bar0_rd(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(0));
        if (mb0 != 0u) break;
        udelay(10);
    }
    if (mb0 != FECS_MAILBOX0_PASS) {
        WARN("ga10b-cold: FECS ctxsw ucode not ready (MAILBOX0=0x%08x "
             "MAILBOX6=0x%08x CPUCTL=0x%08x)", (unsigned)mb0,
             (unsigned)bar0_rd(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(6)),
             (unsigned)bar0_rd(NV_PGRAPH_PRI_FECS_CPUCTL));
        return -1;
    }
    bar0_wr(NV_PGRAPH_PRI_FECS_CURRENT_CTX, 0u);

    /* Watchdog: FECS acknowledges without a mailbox value, so only
     * the push matters (nvgpu does not wait for this one either). */
    bar0_wr(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(0), 0u);
    bar0_wr(NV_PGRAPH_PRI_FECS_METHOD_DATA, FECS_WATCHDOG_TIMEOUT);
    bar0_wr(NV_PGRAPH_PRI_FECS_METHOD_PUSH, FECS_METHOD_SET_WATCHDOG_TIMEOUT);
    udelay(100);

    if (fecs_method(FECS_METHOD_DISCOVER_IMAGE_SIZE, 0u, &gr_ctx_image_size) != 0) {
        WARN("ga10b-cold: FECS did not answer DISCOVER_IMAGE_SIZE");
        return -1;
    }
    INFO("ga10b-cold: FECS/GPCCS running, GR context image 0x%x bytes",
         (unsigned)gr_ctx_image_size);
    return 0;
}

/* ga10b_ltc_lts_set_mgmt_setup (errata 200601972) and
 * ga10b_fb_set_atomic_mode: RMW atomics, sys ncoh atomics in L2,
 * mirrored into the HSHUB. nvgpu runs both right after ACR, before the
 * PMU boots (trace 11220-11229). */
static void ltc_fb_atomic_setup(void)
{
    uint32_t v = bar0_rd(NV_PLTCG_LTCS_LTSS_TSTG_SET_MGMT_3);
    bar0_wr(NV_PLTCG_LTCS_LTSS_TSTG_SET_MGMT_3, v | LTC_SET_MGMT_3_DISALLOW_CLEAN_IMM);

    v = bar0_rd(NV_PFB_PRI_MMU_CTRL);
    v = (v & ~(FB_MMU_CTRL_ATOMIC_CAP_MODE_M | FB_MMU_CTRL_ATOMIC_SYS_NCOH_M)) |
        FB_MMU_CTRL_ATOMIC_CAP_MODE_RMW;
    bar0_wr(NV_PFB_PRI_MMU_CTRL, v);
    v = bar0_rd(NV_PFB_FBHUB_NUM_ACTIVE_LTCS);
    v = (v & ~FB_HUB_SYS_ATOMIC_MODE_M) | FB_HUB_SYS_ATOMIC_MODE_USE_RMW;
    bar0_wr(NV_PFB_FBHUB_NUM_ACTIVE_LTCS, v);
    bar0_wr(NV_PFB_HSHUB0_NUM_ACTIVE_LTCS, v);

}

/* ---- Step 9: GR init support ----
 *
 * What nvgpu does between FECS start and golden context creation:
 * ga10b_gr_init_gpc_mmu (GPC MMU copies of the FB MMU state), the
 * constant table (ga10b_gr_init_table.c) and the copy-engine device
 * enables. The table encodes this SKU's floorsweeping, so check it
 * first. The CBC (compression backing store) is not set up: SLM-OS maps
 * GPU memory with uncompressed kinds only. */
static int step_gr_init(void)
{
    uint32_t gpc_fuse = bar0_rd(NV_FUSE_STATUS_OPT_GPC);
    uint32_t tpcs = bar0_rd(NV_PGRAPH_PRI_GPC0_GPCCS_FS_GPC) & 0x1Fu;
    if (gpc_fuse != GA10B_COLD_OPT_GPC_EXPECTED || tpcs != GA10B_COLD_TPC_COUNT) {
        WARN("ga10b-cold: unexpected floorsweeping (opt_gpc=0x%x, %u TPCs); "
             "the GR init table is for 1 GPC / 4 TPCs", (unsigned)gpc_fuse,
             (unsigned)tpcs);
        return -1;
    }

    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_CTRL,
            bar0_rd(NV_PFB_PRI_MMU_CTRL) & GPC_MMU_CTRL_COPY_MASK);
    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_PM_UNIT_MASK, 0u);
    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_PM_REQ_MASK, 0u);
    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_DEBUG_CTRL, bar0_rd(NV_PFB_PRI_MMU_DEBUG_CTRL));
    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_DEBUG_WR, bar0_rd(NV_PFB_PRI_MMU_DEBUG_WR));
    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_DEBUG_RD, bar0_rd(NV_PFB_PRI_MMU_DEBUG_RD));
    bar0_wr(NV_PGRAPH_PRI_GPCS_MMU_NUM_ACTIVE_LTCS,
            bar0_rd(NV_PFB_FBHUB_NUM_ACTIVE_LTCS_MMU));

    for (unsigned i = 0; i < ga10b_gr_init_support_writes_count; i++) {
        bar0_wr(ga10b_gr_init_support_writes[i].offset,
                ga10b_gr_init_support_writes[i].value);
    }

    if (device_enable(GA10B_LCE_DEVICE_ENABLE_MASK, true) != 0) return -1;
    if (wait_gr_idle() != 0) return -1;
    INFO("ga10b-cold: GR init support done (%u table writes)",
         ga10b_gr_init_support_writes_count);
    return 0;
}

/* ---- GPU memory ops shared by the PMU VM and channel code ---- */
static uint32_t ops_rd32(void *ctx, uint32_t off) { (void)ctx; return bar0_rd(off); }
static void ops_wr32(void *ctx, uint32_t off, uint32_t v) { (void)ctx; bar0_wr(off, v); }
static void ops_udelay(void *ctx, uint32_t us) { (void)ctx; udelay(us); }
static void *ops_alloc_pages(void *ctx, uint32_t npages, uint64_t *phys)
{
    (void)ctx;
    void *p = pmm_alloc_pages(npages);
    if (!p) return NULL;
    memset(p, 0, (size_t)npages * 4096u);
    cache_clean_range(p, (size_t)npages * 4096u);
    *phys = (uint64_t)(uintptr_t)p;   /* identity map, SMMU bypass */
    return p;
}
static void ops_free_pages(void *ctx, void *cpu, uint32_t npages)
{
    (void)ctx;
    pmm_free_pages(cpu, npages);
}
static void *ops_phys_to_cpu(void *ctx, uint64_t phys) { (void)ctx; return (void *)(uintptr_t)phys; }
static void ops_clean(void *ctx, const void *p, size_t n) { (void)ctx; cache_clean_range(p, n); }
static void ops_inval(void *ctx, const void *p, size_t n) { (void)ctx; cache_invalidate_range(p, n); }
static void ops_log(void *ctx, const char *line) { (void)ctx; INFO("pmu-fw: %s", line); }

static const struct ga10b_gpu_ops cold_gpu_ops = {
    .rd32 = ops_rd32, .wr32 = ops_wr32, .udelay = ops_udelay,
    .alloc_pages = ops_alloc_pages, .free_pages = ops_free_pages,
    .phys_to_cpu = ops_phys_to_cpu,
    .cache_clean = ops_clean, .cache_inval = ops_inval,
};

static const struct ga10b_pmu_ops cold_pmu_ops = {
    .rd32 = ops_rd32, .wr32 = ops_wr32, .delay_us = ops_udelay,
    .dma_clean = ops_clean, .dma_inval = ops_inval, .log_line = ops_log,
};

/* ---- Step 7: LS PMU (defined here, after the GPU memory ops) ----
 *
 * nvgpu_pmu_rtos_init on L4T: the PMU's own VM (nvgpu_init_system_vm)
 * with the super surface and the RISC-V trace buffer mapped at fixed
 * VAs below 512 MB, an instance block pointing at it, then the boot
 * arguments through MAILBOX0/1 (EXTMEM2 offset), RISC-V start, BROM
 * and priv-lockdown waits, and the RTOS INIT message (queue layout in
 * the super surface). SLM-OS polls instead of taking the PMU IRQ, as
 * nvgpu's own wait loop does. Perfmon and ELPG stay off (nvgpu's prod
 * L4T build disables both). */
#define PMU_VM_SUPER_SURFACE_VA  0x00100000ull
#define PMU_VM_TRACE_BUF_VA      0x00210000ull

static struct ga10b_vm cold_pmu_vm;

static int pmu_buf(uint32_t bytes, struct ga10b_pmu_buf *b)
{
    struct ga10b_dma d;
    if (ga10b_dma_alloc(&cold_gpu_ops, bytes, &d) != GA10B_CHAN_OK) return -1;
    b->cpu = d.cpu;
    b->addr = d.phys;
    b->size = d.size;
    return 0;
}

/* PMU buffers, VM and instance block are never freed, also on failure:
 * the RISC-V core may still be running and reading them by physical or
 * VM address after a timeout. A retried step allocates new ones. */
static int step_pmu(void)
{
    ltc_fb_atomic_setup();

    struct ga10b_pmu_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (pmu_buf(GA10B_PMU_BOOT_ARGS_SIZE, &cfg.boot_args) ||
        pmu_buf(GA10B_PMU_CORE_DUMP_SIZE, &cfg.core_dump) ||
        pmu_buf(GA10B_PMU_SUPER_SURFACE_SIZE, &cfg.super_surface) ||
        pmu_buf(GA10B_PMU_TRACE_BUF_SIZE, &cfg.trace_buf)) {
        WARN("ga10b-cold: out of memory for PMU buffers");
        return -1;
    }
    if (ga10b_vm_init(&cold_gpu_ops, &cold_pmu_vm) != GA10B_CHAN_OK ||
        ga10b_vm_map(&cold_gpu_ops, &cold_pmu_vm, PMU_VM_SUPER_SURFACE_VA,
                     cfg.super_surface.addr, cfg.super_surface.size, 0) != GA10B_CHAN_OK ||
        ga10b_vm_map(&cold_gpu_ops, &cold_pmu_vm, PMU_VM_TRACE_BUF_VA,
                     cfg.trace_buf.addr, cfg.trace_buf.size, 0) != GA10B_CHAN_OK) {
        WARN("ga10b-cold: PMU VM setup failed");
        return -1;
    }
    cfg.super_surface.gpu_va = PMU_VM_SUPER_SURFACE_VA;
    cfg.trace_buf.gpu_va = PMU_VM_TRACE_BUF_VA;

    struct ga10b_dma inst;
    if (ga10b_dma_alloc(&cold_gpu_ops, GA10B_PMU_INST_BLOCK_SIZE, &inst) != GA10B_CHAN_OK) {
        WARN("ga10b-cold: out of memory for the PMU instance block");
        return -1;
    }
    ga10b_pmu_build_inst_block(inst.cpu, cold_pmu_vm.pdb.phys, false);
    cache_clean_range(inst.cpu, GA10B_PMU_INST_BLOCK_SIZE);
    ga10b_vm_tlb_invalidate(&cold_gpu_ops, &cold_pmu_vm);
    cfg.inst_block_addr = inst.phys;
    cfg.sysmem_coherent = false;

    int rc = ga10b_pmu_init(&cold_pmu, &cold_pmu_ops, &cfg);
    if (rc == GA10B_PMU_OK) rc = ga10b_pmu_boot(&cold_pmu);
    if (rc == GA10B_PMU_OK) rc = ga10b_pmu_wait_init(&cold_pmu, 3000);
    ga10b_pmu_dbg_drain(&cold_pmu);
    if (rc != GA10B_PMU_OK) {
        WARN("ga10b-cold: PMU boot failed rc=%d (last_error=%d BR_RETCODE=0x%x "
             "IRQSTAT=0x%x halted=%d msgs=%u events=%u)", rc,
             cold_pmu.last_error, (unsigned)cold_pmu.brom_retcode,
             (unsigned)cold_pmu.last_irqstat, (int)cold_pmu.halted,
             (unsigned)cold_pmu.msgs_seen, (unsigned)cold_pmu.events_seen);
        return -1;
    }
    INFO("ga10b-cold: PMU RTOS up (cmd queue %u, %u msgs)",
         (unsigned)cold_pmu.cmdq_index, (unsigned)cold_pmu.msgs_seen);
    return 0;
}

/* ---- Step 10: golden context ----
 *
 * The rest of nvgpu_gr_falcon_init_ctx_state (PM/zcull/preemption image
 * sizes) and the SM L1 tag write, then nvgpu_gr_obj_ctx_init_golden_
 * context_image: global context buffers, a temporary VM + instance
 * block + gr_ctx, BIND_PTR, sw_ctx_load, bundle/method init, and FECS
 * GOLDEN_IMAGE_SAVE. ga10b_grctx keeps a CPU copy for channels. */
static struct ga10b_grctx cold_grctx;

static int step_golden(void)
{
    int rc = ga10b_grctx_init(&cold_grctx, &cold_netlist, gr_ctx_image_size);
    if (rc == GA10B_GRCTX_OK) rc = ga10b_grctx_init_support(&cold_gpu_ops, &cold_grctx);
    if (rc == GA10B_GRCTX_OK) rc = ga10b_grctx_golden_create(&cold_gpu_ops, &cold_grctx);
    if (rc != GA10B_GRCTX_OK) {
        WARN("ga10b-cold: golden context failed rc=%d (FECS MAILBOX0=0x%08x "
             "MAILBOX6=0x%08x GR_STATUS=0x%08x)", rc,
             (unsigned)bar0_rd(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(0)),
             (unsigned)bar0_rd(NV_PGRAPH_PRI_FECS_CTXSW_MAILBOX(6)),
             (unsigned)bar0_rd(NV_PGRAPH_STATUS));
        return -1;
    }
    INFO("ga10b-cold: golden context image saved (0x%x bytes)",
         (unsigned)cold_grctx.golden_size);
    return 0;
}

/* ---- Step 11: channel ----
 *
 * What the Linux helper's ioctls do kernel-side (ALLOC_AS, OPEN_TSG,
 * OPEN_CHANNEL, CREATE_SUBCONTEXT, SETUP_BIND, ALLOC_OBJ_CTX 0xC7C0,
 * SET_PREEMPTION_MODE): address space, instance block, USERD, GPFIFO,
 * pushbuffer, semaphore page and a GR context loaded from the golden
 * image, then the runlist submit and CHRAM enable. The result is
 * installed as the active handoff, so `nvgpu submit` /
 * `submit-compute` and the dispatch paths run on it. */
static struct ga10b_chan cold_chan;
static struct ga10b_gr_chan_ctx cold_chan_gr;

static int step_channel(void)
{
    struct ga10b_chan_params p;
    ga10b_chan_default_params(&p);
    int rc = ga10b_chan_create(&cold_gpu_ops, &cold_grctx, &p, &cold_chan_gr, &cold_chan);
    if (rc != GA10B_CHAN_OK) {
        WARN("ga10b-cold: channel create failed rc=%d", rc);
        return -1;
    }
    struct ga10b_channel_handoff h;
    memset(&h, 0, sizeof(h));
    ga10b_chan_fill_handoff(&cold_chan, &h);
    if (slm_gpu_install_handoff(ga10b_bringup_state(), &h) != 0) {
        WARN("ga10b-cold: channel handoff rejected");
        return -1;
    }
    INFO("ga10b-cold: channel %u (TSG %u) on the runlist, doorbell token 0x%x, "
         "inst 0x%lx", (unsigned)p.chid, (unsigned)p.tsgid,
         (unsigned)cold_chan.work_submit_token,
         (unsigned long)cold_chan.inst.phys);
    return 0;
}

/* ---- Step 12: MNIST pipeline ----
 *
 * What scripts/gpu-kernel-mnist.c --preserve-for-kexec --qmd-pool sets
 * up on Linux: shaders from the embedded operator library, weights from
 * the embedded mnist.onnx, cbufs, outputs, QMD pool and the v7 pipeline
 * ops, installed as the active handoff so slm_gpu_run_mnist and the
 * runtime's MNIST fastpath dispatch on this channel. On failure the
 * channel handoff from step 11 stays installed. */
static int step_mnist(void)
{
    return ga10b_mnist_cold_install(&cold_gpu_ops, &cold_chan) == 0 ? 0 : -1;
}

typedef int (*cold_step_fn)(void);

static const cold_step_fn step_fns[GA10B_COLD_STEP_COUNT] = {
    [GA10B_COLD_POWER] = step_power,
    [GA10B_COLD_PRI]   = step_pri,
    [GA10B_COLD_ENGINES] = step_engines,
    [GA10B_COLD_FB]      = step_fb,
    [GA10B_COLD_GR_HW]   = step_gr_hw,
    [GA10B_COLD_ACR]     = step_acr,
    [GA10B_COLD_PMU]     = step_pmu,
    [GA10B_COLD_FALCONS] = step_falcons,
    [GA10B_COLD_GR_INIT] = step_gr_init,
    [GA10B_COLD_GOLDEN]  = step_golden,
    [GA10B_COLD_CHANNEL] = step_channel,
    [GA10B_COLD_MNIST]   = step_mnist,
};

/* Set while a ga10b_cold_run is driving the hardware. A flag rather
 * than a spinlock: the steps spin for up to seconds. */
static atomic_flag cold_running = ATOMIC_FLAG_INIT;

int ga10b_cold_run(enum ga10b_cold_step last)
{
    if ((unsigned)last >= GA10B_COLD_STEP_COUNT) return -1;
    if (atomic_flag_test_and_set_explicit(&cold_running, memory_order_acquire)) {
        return GA10B_COLD_ERR_BUSY;
    }
    int rc = 0;
    for (unsigned s = (unsigned)cold_done + 1u; s <= (unsigned)last; s++) {
        INFO("ga10b-cold: step %u (%s)", s, step_names[s]);
        if (step_fns[s]() != 0) {
            WARN("ga10b-cold: step %u (%s) failed", s, step_names[s]);
            rc = -(int)s;
            break;
        }
        cold_done = (enum ga10b_cold_step)s;
    }
    atomic_flag_clear_explicit(&cold_running, memory_order_release);
    return rc;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
