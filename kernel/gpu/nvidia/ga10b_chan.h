/*
 * ga10b_chan.h - GA10B address space, TSG, channel and runlist built by
 * SLM-OS, without Linux (#1088).
 *
 * Reproduces kernel-side what scripts/gpu-launch-common.c makes nvgpu
 * do through raw ioctls (ALLOC_AS, OPEN_TSG, OPEN_CHANNEL, AS_BIND,
 * CREATE_SUBCONTEXT, BIND_CHANNEL_EX, WDT disable, SETUP_BIND with
 * USERMODE, ALLOC_OBJ_CTX 0xC7C0, SET_PREEMPTION_MODE, MAP_BUFFER_EX,
 * TSG_ENABLE), following nvgpu L4T R36.4.x:
 *
 *   address space   common/mm/vm.c, hal/mm/gmmu/gmmu_ga10b_fusa.c
 *   instance block  common/mm/mm.c:99 nvgpu_alloc_inst_block,
 *                   hal/fifo/ramin_ga10b_fusa.c, ramin_gv11b_fusa.c
 *   RAMFC           hal/fifo/ramfc_ga10b_fusa.c ga10b_ramfc_setup
 *   USERD           hal/fifo/userd_ga10b.c ga10b_userd_init_mem
 *   runlist         hal/fifo/runlist_ram_gv11b_fusa.c (entry format),
 *                   runlist_fifo_ga10b_fusa.c (submit)
 *   CHRAM           hal/fifo/channel_ga10b_fusa.c
 *   doorbell token  hal/fifo/usermode_tu104_fusa.c:58
 *
 * All memory is system memory reached physically (SMMU bypass) and
 * every structure uses the aperture nvgpu gives a plain
 * nvgpu_dma_alloc on this SoC: APERTURE_SYSMEM = non-coherent
 * (os/linux/linux-dma.c:320; NVGPU_USE_COHERENT_SYSMEM only changes
 * PMU paths). CPU caches are not coherent with the GPU, so every CPU
 * write to GPU-visible memory is cleaned to DRAM before the GPU can
 * read it.
 *
 * Hardware access goes through struct ga10b_gpu_ops so the code runs
 * unchanged in host tests; the pure encoders below take no ops.
 */
#ifndef GPU_NVIDIA_GA10B_CHAN_H
#define GPU_NVIDIA_GA10B_CHAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ga10b_cold.h"   /* struct ga10b_reg_write */

struct ga10b_channel_handoff;

/* ---- Platform hooks ---- */

struct ga10b_gpu_ops {
    void *ctx;
    uint32_t (*rd32)(void *ctx, uint32_t bar0_off);
    void (*wr32)(void *ctx, uint32_t bar0_off, uint32_t val);
    void (*udelay)(void *ctx, uint32_t us);
    /* `npages` physically contiguous, 4 KB aligned, ZEROED pages.
     * Returns the CPU pointer (NULL on failure) and the physical
     * address in *phys. */
    void *(*alloc_pages)(void *ctx, uint32_t npages, uint64_t *phys);
    void (*free_pages)(void *ctx, void *cpu, uint32_t npages);
    /* CPU pointer for a physical address returned by alloc_pages
     * (identity on Jetson). Used to walk page tables. */
    void *(*phys_to_cpu)(void *ctx, uint64_t phys);
    void (*cache_clean)(void *ctx, const void *p, size_t len);
    void (*cache_inval)(void *ctx, const void *p, size_t len);
};

/* A physically contiguous GPU-visible allocation. */
struct ga10b_dma {
    void *cpu;
    uint64_t phys;
    uint32_t size;      /* bytes, multiple of 4096 */
};

#define GA10B_PAGE_SIZE 4096u

int ga10b_dma_alloc(const struct ga10b_gpu_ops *ops, uint32_t bytes,
                    struct ga10b_dma *out);
