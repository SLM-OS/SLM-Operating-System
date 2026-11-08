/*
 * ga10b_chan.c - GA10B address space, TSG, channel and runlist built by
 * SLM-OS (#1088). See ga10b_chan.h for the overview and sources.
 *
 * Pure code plus ops callbacks: no MMIO base, no allocator, no cache
 * instructions of its own, so it builds on every platform and the
 * kernel tests run it against fake ops under QEMU and on the host.
 */

#include "ga10b_chan.h"
#include "ga10b_grctx.h"
#include "ga10b_channel_handoff.h"

#include "string.h"

/* ---- dma ---- */

int ga10b_dma_alloc(const struct ga10b_gpu_ops *ops, uint32_t bytes,
                    struct ga10b_dma *out)
{
    if (!ops || !out || bytes == 0u) return GA10B_CHAN_ERR_ARG;
    uint32_t pages = (bytes + GA10B_PAGE_SIZE - 1u) / GA10B_PAGE_SIZE;
    uint64_t phys = 0;
    void *cpu = ops->alloc_pages(ops->ctx, pages, &phys);
    if (!cpu || (phys & (GA10B_PAGE_SIZE - 1u)) != 0u) {
        if (cpu) ops->free_pages(ops->ctx, cpu, pages);
        out->cpu = NULL;
        out->phys = 0;
        out->size = 0;
        return GA10B_CHAN_ERR_NOMEM;
    }
    out->cpu = cpu;
    out->phys = phys;
    out->size = pages * GA10B_PAGE_SIZE;
    return GA10B_CHAN_OK;
}

void ga10b_dma_free(const struct ga10b_gpu_ops *ops, struct ga10b_dma *d)
{
    if (!ops || !d || !d->cpu) return;
    ops->free_pages(ops->ctx, d->cpu, d->size / GA10B_PAGE_SIZE);
    d->cpu = NULL;
    d->phys = 0;
    d->size = 0;
}

void ga10b_dma_clean(const struct ga10b_gpu_ops *ops, const struct ga10b_dma *d)
{
    if (ops && d && d->cpu) ops->cache_clean(ops->ctx, d->cpu, d->size);
}

static int poll_clear(const struct ga10b_gpu_ops *ops, uint32_t reg, uint32_t mask,
                      uint32_t timeout_us)
{
    for (uint32_t t = 0; t <= timeout_us; t += 2u) {
        if ((ops->rd32(ops->ctx, reg) & mask) == 0u) return GA10B_CHAN_OK;
        ops->udelay(ops->ctx, 2u);
    }
    return GA10B_CHAN_ERR_TIMEOUT;
}

/* ==================================================================
 * GMMU
 * ================================================================== */

static void *table_alloc(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                         uint64_t *phys)
{
    void *p = ops->alloc_pages(ops->ctx, 1u, phys);
    if (p) {
        memset(p, 0, GA10B_PAGE_SIZE);
        ops->cache_clean(ops->ctx, p, GA10B_PAGE_SIZE);
        vm->table_pages++;
    }
    return p;
}

int ga10b_vm_init(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm)
{
    if (!ops || !vm) return GA10B_CHAN_ERR_ARG;
    memset(vm, 0, sizeof(*vm));
    uint64_t phys = 0;
    void *pdb = table_alloc(ops, vm, &phys);
    if (!pdb) return GA10B_CHAN_ERR_NOMEM;
    vm->pdb.cpu = pdb;
    vm->pdb.phys = phys;
    vm->pdb.size = GA10B_PAGE_SIZE;
    vm->user_next = GA10B_VM_USER_BASE;
    vm->kernel_next = GA10B_VM_KERNEL_BASE;
    return GA10B_CHAN_OK;
}

/* Next-level table from a regular PDE (w[0] aperture bits 2:1). */
static uint64_t pde_next(const uint32_t *e)
{
    if ((e[0] & 0x6u) == 0u) return 0;
    return ((((uint64_t)e[1] << 24) | ((e[0] >> 8) & 0xffffffu)) << 12);
}

/* Small-PT address from a dual PDE0 (small half in words 2/3). */
static uint64_t pde0_small_next(const uint32_t *e)
{
    if ((e[2] & 0x6u) == 0u) return 0;
    return ((((uint64_t)e[3] << 24) | ((e[2] >> 8) & 0xffffffu)) << 12);
}

/* Big-PT address from a dual PDE0 (big half in words 0/1, 256 B units). */
static uint64_t pde0_big_next(const uint32_t *e)
{
    if ((e[0] & 0x6u) == 0u) return 0;
    return ((((uint64_t)e[1] << 28) | ((e[0] >> 4) & 0xfffffffu)) << 8);
}

static int ensure_pde(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                      uint32_t *table, uint32_t idx, uint32_t **next)
{
    uint32_t *e = table + idx * (GA10B_VM_PDE_ENTRY_BYTES / 4u);
    uint64_t phys = pde_next(e);
    if (phys == 0) {
        void *t = table_alloc(ops, vm, &phys);
        if (!t) return GA10B_CHAN_ERR_NOMEM;
        ga10b_pde_encode(phys, e);
        ops->cache_clean(ops->ctx, e, GA10B_VM_PDE_ENTRY_BYTES);
        *next = (uint32_t *)t;
        return GA10B_CHAN_OK;
    }
    *next = (uint32_t *)ops->phys_to_cpu(ops->ctx, phys);
    return *next ? GA10B_CHAN_OK : GA10B_CHAN_ERR_ARG;
}

static int ensure_pde0(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                       uint32_t *table, uint32_t idx, uint32_t **next)
{
    uint32_t *e = table + idx * (GA10B_VM_PDE0_ENTRY_BYTES / 4u);
    /* nvgpu treats a PDE0 with both halves set as corrupt
     * (ga10b_get_pde0_pgsz): each 2 MB of VA is one page size. */
    if (pde0_big_next(e) != 0) return GA10B_CHAN_ERR_VA;
    uint64_t phys = pde0_small_next(e);
    if (phys == 0) {
        void *t = table_alloc(ops, vm, &phys);
        if (!t) return GA10B_CHAN_ERR_NOMEM;
        ga10b_pde0_small_encode(phys, e);
        ops->cache_clean(ops->ctx, e, GA10B_VM_PDE0_ENTRY_BYTES);
        *next = (uint32_t *)t;
        return GA10B_CHAN_OK;
    }
    *next = (uint32_t *)ops->phys_to_cpu(ops->ctx, phys);
    return *next ? GA10B_CHAN_OK : GA10B_CHAN_ERR_ARG;
}

