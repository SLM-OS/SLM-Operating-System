/*
 * ga10b_pmu.h - Host side of the GA10B LS PMU RTOS bring-up (#1088).
 *
 * On a secure iGPU the GR falcons are "lazy bootstrapped": ACR loads
 * FECS/GPCCS into WPR but leaves starting them to the LS PMU, so
 * after a cold boot SLM-OS must run the PMU RTOS and ask it to
 * bootstrap FECS and GPCCS, exactly as nvgpu does (L4T R36.4.x, with
 * CONFIG_NVGPU_LS_PMU, NVGPU_PMU_NEXT_CORE_ENABLED, PKC signatures,
 * NVGPU_SUPPORT_PMU_RTOS_FBQ and NVGPU_SUPPORT_PMU_SUPER_SURFACE, the
 * last two set by hal_ga10b.c:2034-2038 whenever the LS PMU is
 * supported). The PMU ucode itself is loaded by ACR (it is the
 * non-lazy PMU_NEXT_CORE entry of the LS blob, ga10b_acr_blob.h).
 *
 * nvgpu sequence mirrored here (cross-checked against the MMIO trace
 * of nvgpu's cold power-on on jetson-nano-2, /tmp/mmio.txt lines
 * 11249-11537, raw ftrace in ~/slmos-ref/derivatives/nvgpu-traces/
 * ga10b-r36.4.7-cold-poweron-2026-11-03.trace):
 *
 *  1. nvgpu_pmu_rtos_init (common/pmu/pmu_rtos_init.c:385-500):
 *     super surface allocated in the PMU VM with its own GPU VA in
 *     its header (super_surface.c:41-55); boot args built by
 *     nvgpu_pmu_next_core_rtos_args_setup (:261-306) and their
 *     address + NV_NEXT_CORE_AMAP_EXTMEM2_START written to
 *     PMU MAILBOX0/1 (trace: W 10a040 0761f000, W 10a044 80600001);
 *     RISC-V CPUCTL.STARTCPU (ga10b_falcon_bootstrap, trace W 10b388
 *     1); BROM completion (BR_RETCODE pass, falcon.c
 *     nvgpu_falcon_wait_for_nvriscv_brom_completion) and the priv
 *     lockdown release (HWCFG2 bit 13 clear, pmu_rtos_init.c:
 *     349-371).
 *  2. INIT message: the PMU posts a CMDMGMT/INIT_DGPU message in
 *     the super-surface FB message queue (pmu_msg.c:323-361,
 *     trace: R/W 10a4cc 0 -> 1). It gives the physical queue ids
 *     and the DMEM "sw managed area" that backs the FBQ heap.
 *  3. (Optional, skipped) perfmon init command: perfmon is disabled
 *     on CONFIG_NVGPU_EMB_LINUX_PROD_BUILD (platform_ga10b_tegra.c:
 *     776-786), so a supported nvgpu configuration runs without it.
 *     It was cmd #1 in the trace (W 10a804 1).
 *  4. LSFM (lsfm_sw_gv100.c, used for GA10B by lsfm.c:87-90):
 *     RPC ACR/INIT_WPR_REGION {region 1, offset 0} then RPC
 *     ACR/BOOTSTRAP_GR_FALCONS {mask FECS|GPCCS, flags RESET_YES}
 *     (trace cmds #2/#3: W 10a804 2, 3 and messages 3->4, 4->5).
 *     Both are FBQ commands on the LPQ (pmu_cmd.c:719 posts RPCs on
 *     PMU_COMMAND_QUEUE_LPQ, whose physical queue id is 1 on this
 *     firmware: trace head register 0x10a804).
 *  5. The caller then starts GPCCS and FECS through their CPUCTL
 *     aliases (gr_falcon.c:740-741, trace line 11537 onwards).
 *
 * SLM-OS has no PMU interrupt, so everything nvgpu does in its ISR
 * (gk20a_pmu_isr: IRQSTAT & (RISCV IRQMASK & IRQDEST), clear via
 * IRQSCLR, SWGEN1 = RISC-V debug buffer, SWGEN0 = message queue) is
 * done by ga10b_pmu_poll(), which every wait loop calls.
 *
 * The module only touches hardware through `struct ga10b_pmu_ops`
 * and caller-provided DMA buffers, so it is host-testable with a
 * fake register file (kernel/tests/test_ga10b_pmu.c).
 *
 * Little-endian only; wire structs are copied in host byte order.
 */
#ifndef GPU_NVIDIA_GA10B_PMU_H
#define GPU_NVIDIA_GA10B_PMU_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ---- BAR0 registers (hw_pwr_ga10b.h, hw_falcon_ga10b.h,
 *      hw_priscv_ga10b.h; PMU falcon at 0x10a000, its RISC-V at
 *      pwr_falcon2_pwr_base_r() = 0x10b000) ---- */