void ga10b_dma_free(const struct ga10b_gpu_ops *ops, struct ga10b_dma *d);
/* Clean `d` to DRAM (after the CPU filled it). */
void ga10b_dma_clean(const struct ga10b_gpu_ops *ops, const struct ga10b_dma *d);

/* Error codes. */
#define GA10B_CHAN_OK            0
#define GA10B_CHAN_ERR_ARG      (-1)
#define GA10B_CHAN_ERR_NOMEM    (-2)
#define GA10B_CHAN_ERR_TIMEOUT  (-3)
#define GA10B_CHAN_ERR_VA       (-4)   /* VA range exhausted / misaligned */
#define GA10B_CHAN_ERR_HW       (-5)

/* ==================================================================
 * GMMU (new page-table format, 5 levels; gmmu_ga10b_fusa.c:353)
 * ================================================================== */

/* VA bits per level: PDE3 [48:47] 8-byte entries, PDE2 [46:38],
 * PDE1 [37:29], PDE0 [28:21] 16-byte dual entries
 * (GA10B_PDE0_ENTRY_SIZE, gmmu_ga10b_fusa.c:37), PTE [20:12] for 4 KB
 * pages. SLM-OS maps only 4 KB pages, so only the small half of each
 * dual PDE0 is used; the big half stays invalid. */
#define GA10B_VM_PDE0_ENTRY_BYTES   16u
#define GA10B_VM_PDE_ENTRY_BYTES    8u
#define GA10B_VM_PTE_ENTRY_BYTES    8u

static inline uint32_t ga10b_vm_idx_pde3(uint64_t va) { return (uint32_t)((va >> 47) & 0x3u); }
static inline uint32_t ga10b_vm_idx_pde2(uint64_t va) { return (uint32_t)((va >> 38) & 0x1ffu); }
static inline uint32_t ga10b_vm_idx_pde1(uint64_t va) { return (uint32_t)((va >> 29) & 0x1ffu); }
static inline uint32_t ga10b_vm_idx_pde0(uint64_t va) { return (uint32_t)((va >> 21) & 0xffu); }
static inline uint32_t ga10b_vm_idx_pte(uint64_t va)  { return (uint32_t)((va >> 12) & 0x1ffu); }

/* gmmu_new_* field values (hw_gmmu_ga10b.h). */
#define GA10B_GMMU_APERTURE_SYS_NCOH  0x6u   /* *_aperture_sys_mem_ncoh_f() */
#define GA10B_GMMU_VOL                0x8u   /* *_vol_true_f() */
#define GA10B_GMMU_PTE_VALID          0x1u
#define GA10B_GMMU_PTE_PRIV           0x20u
#define GA10B_GMMU_PTE_RO             0x40u

/* PDE3/2/1 (ga10b_update_gmmu_pde3_locked): w0 = aperture | vol |
 * (phys >> 12)[23:0] << 8, w1 = (phys >> 12) >> 24. */
static inline void ga10b_pde_encode(uint64_t next_phys, uint32_t w[2])
{
    uint64_t a = next_phys >> 12;
    w[0] = GA10B_GMMU_APERTURE_SYS_NCOH | GA10B_GMMU_VOL |
           (uint32_t)((a & 0xffffffu) << 8);
    w[1] = (uint32_t)(a >> 24);
}

/* Dual PDE0, small half only (ga10b_update_gmmu_pde0_locked):
 * w[0..1] = big half = 0, w2 = small aperture | vol | addr, w3 = hi. */
static inline void ga10b_pde0_small_encode(uint64_t pt_phys, uint32_t w[4])
{
    uint64_t a = pt_phys >> 12;
    w[0] = 0;
    w[1] = 0;
    w[2] = GA10B_GMMU_APERTURE_SYS_NCOH | GA10B_GMMU_VOL |
           (uint32_t)((a & 0xffffffu) << 8);
    w[3] = (uint32_t)(a >> 24);
}

/* Mapping flags. */
#define GA10B_MAP_CACHEABLE  (1u << 0)  /* GPU L2 may cache: no VOL bit */
#define GA10B_MAP_PRIV       (1u << 1)  /* privileged (ctx buffers) */
#define GA10B_MAP_RO         (1u << 2)