int ga10b_vm_map(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                 uint64_t gpu_va, uint64_t phys, uint64_t size, uint32_t flags)
{
    if (!ops || !vm || !vm->pdb.cpu || size == 0u) return GA10B_CHAN_ERR_ARG;
    if ((gpu_va | phys | size) & (GA10B_PAGE_SIZE - 1u)) return GA10B_CHAN_ERR_VA;
    if (gpu_va + size < gpu_va || gpu_va + size > (1ull << 49)) return GA10B_CHAN_ERR_VA;

    for (uint64_t off = 0; off < size; off += GA10B_PAGE_SIZE) {
        uint64_t va = gpu_va + off;
        uint32_t *l2, *l1, *l0, *pt;
        int rc = ensure_pde(ops, vm, (uint32_t *)vm->pdb.cpu, ga10b_vm_idx_pde3(va), &l2);
        if (rc == GA10B_CHAN_OK) rc = ensure_pde(ops, vm, l2, ga10b_vm_idx_pde2(va), &l1);
        if (rc == GA10B_CHAN_OK) rc = ensure_pde(ops, vm, l1, ga10b_vm_idx_pde1(va), &l0);
        if (rc == GA10B_CHAN_OK) rc = ensure_pde0(ops, vm, l0, ga10b_vm_idx_pde0(va), &pt);
        if (rc != GA10B_CHAN_OK) return rc;
        uint32_t *e = pt + ga10b_vm_idx_pte(va) * (GA10B_VM_PTE_ENTRY_BYTES / 4u);
        ga10b_pte_encode(phys + off, flags, e);
        ops->cache_clean(ops->ctx, e, GA10B_VM_PTE_ENTRY_BYTES);
    }
    return GA10B_CHAN_OK;
}

/* A big-page table is 256 B (32 entries, 8 B each), so one 4 KB page
 * holds 16 of them. free_level frees the page when it reaches the
 * table at offset 0; tables are never given back while the VM lives. */
static void *big_table_alloc(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                             uint64_t *phys)
{
    if (!vm->big_slab_cpu || vm->big_slab_used == GA10B_VM_BIG_TABLES_PER_PAGE) {
        uint64_t p = 0;
        void *c = table_alloc(ops, vm, &p);
        if (!c) return NULL;
        vm->big_slab_cpu = c;
        vm->big_slab_phys = p;
        vm->big_slab_used = 0;
    }
    uint32_t off = vm->big_slab_used++ * GA10B_VM_BIG_TABLE_BYTES;
    *phys = vm->big_slab_phys + off;
    return (uint8_t *)vm->big_slab_cpu + off;
}

static int ensure_pde0_big(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                           uint32_t *table, uint32_t idx, uint32_t **next)
{
    uint32_t *e = table + idx * (GA10B_VM_PDE0_ENTRY_BYTES / 4u);
    if (pde0_small_next(e) != 0) return GA10B_CHAN_ERR_VA;
    uint64_t phys = pde0_big_next(e);
    if (phys == 0) {
        void *t = big_table_alloc(ops, vm, &phys);
        if (!t) return GA10B_CHAN_ERR_NOMEM;
        ga10b_pde0_big_encode(phys, e);
        ops->cache_clean(ops->ctx, e, GA10B_VM_PDE0_ENTRY_BYTES);
        *next = (uint32_t *)t;
        return GA10B_CHAN_OK;
    }
    *next = (uint32_t *)ops->phys_to_cpu(ops->ctx, phys);
    return *next ? GA10B_CHAN_OK : GA10B_CHAN_ERR_ARG;
}

int ga10b_vm_map_big(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                     uint64_t gpu_va, uint64_t phys, uint64_t size, uint32_t flags)
{
    if (!ops || !vm || !vm->pdb.cpu || size == 0u) return GA10B_CHAN_ERR_ARG;
    if ((gpu_va | phys | size) & (GA10B_BIG_PAGE_SIZE - 1u)) return GA10B_CHAN_ERR_VA;
    if (gpu_va + size < gpu_va || gpu_va + size > (1ull << 49)) return GA10B_CHAN_ERR_VA;

    for (uint64_t off = 0; off < size; ) {
        uint64_t va = gpu_va + off;
        uint32_t *l2, *l1, *l0, *pt;
        int rc = ensure_pde(ops, vm, (uint32_t *)vm->pdb.cpu, ga10b_vm_idx_pde3(va), &l2);
        if (rc == GA10B_CHAN_OK) rc = ensure_pde(ops, vm, l2, ga10b_vm_idx_pde2(va), &l1);
        if (rc == GA10B_CHAN_OK) rc = ensure_pde(ops, vm, l1, ga10b_vm_idx_pde1(va), &l0);
        if (rc == GA10B_CHAN_OK) rc = ensure_pde0_big(ops, vm, l0, ga10b_vm_idx_pde0(va), &pt);
        if (rc != GA10B_CHAN_OK) return rc;
        /* Fill this 2 MB's run of entries and clean them together. */
        uint32_t first = ga10b_vm_idx_pte_big(va);
        uint32_t n = 0;
        for (uint32_t i = first; i < GA10B_VM_BIG_PTES && off < size; i++) {
            ga10b_pte_encode(phys + off, flags, pt + i * (GA10B_VM_PTE_ENTRY_BYTES / 4u));
            off += GA10B_BIG_PAGE_SIZE;
            n++;
        }
        ops->cache_clean(ops->ctx, pt + first * (GA10B_VM_PTE_ENTRY_BYTES / 4u),
                         (size_t)n * GA10B_VM_PTE_ENTRY_BYTES);
    }
    return GA10B_CHAN_OK;
}

/* The PDE0 entry covering `va`, or NULL when no PDE1 table reaches it. */
static const uint32_t *pde0_entry(const struct ga10b_gpu_ops *ops, const struct ga10b_vm *vm,
                                  uint64_t va)
{
    const uint32_t *t = (const uint32_t *)vm->pdb.cpu;
    uint32_t idx[3] = { ga10b_vm_idx_pde3(va), ga10b_vm_idx_pde2(va), ga10b_vm_idx_pde1(va) };
    for (unsigned l = 0; l < 3u; l++) {
        uint64_t n = pde_next(t + idx[l] * 2u);
        if (n == 0) return NULL;
        t = (const uint32_t *)ops->phys_to_cpu(ops->ctx, n);
        if (!t) return NULL;
    }
    return t + ga10b_vm_idx_pde0(va) * 4u;
}