#define GA10B_PWR_FALCON_IRQSCLR        0x0010a004u
#define GA10B_PWR_FALCON_IRQSTAT        0x0010a008u
#define GA10B_PWR_FALCON_MAILBOX0       0x0010a040u
#define GA10B_PWR_FALCON_MAILBOX1       0x0010a044u
#define GA10B_PWR_FALCON_HWCFG2         0x0010a0f4u
#define GA10B_PWR_FALCON_HWCFG          0x0010a108u
#define GA10B_PWR_FALCON_DMEMC0         0x0010a1c0u
#define GA10B_PWR_FALCON_DMEMD0         0x0010a1c4u
#define GA10B_PWR_PMU_MSGQ_HEAD         0x0010a4c8u
#define GA10B_PWR_PMU_MSGQ_TAIL         0x0010a4ccu
#define GA10B_PWR_PMU_QUEUE_HEAD(i)     (0x0010a800u + (i) * 4u)
#define GA10B_PWR_PMU_QUEUE_TAIL(i)     (0x0010a820u + (i) * 4u)
#define GA10B_PWR_PMU_QUEUE_COUNT       8u     /* pwr_pmu_queue_head__size_1_v */
#define GA10B_PWR_RISCV_IRQMASK         0x0010b528u
#define GA10B_PWR_RISCV_IRQDEST         0x0010b52cu
#define GA10B_PWR_RISCV_CPUCTL          0x0010b388u
#define GA10B_PWR_RISCV_BR_RETCODE      0x0010b65cu
#define GA10B_PWR_RISCV_BCR_CTRL        0x0010b668u

/* Field values */
#define GA10B_RISCV_CPUCTL_STARTCPU     0x1u        /* priscv_priscv_cpuctl_startcpu_true_f */
#define GA10B_RISCV_CPUCTL_HALTED       (1u << 4)   /* priscv_priscv_cpuctl_halted_v */
#define GA10B_RISCV_BR_RETCODE_MASK     0x3u
#define GA10B_RISCV_BR_RETCODE_PASS     0x3u
#define GA10B_RISCV_BR_RETCODE_FAIL     0x2u
#define GA10B_FALCON_HWCFG2_PRIV_LOCKDOWN (1u << 13) /* riscv_br_priv_lockdown */
#define GA10B_FALCON_IRQ_HALT           (1u << 4)
#define GA10B_FALCON_IRQ_SWGEN0         (1u << 6)   /* message queue */
#define GA10B_FALCON_IRQ_SWGEN1         (1u << 7)   /* RISC-V debug buffer */
#define GA10B_FALCON_DMEMC_AINCR        (1u << 25)  /* falcon_falcon_dmemc_aincr_f(1) */
/* falcon_falcon_hwcfg_dmem_size_v: bits 17:9, in 256-byte blocks */
#define GA10B_FALCON_HWCFG_DMEM_SIZE(v) ((((v) >> 9) & 0x1ffu) << 8)

/* pwr_pmu_new_instblk_* (ctx_bind_addr encoding) */
#define GA10B_PMU_INSTBLK_PTR_MASK      0x0fffffffu
#define GA10B_PMU_INSTBLK_TARGET_SYS_COH  0x20000000u
#define GA10B_PMU_INSTBLK_TARGET_SYS_NCOH 0x30000000u
#define GA10B_PMU_INSTBLK_VALID         (1u << 30)

/* ---- Sizes the caller allocates ---- */
#define GA10B_PMU_BOOT_ARGS_SIZE        184u        /* sizeof(nv_pmu_boot_params) */
#define GA10B_PMU_CORE_DUMP_SIZE        8192u       /* NV_REG_STR_NEXT_CORE_DUMP_SIZE_DEFAULT */
#define GA10B_PMU_TRACE_BUF_SIZE        0x4000u     /* PMU_RTOS_TRACE_BUFSIZE, fw.h:40 */
#define GA10B_PMU_SUPER_SURFACE_SIZE    (1024u * 1024u) /* SUPER_SURFACE_SIZE */
#define GA10B_PMU_INST_BLOCK_SIZE       0x1000u     /* ram_in_alloc_size_v */

/* include/nvgpu/pmu.h:349-357 */
#define GA10B_NEXT_CORE_AMAP_EXTMEM2_START 0x8060000000000000ull
#define GA10B_NEXT_CORE_BOOTLDR_VERSION   1u
#define GA10B_NEXT_CORE_BOOTLDR_BOOT_TYPE_RM \
    (((uint32_t)'N' << 24) | ((uint32_t)'V' << 16) | ((uint32_t)'R' << 8) | (uint32_t)'M')
