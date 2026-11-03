/*
 * ga10b_pmu.c - Host side of the GA10B LS PMU RTOS bring-up (#1088).
 *
 * Port of the nvgpu paths listed in ga10b_pmu.h (L4T R36.4.x). Each
 * function names the nvgpu function it mirrors; register access
 * order matches the jetson-nano-2 cold power-on trace.
 */

#include "ga10b_pmu.h"

#include "string.h"

#if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__)
#error "ga10b_pmu: wire structs are written in host order (LE only)"
#endif

/* Timeouts and steps. */
#define BROM_TIMEOUT_MS         100u   /* NVRISCV_BR_COMPLETION_TIMEOUT_SILICON_MS */
#define BROM_POLL_MS            5u     /* NVRISCV_BR_COMPLETION_POLLING_TIME_INTERVAL_MS */
#define LOCKDOWN_TIMEOUT_MS     3000u  /* nvgpu_get_poll_timeout (NVGPU_DEFAULT_POLL_TIMEOUT_MS) */
#define LOCKDOWN_POLL_US        1u     /* PMU_PRIV_LOCKDOWN_RELEASE_POLLING_US */
#define WAIT_POLL_US            100u   /* our poll step in the ack waits */
#define WPR_REGION_SETTLE_MS    2u     /* lsfm_sw_gv100.c:57-61 (silicon) */
#define BOOTSTRAP_SETTLE_MS     5u     /* lsfm_sw_gv100.c:103-107 (silicon) */

#define SEQ_FREE      0u
#define SEQ_PENDING   1u
#define SEQ_DONE      2u

static uint32_t rd(struct ga10b_pmu *pmu, uint32_t reg)
{
    return pmu->ops->rd32(pmu->ops->ctx, reg);
}

static void wr(struct ga10b_pmu *pmu, uint32_t reg, uint32_t val)
{
    pmu->ops->wr32(pmu->ops->ctx, reg, val);
}

static void delay_us(struct ga10b_pmu *pmu, uint32_t us)
{
    pmu->ops->delay_us(pmu->ops->ctx, us);
}

static void dma_clean(struct ga10b_pmu *pmu, const void *p, size_t n)
{
    if (pmu->ops->dma_clean != nullptr) {
        pmu->ops->dma_clean(pmu->ops->ctx, p, n);
    }
}

static void dma_inval(struct ga10b_pmu *pmu, const void *p, size_t n)
{
    if (pmu->ops->dma_inval != nullptr) {
        pmu->ops->dma_inval(pmu->ops->ctx, p, n);
    }
}

static uint8_t *ss(struct ga10b_pmu *pmu)
{
    return (uint8_t *)pmu->cfg.super_surface.cpu;
}

static int fail(struct ga10b_pmu *pmu, int err)
{
    pmu->last_error = err;
    return err;
}

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

uint32_t ga10b_pmu_inst_block_config(uint64_t inst_block_addr, bool coherent)
{
    /* nvgpu_inst_block_ptr: addr >> ram_in_base_shift_v() (12). */
    uint32_t ptr = (uint32_t)(inst_block_addr >> 12);
    return (ptr & GA10B_PMU_INSTBLK_PTR_MASK) | GA10B_PMU_INSTBLK_VALID |
           (coherent ? GA10B_PMU_INSTBLK_TARGET_SYS_COH : GA10B_PMU_INSTBLK_TARGET_SYS_NCOH);
}

/* hw_ram_ga10b.h:62-116 (+ ram_in_sc_page_dir_base_target_invalid_v
 * = 1 from the gv11b header ramin_gv11b_fusa.c includes). */
#define RAM_IN_PDB_LO_W              128u
#define RAM_IN_PDB_HI_W              129u
#define RAM_IN_TARGET_SYS_COH        0x2u
#define RAM_IN_TARGET_SYS_NCOH       0x3u
#define RAM_IN_PDB_VOL_TRUE          0x4u
#define RAM_IN_USE_VER2_PT_FORMAT    0x400u
#define RAM_IN_BIG_PAGE_SIZE_64KB    0x800u
#define RAM_IN_SC_PDB_VALID_LONG_W   166u
#define RAM_IN_SC_PDB_BASE_W         168u
#define RAM_IN_SC_TARGET_INVALID     0x1u
#define RAM_IN_SC_VOL_TRUE           (1u << 2)
#define RAM_IN_SC_VER2               (1u << 10)
#define RAM_IN_SC_BIG_PAGE           (1u << 11)
#define GA10B_MAX_SUBCTX             64u    /* gr_pri_fe_chip_def_info_max_veid_count_init_v */

void ga10b_pmu_build_inst_block(uint32_t inst[GA10B_PMU_INST_BLOCK_SIZE / 4u],
                                uint64_t pdb_addr, bool coherent)
{
    if (inst == nullptr) {
        return;
    }
    memset(inst, 0, GA10B_PMU_INST_BLOCK_SIZE);

    uint32_t lo = (uint32_t)(pdb_addr >> 12);          /* u64_lo32(pdb >> base_shift) */
    uint32_t hi = (uint32_t)(pdb_addr >> 32);
    uint32_t target = coherent ? RAM_IN_TARGET_SYS_COH : RAM_IN_TARGET_SYS_NCOH;

    /* ga10b_ramin_init_pdb; gm20b_ramin_set_big_page_size(64K) then
     * re-sets the already-set 64 KiB bit. */
    inst[RAM_IN_PDB_LO_W] = target | RAM_IN_PDB_VOL_TRUE | RAM_IN_BIG_PAGE_SIZE_64KB |
                            ((lo & 0xfffffu) << 12) | RAM_IN_USE_VER2_PT_FORMAT;
    inst[RAM_IN_PDB_HI_W] = hi;

    /* gv11b_ramin_init_subctx_pdb_map + set_subctx_pdb_info(VEID0)
     * + init_subctx_pdb: 4 words per VEID; words 2/3 stay zero
     * (kzalloc'd map). */
    for (uint32_t i = 0; i < GA10B_MAX_SUBCTX; i++) {
        inst[RAM_IN_SC_PDB_BASE_W + 4u * i + 0u] = RAM_IN_SC_TARGET_INVALID;
        inst[RAM_IN_SC_PDB_BASE_W + 4u * i + 1u] = 0u;
    }
    inst[RAM_IN_SC_PDB_BASE_W + 0u] = target | RAM_IN_SC_VOL_TRUE | RAM_IN_SC_VER2 |
                                      RAM_IN_SC_BIG_PAGE | ((lo & 0xfffffu) << 12);
    inst[RAM_IN_SC_PDB_BASE_W + 1u] = hi;

    /* gv11b_ramin_init_subctx_valid_mask: only VEID 0. */
    inst[RAM_IN_SC_PDB_VALID_LONG_W + 0u] = 0x1u;
    inst[RAM_IN_SC_PDB_VALID_LONG_W + 1u] = 0x0u;
}