void ga10b_vm_unmap(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                    uint64_t gpu_va, uint64_t size)
{
    if (!ops || !vm || !vm->pdb.cpu) return;
    uint64_t end = gpu_va + size;
    for (uint64_t va = gpu_va; va < end; ) {
        uint64_t next_2m = (va + GA10B_VM_PDE0_SPAN) & ~(uint64_t)(GA10B_VM_PDE0_SPAN - 1u);
        uint64_t stop = next_2m < end ? next_2m : end;
        const uint32_t *e = pde0_entry(ops, vm, va);
        if (e) {
            uint64_t big = pde0_big_next(e);
            uint64_t small = pde0_small_next(e);
            uint64_t pt_phys = big ? big : small;
            uint32_t *pt = pt_phys ? (uint32_t *)ops->phys_to_cpu(ops->ctx, pt_phys) : NULL;
            if (pt) {
                uint32_t step = big ? GA10B_BIG_PAGE_SIZE : GA10B_PAGE_SIZE;
                uint32_t first = big ? ga10b_vm_idx_pte_big(va) : ga10b_vm_idx_pte(va);
                uint32_t n = (uint32_t)((stop - va + step - 1u) / step);
                memset(pt + first * 2u, 0, (size_t)n * GA10B_VM_PTE_ENTRY_BYTES);
                ops->cache_clean(ops->ctx, pt + first * 2u,
                                 (size_t)n * GA10B_VM_PTE_ENTRY_BYTES);
            }
        }
        va = stop;
    }
}

uint64_t ga10b_vm_reserve(struct ga10b_vm *vm, uint64_t size, uint64_t align,
                          bool kernel)
{
    if (!vm || size == 0u || align < GA10B_PAGE_SIZE || (align & (align - 1u))) {
        return 0;
    }
    uint64_t *next = kernel ? &vm->kernel_next : &vm->user_next;
    uint64_t limit = kernel ? GA10B_VM_KERNEL_LIMIT : GA10B_VM_USER_LIMIT;
    uint64_t va = (*next + align - 1u) & ~(align - 1u);
    uint64_t len = (size + GA10B_PAGE_SIZE - 1u) & ~(uint64_t)(GA10B_PAGE_SIZE - 1u);
    if (va < *next || va + len < va || va + len > limit) return 0;
    *next = va + len;
    return va;
}

int ga10b_vm_map_dma(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm,
                     const struct ga10b_dma *d, uint32_t flags, bool kernel,
                     uint64_t *gpu_va)
{
    if (!d || !d->cpu || !gpu_va) return GA10B_CHAN_ERR_ARG;
    uint64_t va = ga10b_vm_reserve(vm, d->size, GA10B_PAGE_SIZE, kernel);
    if (va == 0) return GA10B_CHAN_ERR_VA;
    int rc = ga10b_vm_map(ops, vm, va, d->phys, d->size, flags);
    if (rc == GA10B_CHAN_OK) *gpu_va = va;
    return rc;
}

uint64_t ga10b_vm_lookup(const struct ga10b_gpu_ops *ops, const struct ga10b_vm *vm,
                         uint64_t gpu_va)
{
    if (!ops || !vm || !vm->pdb.cpu) return 0;
    const uint32_t *t = (const uint32_t *)vm->pdb.cpu;
    uint32_t idx[3] = { ga10b_vm_idx_pde3(gpu_va), ga10b_vm_idx_pde2(gpu_va),
                        ga10b_vm_idx_pde1(gpu_va) };
    for (unsigned l = 0; l < 3u; l++) {
        uint64_t n = pde_next(t + idx[l] * 2u);
        if (n == 0) return 0;
        t = (const uint32_t *)ops->phys_to_cpu(ops->ctx, n);
        if (!t) return 0;
    }
    const uint32_t *d = t + ga10b_vm_idx_pde0(gpu_va) * 4u;
    uint64_t big = pde0_big_next(d);
    uint64_t pt = big ? big : pde0_small_next(d);
    if (pt == 0) return 0;
    t = (const uint32_t *)ops->phys_to_cpu(ops->ctx, pt);
    if (!t) return 0;
    const uint32_t *e = t + (big ? ga10b_vm_idx_pte_big(gpu_va) : ga10b_vm_idx_pte(gpu_va)) * 2u;
    if ((e[0] & GA10B_GMMU_PTE_VALID) == 0u) return 0;
    uint64_t page = (((uint64_t)(e[1] & 0xffffffu) << 24) | ((e[0] >> 8) & 0xffffffu)) << 12;
    uint64_t mask = big ? (uint64_t)(GA10B_BIG_PAGE_SIZE - 1u) : (uint64_t)(GA10B_PAGE_SIZE - 1u);
    return page | (gpu_va & mask);
}

static void free_level(const struct ga10b_gpu_ops *ops, uint32_t *t, unsigned level)
{
    /* level 0 = PDE3 (PDB), 1 = PDE2, 2 = PDE1, 3 = PDE0, 4 = PT */
    if (level <= 2u) {
        unsigned n = (level == 0u) ? 4u : 512u;
        for (unsigned i = 0; i < n; i++) {
            uint64_t p = pde_next(t + i * 2u);
            if (p) {
                uint32_t *c = (uint32_t *)ops->phys_to_cpu(ops->ctx, p);
                if (c) {
                    free_level(ops, c, level + 1u);
                    ops->free_pages(ops->ctx, c, 1u);
                }
            }
        }
    } else if (level == 3u) {
        for (unsigned i = 0; i < 256u; i++) {
            uint64_t p = pde0_small_next(t + i * 4u);
            if (p) {
                void *c = ops->phys_to_cpu(ops->ctx, p);
                if (c) ops->free_pages(ops->ctx, c, 1u);
            }
            /* Big tables share slab pages: free each page once, at the
             * table at its start (see big_table_alloc). */
            uint64_t b = pde0_big_next(t + i * 4u);
            if (b && (b & (GA10B_PAGE_SIZE - 1u)) == 0u) {
                void *c = ops->phys_to_cpu(ops->ctx, b);
                if (c) ops->free_pages(ops->ctx, c, 1u);
            }
        }
    }
}