#define GA10B_PMU_CPU_FREQ_HZ           204000000u  /* pmu_set_cmd_line_args_cpu_freq_v5 */
#define GA10B_PMU_DMAIDX_UCODE          0u          /* GK20A_PMU_DMAIDX_UCODE */
#define GA10B_PMU_DMAIDX_VIRT           1u          /* GK20A_PMU_DMAIDX_VIRT */

/* ---- Super surface layout (super_surface_priv.h:33-99) ----
 *   0x00000 ssmd[32]            (32 x 16 B, filled by the PMU)
 *   0x00200 fbq.cmd_queues      (2 queues x 16 elements x 2048 B)
 *   0x10200 fbq.msg_queue       (16 elements x 64 B)
 *   0x10600 hdr                 (super_surface_hdr, padded to 16 B)
 *   0x10610 unmapped members    (rest of the 1 MiB) */
#define GA10B_SS_SSMD_OFFSET            0x00000u
#define GA10B_SS_FBQ_CMD_OFFSET         0x00200u
#define GA10B_SS_FBQ_MSG_OFFSET         0x10200u
#define GA10B_SS_HDR_OFFSET             0x10600u
#define GA10B_SS_HDR_ADDR_LO_OFFSET     (GA10B_SS_HDR_OFFSET + 0u)
#define GA10B_SS_HDR_ADDR_HI_OFFSET     (GA10B_SS_HDR_OFFSET + 4u)
/* nvgpu_cmdif.h:62-80 */
#define GA10B_FBQ_CMD_ELEMENT_SIZE      2048u
#define GA10B_FBQ_CMD_NUM_ELEMENTS      16u
#define GA10B_FBQ_CMD_COUNT             2u
#define GA10B_FBQ_MSG_ELEMENT_SIZE      64u
#define GA10B_FBQ_MSG_NUM_ELEMENTS      16u
#define GA10B_SS_CMD_QUEUE_OFFSET(id) \
    (GA10B_SS_FBQ_CMD_OFFSET + (id) * GA10B_FBQ_CMD_NUM_ELEMENTS * GA10B_FBQ_CMD_ELEMENT_SIZE)
#define GA10B_SS_MSG_ELEMENT_OFFSET(i) \
    (GA10B_SS_FBQ_MSG_OFFSET + (i) * GA10B_FBQ_MSG_ELEMENT_SIZE)

/* ---- Logical queue ids (pmuif/cmn.h:33-38) and init-message
 *      indexes (pmuif/init.h:104-110) ---- */
#define GA10B_PMU_COMMAND_QUEUE_HPQ     0u
#define GA10B_PMU_COMMAND_QUEUE_LPQ     1u
#define GA10B_PMU_MESSAGE_QUEUE         4u
#define GA10B_PMU_QUEUE_COUNT_FOR_V5    4u
#define GA10B_PMU_QUEUE_HPQ_IDX_FOR_V3  0u
#define GA10B_PMU_QUEUE_LPQ_IDX_FOR_V3  1u
#define GA10B_PMU_QUEUE_MSG_IDX_FOR_V5  3u

/* ---- Units, flags, RPC ids ---- */
#define GA10B_PMU_UNIT_REWIND           0x00u
#define GA10B_PMU_UNIT_CMDMGMT          0x01u
#define GA10B_PMU_UNIT_INIT             0x07u
#define GA10B_PMU_UNIT_ACR              0x0Au
#define GA10B_PMU_UNIT_INIT_DGPU        0x0Fu
#define GA10B_PMU_UNIT_RC               0x1Fu
#define GA10B_PMU_UNIT_END              0x23u
#define GA10B_PMU_UNIT_TEST_START       0xFEu
#define GA10B_PMU_CMD_FLAGS_STATUS      0x01u
#define GA10B_PMU_CMD_FLAGS_INTR        0x02u
#define GA10B_PMU_CMD_FLAGS_EVENT       0x04u
#define GA10B_PMU_CMD_FLAGS_RPC_EVENT   0x08u
#define GA10B_PMU_CMD_FLAGS_PMU_MASK    0xF0u
#define GA10B_PMU_RPC_CMD_ID            0x80u       /* NV_PMU_RPC_CMD_ID */
#define GA10B_PMU_RPC_MSG_ID            0x80u       /* NV_PMU_RPC_MSG_ID */
#define GA10B_PMU_INIT_MSG_TYPE_PMU_INIT 0u
#define GA10B_PMU_RC_MSG_TYPE_UNHANDLED_CMD 0u
#define GA10B_PMU_RPC_ID_ACR_INIT_WPR_REGION      0x00u
#define GA10B_PMU_RPC_ID_ACR_BOOTSTRAP_GR_FALCONS 0x03u
#define GA10B_PMU_ACR_BOOTSTRAP_FLAGS_RESET_YES   0u
#define GA10B_PMU_DMEM_ALLOC_ALIGNMENT  32u         /* flcnif_cmn.h:121 */

