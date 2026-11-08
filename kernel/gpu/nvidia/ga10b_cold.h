/*
 * ga10b_cold.h - GA10B bring-up from cold, without Linux (#1088).
 *
 * After a UEFI-direct boot nothing has initialised the GPU: nvgpu
 * never ran, so the PRI ring is down, engines sit in reset, no ACR
 * has run on the GSP and FECS/GPCCS hold no ucode. The kexec path
 * inherits all of that from Linux; this module rebuilds it, following
 * nvgpu's nvgpu_finalize_poweron (L4T R36.4.x) step by step. The
 * order and register values were cross-checked against an MMIO trace
 * of nvgpu's first power-on on jetson-nano-2 (see ga10b_cold.c).
 *
 * Steps run in order; each one records itself as done so a shell
 * retry (`nvgpu cold`) resumes after the last step that succeeded.
 */
#ifndef GPU_NVIDIA_GA10B_COLD_H
#define GPU_NVIDIA_GA10B_COLD_H

#include <stdint.h>
#include <stdbool.h>

enum ga10b_cold_step {
    GA10B_COLD_NONE = 0,
    GA10B_COLD_POWER,       /* BPMP: GPU partition on, clocks enabled */
    GA10B_COLD_PRI,         /* PRI ring enumerated, PBUS interrupts set */
    GA10B_COLD_ENGINES,     /* engines out of reset, GR reset cycled */
    GA10B_COLD_FB,          /* FB/MMU: sysmem flush + MMU debug pages */
    GA10B_COLD_GR_HW,       /* GR non-ctx register loads from the netlist */
    GA10B_COLD_ACR,         /* ACR on the GSP loads PMU/FECS/GPCCS into WPR */
    GA10B_COLD_PMU,         /* LS PMU RTOS booted, INIT message received */
    GA10B_COLD_FALCONS,     /* FECS/GPCCS started, ctxsw ucode ready */
    GA10B_COLD_GR_INIT,     /* GR/FB/LTC/CE setup nvgpu does after start */
    GA10B_COLD_GOLDEN,      /* golden GR context image saved by FECS */
    GA10B_COLD_CHANNEL,     /* TSG + channel on the runlist, handoff installed */
    GA10B_COLD_MNIST,       /* MNIST v7 pipeline built on the channel */
    GA10B_COLD_POLICIES,    /* sched/eviction pipelines + operator library (#1092) */
    GA10B_COLD_STEP_COUNT,
};

/* Returned by ga10b_cold_run when another caller (boot, or a second
 * shell session) is already running the sequence. */
#define GA10B_COLD_ERR_BUSY (-100)

/* Run every step after the last completed one, up to and including
 * `last`. Returns 0 when `last` is reached, the negative number of
 * the step that failed, or GA10B_COLD_ERR_BUSY. Not reentrant: a
 * concurrent call returns GA10B_COLD_ERR_BUSY instead of driving the
 * hardware sequence twice. */
int ga10b_cold_run(enum ga10b_cold_step last);

/* Highest step completed so far (GA10B_COLD_NONE before any). */
enum ga10b_cold_step ga10b_cold_progress(void);

/* Short name for a step, for logs and the shell. */
const char *ga10b_cold_step_name(enum ga10b_cold_step step);

/* ---- Recovery after a GPU step timeout (#1171) ---- */

struct ga10b_cold_recovery {
    int rc;                 /* 0: the channel works again (probe passed) */
    int level;              /* last level tried: 1 channel reset, 2 GR engine reset */
    int l1_rc, l2_rc;       /* per level: 0, or the step that failed */
    uint32_t l1_us, l2_us, probe_us;
    uint32_t latched;       /* a fault was latched: straight to level 2 */
    uint32_t probe_failed;  /* ga10b_bringup_probe_channel: 1 host, 2 compute */
    uint32_t evict_failed;
    /* State when recovery started (and CHRAM before level 2 / at the end). */
    uint32_t chram, chram_l2, chram_after;
    uint32_t eng_status0, pbdma_status, pbdma_intr0;
    uint32_t gr_status, gr_intr, gr_exception;
    uint32_t fecs_mb0, fecs_mb6;
    uint32_t fault_status, fault_info, fault_addr_lo;
    uint32_t chan_flags;        /* GA10B_CHAN_RESET_* used */
    /* When the probe failed: where the channel stood. */
    uint32_t eng_status_fail, pbdma_status_fail, gr_status_fail;
    uint32_t fecs_mb0_fail, fecs_mb6_fail, fecs_ctx_fail;
};