void ga10b_vm_destroy(const struct ga10b_gpu_ops *ops, struct ga10b_vm *vm)
{
    if (!ops || !vm || !vm->pdb.cpu) return;
    free_level(ops, (uint32_t *)vm->pdb.cpu, 0u);
    ops->free_pages(ops->ctx, vm->pdb.cpu, 1u);
    memset(vm, 0, sizeof(*vm));
}

/* gm20b_fb_tlb_invalidate (trace lines 17473-17508). */
#define NV_PFB_PRI_MMU_CTRL             0x100c80u
#define NV_PFB_PRI_MMU_INVALIDATE_PDB   0x100cb8u
#define NV_PFB_PRI_MMU_INVALIDATE       0x100cbcu
#define MMU_CTRL_PRI_FIFO_EMPTY         (1u << 15)
#define MMU_CTRL_PRI_FIFO_SPACE(v)      (((v) >> 16) & 0xffu)
#define MMU_INVALIDATE_ALL_VA_TRIGGER   0x80000001u

int ga10b_vm_tlb_invalidate(const struct ga10b_gpu_ops *ops, const struct ga10b_vm *vm)
{
    if (!ops || !vm) return GA10B_CHAN_ERR_ARG;
    unsigned i;
    for (i = 0; i < 1000u; i++) {
        if (MMU_CTRL_PRI_FIFO_SPACE(ops->rd32(ops->ctx, NV_PFB_PRI_MMU_CTRL)) != 0u) break;
        ops->udelay(ops->ctx, 2u);
    }
    if (i == 1000u) return GA10B_CHAN_ERR_TIMEOUT;
    ops->wr32(ops->ctx, NV_PFB_PRI_MMU_INVALIDATE_PDB,
              ga10b_tlb_invalidate_pdb_val(vm->pdb.phys));
    ops->wr32(ops->ctx, NV_PFB_PRI_MMU_INVALIDATE, MMU_INVALIDATE_ALL_VA_TRIGGER);
    for (i = 0; i < 1000u; i++) {
        if (ops->rd32(ops->ctx, NV_PFB_PRI_MMU_CTRL) & MMU_CTRL_PRI_FIFO_EMPTY) {
            return GA10B_CHAN_OK;
        }
        ops->udelay(ops->ctx, 2u);
    }
    return GA10B_CHAN_ERR_TIMEOUT;
}

/* ==================================================================
 * Instance block
 * ================================================================== */

void ga10b_inst_set_magic(uint32_t *w)
{
    w[GA10B_RAMIN_W_ENGINE_FW_MAGIC] = GA10B_RAMIN_ENGINE_FW_MAGIC;
}

void ga10b_inst_init_pdb(uint32_t *w, uint64_t pdb_phys)
{
    w[GA10B_RAMIN_W_PAGE_DIR_BASE_LO] =
        GA10B_RAMIN_TARGET_SYS_NCOH | GA10B_RAMIN_PDB_VOL | GA10B_RAMIN_BIG_PAGE_64K |
        ((uint32_t)pdb_phys & 0xfffff000u) | GA10B_RAMIN_VER2_PT;
    w[GA10B_RAMIN_W_PAGE_DIR_BASE_HI] = (uint32_t)(pdb_phys >> 32);
}

uint32_t ga10b_subctx_pdb_format_word(uint64_t pdb_phys, bool replayable)
{
    /* target ncoh (bits 1:0), vol (2), ver2 (10), 64K big pages (11),
     * pdb[31:12] (ram_in_sc_page_dir_base_lo_0_f), replay tex/gcc (4/5). */
    uint32_t w = GA10B_RAMIN_TARGET_SYS_NCOH | (1u << 2) | (1u << 10) | (1u << 11) |
                 ((uint32_t)pdb_phys & 0xfffff000u);
    if (replayable) w |= (1u << 4) | (1u << 5);
    return w;
}

void ga10b_inst_set_subctxs(uint32_t *w, uint64_t valid_mask, uint64_t pdb_phys,
                            bool replayable)
{
    for (uint32_t i = 0; i < GA10B_MAX_SUBCTX; i++) {
        bool on = ((valid_mask >> i) & 1u) != 0u;
        w[GA10B_RAMIN_W_SC_PDB_LO(i)] = on ? ga10b_subctx_pdb_format_word(pdb_phys, replayable)
                                           : GA10B_RAMIN_SC_TARGET_INVALID;
        w[GA10B_RAMIN_W_SC_PDB_HI(i)] = on ? (uint32_t)(pdb_phys >> 32) : 0u;
        w[GA10B_RAMIN_W_SC_PDB_LO(i) + 2u] = 0u;   /* two unused words per entry */
        w[GA10B_RAMIN_W_SC_PDB_LO(i) + 3u] = 0u;
    }
    w[GA10B_RAMIN_W_SC_PDB_VALID(0u)] = (uint32_t)valid_mask;
    w[GA10B_RAMIN_W_SC_PDB_VALID(32u)] = (uint32_t)(valid_mask >> 32);
}

void ga10b_inst_set_gr_ptr(uint32_t *w, uint64_t gpu_va)
{
    /* engine_cs = wfi (0), wfi_mode = virtual (bit 2), ptr_lo = va[31:12]. */
    w[GA10B_RAMIN_W_ENGINE_WFI_TARGET] = (1u << 2) | ((uint32_t)gpu_va & 0xfffff000u);
    w[GA10B_RAMIN_W_ENGINE_WFI_PTR_HI] = (uint32_t)(gpu_va >> 32) & 0xffu;
}

void ga10b_inst_set_eng_method_buffer(uint32_t *w, uint64_t gpu_va)
{
    w[GA10B_RAMIN_W_ENG_METHOD_LO] = (uint32_t)gpu_va;
    w[GA10B_RAMIN_W_ENG_METHOD_HI] = (uint32_t)(gpu_va >> 32);
}

/* ---- RAMFC ---- */

uint32_t ga10b_pbdma_acquire_val(uint64_t timeout_ms)
{
    uint32_t val = 0x2u | 0x100u;   /* retry_man_2 | retry_exp_2 */
    if (timeout_ms == 0u) return val;
    /* 80% of the watchdog, in ns, in 1024 ns units. */
    uint64_t t = (timeout_ms * ((1000000u * 80u) / 100u)) / 1024u;
    uint32_t exp = 0;
    while (t > 0xffffu && exp <= 0xfu) {
        t >>= 1;
        exp++;
    }
    uint32_t man;
    if (exp > 0xfu) {
        exp = 0xfu;
        man = 0xffffu;
    } else {
        man = (uint32_t)t;
    }
    return val | ((exp & 0xfu) << 11) | ((man & 0xffffu) << 15) | 0x80000000u;
}