/* ---- Wire structures (exact nvgpu layouts) ---- */

/* struct falc_u64 / flcn_mem_desc_v0, flcnif_cmn.h:31-34, 71-74 */
struct ga10b_pmu_falc_u64 { uint32_t lo; uint32_t hi; };
struct ga10b_pmu_flcn_mem_desc {
    struct ga10b_pmu_falc_u64 address;
    uint32_t params;            /* size[23:0] | dma_idx << 24 */
};

/* struct nv_next_core_bootldr_params / _rtos_params / _boot_params,
 * include/nvgpu/pmu.h:328-374 */
struct ga10b_next_core_bootldr_params {
    uint32_t boot_type;
    uint16_t size;
    uint8_t  version;
};
struct ga10b_next_core_rtos_params {
    uint64_t core_dump_phys;
    uint32_t core_dump_size;
};
struct ga10b_next_core_boot_params {
    struct ga10b_next_core_bootldr_params bl;
    struct ga10b_next_core_rtos_params rtos;
    uint32_t dummy[24];
};

/* struct pmu_cmdline_args_v7, pmuif/init.h:74-83 */
struct ga10b_pmu_cmdline_args_v7 {
    uint32_t cpu_freq_hz;
    struct ga10b_pmu_flcn_mem_desc trace_buf;
    uint8_t  raise_priv_sec;
    struct ga10b_pmu_flcn_mem_desc gc6_ctx;
    struct ga10b_pmu_flcn_mem_desc gc6_bsod_ctx;
    struct ga10b_pmu_flcn_mem_desc super_surface;
    uint32_t flags;
    uint32_t ctx_bind_addr;
};

/* struct nv_pmu_boot_params, include/nvgpu/pmu.h:376-379 */
struct ga10b_nv_pmu_boot_params {
    struct ga10b_next_core_boot_params boot_params;
    struct ga10b_pmu_cmdline_args_v7 cmd_line_args;
};

/* struct pmu_hdr, flcnif_cmn.h:106-111 */
struct ga10b_pmu_hdr {
    uint8_t unit_id;
    uint8_t size;
    uint8_t ctrl_flags;
    uint8_t seq_id;
};

/* struct nv_pmu_rpc_cmd / nv_pmu_rpc_msg, pmuif/rpc.h:28-63 */
struct ga10b_pmu_rpc_cmd {
    uint8_t  cmd_type;
    uint8_t  flags;
    uint16_t rpc_dmem_size;
    uint32_t rpc_dmem_ptr;
};
struct ga10b_pmu_rpc_msg {
    uint8_t  msg_type;
    uint8_t  flags;
    uint16_t rpc_dmem_size;
    uint32_t rpc_dmem_ptr;
};

/* struct pmu_cmd with the rpc member of its union (cmd.h:40-51) */
struct ga10b_pmu_rpc_cmd_full {
    struct ga10b_pmu_hdr hdr;
    struct ga10b_pmu_rpc_cmd rpc;
};

/* struct nv_falcon_fbq_hdr / nv_falcon_fbq_msgq_hdr, flcnif_cmn.h:137-154 */
struct ga10b_falcon_fbq_hdr {
    uint8_t  element_index;
    uint8_t  padding[3];
    uint16_t heap_size;
    uint16_t heap_offset;
};
struct ga10b_falcon_fbq_msgq_hdr {
    uint16_t sequence_number;
    uint16_t checksum;
};

/* struct nv_pmu_rpc_header, pmuif/cmn.h:96-109 */
struct ga10b_pmu_rpc_header {
    uint8_t  unit_id;
    uint8_t  function;
    uint8_t  flags;
    uint8_t  flcn_status;
    uint32_t exec_time_nv_ns;
    uint32_t exec_time_pmu_ns;
};

/* pmuif/acr.h:127-160 */
struct ga10b_pmu_rpc_acr_init_wpr_region {
    struct ga10b_pmu_rpc_header hdr;
    uint32_t wpr_region_id;
    uint32_t wpr_offset;
    uint32_t scratch[1];
};
struct ga10b_pmu_rpc_acr_bootstrap_gr_falcons {
    struct ga10b_pmu_rpc_header hdr;
    uint32_t falcon_id_mask;
    uint32_t flags;
    uint32_t falcon_va_mask;
    struct ga10b_pmu_falc_u64 wpr_base_virtual;
    uint32_t scratch[1];
};

