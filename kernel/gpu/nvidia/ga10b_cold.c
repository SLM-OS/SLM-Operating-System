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
#include "cache.h"
#include "string.h"

#include <stdint.h>
#include <stdbool.h>

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

static enum ga10b_cold_step cold_done = GA10B_COLD_NONE;

static const char *const step_names[GA10B_COLD_STEP_COUNT] = {
    [GA10B_COLD_NONE]  = "none",
    [GA10B_COLD_POWER] = "power",
    [GA10B_COLD_PRI]   = "pri",
    [GA10B_COLD_ENGINES] = "engines",
    [GA10B_COLD_FB]      = "fb",
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
    if (rc != 0)
        WARN("ga10b-cold: clk %s (%u) enable rc=%d", name, (unsigned)id, rc);
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
    for (unsigned g = 0; g < 2u; g++) {
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

    unsigned retry = 0;
    while (bar0_rd(NV_PPRIV_MASTER_RING_COMMAND) != 0u && retry < 1000u) {
        udelay(10);
        retry++;
    }
    if (retry == 1000u) {
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
static int step_engines(void)
{
    uint32_t row1 = bar0_rd(NV_PTOP_DEVICE_INFO2(GA10B_GR_DEVINFO_ROW1));
    if ((row1 & 0xFFu) != GA10B_GR_RESET_ID) {
        WARN("ga10b-cold: device_info2[1]=0x%08x, GR reset_id %u expected",
             (unsigned)row1, (unsigned)GA10B_GR_RESET_ID);
        return -1;
    }
    if (device_enable(ga10b_cold_reset_id_mask(GA10B_EARLY_RESET_ID), true) != 0)
        return -1;

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
        for (uint32_t b = 0; b < 4 && i + b < len; b++)
            w |= (uint32_t)src[i + b] << (8 * b);
        bar0_wr(data_reg, w);
    }
}

static int wait_bits_clear(uint32_t reg, uint32_t mask, uint32_t timeout_us)
{
    for (uint32_t t = 0; t < timeout_us; t += 10) {
        if ((bar0_rd(reg) & mask) == 0) return 0;
        udelay(10);
    }
    return -1;
}

int ga10b_cold_gsp_boot_acr_blobs(const uint8_t *code, uint32_t code_len,
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

    /* DMEM size: HWCFG bits 17:9 in 256-byte blocks (falcon_hwcfg_dmem_size). */
    uint32_t dmem_size = ((bar0_rd(NV_PGSP_FALCON_HWCFG) >> 9) & 0x1FFu) * 256u;
    if (code_len & 3u || data_len & 3u || manifest_len & 3u ||
        data_len + manifest_len > dmem_size) {
        WARN("ga10b-cold: ACR image does not fit (data %u + manifest %u > "
             "DMEM %u)", (unsigned)data_len, (unsigned)manifest_len,
             (unsigned)dmem_size);
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
    for (uint32_t t = 0; t < 5000000u; t += 10) {     /* 5 s */
        ret = bar0_rd(NV_PGSP_RISCV_BR_RETCODE);
        if (RISCV_BR_RETCODE_RESULT(ret) == RISCV_BR_RETCODE_PASS ||
            RISCV_BR_RETCODE_RESULT(ret) == RISCV_BR_RETCODE_FAIL)
            break;
        udelay(10);
    }
    if (RISCV_BR_RETCODE_RESULT(ret) != RISCV_BR_RETCODE_PASS) {
        WARN("ga10b-cold: GSP BROM rejected ACR (BR_RETCODE=0x%08x)",
             (unsigned)ret);
        return -1;
    }

    uint32_t ctl = 0;
    for (uint32_t t = 0; t < 10000000u; t += 10) {    /* 10 s */
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

typedef int (*cold_step_fn)(void);

static const cold_step_fn step_fns[GA10B_COLD_STEP_COUNT] = {
    [GA10B_COLD_POWER] = step_power,
    [GA10B_COLD_PRI]   = step_pri,
    [GA10B_COLD_ENGINES] = step_engines,
    [GA10B_COLD_FB]      = step_fb,
};

int ga10b_cold_run(enum ga10b_cold_step last)
{
    if ((unsigned)last >= GA10B_COLD_STEP_COUNT) return -1;
    for (unsigned s = (unsigned)cold_done + 1u; s <= (unsigned)last; s++) {
        INFO("ga10b-cold: step %u (%s)", s, step_names[s]);
        if (step_fns[s]() != 0) {
            WARN("ga10b-cold: step %u (%s) failed", s, step_names[s]);
            return -(int)s;
        }
        cold_done = (enum ga10b_cold_step)s;
    }
    return 0;
}

#endif /* PLATFORM_JETSON_ORIN_NANO */