/* PTE (ga10b_update_pte): kind 0 (pitch), VOL unless cacheable. */
static inline void ga10b_pte_encode(uint64_t phys, uint32_t flags, uint32_t w[2])
{
    uint64_t a = phys >> 12;
    w[0] = GA10B_GMMU_PTE_VALID | GA10B_GMMU_APERTURE_SYS_NCOH |
           (uint32_t)((a & 0xffffffu) << 8);
    if (flags & GA10B_MAP_PRIV) w[0] |= GA10B_GMMU_PTE_PRIV;
    if (flags & GA10B_MAP_RO) w[0] |= GA10B_GMMU_PTE_RO;
    if (!(flags & GA10B_MAP_CACHEABLE)) w[0] |= GA10B_GMMU_VOL;
    w[1] = (uint32_t)(a >> 24);
}

/* GPU virtual layout. User buffers (GPFIFO, pushbuffer, shaders, ...)
 * start where the helper's ALLOC_AS range starts (0x4000000,
 * gpu-launch-common.c:300); context buffers go in the kernel range
 * above the user range, as nvgpu places them (its golden VM puts the
 * global buffers at 0x20_0000_0000+, MMIO trace line 18154). Both stay
 * below 2^40, which ga10b_validate_handoff requires. */
#define GA10B_VM_USER_BASE    0x0000000004000000ull
#define GA10B_VM_USER_LIMIT   0x0000002000000000ull
#define GA10B_VM_KERNEL_BASE  0x0000002000000000ull
#define GA10B_VM_KERNEL_LIMIT 0x0000004000000000ull

struct ga10b_vm {
    struct ga10b_dma pdb;       /* PDE3 page */
    uint64_t user_next;
    uint64_t kernel_next;
    uint32_t table_pages;       /* page-table pages allocated (incl. PDB) */
};

int ga10b_vm_init(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm);
/* Free every page-table page (not the mapped memory). */
void ga10b_vm_destroy(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm);
/* Reserve `size` bytes of VA (rounded up to 4 KB, aligned to `align`,
 * a power of two >= 4096). Returns 0 when the range is exhausted. */
uint64_t ga10b_vm_reserve(struct ga10b_vm *vm, uint64_t size, uint64_t align,
                          bool kernel);
/* Map [phys, phys + size) at gpu_va with 4 KB PTEs; all three must be
 * 4 KB aligned. Page-table pages are allocated on demand and every
 * entry written is cleaned. Call ga10b_vm_tlb_invalidate afterwards
 * if the GPU may already have walked this VM. */
int ga10b_vm_map(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                 uint64_t gpu_va, uint64_t phys, uint64_t size, uint32_t flags);
/* Reserve VA and map a dma buffer in one step. */
int ga10b_vm_map_dma(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                     const struct ga10b_dma *d, uint32_t flags, bool kernel,
                     uint64_t *gpu_va);
/* Software walk: physical address mapped at gpu_va, 0 if unmapped. */
uint64_t ga10b_vm_lookup(const struct ga10b_gpu_ops *ops, const struct ga10b_vm *vm,
                         uint64_t gpu_va);
/* gm20b_fb_tlb_invalidate for this VM's PDB (all VA). */
int ga10b_vm_tlb_invalidate(const struct ga10b_gpu_ops *ops, const struct ga10b_vm *vm);

/* fb_mmu_invalidate_pdb value: addr >> 12 in bits 31:4, aperture
 * sys_mem (0x2) (fb_mmu_invalidate_pdb_addr_f / _aperture_sys_mem_f).
 * Trace line 17474: PDB 0x1080ad000 -> 0x01080ad2. */
static inline uint32_t ga10b_tlb_invalidate_pdb_val(uint64_t pdb_phys)
{
    return (uint32_t)(((pdb_phys >> 12) & 0x0fffffffu) << 4) | 0x2u;
}