/* struct pmu_nvgpu_rpc_header + pmu_nvgpu_rpc_struct_cmdmgmt_init,
 * pmuif/cmn.h:112-119, pmuif/init.h:129-159 */
struct ga10b_pmu_nvgpu_rpc_header {
    uint8_t unit_id;
    uint8_t function;
    struct ga10b_pmu_falc_u64 rpc_transfer_time;
};
struct ga10b_pmu_init_msg {
    struct ga10b_pmu_nvgpu_rpc_header hdr;
    uint8_t  status;
    uint8_t  queue_phy_id[GA10B_PMU_QUEUE_COUNT_FOR_V5];
    uint16_t queue_size[GA10B_PMU_QUEUE_COUNT_FOR_V5];
    uint16_t queues_start;
    uint16_t sw_managed_area_offset;
    uint16_t sw_managed_area_size;
    uint16_t os_debug_entry_point;
    uint8_t  brss_data[24];
};

/* struct nvgpu_falcon_dbg_buf_metadata, common/falcon/falcon_debug.h:27-40 */
struct ga10b_falcon_dbg_buf_metadata {
    uint32_t read_offset;
    uint32_t write_offset;
    uint32_t buffer_size;
    uint32_t magic;
};
#define GA10B_NVRISCV_DEBUG_BUFFER_MAGIC 0xf007ba11u
#define GA10B_RISCV_DMESG_BUFFER_SIZE    0x1000u    /* NV_RISCV_DMESG_BUFFER_SIZE */
#define GA10B_RISCV_DEBUG_BUFFER_QUEUE   7u         /* NV_RISCV_DEBUG_BUFFER_QUEUE */

static_assert(sizeof(struct ga10b_pmu_flcn_mem_desc) == 12, "flcn_mem_desc_v0");
static_assert(sizeof(struct ga10b_next_core_bootldr_params) == 8, "bootldr_params");
static_assert(sizeof(struct ga10b_next_core_rtos_params) == 16, "rtos_params");
static_assert(offsetof(struct ga10b_next_core_boot_params, rtos) == 8, "boot_params.rtos");
static_assert(offsetof(struct ga10b_next_core_boot_params, dummy) == 24, "boot_params.dummy");
static_assert(sizeof(struct ga10b_next_core_boot_params) == 120, "next_core_boot_params");
static_assert(offsetof(struct ga10b_pmu_cmdline_args_v7, trace_buf) == 4, "args.trace_buf");
static_assert(offsetof(struct ga10b_pmu_cmdline_args_v7, raise_priv_sec) == 16, "args.raise_priv_sec");
static_assert(offsetof(struct ga10b_pmu_cmdline_args_v7, gc6_ctx) == 20, "args.gc6_ctx");
static_assert(offsetof(struct ga10b_pmu_cmdline_args_v7, super_surface) == 44, "args.super_surface");
static_assert(offsetof(struct ga10b_pmu_cmdline_args_v7, flags) == 56, "args.flags");
static_assert(offsetof(struct ga10b_pmu_cmdline_args_v7, ctx_bind_addr) == 60, "args.ctx_bind_addr");
static_assert(sizeof(struct ga10b_pmu_cmdline_args_v7) == 64, "pmu_cmdline_args_v7");
static_assert(offsetof(struct ga10b_nv_pmu_boot_params, cmd_line_args) == 120, "boot.cmd_line_args");
static_assert(sizeof(struct ga10b_nv_pmu_boot_params) == GA10B_PMU_BOOT_ARGS_SIZE, "nv_pmu_boot_params");
static_assert(sizeof(struct ga10b_pmu_hdr) == 4, "pmu_hdr");
static_assert(sizeof(struct ga10b_pmu_rpc_cmd) == 8, "nv_pmu_rpc_cmd");
static_assert(sizeof(struct ga10b_pmu_rpc_msg) == 8, "nv_pmu_rpc_msg");
static_assert(sizeof(struct ga10b_pmu_rpc_cmd_full) == 12, "pmu_cmd (rpc)");
static_assert(sizeof(struct ga10b_falcon_fbq_hdr) == 8, "nv_falcon_fbq_hdr");
static_assert(offsetof(struct ga10b_falcon_fbq_hdr, heap_offset) == 6, "fbq_hdr.heap_offset");
static_assert(sizeof(struct ga10b_falcon_fbq_msgq_hdr) == 4, "nv_falcon_fbq_msgq_hdr");
static_assert(sizeof(struct ga10b_pmu_rpc_header) == 12, "nv_pmu_rpc_header");
static_assert(offsetof(struct ga10b_pmu_rpc_header, flcn_status) == 3, "rpc_header.flcn_status");
static_assert(sizeof(struct ga10b_pmu_rpc_acr_init_wpr_region) == 24, "acr_init_wpr_region");
static_assert(offsetof(struct ga10b_pmu_rpc_acr_bootstrap_gr_falcons, wpr_base_virtual) == 24,
              "bootstrap_gr_falcons.wpr_base_virtual");