static uint32_t ilog2_u32(uint32_t v)
{
    uint32_t r = 0;
    while (v > 1u) {
        v >>= 1;
        r++;
    }
    return r;
}

int ga10b_ramfc_setup(uint32_t *w, const struct ga10b_ramfc_params *p)
{
    if (!w || !p || p->gpfifo_entries < 2u ||
        (p->gpfifo_entries & (p->gpfifo_entries - 1u)) != 0u ||
        p->chid > 0xfffu || p->veid >= GA10B_MAX_SUBCTX) {
        return GA10B_CHAN_ERR_ARG;
    }

    memset(w, 0, GA10B_RAMFC_BYTES);
    w[GA10B_RAMFC_W_GP_BASE] = (uint32_t)p->gpfifo_va & 0xfffffff8u;
    w[GA10B_RAMFC_W_GP_BASE_HI] = ((uint32_t)(p->gpfifo_va >> 32) & 0xffu) |
                                  ((ilog2_u32(p->gpfifo_entries) & 0x1fu) << 16);
    w[GA10B_RAMFC_W_SIGNATURE] = GA10B_GPFIFO_CLASS;
    w[GA10B_RAMFC_W_PB_HEADER] = 0x00400000u | 0x20000000u;   /* first | type_inc */
    w[GA10B_RAMFC_W_SUBDEVICE] = 0x1u | 0x10000000u | 0x20000000u;  /* id 1 | active | dma */
    w[GA10B_RAMFC_W_TARGET] = (p->rleng_id & 0x3u) | 0x10000u | 0x20000u;
    w[GA10B_RAMFC_W_ACQUIRE] = ga10b_pbdma_acquire_val(p->acquire_timeout_ms);
    w[GA10B_RAMFC_W_SET_CHANNEL_INFO] |= ((p->veid & 0x3fu) << 8) | ((p->chid & 0xfffu) << 16);
    w[GA10B_RAMIN_W_ENGINE_WFI_VEID] = p->veid & 0x3fu;
    w[GA10B_RAMFC_W_INTR_NOTIFY] = (p->eng_intr_vector & 0xfffu) | 0x80000000u;
    w[GA10B_RAMFC_W_CONFIG] |= 0x1000u;   /* userd writeback */
    return GA10B_CHAN_OK;
}

void ga10b_userd_init(uint32_t *u)
{
    static const uint8_t words[] = { 16, 17, 18, 19, 22, 23, 24,
                                     GA10B_USERD_W_GP_GET, GA10B_USERD_W_GP_PUT };
    for (unsigned i = 0; i < sizeof(words); i++) {
        u[words[i]] = 0u;
    }
}

/* ---- Runlist ---- */

void ga10b_runlist_tsg_entry(uint32_t w[4], uint32_t tsgid, uint32_t num_channels,
                             uint32_t timeslice)
{
    uint32_t timeout = timeslice;
    uint32_t scale = 0;
    while (timeout > 0xffu) {
        timeout >>= 1;
        scale++;
    }
    if (scale > 0xfu) {
        timeout = 0xffu;
        scale = 0xfu;
    }
    w[0] = 0x1u | (scale << 16) | (timeout << 24);
    w[1] = num_channels & 0xffu;
    w[2] = tsgid & 0xfffu;
    w[3] = 0;
}

void ga10b_runlist_chan_entry(uint32_t w[4], uint32_t chid, uint64_t inst_phys,
                              uint64_t userd_phys, uint32_t runqueue)
{
    w[0] = ((runqueue & 0x1u) << 1) | (0x3u << 4) | (0x3u << 6) |
           ((((uint32_t)userd_phys >> 8) & 0xffffffu) << 8);
    w[1] = (uint32_t)(userd_phys >> 32);
    w[2] = ((uint32_t)inst_phys & 0xfffff000u) | (chid & 0xfffu);
    w[3] = (uint32_t)(inst_phys >> 32);
}

/* ---- FIFO init (generated from the MMIO trace) ---- */

/* Generated by scripts/tools/gen-ga10b-reg-table.py from trace lines
 * 244..368 (--skip ga10b_mc_enable); do not edit by hand.
 * c00300/700/b00/f00: runlist_virtual_channel_cfg(0) |= mask_hw_init |
 *   pending_enable (ga10b_usermode_setup_hw) for the four runlists;
 * c00160/164 ...: runlist interrupt vector ids (ga10b_fifo_runlist_
 *   intr_vectorid_init) and intr_0 enables (ga10b_fifo_enable_intr);
 * c00220/260/2a0 ...: engine ctxsw timeouts (0x186a0, enabled);
 * 04x108/148/170/180: PBDMA interrupt clear and enables;
 * c00028 ...: userd writeback timer (ga10b_fifo_config_userd_
 *   writeback_timer). */