/* nvgpu_pmu_allocator_surface_describe (common/pmu/allocator.c) */
static void surface_describe(struct ga10b_pmu_flcn_mem_desc *d, uint64_t gpu_va, size_t size)
{
    d->address.lo = (uint32_t)gpu_va;
    d->address.hi = (uint32_t)(gpu_va >> 32);
    d->params = ((uint32_t)size & 0xFFFFFFu) | (GA10B_PMU_DMAIDX_VIRT << 24);
}

void ga10b_pmu_build_boot_params(struct ga10b_nv_pmu_boot_params *bp,
                                 const struct ga10b_pmu_config *cfg)
{
    if (bp == nullptr || cfg == nullptr) {
        return;
    }
    memset(bp, 0, sizeof(*bp));

    /* nvgpu_pmu_rtos_cmdline_args_init: trace size/idx are no-ops in
     * v5 ("set by surface describe"); trace_dma_base describes
     * pmu->trace_buf (mem->size = PMU_RTOS_TRACE_BUFSIZE); cpu_freq_v5
     * ignores its argument and stores 204 MHz; super surface v7
     * describes the super surface (mem->size = sizeof(struct
     * super_surface) = 1 MiB). raise_priv_sec (v5 secure_mode) is
     * only set by the non-next-core lsfm path, so it stays 0. */
    struct ga10b_pmu_cmdline_args_v7 *a = &bp->cmd_line_args;
    a->cpu_freq_hz = GA10B_PMU_CPU_FREQ_HZ;
    surface_describe(&a->trace_buf, cfg->trace_buf.gpu_va, GA10B_PMU_TRACE_BUF_SIZE);
    surface_describe(&a->super_surface, cfg->super_surface.gpu_va, GA10B_PMU_SUPER_SURFACE_SIZE);
    /* cmd_line_args->ctx_bind_addr = g->ops.pmu.get_inst_block_config(g) */
    a->ctx_bind_addr = ga10b_pmu_inst_block_config(cfg->inst_block_addr, cfg->sysmem_coherent);

    /* rtos params: core dump buffer physical address and size */
    bp->boot_params.rtos.core_dump_phys = cfg->core_dump.addr;
    bp->boot_params.rtos.core_dump_size = GA10B_PMU_CORE_DUMP_SIZE;

    /* boot loader params */
    bp->boot_params.bl.boot_type = GA10B_NEXT_CORE_BOOTLDR_BOOT_TYPE_RM;
    bp->boot_params.bl.size = (uint16_t)sizeof(struct ga10b_nv_pmu_boot_params);
    bp->boot_params.bl.version = GA10B_NEXT_CORE_BOOTLDR_VERSION;
}

/* ------------------------------------------------------------------ */
/* Init / boot                                                         */
/* ------------------------------------------------------------------ */