static_assert(sizeof(struct ga10b_pmu_rpc_acr_bootstrap_gr_falcons) == 36,
              "acr_bootstrap_gr_falcons");
static_assert(sizeof(struct ga10b_pmu_nvgpu_rpc_header) == 12, "pmu_nvgpu_rpc_header");
static_assert(offsetof(struct ga10b_pmu_init_msg, status) == 12, "init.status");
static_assert(offsetof(struct ga10b_pmu_init_msg, queue_phy_id) == 13, "init.queue_phy_id");
static_assert(offsetof(struct ga10b_pmu_init_msg, queue_size) == 18, "init.queue_size");
static_assert(offsetof(struct ga10b_pmu_init_msg, sw_managed_area_offset) == 28, "init.sw_off");
static_assert(offsetof(struct ga10b_pmu_init_msg, sw_managed_area_size) == 30, "init.sw_size");
static_assert(offsetof(struct ga10b_pmu_init_msg, os_debug_entry_point) == 32, "init.os_debug");
static_assert(sizeof(struct ga10b_pmu_init_msg) == 60, "cmdmgmt_init");
static_assert(sizeof(struct ga10b_falcon_dbg_buf_metadata) == 16, "dbg_buf_metadata");
/* The FB queue layout the offsets above encode. */
static_assert(GA10B_SS_FBQ_MSG_OFFSET == GA10B_SS_FBQ_CMD_OFFSET +
              GA10B_FBQ_CMD_COUNT * GA10B_FBQ_CMD_NUM_ELEMENTS * GA10B_FBQ_CMD_ELEMENT_SIZE,
              "ss msg queue offset");
static_assert(GA10B_SS_HDR_OFFSET == GA10B_SS_FBQ_MSG_OFFSET +
              GA10B_FBQ_MSG_NUM_ELEMENTS * GA10B_FBQ_MSG_ELEMENT_SIZE, "ss hdr offset");

/* ---- Host interface ---- */

struct ga10b_pmu_ops {
    void *ctx;
    uint32_t (*rd32)(void *ctx, uint32_t reg);           /* BAR0 */
    void (*wr32)(void *ctx, uint32_t reg, uint32_t val);  /* BAR0 */
    void (*delay_us)(void *ctx, uint32_t us);
    /* CPU writes to a DMA buffer -> visible to the GPU (DC CVAC + DSB). */
    void (*dma_clean)(void *ctx, const void *p, size_t n);
    /* GPU writes to a DMA buffer -> visible to the CPU (DC CIVAC + DSB). */
    void (*dma_inval)(void *ctx, const void *p, size_t n);
    /* Optional: one line of the PMU's RISC-V debug buffer. */
    void (*log_line)(void *ctx, const char *line);
};

/* A caller-owned DMA buffer. `addr` is the address the GPU uses for
 * physical (DMAIDX_UCODE / EXTMEM2) access, i.e. what nvgpu_mem_get_addr
 * returns for a PHYSICALLY_ADDRESSED allocation. `gpu_va` is its
 * mapping in the PMU VM (only for the super surface and trace buffer,
 * which nvgpu maps with nvgpu_dma_alloc_map(pmu.vm, ...)). */
struct ga10b_pmu_buf {
    void *cpu;
    uint64_t addr;
    uint64_t gpu_va;
    size_t size;
};

struct ga10b_pmu_config {
    struct ga10b_pmu_buf boot_args;     /* >= GA10B_PMU_BOOT_ARGS_SIZE, cpu + addr */
    struct ga10b_pmu_buf core_dump;     /* >= GA10B_PMU_CORE_DUMP_SIZE, addr */
    struct ga10b_pmu_buf super_surface; /* >= GA10B_PMU_SUPER_SURFACE_SIZE, cpu + gpu_va */
    struct ga10b_pmu_buf trace_buf;     /* >= GA10B_PMU_TRACE_BUF_SIZE, gpu_va */
    uint64_t inst_block_addr;           /* PMU VM instance block, 4 KiB aligned */
    bool sysmem_coherent;               /* false on Orin: nvgpu uses SYS_NCOH */
};