const struct ga10b_reg_write ga10b_chan_fifo_init_writes[] = {
    /* nvgpu_runlist_writel */
    { 0xc00300u, 0x800007ffu },
    { 0xc00700u, 0x800007ffu },
    { 0xc00f00u, 0x800007ffu },
    { 0xc00b00u, 0x800007ffu },
    { 0xc00160u, 0x000000a0u },
    { 0xc00164u, 0x000000e0u },
    { 0xc00100u, 0x00000000u },
    { 0xc00560u, 0x000000a1u },
    { 0xc00564u, 0x000000e1u },
    { 0xc00500u, 0x00000000u },
    { 0xc00d60u, 0x000000a2u },
    { 0xc00d64u, 0x000000e2u },
    { 0xc00d00u, 0x00000000u },
    { 0xc00960u, 0x000000a3u },
    { 0xc00964u, 0x000000e3u },
    { 0xc00900u, 0x00000000u },
    { 0xc00100u, 0x00000001u },
    { 0xc00220u, 0x800186a0u },
    { 0xc00100u, 0x00000002u },
    { 0xc00260u, 0x800186a0u },
    { 0xc00100u, 0x00000004u },
    { 0xc002a0u, 0x800186a0u },
    { 0xc00500u, 0x00000001u },
    { 0xc00620u, 0x800186a0u },
    { 0xc00500u, 0x00000002u },
    { 0xc00500u, 0x00000004u },
    { 0xc00d00u, 0x00000001u },
    { 0xc00e20u, 0x800186a0u },
    { 0xc00d00u, 0x00000002u },
    { 0xc00d00u, 0x00000004u },
    { 0xc00900u, 0x00000001u },
    { 0xc00a20u, 0x800186a0u },
    { 0xc00900u, 0x00000002u },
    { 0xc00900u, 0x00000004u },
    /* ga10b_pbdma_clear_all_intr */
    { 0x040108u, 0xffffffffu },
    { 0x040148u, 0xffffffffu },
    /* ga10b_pbdma_intr_enable */
    { 0x040170u, 0xcfafe000u },
    { 0x040180u, 0x8000001fu },
    /* ga10b_pbdma_clear_all_intr */
    { 0x040908u, 0xffffffffu },
    { 0x040948u, 0xffffffffu },
    /* ga10b_pbdma_intr_enable */
    { 0x040970u, 0xcfafe000u },
    { 0x040980u, 0x8000001fu },
    /* ga10b_pbdma_clear_all_intr */
    { 0x041108u, 0xffffffffu },
    { 0x041148u, 0xffffffffu },
    /* ga10b_pbdma_intr_enable */
    { 0x041170u, 0xcfafe000u },
    { 0x041180u, 0x8000001fu },
    /* ga10b_pbdma_clear_all_intr */
    { 0x041908u, 0xffffffffu },
    { 0x041948u, 0xffffffffu },
    /* ga10b_pbdma_intr_enable */
    { 0x041970u, 0xcfafe000u },
    { 0x041980u, 0x8000001fu },
    /* ga10b_pbdma_clear_all_intr */
    { 0x042108u, 0xffffffffu },
    { 0x042148u, 0xffffffffu },
    /* ga10b_pbdma_intr_enable */
    { 0x042170u, 0xcfafe000u },
    { 0x042180u, 0x8000001fu },
    /* ga10b_pbdma_clear_all_intr */
    { 0x042908u, 0xffffffffu },
    { 0x042948u, 0xffffffffu },
    /* ga10b_pbdma_intr_enable */
    { 0x042970u, 0xcfafe000u },
    { 0x042980u, 0x8000001fu },
    /* nvgpu_runlist_writel */
    { 0xc00100u, 0x00000000u },
    { 0xc00120u, 0x00031007u },
    { 0xc00148u, 0xffffffffu },
    { 0xc00160u, 0x800000a0u },
    { 0xc00164u, 0x000000e0u },
    { 0xc00500u, 0x00000000u },
    { 0xc00520u, 0x00031007u },
    { 0xc00548u, 0xffffffffu },
    { 0xc00560u, 0x800000a1u },
    { 0xc00564u, 0x000000e1u },
    { 0xc00d00u, 0x00000000u },
    { 0xc00d20u, 0x00031007u },
    { 0xc00d48u, 0xffffffffu },
    { 0xc00d60u, 0x800000a2u },
    { 0xc00d64u, 0x000000e2u },
    { 0xc00900u, 0x00000000u },
    { 0xc00920u, 0x00031007u },
    { 0xc00948u, 0xffffffffu },
    { 0xc00960u, 0x800000a3u },
    { 0xc00964u, 0x000000e3u },
    { 0xc00028u, 0x00000064u },
    { 0xc00428u, 0x00000064u },
    { 0xc00c28u, 0x00000064u },
    { 0xc00828u, 0x00000064u },
};
const unsigned ga10b_chan_fifo_init_writes_count =
    sizeof(ga10b_chan_fifo_init_writes) / sizeof(ga10b_chan_fifo_init_writes[0]);

void ga10b_chan_fifo_init(const struct ga10b_gpu_ops *ops)
{
    for (unsigned i = 0; i < ga10b_chan_fifo_init_writes_count; i++) {
        ops->wr32(ops->ctx, ga10b_chan_fifo_init_writes[i].offset,
                  ga10b_chan_fifo_init_writes[i].value);
    }
}

/* ==================================================================
 * Channel
 * ================================================================== */

void ga10b_chan_default_params(struct ga10b_chan_params *p)
{
    memset(p, 0, sizeof(*p));
    p->chid = 1;
    p->tsgid = 1;
    p->veid = 0;
    p->runlist_pri_base = GA10B_GR_RUNLIST_PRI_BASE;
    p->runlist_id = GA10B_GR_RUNLIST_ID;
    p->gpfifo_entries = 512u;
    p->pushbuf_bytes = 65536u;
    p->timeslice_us = GA10B_TSG_TIMESLICE_DEFAULT_US;
    p->compute_preempt = GA10B_PREEMPT_CILP;
    p->eng_method_buffer_va = 0;
}

static void chan_free(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch)
{
    ga10b_dma_free(ops, &ch->runlist);
    ga10b_dma_free(ops, &ch->sema);
    ga10b_dma_free(ops, &ch->pushbuf);
    ga10b_dma_free(ops, &ch->gpfifo);
    ga10b_dma_free(ops, &ch->userd);
    ga10b_dma_free(ops, &ch->inst);
    ga10b_vm_destroy(ops, &ch->vm);
}

int ga10b_chan_runlist_submit(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch)
{
    if (!ops || !ch || !ch->runlist.cpu) return GA10B_CHAN_ERR_ARG;
    uint32_t *rl = (uint32_t *)ch->runlist.cpu;
    memset(rl, 0, 2u * GA10B_RUNLIST_ENTRY_BYTES);
    ga10b_runlist_tsg_entry(rl, ch->p.tsgid, 1u, ch->p.timeslice_us);
    ga10b_runlist_chan_entry(rl + 4, ch->p.chid, ch->inst.phys, ch->userd.phys, 0u);
    ops->cache_clean(ops->ctx, rl, 2u * GA10B_RUNLIST_ENTRY_BYTES);

    uint32_t b = ch->p.runlist_pri_base;
    ops->wr32(ops->ctx, b + GA10B_RL_SUBMIT_BASE_LO, ga10b_runlist_base_lo(ch->runlist.phys));
    ops->wr32(ops->ctx, b + GA10B_RL_SUBMIT_BASE_HI, ga10b_runlist_base_hi(ch->runlist.phys));
    ops->wr32(ops->ctx, b + GA10B_RL_SUBMIT, ga10b_runlist_submit_val(0u, 2u));
    return poll_clear(ops, b + GA10B_RL_SUBMIT_INFO, GA10B_RL_SUBMIT_INFO_PENDING, 200000u);
}