/* Recovery options: GA10B_CHAN_RESET_* bits for the channel reset, plus: */
#define GA10B_COLD_RECOVER_CLEAR_FIRST  0x100u  /* clear latched MMU/PBDMA/runlist state first */
#define GA10B_COLD_RECOVER_DEFAULT_OPTS 0x0u
void ga10b_cold_recover_set_opts(uint32_t opts);
uint32_t ga10b_cold_recover_get_opts(void);

/* Bring the cold channel back after a timed-out or faulted step. Task
 * context, IRQs on, no spinlock held; the caller has shut every other
 * dispatcher out first (slm_ffi.c). `level`: 0 chooses (level 1 unless
 * an error is latched, then level 2), 1 level 1 only, 2 level 2 only.
 * Returns 0 when the probe passed, else a negative code (also in r->rc).
 * Prints nothing: the caller prints one line from `r`. */
int ga10b_cold_recover(int level, struct ga10b_cold_recovery *r);

/* One BAR0 register write of a generated init table. */
struct ga10b_reg_write {
    uint32_t offset;
    uint32_t value;
};

/* Constant GR programming after FECS start (ga10b_gr_init_table.c). */
extern const struct ga10b_reg_write ga10b_gr_init_support_writes[];
extern const unsigned ga10b_gr_init_support_writes_count;

/* ---- Pure helpers (host-testable) ---- */

/* ACR image fits the GSP falcon: lengths are multiples of 4, the code
 * fits IMEM, and data + manifest fit DMEM (summed in 64 bits). */
static inline bool ga10b_cold_acr_image_fits(uint32_t code_len, uint32_t data_len,
                                             uint32_t manifest_len,
                                             uint32_t imem_size, uint32_t dmem_size)
{
    if ((code_len | data_len | manifest_len) & 3u) return false;
    if (code_len > imem_size) return false;
    return (uint64_t)data_len + manifest_len <= dmem_size;
}

/* NV_PFB_PRI_MMU_DEBUG_WR/RD value for a 4 KB-aligned sysmem page:
 * address >> 12 in bits 31:4, aperture SYS_MEM_NONCOHERENT (3), VOL=0
 * (fb_mmu_debug_wr_addr_f / _aperture_sys_mem_ncoh_f). */
static inline uint32_t ga10b_cold_mmu_debug_val(uint64_t phys)
{
    return (uint32_t)(((phys >> 12) & 0x0FFFFFFFu) << 4) | 0x3u;
}

/* PRI ring "start results" register: connectivity field (bit 0) reads
 * 1 when enumeration passed (pri_ringmaster_start_results_connectivity). */
static inline bool ga10b_cold_pri_start_passed(uint32_t start_results)
{
    return (start_results & 0x1u) == 0x1u;
}

/* NV_FUSE_STATUS_OPT_GPC (BAR0 0x820c1c): bit n set = GPC n is
 * floorswept. Its gpcNclk is then not provided by BPMP (enabling it
 * returns -EINVAL), matching nvgpu's ga10b_tegra_is_clock_available. */
static inline bool ga10b_cold_gpc_present(uint32_t fuse_status_opt_gpc,
                                          unsigned gpc)
{
    return (fuse_status_opt_gpc & (1u << gpc)) == 0u;
}

/* NV_PTOP_DEVICE_INFO2 row 1 of an engine entry: reset_id in bits
 * 7:0 (top_device_info2_dev_reset_id_v). Its PMC_DEVICE_ENABLE bit is
 * register reset_id / 32, bit reset_id % 32. */
static inline uint32_t ga10b_cold_reset_id_mask(uint32_t reset_id)
{
    return 1u << (reset_id % 32u);
}

/* Recovery goes straight to a GR engine reset when an error is latched:
 * an MMU fault (FAULT_STATUS valid, bit 31, or FAULT_INFO valid, bit 31),
 * the channel's ENG_FAULTED (CHRAM bit 5), or a pending GR interrupt or
 * exception. nvgpu resets the engine after every MMU fault; a context
 * with a faulted SM can't be saved by a preempt anyway. */
static inline bool ga10b_cold_recover_needs_engine_reset(uint32_t chram, uint32_t fault_status,
                                                        uint32_t fault_info, uint32_t gr_intr,
                                                        uint32_t gr_exception)
{
    return (chram & (1u << 5)) != 0u || (fault_status & 0x80000000u) != 0u ||
           (fault_info & 0x80000000u) != 0u || gr_intr != 0u || gr_exception != 0u;
}

/* A PRI read that the ring could not route returns a 0xbadfXXXX
 * poison value instead of faulting. */
static inline bool ga10b_cold_is_pri_poison(uint32_t v)
{
    return (v & 0xFFFF0000u) == 0xBADF0000u;
}

#endif /* GPU_NVIDIA_GA10B_COLD_H */