#define GA10B_PMU_OK            0
#define GA10B_PMU_ERR_ARG      (-1)
#define GA10B_PMU_ERR_TIMEOUT  (-2)
#define GA10B_PMU_ERR_BROM     (-3)   /* BR_RETCODE reported FAIL */
#define GA10B_PMU_ERR_PROTO    (-4)   /* malformed / unexpected message */
#define GA10B_PMU_ERR_NOSPACE  (-5)   /* FBQ heap or queue full */
#define GA10B_PMU_ERR_RPC      (-6)   /* RPC flcn_status != 0 or unhandled cmd */
#define GA10B_PMU_ERR_STATE    (-7)   /* wrong state (not booted / not ready) */
#define GA10B_PMU_ERR_HALT     (-8)   /* PMU raised its HALT interrupt */

#define GA10B_PMU_MAX_SEQ       256u
#define GA10B_PMU_HEAP_MAX_BLKS 2048u  /* 64 KiB DMEM / 32 B */

/* Driver state. Large (~10.5 KiB: sequences, debug and work buffers): keep it in
 * static storage. All fields are private except the diagnostics. */
struct ga10b_pmu {
    const struct ga10b_pmu_ops *ops;
    struct ga10b_pmu_config cfg;
    bool booted;
    bool fw_ready;
    bool halted;

    /* From the INIT message. */
    struct ga10b_pmu_init_msg init;
    uint8_t init_unit_id;
    uint32_t cmdq_index;        /* physical queue of the LPQ */

    /* FB command queue (engine_fb_queue.c) */
    uint32_t cmdq_tail;         /* fbq.tail, software-only */
    uint32_t cmdq_in_use;       /* element_in_use bitmap */

    /* DMEM heap (BITMAP_ALLOCATOR, 32 B blocks, next-fit) */
    uint32_t heap_base;
    uint32_t heap_blks;
    uint32_t heap_next_blk;
    uint32_t heap_bitmap[GA10B_PMU_HEAP_MAX_BLKS / 32u];

    /* Sequences (pmu_seq.c). One slot per id: in-flight bookkeeping. */
    uint32_t seq_tbl[GA10B_PMU_MAX_SEQ / 32u];
    struct {
        uint8_t  state;         /* 0 free, 1 pending (posted), 2 completed */
        uint8_t  function;
        uint8_t  flcn_status;
        uint8_t  element;
        uint16_t heap_offset;
        uint16_t heap_size;
        uint16_t rpc_size;
        int      result;
    } seq[GA10B_PMU_MAX_SEQ];

    /* LSFM state (lsfm_sw_gv100.c) */
    bool wpr_init_done;
    bool falcons_loaded;

    /* RISC-V debug buffer (falcon_debug.c) */
    bool dbg_meta_ok;
    uint32_t dbg_dmem_offset;
    uint32_t dbg_buffer_size;
    uint32_t dbg_read_offset;
    char dbg_buf[GA10B_RISCV_DMESG_BUFFER_SIZE + 1u];

    uint8_t work[GA10B_FBQ_CMD_ELEMENT_SIZE];

    /* Diagnostics */
    uint32_t msgs_seen;
    uint32_t events_seen;
    uint8_t  last_event_unit;
    uint32_t last_irqstat;
    uint32_t brom_retcode;
    int      last_error;
};

/* ---- Pure helpers ---- */

/* ga10b_pmu_get_inst_block_config (hal/pmu/pmu_ga10b.c:157-168):
 * ptr = addr >> 12 | VALID | target (sys_coh / sys_ncoh). */
uint32_t ga10b_pmu_inst_block_config(uint64_t inst_block_addr, bool coherent);

/* Fill a 4 KiB PMU-VM instance block as nvgpu_init_system_vm does
 * (common/mm/mm.c:193-246 -> gv11b_mm_init_inst_block_core,
 * hal/mm/mm_gv11b_fusa.c:46-87): the block is zeroed (nvgpu's DMA
 * allocation is), the PDB words written by ga10b_ramin_init_pdb
 * (hal/fifo/ramin_ga10b_fusa.c), big page size 64 KiB, all 64
 * subcontext PDB slots invalid except VEID 0 = the same PDB
 * (gv11b_ramin_set_subctx_pdb_info), and the valid mask = VEID 0.
 * `pdb_addr` must be 4 KiB aligned. */
void ga10b_pmu_build_inst_block(uint32_t inst[GA10B_PMU_INST_BLOCK_SIZE / 4u],
                                uint64_t pdb_addr, bool coherent);