/* ==================================================================
 * Instance block (4 KB, hw_ram_ga10b.h)
 * ================================================================== */

#define GA10B_INST_BLOCK_BYTES          4096u   /* ram_in_alloc_size_v */
#define GA10B_RAMFC_BYTES               512u    /* ram_fc_size_val_v */
#define GA10B_MAX_SUBCTX                64u     /* gr_pri_fe_chip_def_info_max_veid_count_init_v */

/* Word indices. */
#define GA10B_RAMIN_W_PAGE_DIR_BASE_LO  128u
#define GA10B_RAMIN_W_PAGE_DIR_BASE_HI  129u
#define GA10B_RAMIN_W_ENGINE_FW_MAGIC   131u
#define GA10B_RAMIN_W_ENGINE_WFI_TARGET 132u
#define GA10B_RAMIN_W_ENGINE_WFI_PTR_HI 133u
#define GA10B_RAMIN_W_ENGINE_WFI_VEID   134u
#define GA10B_RAMIN_W_ENG_METHOD_LO     136u
#define GA10B_RAMIN_W_ENG_METHOD_HI     137u
#define GA10B_RAMIN_W_SC_PDB_VALID(i)   (166u + (i) / 32u)
#define GA10B_RAMIN_W_SC_PDB_LO(i)      (168u + (i) * 4u)
#define GA10B_RAMIN_W_SC_PDB_HI(i)      (169u + (i) * 4u)

#define GA10B_RAMIN_ENGINE_FW_MAGIC     0xcafeca11u
#define GA10B_RAMIN_TARGET_SYS_NCOH     0x3u
#define GA10B_RAMIN_PDB_VOL             0x4u
#define GA10B_RAMIN_VER2_PT             0x400u
#define GA10B_RAMIN_BIG_PAGE_64K        0x800u
#define GA10B_RAMIN_SC_TARGET_INVALID   0x1u    /* ram_in_sc_page_dir_base_target_invalid_v */

static_assert(GA10B_RAMIN_W_SC_PDB_HI(GA10B_MAX_SUBCTX - 1u) * 4u + 4u
              <= GA10B_INST_BLOCK_BYTES, "subctx PDB array fits the inst block");
static_assert(GA10B_RAMFC_BYTES / 4u <= GA10B_RAMIN_W_PAGE_DIR_BASE_LO,
              "RAMFC lies below the RAMIN page directory words");

/* nvgpu_alloc_inst_block + ga10b_ramin_set_magic_value. */
void ga10b_inst_set_magic(uint32_t *w);
/* ga10b_ramin_init_pdb (+ gm20b set_big_page_size 64K, already set). */
void ga10b_inst_init_pdb(uint32_t *w, uint64_t pdb_phys);
/* gv11b_ramin_set_subctx_pdb_info format word for an added subctx. */
uint32_t ga10b_subctx_pdb_format_word(uint64_t pdb_phys, bool replayable);
/* gv11b_ramin_init_subctx_pdb + init_subctx_valid_mask for a TSG whose
 * only valid subcontexts are the bits in `valid_mask` (64 VEIDs), all
 * on the same PDB. Invalid entries get target=invalid, hi=0. */
void ga10b_inst_set_subctxs(uint32_t *w, uint64_t valid_mask, uint64_t pdb_phys,
                            bool replayable);
/* gv11b_ramin_set_gr_ptr: engine_wfi_target/ptr_hi from a GPU VA. */
void ga10b_inst_set_gr_ptr(uint32_t *w, uint64_t gpu_va);
/* gv11b_ramin_set_eng_method_buffer. */
void ga10b_inst_set_eng_method_buffer(uint32_t *w, uint64_t gpu_va);

/* ---- RAMFC (ga10b_ramfc_setup) ---- */