int ga10b_chan_runlist_clear(const struct ga10b_gpu_ops *ops, struct ga10b_chan *ch)
{
    if (!ops || !ch) return GA10B_CHAN_ERR_ARG;
    uint32_t b = ch->p.runlist_pri_base;
    /* ga10b_runlist_hw_submit with count 0 writes only the length. */
    ops->wr32(ops->ctx, b + GA10B_RL_SUBMIT, ga10b_runlist_submit_val(0u, 0u));
    return poll_clear(ops, b + GA10B_RL_SUBMIT_INFO, GA10B_RL_SUBMIT_INFO_PENDING, 200000u);
}

void ga10b_chan_chram_write(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch,
                            uint32_t update)
{
    ops->wr32(ops->ctx, ch->chram_base + ch->p.chid * 4u, update);
}

int ga10b_chan_preempt_tsg(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch)
{
    return ga10b_chan_preempt_tsg_within(ops, ch, 200000u);
}

int ga10b_chan_preempt_tsg_within(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch,
                                  uint32_t timeout_us)
{
    uint32_t b = ch->p.runlist_pri_base;
    ops->wr32(ops->ctx, b + GA10B_RL_PREEMPT,
              (ch->p.tsgid & 0xfffu) | GA10B_RL_PREEMPT_TYPE_TSG);
    return poll_clear(ops, b + GA10B_RL_PREEMPT, GA10B_RL_PREEMPT_TSG_PENDING, timeout_us);
}

int ga10b_chan_preempt_runlist(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch,
                               uint32_t timeout_us)
{
    uint32_t b = ch->p.runlist_pri_base;
    /* nvgpu_fifo_preempt_runlists_for_rc: kicks everything off the
     * runlist and cancels a pending context load. Only the PBDMA half
     * is waited for, as gv11b_fifo_recover does: the engine half may
     * never finish on a hung engine, which the caller resets next. */
    ops->wr32(ops->ctx, b + GA10B_RL_PREEMPT, GA10B_RL_PREEMPT_TYPE_RUNLIST);
    return poll_clear(ops, ch->chram_base + ch->p.chid * 4u, GA10B_CHRAM_ON_PBDMA, timeout_us);
}

uint32_t ga10b_chan_chram_read(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch)
{
    return ops->rd32(ops->ctx, ch->chram_base + ch->p.chid * 4u);
}

void ga10b_chan_sched_enable(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch,
                             bool enable)
{
    ops->wr32(ops->ctx, ch->p.runlist_pri_base + GA10B_RL_SCHED_DISABLE, enable ? 0u : 1u);
}

void ga10b_chan_ring_doorbell(const struct ga10b_gpu_ops *ops, const struct ga10b_chan *ch)
{
    ops->wr32(ops->ctx, GA10B_USERMODE_DOORBELL, ch->work_submit_token);
}

/* RAMFC as SETUP_BIND programs it: GP_GET/GP_PUT 0, no pending method,
 * no semaphore acquire in progress. */
static int chan_ramfc(struct ga10b_chan *ch)
{
    struct ga10b_ramfc_params rp = {
        .gpfifo_va = ch->gpfifo_va,
        .gpfifo_entries = ch->p.gpfifo_entries,
        .chid = ch->p.chid,
        .veid = ch->p.veid,
        .rleng_id = 0,
        .eng_intr_vector = 0,
        .acquire_timeout_ms = 0,    /* WDT disabled before SETUP_BIND */
    };
    return ga10b_ramfc_setup((uint32_t *)ch->inst.cpu, &rp);
}

int ga10b_chan_reset(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *grctx,
                     struct ga10b_chan *ch, uint32_t flags)
{
    if (!ops || !grctx || !ch || !ch->inst.cpu || !ch->userd.cpu || !ch->gr) {
        return GA10B_CHAN_ERR_ARG;
    }
    /* gv11b_fifo_locked_abort_runlist_active_tsgs: the TSG off the
     * runlist first, so the host drops what it still holds of it. */
    if (flags & GA10B_CHAN_RESET_RL_CLEAR) {
        (void)ga10b_chan_runlist_clear(ops, ch);
    }
    /* ga10b_channel_unbind: the CHRAM entry back to empty, faulted bits
     * and pending state included. */
    ga10b_chan_chram_write(ops, ch, GA10B_CHRAM_CLEAR_CHANNEL);
    if (ga10b_grctx_chan_reset(ops, grctx, ch->p.compute_preempt, ch->gr) != 0) {
        return GA10B_CHAN_ERR_HW;
    }
    int rc = chan_ramfc(ch);
    if (rc != GA10B_CHAN_OK) return rc;
    ga10b_dma_clean(ops, &ch->inst);
    ga10b_userd_init((uint32_t *)ch->userd.cpu);
    ga10b_dma_clean(ops, &ch->userd);
    /* Bind again as ga10b_chan_create does, then let the runlist run. */
    rc = ga10b_chan_runlist_submit(ops, ch);
    ga10b_chan_sched_enable(ops, ch, true);
    if (rc != GA10B_CHAN_OK) return rc;
    if (!(flags & GA10B_CHAN_RESET_NO_RELOAD)) {
        ga10b_chan_chram_write(ops, ch, GA10B_CHRAM_FORCE_CTX_RELOAD);
    }
    ga10b_chan_chram_write(ops, ch, GA10B_CHRAM_ENABLE_CHANNEL);
    ch->enabled = true;
    return GA10B_CHAN_OK;
}

int ga10b_chan_create(const struct ga10b_gpu_ops *ops, struct ga10b_grctx *grctx,
                      const struct ga10b_chan_params *p, struct ga10b_gr_chan_ctx *gr,
                      struct ga10b_chan *ch)
{
    if (!ops || !grctx || !p || !gr || !ch) return GA10B_CHAN_ERR_ARG;
    if (p->chid == 0u || p->chid > 0x7ffu || p->tsgid > 0xfffu ||
        p->veid >= GA10B_MAX_SUBCTX || p->gpfifo_entries < 2u ||
        p->gpfifo_entries > GA10B_CHAN_MAX_GPFIFO_ENTRIES ||
        (p->gpfifo_entries & (p->gpfifo_entries - 1u)) != 0u ||
        p->pushbuf_bytes == 0u || p->timeslice_us == 0u) {
        return GA10B_CHAN_ERR_ARG;
    }