/* nvgpu_pmu_next_core_rtos_args_setup (pmu_rtos_init.c:261-306) and
 * nvgpu_pmu_rtos_cmdline_args_init (:239-258) with the v5/v7 fw ops
 * of APP_VERSION_NVGPU_NEXT_CORE (fw_ver_ops.c:135-185, 1380-1475).
 * Unset fields are zero (nvgpu's args live in a kzalloc'd union; the
 * stack boot_params padding/dummy is uninitialised there, zero here). */
void ga10b_pmu_build_boot_params(struct ga10b_nv_pmu_boot_params *bp,
                                 const struct ga10b_pmu_config *cfg);

/* The value nvgpu writes to MAILBOX0/1: boot-args address +
 * NV_NEXT_CORE_AMAP_EXTMEM2_START. */
static inline uint64_t ga10b_pmu_mailbox_addr(uint64_t boot_args_addr)
{
    return GA10B_NEXT_CORE_AMAP_EXTMEM2_START + boot_args_addr;
}

/* ---- Bring-up ---- */

/* Bind ops + config and reset the software state. Does not touch
 * hardware. Returns GA10B_PMU_ERR_ARG for missing ops/buffers or
 * short / unaligned buffers. */
int ga10b_pmu_init(struct ga10b_pmu *pmu, const struct ga10b_pmu_ops *ops,
                   const struct ga10b_pmu_config *cfg);

/* Prepare the super surface + boot args in memory, write MAILBOX0/1,
 * start the RISC-V core, wait for the BROM (100 ms, 5 ms steps) and
 * the priv-lockdown release (3000 ms). Requires that ACR has already
 * loaded the PMU (LS blob with include_pmu). */
int ga10b_pmu_boot(struct ga10b_pmu *pmu);

/* Service the PMU once, as nvgpu's ISR would: IRQSTAT & mask, clear,
 * drain the debug buffer on SWGEN1, process every queued message
 * (the INIT message first). Returns 0 or a negative error. */
int ga10b_pmu_poll(struct ga10b_pmu *pmu);

/* Poll until the INIT message has been processed. */
int ga10b_pmu_wait_init(struct ga10b_pmu *pmu, uint32_t timeout_ms);

/* nvgpu_pmu_rpc_execute (pmu_cmd.c:670-760) without copy-back: post
 * `rpc` (whose first member is a ga10b_pmu_rpc_header with unit_id
 * and function set; size_rpc excludes the trailing scratch) on the
 * LPQ. Returns the sequence id (>= 0) or a negative error. */
int ga10b_pmu_rpc_post(struct ga10b_pmu *pmu, const void *rpc, uint16_t size_rpc,
                       uint16_t size_scratch);

/* Poll until sequence `seq` has completed; copies the RPC structure
 * the PMU wrote back (size_rpc bytes from the FBQ element) into
 * `out` when non-null and returns 0, GA10B_PMU_ERR_RPC when its
 * flcn_status is non-zero (or the PMU replied "unhandled cmd"), or
 * GA10B_PMU_ERR_TIMEOUT. */
int ga10b_pmu_rpc_wait(struct ga10b_pmu *pmu, int seq, void *out, uint32_t timeout_ms);

/* lsfm_int_wpr_region + gv100_pmu_lsfm_init_acr_wpr_region: RPC
 * ACR/INIT_WPR_REGION {wpr_regionId 1, wpr_offset 0}, 2 ms settle,
 * wait for the reply. */
int ga10b_pmu_lsfm_init_wpr_region(struct ga10b_pmu *pmu, uint32_t timeout_ms);

/* gv100_pmu_lsfm_bootstrap_ls_falcon (non-MIG): RPC
 * ACR/BOOTSTRAP_GR_FALCONS {falcon_id_mask, RESET_YES}, 5 ms
 * settle, wait for the reply. `mask` must be a non-empty subset of
 * BIT(FECS=2) | BIT(GPCCS=3). */
int ga10b_pmu_lsfm_bootstrap_gr_falcons(struct ga10b_pmu *pmu, uint32_t falcon_id_mask,
                                        uint32_t timeout_ms);

/* gr_falcon_sec2_or_ls_pmu_coldboot_bootstrap -> nvgpu_pmu_lsfm_
 * bootstrap_ls_falcon (lsfm.c:113-150): wait for INIT if needed,
 * INIT_WPR_REGION once, then BOOTSTRAP_GR_FALCONS for FECS|GPCCS.
 * Afterwards the caller starts GPCCS then FECS (CPUCTL aliases). */
int ga10b_pmu_bootstrap_ctxsw_falcons(struct ga10b_pmu *pmu, uint32_t timeout_ms);

/* Drain the RISC-V debug buffer now (also done on SWGEN1). */
int ga10b_pmu_dbg_drain(struct ga10b_pmu *pmu);

#endif /* GPU_NVIDIA_GA10B_PMU_H */