#define GA10B_RAMFC_W_SIGNATURE         4u
#define GA10B_RAMFC_W_ACQUIRE           12u
#define GA10B_RAMFC_W_GP_BASE           18u
#define GA10B_RAMFC_W_GP_BASE_HI        19u
#define GA10B_RAMFC_W_PB_HEADER         33u
#define GA10B_RAMFC_W_SUBDEVICE         37u
#define GA10B_RAMFC_W_TARGET            43u
#define GA10B_RAMFC_W_HCE_CTRL          57u
#define GA10B_RAMFC_W_CONFIG            61u
#define GA10B_RAMFC_W_INTR_NOTIFY       62u
#define GA10B_RAMFC_W_SET_CHANNEL_INFO  63u

#define GA10B_GPFIFO_CLASS              0xc76fu  /* AMPERE_CHANNEL_GPFIFO_B */

struct ga10b_ramfc_params {
    uint64_t gpfifo_va;
    uint32_t gpfifo_entries;    /* power of two */
    uint32_t chid;
    uint32_t veid;
    uint32_t rleng_id;          /* GR = 0 (device info row 2) */
    uint32_t eng_intr_vector;   /* GR = 0 (runlist engine_status1 intr_id) */
    uint64_t acquire_timeout_ms;/* 0 = watchdog disabled (helper) */
};

/* gm20b_pbdma_acquire_val. */
uint32_t ga10b_pbdma_acquire_val(uint64_t timeout_ms);
/* Zero the RAMFC words and program them; also writes the RAMIN
 * engine_wfi_veid word, as ga10b_ramfc_setup does. */
int ga10b_ramfc_setup(uint32_t *w, const struct ga10b_ramfc_params *p);

/* ---- USERD (ram_userd_*_w) ---- */
#define GA10B_USERD_BYTES               512u    /* 1 << ram_userd_base_shift_v */
#define GA10B_USERD_W_GP_GET            34u
#define GA10B_USERD_W_GP_PUT            35u

/* ga10b_userd_init_mem: zero PUT/GET/REF/PUT_HI/TOP_LEVEL_GET(_HI)/
 * GET_HI/GP_GET/GP_PUT. */
void ga10b_userd_init(uint32_t *userd);

/* ---- GPFIFO entry (gm20b_pbdma_format_gpfifo_entry) ---- */
static inline void ga10b_gpfifo_entry(uint64_t pb_va, uint32_t dwords, uint32_t e[2])
{
    e[0] = (uint32_t)pb_va;
    e[1] = (uint32_t)(pb_va >> 32) | ((dwords & 0x1fffffu) << 10);
}

/* ==================================================================
 * Runlist (16-byte entries, runlist_ram_gv11b_fusa.c:46-108)
 *
 * TSG entry:     w0 = type 1 | timeslice_scale << 16 | timeout << 24
 *                w1 = number of channels   w2 = tsgid   w3 = 0
 * Channel entry: w0 = type 0 | runqueue << 1 | inst_target << 4 |
 *                     userd_target << 6 | (userd >> 8) << 8
 *                w1 = userd >> 32
 *                w2 = (inst >> 12) << 12 | chid    w3 = inst >> 32
 *
 * This is NOT the layout in ga10b_gmmu.c build_and_submit_fresh_runlist
 * (word 0 = chid | bit 11 | inst, word 2 = userd | 0xf): that one was
 * decoded from a Linux runlist with the words misread. A Linux channel
 * entry word 0 of 0x2a925ef0 decodes under nvgpu's layout as USERD
 * 0x2a925e00, userd_target ncoh, inst_target ncoh, type channel; and
 * the "mystery" TSG w1 = 1 is the channel count.
 * ================================================================== */

#define GA10B_RUNLIST_ENTRY_BYTES       16u
#define GA10B_TSG_TIMESLICE_DEFAULT_US  1024u   /* NVGPU_TSG_TIMESLICE_DEFAULT_US */

void ga10b_runlist_tsg_entry(uint32_t w[4], uint32_t tsgid, uint32_t num_channels,
                             uint32_t timeslice);
void ga10b_runlist_chan_entry(uint32_t w[4], uint32_t chid, uint64_t inst_phys,
                              uint64_t userd_phys, uint32_t runqueue);