    memset(ch, 0, sizeof(*ch));
    ch->p = *p;
    int rc;

    /* ALLOC_AS: a VM with an empty PDB. */
    rc = ga10b_vm_init(ops, &ch->vm);
    if (rc != GA10B_CHAN_OK) return rc;

    /* OPEN_CHANNEL: nvgpu_channel_alloc_inst -> nvgpu_alloc_inst_block
     * (magic value). AS_BIND: gv11b_mm_init_inst_block -> init_pdb. */
    if ((rc = ga10b_dma_alloc(ops, GA10B_INST_BLOCK_BYTES, &ch->inst)) != GA10B_CHAN_OK) {
        goto fail;
    }
    uint32_t *iw = (uint32_t *)ch->inst.cpu;
    ga10b_inst_set_magic(iw);
    ga10b_inst_init_pdb(iw, ch->vm.pdb.phys);
    /* BIND_CHANNEL_EX: gv11b_tsg_bind_channel_eng_method_buffers. The
     * buffer lives in the BAR2 VM in nvgpu; SLM-OS does not bind BAR2,
     * so it is set only when the caller provides one. */
    if (p->eng_method_buffer_va) {
        ga10b_inst_set_eng_method_buffer(iw, p->eng_method_buffer_va);
    }

    /* SETUP_BIND (usermode): USERD + GPFIFO from the caller's dmabufs. */
    if ((rc = ga10b_dma_alloc(ops, GA10B_PAGE_SIZE, &ch->userd)) != GA10B_CHAN_OK ||
        (rc = ga10b_dma_alloc(ops, p->gpfifo_entries * 8u, &ch->gpfifo)) != GA10B_CHAN_OK ||
        (rc = ga10b_dma_alloc(ops, p->pushbuf_bytes, &ch->pushbuf)) != GA10B_CHAN_OK ||
        (rc = ga10b_dma_alloc(ops, GA10B_PAGE_SIZE, &ch->sema)) != GA10B_CHAN_OK ||
        (rc = ga10b_dma_alloc(ops, GA10B_PAGE_SIZE, &ch->runlist)) != GA10B_CHAN_OK) {
        goto fail;
    }
    ga10b_userd_init((uint32_t *)ch->userd.cpu);
    ga10b_dma_clean(ops, &ch->userd);
    ga10b_dma_clean(ops, &ch->gpfifo);
    ga10b_dma_clean(ops, &ch->pushbuf);
    ga10b_dma_clean(ops, &ch->sema);

    /* User buffers are mapped like MAP_BUFFER_EX without the CACHEABLE
     * flag (the helper passes flags 0): not GPU-L2 cacheable. */
    if ((rc = ga10b_vm_map_dma(ops, &ch->vm, &ch->gpfifo, 0u, false, &ch->gpfifo_va)) != GA10B_CHAN_OK ||
        (rc = ga10b_vm_map_dma(ops, &ch->vm, &ch->pushbuf, 0u, false, &ch->pushbuf_va)) != GA10B_CHAN_OK ||
        (rc = ga10b_vm_map_dma(ops, &ch->vm, &ch->sema, 0u, false, &ch->sema_va)) != GA10B_CHAN_OK) {
        goto fail;
    }

    if ((rc = chan_ramfc(ch)) != GA10B_CHAN_OK) goto fail;

    /* channel.bind -> gv11b_tsg_add_subctx_channel_hw: subctx PDB array
     * and valid mask (not replayable: SETUP_BIND without the flag). */
    ga10b_inst_set_subctxs(iw, 1ull << p->veid, ch->vm.pdb.phys, false);

    /* ALLOC_OBJ_CTX + SET_PREEMPTION_MODE: GR context from the golden
     * image, commit the subctx header pointer into the inst block. */
    ch->gr = gr;
    rc = ga10b_grctx_chan_alloc(ops, grctx, &ch->vm, iw, p->veid, p->compute_preempt, gr);
    if (rc != 0) {
        rc = GA10B_CHAN_ERR_HW;
        goto fail;
    }
    ga10b_dma_clean(ops, &ch->inst);

    rc = ga10b_vm_tlb_invalidate(ops, &ch->vm);
    if (rc != GA10B_CHAN_OK) goto fail_gr;

    ch->chram_base = ga10b_chram_base_from_config(
        ops->rd32(ops->ctx, p->runlist_pri_base + GA10B_RL_CHANNEL_CONFIG));
    ch->work_submit_token = ga10b_doorbell_token(p->chid, p->runlist_id);

    /* nvgpu_channel_update_runlist (SETUP_BIND), then channel enable
     * (channel.bind / TSG_ENABLE). */
    rc = ga10b_chan_runlist_submit(ops, ch);
    if (rc != GA10B_CHAN_OK) goto fail_gr;
    ga10b_chan_chram_write(ops, ch, GA10B_CHRAM_ENABLE_CHANNEL);
    ch->enabled = true;
    return GA10B_CHAN_OK;

fail_gr:
    ga10b_grctx_chan_free(ops, gr);
fail:
    chan_free(ops, ch);
    return rc;
}

void ga10b_chan_fill_handoff(const struct ga10b_chan *ch, struct ga10b_channel_handoff *h)
{
    memset(h, 0, sizeof(*h));
    h->magic = GA10B_CHANNEL_HANDOFF_MAGIC;
    h->version = 2;
    h->channel_id = ch->p.chid;
    h->tsg_id = ch->p.tsgid;
    h->userd_phys = ch->userd.phys;
    h->userd_gp_put_offset = GA10B_USERD_W_GP_PUT * 4u;
    h->userd_gp_get_offset = GA10B_USERD_W_GP_GET * 4u;
    h->gpfifo_phys = ch->gpfifo.phys;
    h->gpfifo_gpu_va = ch->gpfifo_va;
    h->gpfifo_entries = ch->p.gpfifo_entries;
    h->gpfifo_entry_size = 8u;
    h->pushbuf_phys = ch->pushbuf.phys;
    h->pushbuf_gpu_va = ch->pushbuf_va;
    h->pushbuf_size = ch->pushbuf.size;
    h->semaphore_phys = ch->sema.phys;
    h->semaphore_gpu_va = ch->sema_va;
    h->inst_block_phys = ch->inst.phys;
    h->initial_gp_put = 0;
    h->initial_gp_get = 0;
    h->work_submit_token = ch->work_submit_token;
}