int ga10b_pmu_init(struct ga10b_pmu *pmu, const struct ga10b_pmu_ops *ops,
                   const struct ga10b_pmu_config *cfg)
{
    if (pmu == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    memset(pmu, 0, sizeof(*pmu));
    if (ops == nullptr || cfg == nullptr || ops->rd32 == nullptr || ops->wr32 == nullptr ||
        ops->delay_us == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    if (cfg->boot_args.cpu == nullptr || cfg->boot_args.size < GA10B_PMU_BOOT_ARGS_SIZE ||
        cfg->boot_args.addr == 0u ||
        cfg->core_dump.addr == 0u || cfg->core_dump.size < GA10B_PMU_CORE_DUMP_SIZE ||
        cfg->super_surface.cpu == nullptr ||
        cfg->super_surface.size < GA10B_PMU_SUPER_SURFACE_SIZE ||
        cfg->super_surface.gpu_va == 0u ||
        cfg->trace_buf.gpu_va == 0u || cfg->trace_buf.size < GA10B_PMU_TRACE_BUF_SIZE ||
        cfg->inst_block_addr == 0u || (cfg->inst_block_addr & 0xfffu) != 0u ||
        (cfg->inst_block_addr >> 12) > GA10B_PMU_INSTBLK_PTR_MASK) {
        return GA10B_PMU_ERR_ARG;
    }
    pmu->ops = ops;
    pmu->cfg = *cfg;
    return GA10B_PMU_OK;
}

/* nvgpu_falcon_wait_for_nvriscv_brom_completion (falcon.c) +
 * ga10b_falcon_dump_brom_stats. */
static int wait_brom(struct ga10b_pmu *pmu)
{
    uint32_t waited_ms = 0;
    int err = GA10B_PMU_OK;
    for (;;) {
        uint32_t r = rd(pmu, GA10B_PWR_RISCV_BR_RETCODE);
        pmu->brom_retcode = r;
        if ((r & GA10B_RISCV_BR_RETCODE_MASK) == GA10B_RISCV_BR_RETCODE_PASS) {
            break;
        }
        if ((r & GA10B_RISCV_BR_RETCODE_MASK) == GA10B_RISCV_BR_RETCODE_FAIL) {
            err = GA10B_PMU_ERR_BROM;
            break;
        }
        if (waited_ms >= BROM_TIMEOUT_MS) {
            err = GA10B_PMU_ERR_TIMEOUT;
            break;
        }
        delay_us(pmu, BROM_POLL_MS * 1000u);
        waited_ms += BROM_POLL_MS;
    }
    /* dump_brom_stats: HWCFG2, BCR_CTRL if not locked, RETCODE
     * (trace lines 11442-11444). */
    uint32_t hwcfg2 = rd(pmu, GA10B_PWR_FALCON_HWCFG2);
    if ((hwcfg2 & GA10B_FALCON_HWCFG2_PRIV_LOCKDOWN) == 0u) {
        (void)rd(pmu, GA10B_PWR_RISCV_BCR_CTRL);
    }
    pmu->brom_retcode = rd(pmu, GA10B_PWR_RISCV_BR_RETCODE);
    return err;
}

/* nvgpu_pmu_wait_for_priv_lockdown_release (pmu_rtos_init.c:349-371) */
static int wait_lockdown_release(struct ga10b_pmu *pmu)
{
    uint64_t waited_us = 0;
    for (;;) {
        if ((rd(pmu, GA10B_PWR_FALCON_HWCFG2) & GA10B_FALCON_HWCFG2_PRIV_LOCKDOWN) == 0u) {
            return GA10B_PMU_OK;
        }
        if (waited_us >= (uint64_t)LOCKDOWN_TIMEOUT_MS * 1000u) {
            return GA10B_PMU_ERR_TIMEOUT;
        }
        delay_us(pmu, LOCKDOWN_POLL_US);
        waited_us += LOCKDOWN_POLL_US;
    }
}

int ga10b_pmu_boot(struct ga10b_pmu *pmu)
{
    if (pmu == nullptr || pmu->ops == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    if (pmu->booted) {
        return fail(pmu, GA10B_PMU_ERR_STATE);
    }

    /* pmu_sw_setup -> nvgpu_pmu_super_surface_buf_alloc: zeroed DMA
     * buffer with its own GPU VA in hdr.data.address. */
    uint8_t *s = ss(pmu);
    memset(s, 0, GA10B_PMU_SUPER_SURFACE_SIZE);
    uint32_t lo = (uint32_t)pmu->cfg.super_surface.gpu_va;
    uint32_t hi = (uint32_t)(pmu->cfg.super_surface.gpu_va >> 32);
    memcpy(s + GA10B_SS_HDR_ADDR_LO_OFFSET, &lo, 4);
    memcpy(s + GA10B_SS_HDR_ADDR_HI_OFFSET, &hi, 4);
    dma_clean(pmu, s, GA10B_PMU_SUPER_SURFACE_SIZE);
    if (pmu->cfg.trace_buf.cpu != nullptr) {
        memset(pmu->cfg.trace_buf.cpu, 0, GA10B_PMU_TRACE_BUF_SIZE);
        dma_clean(pmu, pmu->cfg.trace_buf.cpu, GA10B_PMU_TRACE_BUF_SIZE);
    }
    if (pmu->cfg.core_dump.cpu != nullptr) {
        memset(pmu->cfg.core_dump.cpu, 0, GA10B_PMU_CORE_DUMP_SIZE);
        dma_clean(pmu, pmu->cfg.core_dump.cpu, GA10B_PMU_CORE_DUMP_SIZE);
    }

    /* nvgpu_pmu_next_core_rtos_args_setup: boot args -> sysmem,
     * address + EXTMEM2 -> MAILBOX0/1. */
    struct ga10b_nv_pmu_boot_params bp;
    ga10b_pmu_build_boot_params(&bp, &pmu->cfg);
    memcpy(pmu->cfg.boot_args.cpu, &bp, sizeof(bp));
    dma_clean(pmu, pmu->cfg.boot_args.cpu, sizeof(bp));
    uint64_t mb = ga10b_pmu_mailbox_addr(pmu->cfg.boot_args.addr);
    wr(pmu, GA10B_PWR_FALCON_MAILBOX0, (uint32_t)mb);
    wr(pmu, GA10B_PWR_FALCON_MAILBOX1, (uint32_t)(mb >> 32));

    /* g->ops.falcon.bootstrap(pmu->flcn, 0): boot vector 0 -> only
     * CPUCTL.STARTCPU (ga10b_falcon_bootstrap). */
    wr(pmu, GA10B_PWR_RISCV_CPUCTL, GA10B_RISCV_CPUCTL_STARTCPU);
    pmu->booted = true;

    int err = wait_brom(pmu);
    if (err != GA10B_PMU_OK) {
        return fail(pmu, err);
    }
    err = wait_lockdown_release(pmu);
    if (err != GA10B_PMU_OK) {
        return fail(pmu, err);
    }
    return GA10B_PMU_OK;
}

/* ------------------------------------------------------------------ */
/* DMEM heap: nvgpu BITMAP_ALLOCATOR (bitmap_allocator.c), 32 B blocks */
/* ------------------------------------------------------------------ */

static bool heap_bit(const struct ga10b_pmu *pmu, uint32_t b)
{
    return (pmu->heap_bitmap[b / 32u] >> (b % 32u)) & 1u;
}

static void heap_set(struct ga10b_pmu *pmu, uint32_t b, uint32_t n, bool v)
{
    for (uint32_t i = b; i < b + n; i++) {
        if (v) {
            pmu->heap_bitmap[i / 32u] |= 1u << (i % 32u);
        } else {
            pmu->heap_bitmap[i / 32u] &= ~(1u << (i % 32u));
        }
    }
}

/* bitmap_find_next_zero_area(bitmap, limit, start, n, 0) */
static uint32_t heap_find_zero_area(const struct ga10b_pmu *pmu, uint32_t limit,
                                    uint32_t start, uint32_t n)
{
    for (uint32_t i = start; i + n <= limit; i++) {
        uint32_t j = 0;
        while (j < n && !heap_bit(pmu, i + j)) {
            j++;
        }
        if (j == n) {
            return i;
        }
        i += j;  /* skip past the set bit */
    }
    return limit;  /* not found (callers test >= limit) */
}

/* nvgpu_bitmap_balloc: next-fit from next_blk, then the area before it. */
static uint32_t heap_alloc(struct ga10b_pmu *pmu, uint32_t len)
{
    if (pmu->heap_blks == 0u || len == 0u) {
        return 0;
    }
    uint32_t blks = (len + GA10B_PMU_DMEM_ALLOC_ALIGNMENT - 1u) / GA10B_PMU_DMEM_ALLOC_ALIGNMENT;
    uint32_t offs = heap_find_zero_area(pmu, pmu->heap_blks, pmu->heap_next_blk, blks);
    if (offs >= pmu->heap_blks) {
        uint32_t limit = pmu->heap_next_blk;
        while (limit < pmu->heap_blks && !heap_bit(pmu, limit)) {
            limit++;   /* find_next_bit(bitmap, num_bits, next_blk) */
        }
        offs = heap_find_zero_area(pmu, limit, 0, blks);
        if (offs >= pmu->heap_next_blk) {
            pmu->heap_next_blk = 0;
            return 0;
        }
    }
    heap_set(pmu, offs, blks, true);
    pmu->heap_next_blk = offs + blks;
    return pmu->heap_base + offs * GA10B_PMU_DMEM_ALLOC_ALIGNMENT;
}

static void heap_free(struct ga10b_pmu *pmu, uint32_t addr, uint32_t len)
{
    if (pmu->heap_blks == 0u || addr < pmu->heap_base || len == 0u) {
        return;
    }
    uint32_t offs = (addr - pmu->heap_base) / GA10B_PMU_DMEM_ALLOC_ALIGNMENT;
    uint32_t blks = (len + GA10B_PMU_DMEM_ALLOC_ALIGNMENT - 1u) / GA10B_PMU_DMEM_ALLOC_ALIGNMENT;
    if (offs + blks <= pmu->heap_blks) {
        heap_set(pmu, offs, blks, false);
    }
}

/* ------------------------------------------------------------------ */
/* RISC-V debug buffer (common/falcon/falcon_debug.c)                  */
/* ------------------------------------------------------------------ */

/* gk20a_falcon_copy_from_dmem, port 0 */
static void copy_from_dmem(struct ga10b_pmu *pmu, uint32_t src, uint8_t *dst, uint32_t size)
{
    wr(pmu, GA10B_PWR_FALCON_DMEMC0, (src & 0x00fffffcu) | GA10B_FALCON_DMEMC_AINCR);
    uint32_t words = size >> 2, bytes = size & 3u;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = rd(pmu, GA10B_PWR_FALCON_DMEMD0);
        memcpy(dst + 4u * i, &v, 4);
    }
    if (bytes != 0u) {
        uint32_t v = rd(pmu, GA10B_PWR_FALCON_DMEMD0);
        memcpy(dst + 4u * words, &v, bytes);
    }
}

static void emit_line(struct ga10b_pmu *pmu, const char *s)
{
    if (pmu->ops->log_line != nullptr) {
        pmu->ops->log_line(pmu->ops->ctx, s);
    }
}

int ga10b_pmu_dbg_drain(struct ga10b_pmu *pmu)
{
    if (pmu == nullptr || pmu->ops == nullptr || !pmu->booted) {
        return GA10B_PMU_ERR_STATE;
    }
    uint32_t write_offset = rd(pmu, GA10B_PWR_PMU_QUEUE_HEAD(GA10B_RISCV_DEBUG_BUFFER_QUEUE));

    if (!pmu->dbg_meta_ok) {
        /* nvgpu_falcon_dbg_buf_init + falcon_fetch_debug_buffer_metadata:
         * metadata sits in the last 16 bytes of DMEM. */
        uint32_t dmem_size = GA10B_FALCON_HWCFG_DMEM_SIZE(rd(pmu, GA10B_PWR_FALCON_HWCFG));
        if (dmem_size < sizeof(struct ga10b_falcon_dbg_buf_metadata)) {
            return GA10B_PMU_ERR_PROTO;
        }
        uint32_t meta_off = dmem_size - (uint32_t)sizeof(struct ga10b_falcon_dbg_buf_metadata);
        struct ga10b_falcon_dbg_buf_metadata m;
        copy_from_dmem(pmu, meta_off, (uint8_t *)&m, sizeof(m));
        if (m.magic != GA10B_NVRISCV_DEBUG_BUFFER_MAGIC ||
            m.buffer_size >= GA10B_RISCV_DMESG_BUFFER_SIZE || (m.buffer_size & 3u) != 0u ||
            m.buffer_size == 0u || m.buffer_size > meta_off) {
            return GA10B_PMU_ERR_PROTO;
        }
        pmu->dbg_buffer_size = m.buffer_size;
        pmu->dbg_dmem_offset = meta_off - m.buffer_size;
        pmu->dbg_read_offset = 0;
        pmu->dbg_meta_ok = true;
    }
    if (write_offset >= pmu->dbg_buffer_size) {
        return GA10B_PMU_ERR_PROTO;
    }
    if (write_offset == pmu->dbg_read_offset) {
        return GA10B_PMU_OK;
    }

    /* falcon_update_debug_buffer_from_dmem */
    uint32_t rd_al = pmu->dbg_read_offset & ~3u;
    uint8_t *b = (uint8_t *)pmu->dbg_buf;
    if (write_offset >= pmu->dbg_read_offset) {
        copy_from_dmem(pmu, pmu->dbg_dmem_offset + rd_al, b + rd_al, write_offset - rd_al);
    } else {
        copy_from_dmem(pmu, pmu->dbg_dmem_offset + rd_al, b + rd_al,
                       pmu->dbg_buffer_size - rd_al);
        copy_from_dmem(pmu, pmu->dbg_dmem_offset, b, write_offset);
    }

    /* Emit NUL-terminated lines; a line may wrap the ring. */
    uint32_t it = pmu->dbg_read_offset;
    char line[160];
    uint32_t n = 0;
    while (it != write_offset) {
        char c = (char)b[it];
        it = (it + 1u) % pmu->dbg_buffer_size;
        if (c != '\0') {
            if (n + 1u < sizeof(line)) {
                line[n++] = c;
            }
            continue;
        }
        line[n] = '\0';
        emit_line(pmu, line);
        n = 0;
        pmu->dbg_read_offset = it;
    }
    /* nvgpu writes back its read offset (end of the last full line). */
    wr(pmu, GA10B_PWR_PMU_QUEUE_TAIL(GA10B_RISCV_DEBUG_BUFFER_QUEUE), pmu->dbg_read_offset);
    return GA10B_PMU_OK;
}

/* ------------------------------------------------------------------ */
/* Message queue                                                       */
/* ------------------------------------------------------------------ */

static uint32_t msgq_head(struct ga10b_pmu *pmu) { return rd(pmu, GA10B_PWR_PMU_MSGQ_HEAD); }
static uint32_t msgq_tail(struct ga10b_pmu *pmu) { return rd(pmu, GA10B_PWR_PMU_MSGQ_TAIL); }

/* pmu_process_init_msg_fb (pmu_msg.c:323-361) + nvgpu_pmu_queues_init
 * (pmu_queue.c) + nvgpu_pmu_allocator_dmem_init (allocator.c). */
static int process_init_msg(struct ga10b_pmu *pmu)
{
    uint32_t tail = msgq_tail(pmu);
    if (tail >= GA10B_FBQ_MSG_NUM_ELEMENTS) {
        return GA10B_PMU_ERR_PROTO;
    }
    /* Read the element and the next one: the init message's declared
     * size can run past its 60-byte slot (nvgpu reads hdr.size bytes
     * from element + 4). */
    uint32_t off = GA10B_SS_MSG_ELEMENT_OFFSET(tail) + (uint32_t)sizeof(struct ga10b_falcon_fbq_msgq_hdr);
    dma_inval(pmu, ss(pmu) + GA10B_SS_FBQ_MSG_OFFSET,
              GA10B_FBQ_MSG_NUM_ELEMENTS * GA10B_FBQ_MSG_ELEMENT_SIZE);
    struct ga10b_pmu_hdr h;
    memcpy(&h, ss(pmu) + off, sizeof(h));
    if (h.unit_id != GA10B_PMU_UNIT_INIT_DGPU && h.unit_id != GA10B_PMU_UNIT_CMDMGMT) {
        return GA10B_PMU_ERR_PROTO;
    }
    /* Need at least up to os_debug_entry_point. */
    const uint32_t need = (uint32_t)(sizeof(h) +
        offsetof(struct ga10b_pmu_init_msg, os_debug_entry_point) + 2u);
    if (h.size < need) {
        return GA10B_PMU_ERR_PROTO;
    }
    uint32_t body = h.size - (uint32_t)sizeof(h);
    if (body > sizeof(pmu->init)) {
        body = sizeof(pmu->init);
    }
    if (off + sizeof(h) + body > GA10B_PMU_SUPER_SURFACE_SIZE) {
        return GA10B_PMU_ERR_PROTO;
    }
    memset(&pmu->init, 0, sizeof(pmu->init));
    memcpy(&pmu->init, ss(pmu) + off + sizeof(h), body);
    if (pmu->init.hdr.function != GA10B_PMU_INIT_MSG_TYPE_PMU_INIT) {
        return GA10B_PMU_ERR_PROTO;
    }
    pmu->init_unit_id = h.unit_id;

    /* "Queue is not yet constructed, so inline next element code". */
    tail = (tail + 1u) % GA10B_FBQ_MSG_NUM_ELEMENTS;
    wr(pmu, GA10B_PWR_PMU_MSGQ_TAIL, tail);

    /* pmu_fb_queue_init: LPQ index = queue_phy_id[LPQ_IDX_FOR_V3]. */
    pmu->cmdq_index = pmu->init.queue_phy_id[GA10B_PMU_QUEUE_LPQ_IDX_FOR_V3];
    if (pmu->cmdq_index >= GA10B_PWR_PMU_QUEUE_COUNT) {
        return GA10B_PMU_ERR_PROTO;
    }
    pmu->cmdq_tail = 0;
    pmu->cmdq_in_use = 0;

    /* DMEM heap over the sw-managed area, 32 B aligned both ends. */
    uint32_t a = GA10B_PMU_DMEM_ALLOC_ALIGNMENT;
    uint32_t start = ((uint32_t)pmu->init.sw_managed_area_offset + a - 1u) & ~(a - 1u);
    uint32_t end = ((uint32_t)pmu->init.sw_managed_area_offset +
                    pmu->init.sw_managed_area_size) & ~(a - 1u);
    /* nvgpu_bitmap_allocator_init moves a zero base up one block
     * (common/mm/allocators/bitmap_allocator.c:457-460) so that no
     * allocation returns 0. The ga10b PMU reports its sw-managed area
     * at DMEM offset 0, so this matters: without it the first heap
     * allocation is 0 and every command fails. */
    if (start == 0u && end > a) start = a;
    pmu->heap_base = start;
    pmu->heap_blks = end > start ? (end - start) / a : 0u;
    if (pmu->heap_blks > GA10B_PMU_HEAP_MAX_BLKS) {
        pmu->heap_blks = GA10B_PMU_HEAP_MAX_BLKS;
    }
    pmu->heap_next_blk = 0;
    memset(pmu->heap_bitmap, 0, sizeof(pmu->heap_bitmap));

    pmu->fw_ready = true;
    return GA10B_PMU_OK;
}

/* State of one nvgpu_engine_fb_queue_pop sequence for a message. */
struct msg_reader {
    uint32_t position;
    uint32_t read_position;
    uint8_t elem[GA10B_FBQ_MSG_ELEMENT_SIZE];
};

/* nvgpu_engine_fb_queue_pop (engine_fb_queue.c) for the message queue. */
static int msg_pop(struct ga10b_pmu *pmu, struct msg_reader *r, void *data, uint32_t size)
{
    r->position = msgq_tail(pmu);
    if (r->position >= GA10B_FBQ_MSG_NUM_ELEMENTS) {
        return GA10B_PMU_ERR_PROTO;
    }
    const uint32_t hoff = (uint32_t)sizeof(struct ga10b_falcon_fbq_msgq_hdr);
    /* The message body starts `hoff` bytes into the element. nvgpu
     * checks only size + read_position against the element size, which
     * lets a firmware-declared size of 61-63 read past the element;
     * bound the actual copy instead. */
    if ((uint64_t)size + r->read_position + hoff > GA10B_FBQ_MSG_ELEMENT_SIZE) {
        return GA10B_PMU_ERR_PROTO;
    }
    struct ga10b_pmu_hdr h;
    if (r->read_position == 0u) {
        const uint8_t *src = ss(pmu) + GA10B_SS_MSG_ELEMENT_OFFSET(r->position);
        dma_inval(pmu, src, GA10B_FBQ_MSG_ELEMENT_SIZE);
        memcpy(r->elem, src, GA10B_FBQ_MSG_ELEMENT_SIZE);
        memcpy(&h, r->elem + hoff, sizeof(h));
        if ((uint32_t)h.size > GA10B_FBQ_MSG_ELEMENT_SIZE - hoff) {
            return GA10B_PMU_ERR_PROTO;   /* "Super Surface read failed" */
        }
    }
    memcpy(&h, r->elem + hoff, sizeof(h));
    memcpy(data, r->elem + r->read_position + hoff, size);
    r->read_position += size;
    if (r->read_position >= h.size) {
        r->read_position = 0;
        r->position = (r->position + 1u) % GA10B_FBQ_MSG_NUM_ELEMENTS;
    }
    wr(pmu, GA10B_PWR_PMU_MSGQ_TAIL, r->position);
    return GA10B_PMU_OK;
}

static void seq_release(struct ga10b_pmu *pmu, uint32_t id)
{
    pmu->seq_tbl[id / 32u] &= ~(1u << (id % 32u));
}

/* engine_fb_queue_set_element_use_state(false) + engine_fb_queue_sweep */
static void cmdq_free_element(struct ga10b_pmu *pmu, uint32_t idx)
{
    pmu->cmdq_in_use &= ~(1u << idx);
    uint32_t head = rd(pmu, GA10B_PWR_PMU_QUEUE_HEAD(pmu->cmdq_index));
    uint32_t tail = pmu->cmdq_tail;
    while (tail != head && tail < GA10B_FBQ_CMD_NUM_ELEMENTS &&
           (pmu->cmdq_in_use & (1u << tail)) == 0u) {
        tail = (tail + 1u) % GA10B_FBQ_CMD_NUM_ELEMENTS;
    }
    pmu->cmdq_tail = tail;
}

/* pmu_response_handle + pmu_payload_extract + pmu_payload_free +
 * nvgpu_pmu_rpc_handler (pmu_msg.c). */
static int handle_response(struct ga10b_pmu *pmu, const struct ga10b_pmu_hdr *h,
                           const uint8_t *body)
{
    uint32_t id = h->seq_id;
    if (pmu->seq[id].state != SEQ_PENDING) {
        return GA10B_PMU_ERR_PROTO;   /* "msg for an unknown sequence" */
    }
    int result = GA10B_PMU_OK;
    uint8_t status = 0;
    if (h->unit_id == GA10B_PMU_UNIT_RC && h->size > sizeof(*h) &&
        body[0] == GA10B_PMU_RC_MSG_TYPE_UNHANDLED_CMD) {
        result = GA10B_PMU_ERR_RPC;
    } else {
        /* Out payload: the PMU wrote the RPC structure back into the
         * command's FBQ element at fbq_out_offset (= fbq hdr + cmd). */
        uint32_t off = GA10B_SS_CMD_QUEUE_OFFSET(GA10B_PMU_COMMAND_QUEUE_LPQ) +
                       (uint32_t)(sizeof(struct ga10b_falcon_fbq_hdr) +
                                  sizeof(struct ga10b_pmu_rpc_cmd_full)) +
                       pmu->seq[id].element * GA10B_FBQ_CMD_ELEMENT_SIZE;
        dma_inval(pmu, ss(pmu) + off, pmu->seq[id].rpc_size);
        struct ga10b_pmu_rpc_header rh;
        memcpy(&rh, ss(pmu) + off, sizeof(rh));
        status = rh.flcn_status;
        if (rh.flcn_status != 0u) {
            result = GA10B_PMU_ERR_RPC;   /* "failed RPC response" */
        }
        if (result == GA10B_PMU_OK && rh.unit_id == GA10B_PMU_UNIT_ACR) {
            /* nvgpu_pmu_lsfm_rpc_handler */
            if (rh.function == GA10B_PMU_RPC_ID_ACR_INIT_WPR_REGION) {
                pmu->wpr_init_done = true;
            } else if (rh.function == GA10B_PMU_RPC_ID_ACR_BOOTSTRAP_GR_FALCONS) {
                pmu->falcons_loaded = true;
            }
        }
    }
    heap_free(pmu, pmu->seq[id].heap_offset, pmu->seq[id].heap_size);
    cmdq_free_element(pmu, pmu->seq[id].element);
    pmu->seq[id].flcn_status = status;
    pmu->seq[id].result = result;
    pmu->seq[id].state = SEQ_DONE;
    seq_release(pmu, id);
    return GA10B_PMU_OK;
}

/* pmu_read_message + the dispatch loop of nvgpu_pmu_process_message. */
static int process_messages(struct ga10b_pmu *pmu)
{
    for (;;) {
        /* nvgpu_engine_fb_queue_is_empty */
        uint32_t head = msgq_head(pmu);
        uint32_t tail = msgq_tail(pmu);
        if (head == tail) {
            return GA10B_PMU_OK;
        }

        struct msg_reader r;
        memset(&r, 0, sizeof(r));
        struct ga10b_pmu_hdr h;
        int err = msg_pop(pmu, &r, &h, sizeof(h));
        if (err != GA10B_PMU_OK) {
            return err;
        }
        if (h.unit_id == GA10B_PMU_UNIT_REWIND) {
            /* FBQ: no rewind, read the header again. */
            err = msg_pop(pmu, &r, &h, sizeof(h));
            if (err != GA10B_PMU_OK) {
                return err;
            }
        }
        if (!(h.unit_id < GA10B_PMU_UNIT_END || h.unit_id >= GA10B_PMU_UNIT_TEST_START)) {
            return GA10B_PMU_ERR_PROTO;
        }
        uint8_t body[GA10B_FBQ_MSG_ELEMENT_SIZE];
        memset(body, 0, sizeof(body));
        if (h.size > sizeof(h)) {
            err = msg_pop(pmu, &r, body, (uint32_t)h.size - (uint32_t)sizeof(h));
            if (err != GA10B_PMU_OK) {
                return err;
            }
        }
        pmu->msgs_seen++;

        uint8_t flags = (uint8_t)(h.ctrl_flags & ~GA10B_PMU_CMD_FLAGS_PMU_MASK);
        if (flags == GA10B_PMU_CMD_FLAGS_EVENT || flags == GA10B_PMU_CMD_FLAGS_RPC_EVENT) {
            /* pmu_handle_event: perfmon/perf/PG events only; none of
             * those units is running here. */
            pmu->events_seen++;
            pmu->last_event_unit = h.unit_id;
            continue;
        }
        err = handle_response(pmu, &h, body);
        if (err != GA10B_PMU_OK) {
            return err;
        }
    }
}

int ga10b_pmu_poll(struct ga10b_pmu *pmu)
{
    if (pmu == nullptr || pmu->ops == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    if (!pmu->booted) {
        return GA10B_PMU_ERR_STATE;
    }

    /* gk20a_pmu_isr: stat & (RISCV IRQMASK & IRQDEST), cleared before
     * handling (trace: W 10a004 then the debug-buffer / queue reads).
     * nvgpu's ISR runs only when the host interrupt line fired, i.e.
     * when a host-routed, enabled bit is set; its "clear unhandled"
     * branch (stat & mask == 0) never runs from polling, so a poll
     * with no host-routed bit pending leaves IRQSTAT alone (bits
     * routed to the PMU itself are the firmware's to clear). */
    uint32_t intr = rd(pmu, GA10B_PWR_FALCON_IRQSTAT);
    int err = GA10B_PMU_OK;
    if (intr != 0u) {
        uint32_t mask = rd(pmu, GA10B_PWR_RISCV_IRQMASK) & rd(pmu, GA10B_PWR_RISCV_IRQDEST);
        pmu->last_irqstat = intr;
        intr &= mask;
        if (intr != 0u) {
            wr(pmu, GA10B_PWR_FALCON_IRQSCLR, intr);
        }
        if ((intr & GA10B_FALCON_IRQ_HALT) != 0u) {
            pmu->halted = true;
        }
        if ((intr & GA10B_FALCON_IRQ_SWGEN1) != 0u) {
            (void)ga10b_pmu_dbg_drain(pmu);   /* ga10b_pmu_handle_swgen1_irq */
        }
    }

    /* Message queue: polled every time (no IRQ), INIT first. */
    if (!pmu->fw_ready) {
        if (msgq_head(pmu) != msgq_tail(pmu)) {
            err = process_init_msg(pmu);
        }
    } else {
        err = process_messages(pmu);
    }
    if (err != GA10B_PMU_OK) {
        return fail(pmu, err);
    }
    if (pmu->halted) {
        return fail(pmu, GA10B_PMU_ERR_HALT);
    }
    return GA10B_PMU_OK;
}

int ga10b_pmu_wait_init(struct ga10b_pmu *pmu, uint32_t timeout_ms)
{
    if (pmu == nullptr || pmu->ops == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    uint64_t waited = 0;
    for (;;) {
        int err = ga10b_pmu_poll(pmu);
        if (err != GA10B_PMU_OK) {
            return err;
        }
        if (pmu->fw_ready) {
            return GA10B_PMU_OK;
        }
        if (waited >= (uint64_t)timeout_ms * 1000u) {
            return fail(pmu, GA10B_PMU_ERR_TIMEOUT);
        }
        delay_us(pmu, WAIT_POLL_US);
        waited += WAIT_POLL_US;
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

int ga10b_pmu_rpc_post(struct ga10b_pmu *pmu, const void *rpc, uint16_t size_rpc,
                       uint16_t size_scratch)
{
    if (pmu == nullptr || pmu->ops == nullptr || rpc == nullptr ||
        size_rpc < sizeof(struct ga10b_pmu_rpc_header)) {
        return GA10B_PMU_ERR_ARG;
    }
    if (!pmu->fw_ready) {
        return fail(pmu, GA10B_PMU_ERR_STATE);   /* "PMU is not ready to process RPC" */
    }

    const uint32_t hdr_sz = (uint32_t)sizeof(struct ga10b_falcon_fbq_hdr);
    const uint32_t cmd_sz = (uint32_t)sizeof(struct ga10b_pmu_rpc_cmd_full);
    if (hdr_sz + cmd_sz + (uint32_t)size_rpc + size_scratch > GA10B_FBQ_CMD_ELEMENT_SIZE) {
        return GA10B_PMU_ERR_ARG;
    }

    /* nvgpu_pmu_rpc_execute: cmd header from the RPC header. */
    struct ga10b_pmu_rpc_header rh;
    memcpy(&rh, rpc, sizeof(rh));
    struct ga10b_pmu_rpc_cmd_full cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.unit_id = rh.unit_id;
    cmd.hdr.size = (uint8_t)cmd_sz;            /* PMU_CMD_HDR_SIZE + sizeof(nv_pmu_rpc_cmd) */
    cmd.rpc.cmd_type = GA10B_PMU_RPC_CMD_ID;
    cmd.rpc.flags = rh.flags;
    if (!(cmd.hdr.unit_id < GA10B_PMU_UNIT_END || cmd.hdr.unit_id >= GA10B_PMU_UNIT_TEST_START)) {
        return GA10B_PMU_ERR_ARG;
    }

    /* nvgpu_pmu_seq_acquire: first free id. */
    uint32_t id = GA10B_PMU_MAX_SEQ;
    for (uint32_t i = 0; i < GA10B_PMU_MAX_SEQ; i++) {
        if ((pmu->seq_tbl[i / 32u] & (1u << (i % 32u))) == 0u) {
            id = i;
            break;
        }
        }
    if (id == GA10B_PMU_MAX_SEQ) {
        return fail(pmu, GA10B_PMU_ERR_NOSPACE);
    }
    cmd.hdr.seq_id = (uint8_t)id;
    cmd.hdr.ctrl_flags = GA10B_PMU_CMD_FLAGS_STATUS | GA10B_PMU_CMD_FLAGS_INTR;

    /* pmu_fbq_cmd_setup: heap space for fbq hdr + cmd + rpc + scratch. */
    uint32_t need = ((uint32_t)size_rpc + size_scratch + hdr_sz + cmd_sz + 3u) & ~3u;
    uint32_t heap_offset = heap_alloc(pmu, need);
    if (heap_offset == 0u || heap_offset > 0xFFFFu) {
        return fail(pmu, GA10B_PMU_ERR_NOSPACE);
    }

    /* engine_fb_queue_has_room + prepare_write (two head reads, as in
     * the trace: "R 10a804 ... x2"). */
    uint32_t head = rd(pmu, GA10B_PWR_PMU_QUEUE_HEAD(pmu->cmdq_index));
    if (head >= GA10B_FBQ_CMD_NUM_ELEMENTS ||
        (head + 1u) % GA10B_FBQ_CMD_NUM_ELEMENTS == pmu->cmdq_tail) {
        heap_free(pmu, heap_offset, need);
        return fail(pmu, GA10B_PMU_ERR_NOSPACE);   /* -EAGAIN: queue full */
    }
    uint32_t pos = rd(pmu, GA10B_PWR_PMU_QUEUE_HEAD(pmu->cmdq_index));
    if (pos >= GA10B_FBQ_CMD_NUM_ELEMENTS || (pmu->cmdq_in_use & (1u << pos)) != 0u) {
        heap_free(pmu, heap_offset, need);
        return fail(pmu, GA10B_PMU_ERR_PROTO);
    }

    /* Work buffer: fbq hdr | cmd | rpc payload (rest zero). */
    uint8_t *w = pmu->work;
    memset(w, 0, sizeof(pmu->work));
    uint32_t buffer_size = hdr_sz + cmd_sz;           /* nvgpu_pmu_seq_set_buffer_size */
    /* pmu_cmd_payload_setup_rpc: payload at dmem_offset = buffer_size,
     * rpc_dmem_ptr = that + heap_offset. */
    cmd.rpc.rpc_dmem_size = size_rpc;
    cmd.rpc.rpc_dmem_ptr = buffer_size + heap_offset;
    struct ga10b_falcon_fbq_hdr fh;
    memset(&fh, 0, sizeof(fh));
    fh.element_index = (uint8_t)pos;                  /* engine_fb_queue_write */
    fh.heap_size = (uint16_t)need;
    fh.heap_offset = (uint16_t)heap_offset;
    memcpy(w, &fh, sizeof(fh));
    memcpy(w + hdr_sz, &cmd, sizeof(cmd));
    memcpy(w + buffer_size, rpc, size_rpc);

    /* Copy the whole element to the super surface, then head = next. */
    pmu->cmdq_in_use |= 1u << pos;
    uint8_t *dst = ss(pmu) + GA10B_SS_CMD_QUEUE_OFFSET(GA10B_PMU_COMMAND_QUEUE_LPQ) +
                   pos * GA10B_FBQ_CMD_ELEMENT_SIZE;
    memcpy(dst, w, GA10B_FBQ_CMD_ELEMENT_SIZE);
    dma_clean(pmu, dst, GA10B_FBQ_CMD_ELEMENT_SIZE);

    pmu->seq_tbl[id / 32u] |= 1u << (id % 32u);
    pmu->seq[id].state = SEQ_PENDING;
    pmu->seq[id].function = rh.function;
    pmu->seq[id].flcn_status = 0;
    pmu->seq[id].element = (uint8_t)pos;
    pmu->seq[id].heap_offset = (uint16_t)heap_offset;
    pmu->seq[id].heap_size = (uint16_t)need;
    pmu->seq[id].rpc_size = size_rpc;
    pmu->seq[id].result = GA10B_PMU_OK;

    wr(pmu, GA10B_PWR_PMU_QUEUE_HEAD(pmu->cmdq_index), (pos + 1u) % GA10B_FBQ_CMD_NUM_ELEMENTS);
    return (int)id;
}

int ga10b_pmu_rpc_wait(struct ga10b_pmu *pmu, int seq, void *out, uint32_t timeout_ms)
{
    if (pmu == nullptr || pmu->ops == nullptr || seq < 0 || seq >= (int)GA10B_PMU_MAX_SEQ) {
        return GA10B_PMU_ERR_ARG;
    }
    uint32_t id = (uint32_t)seq;
    if (pmu->seq[id].state == SEQ_FREE) {
        return GA10B_PMU_ERR_STATE;
    }
    uint64_t waited = 0;
    while (pmu->seq[id].state != SEQ_DONE) {
        int err = ga10b_pmu_poll(pmu);
        if (err != GA10B_PMU_OK) {
            return err;
        }
        if (pmu->seq[id].state == SEQ_DONE) {
            break;
        }
        if (waited >= (uint64_t)timeout_ms * 1000u) {
            return fail(pmu, GA10B_PMU_ERR_TIMEOUT);
        }
        delay_us(pmu, WAIT_POLL_US);
        waited += WAIT_POLL_US;
    }
    if (out != nullptr) {
        uint32_t off = GA10B_SS_CMD_QUEUE_OFFSET(GA10B_PMU_COMMAND_QUEUE_LPQ) +
                       (uint32_t)(sizeof(struct ga10b_falcon_fbq_hdr) +
                                  sizeof(struct ga10b_pmu_rpc_cmd_full)) +
                       pmu->seq[id].element * GA10B_FBQ_CMD_ELEMENT_SIZE;
        memcpy(out, ss(pmu) + off, pmu->seq[id].rpc_size);
    }
    int result = pmu->seq[id].result;
    pmu->seq[id].state = SEQ_FREE;
    if (result != GA10B_PMU_OK) {
        return fail(pmu, result);
    }
    return GA10B_PMU_OK;
}

int ga10b_pmu_lsfm_init_wpr_region(struct ga10b_pmu *pmu, uint32_t timeout_ms)
{
    if (pmu == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    struct ga10b_pmu_rpc_acr_init_wpr_region rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.wpr_region_id = 0x1u;      /* lsfm_sw_gv100.c:47 */
    rpc.wpr_offset = 0x0u;
    rpc.hdr.unit_id = GA10B_PMU_UNIT_ACR;            /* PMU_RPC_EXECUTE */
    rpc.hdr.function = GA10B_PMU_RPC_ID_ACR_INIT_WPR_REGION;
    rpc.hdr.flags = 0;
    int seq = ga10b_pmu_rpc_post(pmu, &rpc,
                                 (uint16_t)(sizeof(rpc) - sizeof(rpc.scratch)), 0);
    if (seq < 0) {
        return seq;
    }
    delay_us(pmu, WPR_REGION_SETTLE_MS * 1000u);
    int err = ga10b_pmu_rpc_wait(pmu, seq, nullptr, timeout_ms);
    if (err != GA10B_PMU_OK) {
        return err;
    }
    return pmu->wpr_init_done ? GA10B_PMU_OK : fail(pmu, GA10B_PMU_ERR_PROTO);
}

int ga10b_pmu_lsfm_bootstrap_gr_falcons(struct ga10b_pmu *pmu, uint32_t falcon_id_mask,
                                        uint32_t timeout_ms)
{
    const uint32_t ok = (1u << 2) | (1u << 3);   /* FALCON_ID_FECS, FALCON_ID_GPCCS */
    if (pmu == nullptr || falcon_id_mask == 0u || (falcon_id_mask & ~ok) != 0u) {
        return GA10B_PMU_ERR_ARG;
    }
    pmu->falcons_loaded = false;                  /* lsfm->loaded_falcon_id = 0 */
    struct ga10b_pmu_rpc_acr_bootstrap_gr_falcons rpc;
    memset(&rpc, 0, sizeof(rpc));
    rpc.falcon_id_mask = falcon_id_mask;
    rpc.flags = GA10B_PMU_ACR_BOOTSTRAP_FLAGS_RESET_YES;
    rpc.hdr.unit_id = GA10B_PMU_UNIT_ACR;
    rpc.hdr.function = GA10B_PMU_RPC_ID_ACR_BOOTSTRAP_GR_FALCONS;
    int seq = ga10b_pmu_rpc_post(pmu, &rpc,
                                 (uint16_t)(sizeof(rpc) - sizeof(rpc.scratch)), 0);
    if (seq < 0) {
        return seq;
    }
    delay_us(pmu, BOOTSTRAP_SETTLE_MS * 1000u);
    int err = ga10b_pmu_rpc_wait(pmu, seq, nullptr, timeout_ms);
    if (err != GA10B_PMU_OK) {
        return err;
    }
    return pmu->falcons_loaded ? GA10B_PMU_OK : fail(pmu, GA10B_PMU_ERR_PROTO);
}

int ga10b_pmu_bootstrap_ctxsw_falcons(struct ga10b_pmu *pmu, uint32_t timeout_ms)
{
    if (pmu == nullptr) {
        return GA10B_PMU_ERR_ARG;
    }
    int err;
    if (!pmu->wpr_init_done) {
        err = ga10b_pmu_wait_init(pmu, timeout_ms);   /* nvgpu_pmu_wait_fw_ready */
        if (err != GA10B_PMU_OK) {
            return err;
        }
        err = ga10b_pmu_lsfm_init_wpr_region(pmu, timeout_ms);
        if (err != GA10B_PMU_OK) {
            return err;
        }
    }
    return ga10b_pmu_lsfm_bootstrap_gr_falcons(pmu, (1u << 2) | (1u << 3), timeout_ms);
}