/* Runlist PRI registers (hw_runlist_ga10b.h), relative to the runlist
 * PRI base (GR: 0xc00000, device info row 2, trace line 16). */
#define GA10B_RL_CHANNEL_CONFIG         0x004u
#define GA10B_RL_SUBMIT_BASE_LO         0x080u
#define GA10B_RL_SUBMIT_BASE_HI         0x084u
#define GA10B_RL_SUBMIT                 0x088u
#define GA10B_RL_SUBMIT_INFO            0x08cu
#define GA10B_RL_SCHED_DISABLE          0x094u
#define GA10B_RL_PREEMPT                0x098u
#define GA10B_RL_SUBMIT_INFO_PENDING    0x8000u
#define GA10B_RL_PREEMPT_TSG_PENDING    0x100000u
#define GA10B_RL_PREEMPT_TYPE_TSG       0x1000000u
#define GA10B_GR_RUNLIST_PRI_BASE       0xc00000u
#define GA10B_GR_RUNLIST_ID             0u

/* ga10b_runlist_hw_submit field encodings. Trace (mnist line 2):
 * runlist at 0x24a200000 -> lo 0x4a200003, hi 0x2. */
static inline uint32_t ga10b_runlist_base_lo(uint64_t phys)
{
    return (uint32_t)((((uint32_t)phys >> 10) & 0x3fffffu) << 10) | 0x3u;
}
static inline uint32_t ga10b_runlist_base_hi(uint64_t phys)
{
    return (uint32_t)(phys >> 32) & 0xffu;
}
static inline uint32_t ga10b_runlist_submit_val(uint32_t offset, uint32_t length)
{
    return ((offset & 0xffffu) << 16) | (length & 0xffffu);
}
/* runlist_channel_config chram_bar0_offset (trace: 0x00c2000b -> 0xc20000). */
static inline uint32_t ga10b_chram_base_from_config(uint32_t channel_config)
{
    return ((channel_config >> 4) & 0x0fffffffu) << 4;
}

/* CHRAM update values (runlist_chram_channel_update_*_v). */
#define GA10B_CHRAM_ENABLE_CHANNEL      0x2u
#define GA10B_CHRAM_DISABLE_CHANNEL     0x3u
#define GA10B_CHRAM_FORCE_CTX_RELOAD    0x200u
#define GA10B_CHRAM_CLEAR_CHANNEL       0xffffffffu

/* USERMODE doorbell (func_full_phys_offset 0xb80000 + func_doorbell
 * 0x30090) and token (tu104_usermode_doorbell_token: channel_base is 0
 * on native GA10B). */
#define GA10B_USERMODE_DOORBELL         0xbb0090u
static inline uint32_t ga10b_doorbell_token(uint32_t chid, uint32_t runlist_id)
{
    return (chid & 0xfffu) | ((runlist_id & 0x7fu) << 16);
}

/* ==================================================================
 * FIFO init (ga10b_init_fifo_setup_hw + PBDMA interrupt setup)
 * ================================================================== */

/* Constant FIFO programming from nvgpu's first power-on (trace lines
 * 244-368): usermode virtual channel config (doorbells), runlist
 * interrupt vector ids and ctxsw timeouts, PBDMA interrupt clear and
 * enables, userd writeback timer. nvgpu runs this before the GR engine
 * reset; it touches only host (runlist/PBDMA) registers. */
extern const struct ga10b_reg_write ga10b_chan_fifo_init_writes[];
extern const unsigned ga10b_chan_fifo_init_writes_count;
void ga10b_chan_fifo_init(const struct ga10b_gpu_ops *ops);

/* ==================================================================
 * Channel creation
 * ================================================================== */

/* Compute preemption modes (NVGPU_PREEMPTION_MODE_COMPUTE_*). */
enum ga10b_compute_preempt {
    GA10B_PREEMPT_WFI = 1,
    GA10B_PREEMPT_CTA = 2,
    GA10B_PREEMPT_CILP = 4,
};

