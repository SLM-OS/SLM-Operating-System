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
    GA10B_COLD_ACR,         /* ACR on the GSP loads FECS/GPCCS into WPR */
    GA10B_COLD_FALCONS,     /* FECS/GPCCS started, ctxsw ucode ready */
    GA10B_COLD_STEP_COUNT,
};

/* Run every step after the last completed one, up to and including
 * `last`. Returns 0 when `last` is reached, or the negative number of
 * the step that failed. */
int ga10b_cold_run(enum ga10b_cold_step last);

/* Highest step completed so far (GA10B_COLD_NONE before any). */
enum ga10b_cold_step ga10b_cold_progress(void);

/* Short name for a step, for logs and the shell. */
const char *ga10b_cold_step_name(enum ga10b_cold_step step);

/* Boot the ACR HS ucode (acr-gsp text/data/manifest) on the GSP
 * RISC-V core (nvgpu nvgpu_acr_bootstrap_hs_ucode_riscv). `desc_phys`
 * is the physical address of the flcn2_acr_desc the ucode reads
 * through MAILBOX0/1. Returns 0 when the BROM accepted the image and
 * ACR halted with MAILBOX0 == 0. Lengths must be multiples of 4. */
int ga10b_cold_gsp_boot_acr_blobs(const uint8_t *code, uint32_t code_len,
                                  const uint8_t *data, uint32_t data_len,
                                  const uint8_t *manifest, uint32_t manifest_len,
                                  uint64_t desc_phys);

/* ---- Pure helpers (host-testable) ---- */

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

/* A PRI read that the ring could not route returns a 0xbadfXXXX
 * poison value instead of faulting. */
static inline bool ga10b_cold_is_pri_poison(uint32_t v)
{
    return (v & 0xFFFF0000u) == 0xBADF0000u;
}

#endif /* GPU_NVIDIA_GA10B_COLD_H */