/* Upper bound on GPFIFO entries (8 bytes each): keeps the ring size
 * well inside 32 bits and the allocation sane. The helper uses 512. */
#define GA10B_CHAN_MAX_GPFIFO_ENTRIES (1u << 20)

struct ga10b_chan_params {
    uint32_t chid;              /* != 0: a zero doorbell token is rejected
                                   by ga10b_validate_handoff */
    uint32_t tsgid;
    uint32_t veid;              /* subcontext; helper's ASYNC subctx is 1 */
    uint32_t runlist_pri_base;  /* GA10B_GR_RUNLIST_PRI_BASE */
    uint32_t runlist_id;        /* GA10B_GR_RUNLIST_ID */
    uint32_t gpfifo_entries;    /* 512 like the helper */
    uint32_t pushbuf_bytes;     /* 65536 like the helper */
    uint32_t timeslice_us;      /* GA10B_TSG_TIMESLICE_DEFAULT_US */
    enum ga10b_compute_preempt compute_preempt;  /* helper: CILP */
    uint64_t eng_method_buffer_va;  /* BAR2 VA; 0 = leave unset */
};

/* Default parameters mirroring the helper's channel. */
void ga10b_chan_default_params(struct ga10b_chan_params *p);

struct ga10b_grctx;           /* ga10b_grctx.h */
struct ga10b_gr_chan_ctx;

struct ga10b_chan {
    struct ga10b_chan_params p;
    struct ga10b_vm vm;
    struct ga10b_dma inst;      /* instance block (RAMFC + RAMIN) */
    struct ga10b_dma userd;     /* one 4 KB page, USERD at offset 0 */
    struct ga10b_dma gpfifo;
    struct ga10b_dma pushbuf;
    struct ga10b_dma sema;      /* one page for semaphore releases */
    struct ga10b_dma runlist;   /* one page */
    uint64_t gpfifo_va;
    uint64_t pushbuf_va;
    uint64_t sema_va;
    uint32_t chram_base;        /* BAR0 offset of CHRAM[0] */
    uint32_t work_submit_token;
    struct ga10b_gr_chan_ctx *gr; /* owned by the caller (ga10b_grctx.h) */
    bool enabled;
};

/* Build the address space, instance block, USERD, GPFIFO, pushbuffer,
 * semaphore page and the GR context (golden image copy via
 * ga10b_grctx_chan_alloc, so `grctx` must hold a golden image), then
 * submit the runlist and enable the channel in CHRAM. `gr` is caller
 * storage for the per-channel GR context. Order follows nvgpu's ioctl
 * sequence except that the GR context is built before the channel
 * goes on the runlist, so no TSG disable/preempt/enable is needed
 * around it (nvgpu_gr_setup_alloc_obj_ctx preempts a TSG that has
 * never run). */
int ga10b_chan_create(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *grctx,
                      const struct ga10b_chan_params *p, struct ga10b_gr_chan_ctx *gr,
                      struct ga10b_chan *ch);

/* Runlist with this channel's TSG (2 entries), submitted and waited on. */
int ga10b_chan_runlist_submit(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch);
/* Empty runlist submit (removes the TSG). */
int ga10b_chan_runlist_clear(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch);
/* CHRAM update for this channel. */
void ga10b_chan_chram_write(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch,
                            uint32_t update);
/* runlist_preempt for this TSG and wait for the pending bit to clear. */
int ga10b_chan_preempt_tsg(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch);
/* Ring the USERMODE doorbell. */
void ga10b_chan_ring_doorbell(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch);

/* Fill the channel part of a handoff (version 2; v3+ fields left zero
 * for the caller) so ga10b_bringup's dispatch paths can use this
 * channel instead of a Linux-made one. */
void ga10b_chan_fill_handoff(const struct ga10b_chan *ch, struct ga10b_channel_handoff *h);

#endif /* GPU_NVIDIA_GA10B_CHAN_H */
