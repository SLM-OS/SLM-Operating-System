/*
 * slm_prog.c - Per-session GPU program (#475). See slm_prog.h and
 * docs/adr/0003-slm-gpu-graph.md.
 *
 * Shape: everything above struct slm_prog_backend is host-testable with
 * fake ops (kernel/tests/test_slm_prog.c). The Jetson backend at the
 * bottom is the only part that touches hardware.
 *
 * Page-table cost: mapping 1.1 GB with 4 KB PTEs takes about 2.2 MB of
 * tables and 280K PTEs, and a forward pass walks the whole file per
 * token, so a 4 KB TLB would miss constantly. 64 KB PTEs need 140 KB.
 * Boot models and PMM allocations of 64 KB or more are aligned for it.
 * Scattered memory (slm xload's kbuf) falls back to one 4 KB PTE per
 * page, which works and costs both of those.
 */
#include "slm_prog.h"

#include "gpu_handoff.h"
#include "slm_kernels_gen.h"
#include "nvidia/ga10b_chan.h"
#include "nvidia/ga10b_prog.h"
#include "nvidia/ga10b_qmd.h"
#include "string.h"
#include "uart.h"

/* ---- Kernels ----
 *
 * scripts/cuda/slm/ is the source of the ABI. gen-slm-kernel-table.py checks
 * its manifest's offsets against the writes below when the kernel is built,
 * and supplies registers and shared memory. All kernels take block (32,1,1). */

struct slm_sass_info {
    const char *name;
    uint32_t tier;
    uint32_t regs;
    uint32_t smem;
    uint32_t barriers;
};

static const struct slm_sass_info g_sass[SLM_SASS_COUNT] = { SLM_SASS_TABLE };

static inline void put64(uint8_t *a, uint32_t cbuf_off, uint64_t v)
{
    memcpy(a + (cbuf_off - GA10B_PROG_CBUF_ARGS_BASE), &v, 8);
}

static inline void put32(uint8_t *a, uint32_t cbuf_off, uint32_t v)
{
    memcpy(a + (cbuf_off - GA10B_PROG_CBUF_ARGS_BASE), &v, 4);
}

enum mm_variant { MM_SIMT, MM_B1, MM_B8, MM_BS, MM_B64 };

static enum mm_variant mm_variant_of(uint32_t sass)
{
    switch (sass) {
    case SLM_SASS_MM_Q4K_SIMT: case SLM_SASS_MM_Q6K_SIMT: return MM_SIMT;
    case SLM_SASS_MM_Q4K_HMMA_B1: case SLM_SASS_MM_Q6K_HMMA_B1: return MM_B1;
    case SLM_SASS_MM_Q4K_HMMA_BS: case SLM_SASS_MM_Q6K_HMMA_BS: return MM_BS;
    case SLM_SASS_MM_Q4K_HMMA_B64: case SLM_SASS_MM_Q6K_HMMA_B64: return MM_B64;
    default: return MM_B8;
    }
}

/* Decode: B = 1 kernels. Prefill: b8 to 8 rows; past that x in shared memory
 * (bs) when N is big enough to fill the SMs, otherwise b8 split per x tile.
 * The same choice as scripts/cuda/harness/slm_forward.cu add_mm. */
int slm_prog_op_sass(const struct slm_prog_op *op)
{
    if (!op) return -1;
    bool simt = (op->scal[1] & SLM_PROG_MM_SIMT) != 0u;
    uint32_t rows = op->rows;
    bool big = op->dim[1] > 256u;
    switch (op->kernel) {
    case SLM_PROG_K_RMSNORM: return SLM_SASS_RMSNORM_W8;
    case SLM_PROG_K_ROPE: return SLM_SASS_ROPE;
    case SLM_PROG_K_SWIGLU: return SLM_SASS_SWIGLU;
    case SLM_PROG_K_GQA: return SLM_SASS_GQA_W8;
    case SLM_PROG_K_ADD: return SLM_SASS_ADD;
    case SLM_PROG_K_EMBED_Q4K: return SLM_SASS_EMBED_Q4K;
    case SLM_PROG_K_EMBED_Q6K: return SLM_SASS_EMBED_Q6K;
    case SLM_PROG_K_MM_Q4K:
        return simt ? SLM_SASS_MM_Q4K_SIMT : rows == 1u ? SLM_SASS_MM_Q4K_HMMA_B1
             : rows <= 8u ? SLM_SASS_MM_Q4K_HMMA_B8
             : big ? SLM_SASS_MM_Q4K_HMMA_BS : SLM_SASS_MM_Q4K_HMMA_B8;
    case SLM_PROG_K_MM_Q6K:
        return simt ? SLM_SASS_MM_Q6K_SIMT : rows == 1u ? SLM_SASS_MM_Q6K_HMMA_B1
             : rows <= 8u ? SLM_SASS_MM_Q6K_HMMA_B8
             : big ? SLM_SASS_MM_Q6K_HMMA_BS : SLM_SASS_MM_Q6K_HMMA_B8;
    default:
        return -1;
    }
}

/* Warps per CTA of a B = 1 matmul: enough tiles to fill the SMs, never more
 * warps than weight blocks. As mm_nw in the harness. */
static uint32_t mm_nw(uint32_t k, uint32_t n)
{
    uint32_t grid = (n + 14u) / 15u;
    uint32_t want = (200u + grid - 1u) / grid;
    uint32_t nw = k / 256u < 8u ? k / 256u : 8u;
    if (want < nw) nw = want;
    return nw ? nw : 1u;
}

/* Largest element count a launch can cover: grid fields are 32-bit. */
static bool total_of(const struct slm_prog_op *op, uint32_t *total)
{
    uint64_t t = (uint64_t)op->rows * op->dim[0];
    if (t == 0u || t > 0x7FFFFFFFu) return false;
    *total = (uint32_t)t;
    return true;
}

int slm_prog_encode(const struct slm_prog_op *op, uint32_t sass, uint64_t shader_va,
                    struct ga10b_prog_launch *l, uint8_t *args)
{
    if (!op || !l || !args || shader_va == 0u || sass >= SLM_SASS_COUNT || op->rows == 0u) {
        return -1;
    }
    memset(l, 0, sizeof(*l));
    memset(args, 0, GA10B_PROG_CBUF_ARGS_MAX);
    l->shader_gpu_va = shader_va;
    l->args = args;
    l->register_count_v = g_sass[sass].regs;
    l->smem_size_bytes = g_sass[sass].smem;
    l->barrier_count = g_sass[sass].barriers;
    l->block_x = 32;
    l->block_y = 1;
    l->block_z = 1;
    l->grid_y = 1;
    l->grid_z = 1;

    uint32_t rows = op->rows;
    uint32_t total = 0;
    switch (op->kernel) {
    case SLM_PROG_K_MM_Q4K:
    case SLM_PROG_K_MM_Q6K: {
        uint32_t k = op->dim[0], n = op->dim[1], flags = op->scal[0];
        enum mm_variant v = mm_variant_of(sass);
        if (k == 0u || n == 0u || !op->ptr[0] || !op->ptr[1] || !op->ptr[2] ||
            (op->ptr[0] & 15u) || (v == MM_B1 && rows != 1u) ||
            (rows > 64u && v != MM_SIMT)) {
            return -1;
        }
        l->grid_x = (n + 14u) / 15u;    /* a tile holds 15 weight rows */
        switch (v) {
        case MM_SIMT:
            l->grid_x = n;
            l->grid_y = rows;
            break;
        case MM_B1: {
            uint32_t nw = mm_nw(k, n);
            l->block_y = nw;
            flags |= nw << 8;
            break;
        }
        case MM_BS:
            l->grid_x = (n + 119u) / 120u;
            l->block_y = 8;
            break;
        case MM_B64:
            /* One CTA per (tile, x tile): flags SPLIT_XT, the whole K as the range. */
            l->grid_y = (rows + 7u) >> 3;
            flags |= SLM_PROG_MM_SPLIT_XT | ((k / 256u) << 16);
            break;
        case MM_B8:
            /* Past 8 rows: one CTA per (tile, x tile of 8 rows). */
            if (rows > 8u) {
                l->grid_y = (rows + 7u) >> 3;
                flags |= SLM_PROG_MM_SPLIT_XT;
            }
            break;
        }
        put64(args, 0x160, op->ptr[0]);
        put64(args, 0x168, op->ptr[1]);
        put64(args, 0x170, op->ptr[2]);
        put64(args, 0x178, op->ptr[3]);     /* bias: null when absent */
        put32(args, 0x180, k);
        put32(args, 0x184, n);
        put32(args, 0x188, rows);
        put32(args, 0x18C, flags);
        l->args_len = 0x30;
        return 0;
    }
    case SLM_PROG_K_EMBED_Q4K:
    case SLM_PROG_K_EMBED_Q6K:
        if (op->dim[0] == 0u || !op->ptr[0] || !op->ptr[1] || !op->ptr[2]) return -1;
        put64(args, 0x160, op->ptr[0]);
        put64(args, 0x168, op->ptr[1]);
        put64(args, 0x170, op->ptr[2]);
        put32(args, 0x178, op->dim[0]);
        put32(args, 0x17C, rows);
        l->args_len = 0x20;
        l->grid_x = rows;
        return 0;
    case SLM_PROG_K_RMSNORM:
        if (op->dim[0] == 0u || (op->dim[0] & 31u) || !op->ptr[0] || !op->ptr[1] || !op->ptr[2]) {
            return -1;
        }
        put64(args, 0x160, op->ptr[0]);
        put64(args, 0x168, op->ptr[1]);
        put64(args, 0x170, op->ptr[2]);
        put32(args, 0x178, op->dim[0]);
        put32(args, 0x17C, rows);
        put32(args, 0x180, op->scal[0]);
        l->args_len = 0x24;
        l->grid_x = rows;
        l->block_y = 8;
        return 0;
    case SLM_PROG_K_ADD:
    case SLM_PROG_K_SWIGLU:
        if (!total_of(op, &total) || !op->ptr[0] || !op->ptr[1] || !op->ptr[2]) return -1;
        put64(args, 0x160, op->ptr[0]);
        put64(args, 0x168, op->ptr[1]);
        put64(args, 0x170, op->ptr[2]);
        put32(args, 0x178, total);
        l->args_len = 0x1C;
        l->grid_x = (total + 31u) / 32u;
        return 0;
    case SLM_PROG_K_ROPE:
        if (op->dim[0] == 0u || op->dim[1] == 0u || (op->dim[1] & 31u) ||
            !op->ptr[0] || !op->ptr[1] || !op->ptr[2] || !op->ptr[3]) {
            return -1;
        }
        put64(args, 0x160, op->ptr[0]);
        put64(args, 0x168, op->ptr[1]);
        put64(args, 0x170, op->ptr[2]);
        put64(args, 0x178, op->ptr[3]);
        put32(args, 0x180, op->dim[0]);
        put32(args, 0x184, op->dim[1]);
        put32(args, 0x188, rows);
        l->args_len = 0x2C;
        l->grid_x = op->dim[0];
        l->grid_y = rows;
        return 0;
    case SLM_PROG_K_GQA:
        if (op->dim[0] == 0u || op->dim[1] == 0u || op->dim[2] == 0u || (op->dim[2] & 31u) ||
            op->dim[2] > 256u || !op->ptr[0] || !op->ptr[1] || !op->ptr[2] || !op->ptr[3] ||
            !op->ptr[4]) {
            return -1;
        }
        put64(args, 0x160, op->ptr[0]);
        put64(args, 0x168, op->ptr[1]);
        put64(args, 0x170, op->ptr[2]);
        put64(args, 0x178, op->ptr[3]);
        put64(args, 0x180, op->ptr[4]);
        put32(args, 0x188, op->dim[0]);
        put32(args, 0x18C, op->dim[1]);
        put32(args, 0x190, op->dim[2]);
        put32(args, 0x194, rows);
        l->args_len = 0x38;
        l->grid_x = op->dim[0];
        l->grid_y = rows;
        l->block_y = 8;
        return 0;
    default:
        return -1;
    }
}

/* ---- Sessions ---- */

struct prog_shape {
    struct ga10b_prog p;
    bool used;
};

struct prog_session {
    bool open;
    bool dead;
    bool close_pending;     /* closed while the lock was held elsewhere */
    const struct slm_prog_backend *be;
    struct ga10b_dma data;
    struct ga10b_dma control;
    uint64_t data_va, control_va;
    uint64_t data_va_size, control_va_size;
    uint8_t *step_cpu;
    uint32_t step_bytes;
    int wmap;
    struct prog_shape shapes[SLM_PROG_SHAPES];
};

/* A weights mapping outlives sessions: the same model is often launched
 * again, and a boot model's pages never go away. Owned pages can be freed
 * by `slm unload`, so an owned mapping is dropped with its last session.
 * VA is bump-allocated with no way back, so ranges are kept for reuse. */
struct prog_wmap {
    const void *cpu;
    uint64_t bytes;
    uint64_t va;
    uint64_t va_size;
    uint32_t refs;
    bool big;
    bool mapped;
};

#define WMAP_MAX 4
#define VA_FREE_MAX 16

struct prog_va_range {
    uint64_t va;
    uint64_t size;
    bool big;       /* its PDE0s hold big-page tables */
};

static struct prog_session g_sessions[SLM_PROG_MAX_SESSIONS];
static struct prog_wmap g_wmaps[WMAP_MAX];
static struct prog_va_range g_va_free[VA_FREE_MAX];
static const struct slm_prog_backend *g_backend_override;
static uint32_t g_leaked_sessions;

/* ---- Lock ----
 *
 * One lock covers the tables above, the shared GPU VM's page tables (as far
 * as this file writes them) and the one program in flight: run_step holds
 * it from submit to the end of its wait, so the in-flight flag in slm_ffi.c
 * always belongs to the step that set it (the shell and slm-runner each
 * have a session) and slm_gpu_prog_finished can only clear its own.
 *
 * Not a spinlock: the holder waits for the GPU with IRQs on and yields, and
 * may be preempted. A waiter yields too (be->relax), never spins with IRQs
 * masked (#1093): with IRQs off it only tries once. A wait is bounded at
 * one step timeout plus a second, which no healthy holder comes near; past
 * that the caller gets SLM_PROG_ERR_BUSY. slm_prog_close can't fail, so a
 * close that can't take the lock marks the session and the holder frees it
 * on unlock. */
static uint32_t g_lock;

#define PROG_LOCK_WAIT_US (((uint64_t)SLM_PROG_STEP_TIMEOUT_MS + 1000u) * 1000u)

static bool lock_try(void)
{
    return __atomic_exchange_n(&g_lock, 1u, __ATOMIC_ACQUIRE) == 0u;
}

static int prog_lock(const struct slm_prog_backend *be)
{
    if (lock_try()) return 0;
    if (!be || (be->irqs_enabled && !be->irqs_enabled())) return SLM_PROG_ERR_BUSY;
    uint64_t t0 = be->now_us();
    for (;;) {
        be->relax();
        if (lock_try()) return 0;
        if (be->now_us() - t0 >= PROG_LOCK_WAIT_US) return SLM_PROG_ERR_BUSY;
    }
}

static void session_free(struct prog_session *s);

static void prog_unlock(void)
{
    for (;;) {
        for (unsigned i = 0; i < SLM_PROG_MAX_SESSIONS; i++) {
            struct prog_session *s = &g_sessions[i];
            if (s->open && __atomic_load_n(&s->close_pending, __ATOMIC_ACQUIRE)) {
                session_free(s);
            }
        }
        __atomic_store_n(&g_lock, 0u, __ATOMIC_RELEASE);
        /* A close that failed to take the lock after the scan above: take it
         * back and free that session, or leave it to whoever has it now. */
        bool pending = false;
        for (unsigned i = 0; i < SLM_PROG_MAX_SESSIONS; i++) {
            if (__atomic_load_n(&g_sessions[i].close_pending, __ATOMIC_ACQUIRE)) pending = true;
        }
        if (!pending || !lock_try()) return;
    }
}

static const struct slm_prog_backend *jetson_backend(void);
static const char *platform_reason(void);

static const struct slm_prog_backend *backend(void)
{
    return g_backend_override ? g_backend_override : jetson_backend();
}

void slm_prog_backend_set(const struct slm_prog_backend *b)
{
    g_backend_override = b;
    g_lock = 0u;
    g_leaked_sessions = 0u;
    /* Tests run against several fake VMs: forget what the last one held. */
    memset(g_wmaps, 0, sizeof(g_wmaps));
    memset(g_va_free, 0, sizeof(g_va_free));
    memset(g_sessions, 0, sizeof(g_sessions));
}

/* Ranges are 2 MB aligned and whole, so none shares a PDE0 with another.
 * A PDE0 keeps the page size it first had (tables are never given back), so
 * a free range is only reused for the same size and the same page size. */
static uint64_t va_get(const struct slm_prog_backend *be, uint64_t size, bool big)
{
    for (unsigned i = 0; i < VA_FREE_MAX; i++) {
        if (g_va_free[i].va != 0u && g_va_free[i].size == size && g_va_free[i].big == big) {
            uint64_t va = g_va_free[i].va;
            g_va_free[i].va = 0u;
            return va;
        }
    }
    return ga10b_vm_reserve(be->vm, size, GA10B_VM_PDE0_SPAN, false);
}

static void va_put(uint64_t va, uint64_t size, bool big)
{
    for (unsigned i = 0; i < VA_FREE_MAX; i++) {
        if (g_va_free[i].va == 0u) {
            g_va_free[i].va = va;
            g_va_free[i].size = size;
            g_va_free[i].big = big;
            return;
        }
    }
    /* Table full: the VA stays unused. 128 GB of user VA allows it. */
}

static uint64_t span_round(uint64_t n)
{
    return (n + GA10B_VM_PDE0_SPAN - 1u) & ~(uint64_t)(GA10B_VM_PDE0_SPAN - 1u);
}

static uint64_t big_round(uint64_t n)
{
    return (n + GA10B_BIG_PAGE_SIZE - 1u) & ~(uint64_t)(GA10B_BIG_PAGE_SIZE - 1u);
}

static uint32_t rd_table_pages(const struct slm_prog_backend *be)
{
    return be->vm->table_pages;
}

/* Map a weights file. Contiguous and 64 KB aligned: big pages, rounded up
 * to 64 KB (read-only, and only ever read in bounds). Otherwise 4 KB per
 * page, each translated on its own. Returns 0 or SLM_PROG_ERR_*. */
static int map_weights_fresh(const struct slm_prog_backend *be, struct prog_wmap *w)
{
    uint64_t bytes = w->bytes;
    uint64_t va_size = span_round(bytes);
    uint32_t flags = GA10B_MAP_CACHEABLE | GA10B_MAP_RO;
    uint64_t phys = be->virt_to_phys(w->cpu);
    w->big = phys != 0u && be->is_contiguous(w->cpu, bytes) &&
             (phys & (GA10B_BIG_PAGE_SIZE - 1u)) == 0u;
    const uint8_t *c = (const uint8_t *)w->cpu;
    if (!w->big && ((uintptr_t)c & (GA10B_PAGE_SIZE - 1u)) != 0u) return SLM_PROG_ERR_ARG;
    uint64_t va = va_get(be, va_size, w->big);
    if (va == 0u) return SLM_PROG_ERR_MAP;
    int rc;
    if (w->big) {
        rc = ga10b_vm_map_big(be->ops, be->vm, va, phys, big_round(bytes), flags);
    } else {
        rc = GA10B_CHAN_OK;
        for (uint64_t off = 0; off < bytes && rc == GA10B_CHAN_OK; off += GA10B_PAGE_SIZE) {
            uint64_t ph = be->virt_to_phys(c + off);
            rc = ph ? ga10b_vm_map(be->ops, be->vm, va + off, ph, GA10B_PAGE_SIZE, flags)
                    : GA10B_CHAN_ERR_VA;
        }
    }
    if (rc != GA10B_CHAN_OK) {
        /* Pull down whatever part was written. */
        ga10b_vm_unmap(be->ops, be->vm, va, w->big ? big_round(bytes) : bytes);
        va_put(va, va_size, w->big);
        return rc == GA10B_CHAN_ERR_NOMEM ? SLM_PROG_ERR_NOMEM : SLM_PROG_ERR_MAP;
    }
    /* The loader may have left dirty lines (a copy into PMM pages); GPU reads
     * go to DRAM. Boot models were cleaned by the stub, this is then a no-op
     * walk. Once per mapping, not per session. */
    be->ops->cache_clean(be->ops->ctx, w->cpu, bytes);
    w->va = va;
    w->va_size = va_size;
    w->mapped = true;
    return 0;
}

static int wmap_get(const struct slm_prog_backend *be, const void *cpu, uint64_t bytes, int *idx)
{
    int free_slot = -1;
    for (int i = 0; i < WMAP_MAX; i++) {
        struct prog_wmap *w = &g_wmaps[i];
        if (w->mapped && w->cpu == cpu && w->bytes == bytes) {
            w->refs++;
            *idx = i;
            return 0;
        }
        if (!w->mapped && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        /* Every slot holds a mapping. Drop one nobody uses. */
        for (int i = 0; i < WMAP_MAX; i++) {
            struct prog_wmap *w = &g_wmaps[i];
            if (w->refs == 0u) {
                ga10b_vm_unmap(be->ops, be->vm, w->va, w->big ? big_round(w->bytes) : w->bytes);
                va_put(w->va, w->va_size, w->big);
                w->mapped = false;
                free_slot = i;
                break;
            }
        }
        if (free_slot < 0) return SLM_PROG_ERR_NOMEM;
    }
    struct prog_wmap *w = &g_wmaps[free_slot];
    memset(w, 0, sizeof(*w));
    w->cpu = cpu;
    w->bytes = bytes;
    int rc = map_weights_fresh(be, w);
    if (rc != 0) {
        memset(w, 0, sizeof(*w));
        return rc;
    }
    w->refs = 1;
    *idx = free_slot;
    return 0;
}

/* Sessions keep a mapping alive through refs. A mapping at zero refs
 * stays in place for the next launch only if its pages cannot go away:
 * the registry's owned pages are freed on unload, boot-model pages are
 * not. Telling them apart needs the registry, so every mapping is
 * dropped with its last session; relaunch costs a few ms of PTE writes. */
static void wmap_put(const struct slm_prog_backend *be, int idx)
{
    struct prog_wmap *w = &g_wmaps[idx];
    if (w->refs > 0u) w->refs--;
    if (w->refs == 0u && w->mapped) {
        ga10b_vm_unmap(be->ops, be->vm, w->va, w->big ? big_round(w->bytes) : w->bytes);
        va_put(w->va, w->va_size, w->big);
        memset(w, 0, sizeof(*w));
    }
}

static struct prog_session *session(int32_t h)
{
    if (h < 0 || h >= SLM_PROG_MAX_SESSIONS || !g_sessions[h].open) return NULL;
    return &g_sessions[h];
}

static uint32_t align256(uint32_t v)
{
    return (v + 255u) & ~255u;
}

/* Only decode steps are profiled, so only they carry the marked copy. */
static uint32_t shape_marks(unsigned shape)
{
    return shape == 0u ? GA10B_PROG_F_MARKS : 0u;
}

static uint64_t control_bytes(const struct slm_prog_open_args *a)
{
    uint64_t total = align256(a->step_bytes);
    uint32_t f = (a->flags & SLM_PROG_FLAG_PROFILE) ? GA10B_PROG_F_PROFILE : 0u;
    /* Room for the injected acquire in the sizes below as well. */
    f |= (a->flags & SLM_PROG_FLAG_INJECT_HANG) ? GA10B_PROG_F_INJECT_HANG : 0u;
    for (unsigned s = 0; s < SLM_PROG_SHAPES; s++) {
        if (a->max_launches[s] == 0u) continue;
        uint32_t pb, q, c, se;
        ga10b_prog_sizes(a->max_launches[s], f | shape_marks(s), &pb, &q, &c, &se);
        total += (uint64_t)align256(pb) + align256(q) + align256(c) + align256(se);
    }
    return (total + GA10B_PAGE_SIZE - 1u) & ~(uint64_t)(GA10B_PAGE_SIZE - 1u);
}

uint32_t slm_prog_leaked_sessions(void)
{
    return g_leaked_sessions;
}

static void session_free(struct prog_session *s)
{
    const struct slm_prog_backend *be = s->be;
    /* A dead session's last step timed out or its evict failed: the GPU may
     * still be running it, and L2 may hold its dirty lines. Either can land
     * on these pages after they are reused (the CPU KV cache the fallback
     * allocates next, say). Nothing can quiesce a channel in that state, so
     * its arena, control pages and VA ranges are leaked on purpose; the
     * PTEs still go, so a late GPU access faults instead. */
    bool leak = s->dead;
    if (s->data.cpu) {
        if (s->data_va) {
            ga10b_vm_unmap(be->ops, be->vm, s->data_va, big_round(s->data.size));
        }
    }
    if (s->control.cpu && s->control_va) {
        ga10b_vm_unmap(be->ops, be->vm, s->control_va, s->control.size);
    }
    if (s->wmap >= 0) wmap_put(be, s->wmap);
    /* Unmapped PTEs are only gone once the TLB says so. If that fails
     * the pages stay allocated rather than reused under a live walk. */
    if (be->tlb_invalidate && be->tlb_invalidate() != 0) {
        uart_puts("[slm-prog] TLB invalidate failed on close: leaking the arena\n");
        g_leaked_sessions++;
    } else if (leak) {
        uart_printf("[slm-prog] GPU step failed: leaking this session's %llu KB arena "
                    "(the GPU may still write it)\n",
                    (unsigned long long)((s->data.size + s->control.size) / 1024u));
        g_leaked_sessions++;
    } else {
        if (s->data.cpu) ga10b_dma_free(be->ops, &s->data);
        if (s->control.cpu) ga10b_dma_free(be->ops, &s->control);
        if (s->data_va) va_put(s->data_va, s->data_va_size, true);
        if (s->control_va) va_put(s->control_va, s->control_va_size, false);
    }
    memset(s, 0, sizeof(*s));
}

int slm_prog_available(void)
{
    return backend() != NULL;
}

const char *slm_prog_reason_for(bool uefi, bool library_empty, bool channel_up, bool attached)
{
    if (!uefi) return SLM_PROG_WHY_KEXEC;
    /* First: with no SASS nothing later can succeed, and the cold bring-up
     * stops at MNIST, which would otherwise read as a GPU fault. */
    if (library_empty) return SLM_PROG_WHY_NO_SASS;
    if (!channel_up) return SLM_PROG_WHY_NO_CHANNEL;
    if (!attached) return SLM_PROG_WHY_NOT_ATTACHED;
    return NULL;
}

const char *slm_prog_unavailable_reason(void)
{
    if (backend() != NULL) return NULL;
    return platform_reason();
}

static int open_locked(const struct slm_prog_backend *be, const struct slm_prog_open_args *a,
                       struct slm_prog_open_out *out);

int slm_prog_open(const struct slm_prog_open_args *a, struct slm_prog_open_out *out)
{
    const struct slm_prog_backend *be = backend();
    if (!be) return SLM_PROG_ERR_NO_BACKEND;
    if (!a || !out || (a->weights && a->weights_bytes == 0u) || a->data_bytes == 0u ||
        a->data_bytes > 0xFFFF0000ull || a->barrier >= GA10B_PROG_BARRIER_COUNT) {
        return SLM_PROG_ERR_ARG;
    }
    int rc = prog_lock(be);
    if (rc != 0) return rc;
    rc = open_locked(be, a, out);
    prog_unlock();
    return rc;
}

static int open_locked(const struct slm_prog_backend *be, const struct slm_prog_open_args *a,
                       struct slm_prog_open_out *out)
{
    int32_t h = -1;
    for (int32_t i = 0; i < SLM_PROG_MAX_SESSIONS; i++) {
        if (!g_sessions[i].open) {
            h = i;
            break;
        }
    }
    if (h < 0) return SLM_PROG_ERR_NOMEM;
    struct prog_session *s = &g_sessions[h];
    memset(s, 0, sizeof(*s));
    s->be = be;
    s->wmap = -1;
    s->open = true;
    memset(out, 0, sizeof(*out));

    int rc;
    uint32_t tp0 = rd_table_pages(be);
    uint64_t t0 = be->now_us();
    /* No weights: `slm gpu smoke`, which only needs the arena. */
    if (a->weights) {
        rc = wmap_get(be, a->weights, a->weights_bytes, &s->wmap);
        if (rc != 0) goto fail;
        out->weights_map_us = be->now_us() - t0;
        out->weights_gpu_va = g_wmaps[s->wmap].va;
        out->weights_big = g_wmaps[s->wmap].big;
    }
    if (be->prepare_clocks) be->prepare_clocks();

    /* Data: big pages when PMM gives 64 KB alignment (blocks are aligned
     * to their size, so any block this large is). Zeroed by the allocator,
     * cleaned so no dirty CPU line lands on GPU data later. */
    uint64_t data_bytes = big_round(a->data_bytes);
    if (ga10b_dma_alloc(be->ops, (uint32_t)data_bytes, &s->data) != GA10B_CHAN_OK) {
        rc = SLM_PROG_ERR_NOMEM;
        goto fail;
    }
    ga10b_dma_clean(be->ops, &s->data);
    s->data_va_size = span_round(data_bytes);
    s->data_va = va_get(be, s->data_va_size, true);
    if (s->data_va == 0u) {
        rc = SLM_PROG_ERR_MAP;
        goto fail;
    }
    if ((s->data.phys & (GA10B_BIG_PAGE_SIZE - 1u)) != 0u) {
        rc = SLM_PROG_ERR_MAP;   /* PMM contract broken: don't guess */
        goto fail;
    }
    if (ga10b_vm_map_big(be->ops, be->vm, s->data_va, s->data.phys, data_bytes,
                         GA10B_MAP_CACHEABLE) != GA10B_CHAN_OK) {
        rc = SLM_PROG_ERR_MAP;
        goto fail;
    }

    /* Control: GPU-uncached, so the CPU's cleaned writes are what the
     * host and SKED read, and L2 never holds a stale cbuf. */
    uint64_t cbytes = control_bytes(a);
    if (cbytes > 0xFFFF0000ull ||
        ga10b_dma_alloc(be->ops, (uint32_t)cbytes, &s->control) != GA10B_CHAN_OK) {
        rc = SLM_PROG_ERR_NOMEM;
        goto fail;
    }
    s->control_va_size = span_round(cbytes);
    s->control_va = va_get(be, s->control_va_size, false);
    if (s->control_va == 0u || ga10b_vm_map(be->ops, be->vm, s->control_va, s->control.phys,
                                            cbytes, 0u) != GA10B_CHAN_OK) {
        rc = SLM_PROG_ERR_MAP;
        goto fail;
    }

    uint32_t off = 0;
    s->step_cpu = (uint8_t *)s->control.cpu;
    s->step_bytes = a->step_bytes;
    off = align256(a->step_bytes);
    uint32_t pf = 0;
    if (a->flags & SLM_PROG_FLAG_LEAN_QMD) pf |= GA10B_PROG_F_LEAN_QMD;
    if (a->flags & SLM_PROG_FLAG_PROFILE) pf |= GA10B_PROG_F_PROFILE;
    uint32_t sized = pf | ((a->flags & SLM_PROG_FLAG_INJECT_HANG) ? GA10B_PROG_F_INJECT_HANG : 0u);
    for (unsigned i = 0; i < SLM_PROG_SHAPES; i++) {
        if (a->max_launches[i] == 0u) continue;
        uint32_t pb, q, c, se;
        uint32_t fi = sized | shape_marks(i);
        ga10b_prog_sizes(a->max_launches[i], fi, &pb, &q, &c, &se);
        struct ga10b_prog_mem m_pb = { (uint8_t *)s->control.cpu + off, s->control_va + off, pb };
        off += align256(pb);
        struct ga10b_prog_mem m_q = { (uint8_t *)s->control.cpu + off, s->control_va + off, q };
        off += align256(q);
        struct ga10b_prog_mem m_c = { (uint8_t *)s->control.cpu + off, s->control_va + off, c };
        off += align256(c);
        struct ga10b_prog_mem m_s = { (uint8_t *)s->control.cpu + off, s->control_va + off, se };
        off += align256(se);
        uint32_t spf = pf | shape_marks(i);
        if (i == 0u && (a->flags & SLM_PROG_FLAG_INJECT_HANG)) spf |= GA10B_PROG_F_INJECT_HANG;
        if (ga10b_prog_init(&s->shapes[i].p, &m_pb, &m_q, &m_c, &m_s, a->max_launches[i], spf,
                            (enum ga10b_prog_barrier)a->barrier) != GA10B_PROG_OK) {
            rc = SLM_PROG_ERR_ARG;
            goto fail;
        }
        s->shapes[i].used = true;
    }

    if (be->tlb_invalidate && be->tlb_invalidate() != 0) {
        rc = SLM_PROG_ERR_MAP;
        goto fail;
    }
    out->handle = h;
    out->data_gpu_va = s->data_va;
    out->data_cpu = (uint8_t *)s->data.cpu;
    out->step_gpu_va = s->control_va;
    out->step_cpu = s->step_cpu;
    out->table_pages_added = rd_table_pages(be) - tp0;
    return 0;

fail:
    session_free(s);
    return rc;
}

void slm_prog_close(int32_t h)
{
    if (h < 0 || h >= SLM_PROG_MAX_SESSIONS) return;
    const struct slm_prog_backend *be = backend();
    if (prog_lock(be) != 0) {
        /* The holder frees it in prog_unlock. */
        if (g_sessions[h].open) __atomic_store_n(&g_sessions[h].close_pending, true, __ATOMIC_RELEASE);
        if (lock_try()) prog_unlock();
        return;
    }
    struct prog_session *s = session(h);
    if (s) session_free(s);
    prog_unlock();
}

static struct prog_shape *shape_of(struct prog_session *s, uint32_t shape)
{
    if (!s || shape >= SLM_PROG_SHAPES || !s->shapes[shape].used) return NULL;
    return &s->shapes[shape];
}

static int encode_for(const struct prog_session *s, const struct slm_prog_op *op,
                      struct ga10b_prog_launch *l, uint8_t *args)
{
    int sass = slm_prog_op_sass(op);
    if (sass < 0) return SLM_PROG_ERR_ARG;
    uint64_t va = 0;
    if (s->be->kernel_va((uint32_t)sass, &va) != 0) return SLM_PROG_ERR_NO_KERNEL;
    return slm_prog_encode(op, (uint32_t)sass, va, l, args) == 0 ? 0 : SLM_PROG_ERR_ARG;
}

static int slm_prog_add_locked(int32_t h, uint32_t shape, const struct slm_prog_op *op)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps || !op) return SLM_PROG_ERR_ARG;
    struct ga10b_prog_launch l;
    uint8_t args[GA10B_PROG_CBUF_ARGS_MAX];
    int rc = encode_for(s, op, &l, args);
    if (rc != 0) return rc;
    rc = ga10b_prog_add_launch(&ps->p, &l);
    return rc < 0 ? SLM_PROG_ERR_ARG : rc;
}

int slm_prog_add(int32_t h, uint32_t shape, const struct slm_prog_op *op)
{
    int rc = prog_lock(backend());
    if (rc != 0) return rc;
    rc = slm_prog_add_locked(h, shape, op);
    prog_unlock();
    return rc;
}

static int slm_prog_barrier_locked(int32_t h, uint32_t shape)
{
    struct prog_shape *ps = shape_of(session(h), shape);
    if (!ps) return SLM_PROG_ERR_ARG;
    return ga10b_prog_add_barrier(&ps->p) == GA10B_PROG_OK ? 0 : SLM_PROG_ERR_ARG;
}

int slm_prog_barrier(int32_t h, uint32_t shape)
{
    int rc = prog_lock(backend());
    if (rc != 0) return rc;
    rc = slm_prog_barrier_locked(h, shape);
    prog_unlock();
    return rc;
}

static int slm_prog_mark_locked(int32_t h, uint32_t shape)
{
    struct prog_shape *ps = shape_of(session(h), shape);
    if (!ps) return SLM_PROG_ERR_ARG;
    int rc = ga10b_prog_add_mark(&ps->p);
    return rc >= 0 ? rc : SLM_PROG_ERR_ARG;
}

int slm_prog_mark(int32_t h, uint32_t shape)
{
    int rc = prog_lock(backend());
    if (rc != 0) return rc;
    rc = slm_prog_mark_locked(h, shape);
    prog_unlock();
    return rc;
}

static int slm_prog_finish_locked(int32_t h, uint32_t shape)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps) return SLM_PROG_ERR_ARG;
    if (ga10b_prog_finish(&ps->p) != GA10B_PROG_OK) return SLM_PROG_ERR_ARG;
    ga10b_dma_clean(s->be->ops, &s->control);
    return 0;
}

int slm_prog_finish(int32_t h, uint32_t shape)
{
    int rc = prog_lock(backend());
    if (rc != 0) return rc;
    rc = slm_prog_finish_locked(h, shape);
    prog_unlock();
    return rc;
}

static int slm_prog_patch_locked(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps || !op) return SLM_PROG_ERR_ARG;
    uint8_t *dst = ga10b_prog_cbuf_args(&ps->p, idx);
    if (!dst) return SLM_PROG_ERR_ARG;
    struct ga10b_prog_launch l;
    uint8_t args[GA10B_PROG_CBUF_ARGS_MAX];
    int rc = encode_for(s, op, &l, args);
    if (rc != 0) return rc;
    memcpy(dst, args, l.args_len);
    s->be->ops->cache_clean(s->be->ops->ctx, dst, l.args_len);
    return 0;
}

int slm_prog_patch(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op)
{
    int rc = prog_lock(backend());
    if (rc != 0) return rc;
    rc = slm_prog_patch_locked(h, shape, idx, op);
    prog_unlock();
    return rc;
}

static int slm_prog_update_locked(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps || !op) return SLM_PROG_ERR_ARG;
    struct ga10b_prog_launch l;
    uint8_t args[GA10B_PROG_CBUF_ARGS_MAX];
    int rc = encode_for(s, op, &l, args);
    if (rc != 0) return rc;
    if (ga10b_prog_update_launch(&ps->p, idx, &l) != GA10B_PROG_OK) return SLM_PROG_ERR_ARG;
    const struct ga10b_gpu_ops *ops = s->be->ops;
    ops->cache_clean(ops->ctx, ga10b_prog_qmd(&ps->p, idx), GA10B_QMD_SIZE_BYTES);
    ops->cache_clean(ops->ctx, ga10b_prog_cbuf_args(&ps->p, idx) - GA10B_PROG_CBUF_ARGS_BASE,
                     GA10B_PROG_CBUF_STRIDE);
    return 0;
}

int slm_prog_update(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op)
{
    int rc = prog_lock(backend());
    if (rc != 0) return rc;
    rc = slm_prog_update_locked(h, shape, idx, op);
    prog_unlock();
    return rc;
}

/* Cache maintenance takes no lock. It touches only the caller's own session
 * memory (its arena or step page), which nothing else frees while that
 * session is open: the caller owns the session, and a close from elsewhere
 * waits for the holder. Taking the lock here would let a busy lock skip
 * the clean or invalidate silently and compute on stale data (#475 W-A). */
static const struct ga10b_gpu_ops *session_ops(int32_t h)
{
    if (h < 0 || h >= SLM_PROG_MAX_SESSIONS) return NULL;
    const struct prog_session *s = &g_sessions[h];
    if (!__atomic_load_n(&s->open, __ATOMIC_ACQUIRE) || !s->be) return NULL;
    return s->be->ops;
}

void slm_prog_clean(int32_t h, const void *p, size_t len)
{
    const struct ga10b_gpu_ops *ops = session_ops(h);
    if (ops) ops->cache_clean(ops->ctx, p, len);
}

void slm_prog_invalidate(int32_t h, const void *p, size_t len)
{
    const struct ga10b_gpu_ops *ops = session_ops(h);
    if (ops) ops->cache_inval(ops->ctx, p, len);
}

/* Print once per session: a dead program means every later step of the
 * session is on the CPU, which Rust reports. */
static void describe_timeout(const struct prog_session *s, const struct ga10b_prog *p)
{
    uint32_t start = ga10b_prog_sema_payload(p, GA10B_PROG_SEMA_START);
    const char *what = start == GA10B_PROG_PAYLOAD_START
        ? "start semaphore fired, end did not: a kernel hung or faulted"
        : "start semaphore never fired: the channel did not run";
    uart_printf("[slm-prog] step timed out after %u ms: %s\n",
                (unsigned)SLM_PROG_STEP_TIMEOUT_MS, what);
    s->be->dump_fault(what);
}

static int run_step_locked(int32_t h, uint32_t shape, struct slm_prog_run_stats *stats,
                           bool marked, uint64_t *ns, uint32_t cap)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps || !ps->p.finished) return SLM_PROG_ERR_ARG;
    if (s->dead) return SLM_PROG_ERR_DEAD;
    const struct slm_prog_backend *be = s->be;
    const struct ga10b_gpu_ops *ops = be->ops;

    const struct ga10b_prog *p = &ps->p;
    uint32_t sema_len = ga10b_prog_sema_bytes(p);
    ga10b_prog_sema_arm(p);
    ops->cache_clean(ops->ctx, p->sema.cpu, sema_len);
    uint8_t *marks = NULL;
    uint32_t marks_len = 0;
    if (marked) {
        if (!(p->flags & GA10B_PROG_F_MARKS)) return SLM_PROG_ERR_ARG;
        marks = p->sema.cpu + ga10b_prog_mark_slot0(p) * GA10B_PROG_SEMA_SLOT_BYTES;
        marks_len = ga10b_prog_marks_bytes(p);
        memset(marks, 0, marks_len);
        ops->cache_clean(ops->ctx, marks, marks_len);
    }

    uint64_t t0 = be->now_us();
    int rc = be->submit(marked ? ga10b_prog_gp_entry_marked(p) : ga10b_prog_gp_entry(p),
                        p->sema.cpu + GA10B_PROG_SEMA_END * GA10B_PROG_SEMA_SLOT_BYTES,
                        GA10B_PROG_PAYLOAD_END, SLM_PROG_STEP_TIMEOUT_MS);
    if (rc != 0) {
        /* Nothing reached the GPU: not fatal to the session's state, but
         * the session can't run this step; Rust moves it to the CPU. */
        return SLM_PROG_ERR_SUBMIT;
    }
    /* From the doorbell, not from the call: submit may have waited for the
     * dispatch lock behind another GPU consumer. */
    uint64_t deadline = be->now_us() + (uint64_t)SLM_PROG_STEP_TIMEOUT_MS * 1000u;
    bool done = false;
    for (;;) {
        ops->cache_inval(ops->ctx, p->sema.cpu, 2u * GA10B_PROG_SEMA_SLOT_BYTES);
        if (ga10b_prog_sema_payload(p, GA10B_PROG_SEMA_END) == GA10B_PROG_PAYLOAD_END) {
            done = true;
            break;
        }
        if (be->now_us() >= deadline) break;
        be->relax();
    }
    be->finished();
    if (!done) {
        s->dead = true;
        describe_timeout(s, p);
        return SLM_PROG_ERR_TIMEOUT;
    }
    uint64_t t1 = be->now_us();
    if (be->l2_evict() != 0) {
        s->dead = true;
        return SLM_PROG_ERR_EVICT;
    }
    if (stats) {
        stats->wait_us = t1 - t0;
        stats->evict_us = be->now_us() - t1;
        stats->gpu_ns = ga10b_prog_sema_ts(p, GA10B_PROG_SEMA_END) -
                        ga10b_prog_sema_ts(p, GA10B_PROG_SEMA_START);
    }
    if (marked) {
        ops->cache_inval(ops->ctx, marks, marks_len);
        (void)ga10b_prog_marks_ns(p, ns, cap);
    }
    return 0;
}

static int run_step(int32_t h, uint32_t shape, struct slm_prog_run_stats *stats, bool marked,
                    uint64_t *ns, uint32_t cap)
{
    const struct slm_prog_backend *be = backend();
    if (!be) return SLM_PROG_ERR_ARG;
    /* A wait under masked IRQs is the #1093 hang, so refuse outright. */
    if (be->irqs_enabled && !be->irqs_enabled()) return SLM_PROG_ERR_IRQS_OFF;
    int rc = prog_lock(be);
    if (rc != 0) return rc;
    rc = run_step_locked(h, shape, stats, marked, ns, cap);
    prog_unlock();
    return rc;
}

int slm_prog_run(int32_t h, uint32_t shape, struct slm_prog_run_stats *stats)
{
    return run_step(h, shape, stats, false, NULL, 0u);
}

int slm_prog_run_marked(int32_t h, uint32_t shape, struct slm_prog_run_stats *stats,
                        uint64_t *ns, uint32_t cap)
{
    return run_step(h, shape, stats, true, ns, cap);
}

static uint64_t profile_ns_locked(int32_t h, uint32_t shape, uint32_t idx)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps || !(ps->p.flags & GA10B_PROG_F_PROFILE) || idx >= ps->p.n_launches) return 0;
    const struct ga10b_prog *p = &ps->p;
    s->be->ops->cache_inval(s->be->ops->ctx, p->sema.cpu, ga10b_prog_sema_bytes(p));
    uint64_t t = ga10b_prog_sema_ts(p, GA10B_PROG_SEMA_OP0 + idx);
    uint64_t prev = idx == 0u ? ga10b_prog_sema_ts(p, GA10B_PROG_SEMA_START)
                              : ga10b_prog_sema_ts(p, GA10B_PROG_SEMA_OP0 + idx - 1u);
    return t - prev;
}

uint64_t slm_prog_profile_ns(int32_t h, uint32_t shape, uint32_t idx)
{
    if (prog_lock(backend()) != 0) return 0;
    uint64_t ns = profile_ns_locked(h, shape, idx);
    prog_unlock();
    return ns;
}

static bool sass_present(const struct slm_prog_backend *be, uint32_t sass)
{
    uint64_t va = 0;
    return be->kernel_va(sass, &va) == 0 && va != 0u;
}

uint32_t slm_prog_max_batch(bool simt)
{
    const struct slm_prog_backend *be = backend();
    if (!be) return 0u;
    if (simt) {
        return sass_present(be, SLM_SASS_MM_Q4K_SIMT) && sass_present(be, SLM_SASS_MM_Q6K_SIMT)
                   ? 64u : 0u;
    }
    if (!sass_present(be, SLM_SASS_MM_Q4K_HMMA_B1) || !sass_present(be, SLM_SASS_MM_Q6K_HMMA_B1)) {
        return 0u;
    }
    if (!sass_present(be, SLM_SASS_MM_Q4K_HMMA_B8) || !sass_present(be, SLM_SASS_MM_Q6K_HMMA_B8)) {
        return 1u;
    }
    return sass_present(be, SLM_SASS_MM_Q4K_HMMA_BS) && sass_present(be, SLM_SASS_MM_Q6K_HMMA_BS) &&
                   sass_present(be, SLM_SASS_MM_Q4K_HMMA_B64) &&
                   sass_present(be, SLM_SASS_MM_Q6K_HMMA_B64)
               ? 64u : 8u;
}

const char *slm_prog_strerror(int err)
{
    switch (err) {
    case 0: return "ok";
    case SLM_PROG_ERR_NO_BACKEND: return "no SLM-OS-built GPU channel (needs a UEFI-direct boot)";
    case SLM_PROG_ERR_ARG: return "invalid program";
    case SLM_PROG_ERR_NOMEM: return "out of memory for the session arena";
    case SLM_PROG_ERR_MAP: return "GPU mapping failed";
    case SLM_PROG_ERR_NO_KERNEL: return "a kernel is missing from the operator library";
    case SLM_PROG_ERR_TIMEOUT: return "GPU step timed out";
    case SLM_PROG_ERR_SUBMIT: return "GPU submit failed";
    case SLM_PROG_ERR_DEAD: return "GPU program failed earlier";
    case SLM_PROG_ERR_EVICT: return "L2 evict timed out";
    case SLM_PROG_ERR_IRQS_OFF: return "step started with IRQs masked";
    case SLM_PROG_ERR_BUSY: return "another GPU program held the lock too long";
    default: return "unknown error";
    }
}

/* ---- Jetson backend ---- */

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "boot_source.h"
#include "bpmp.h"
#include "kbuf.h"
#include "nvidia/ga10b_cold.h"
#include "oplib_cold.h"
#include "oplib_gpu_as.h"
#include "oplib_pool.h"
#include "sched.h"
#include "slm_ffi.h"
#include "timer.h"
#include "vmm.h"
#include "arch.h"
#include "platform.h"

/* TEGRA234_CLK_EMC (dt-bindings/clock/tegra234-clock.h). */
#define T234_CLK_EMC 31u

/* SLM_OP_KIND_BASE + index, FP16: how build-operator-library.py files them. */
static int jb_kernel_va(uint32_t sass, uint64_t *va)
{
    size_t size = 0;
    return oplib_pool_get_sass_gpu_va(SLM_OP_KIND_BASE + sass, g_sass[sass].tier,
                                      SLM_GPU_DTYPE_FP16, va, &size);
}

static int jb_tlb(void)
{
    const struct oplib_gpu_as *as = oplib_cold_as();
    /* as->tlb_invalidate takes the dispatch lock itself (oplib_cold.c). */
    return as ? as->tlb_invalidate(as->ctx) : -1;
}

/* The kernel's RAM identity map, or a kbuf translated page by page. */
static uint64_t jb_v2p(const void *p)
{
    if (kbuf_owns_va(p)) return vmm_virt_to_phys((uint64_t)(uintptr_t)p);
    return (uint64_t)(uintptr_t)p;
}

static bool jb_contig(const void *p, uint64_t len)
{
    (void)len;
    return !kbuf_owns_va(p);
}

static uint64_t jb_now_us(void)
{
    return slm_get_time_ns() / 1000u;
}

static void jb_relax(void)
{
    /* yield runs whatever is ready; then pause briefly so an idle system
     * doesn't hammer the semaphore line. */
    yield();
    timer_busy_wait_us(20u);
}

static bool jb_irqs(void)
{
    return arch_irqs_enabled() != 0;
}

/* LPDDR5 at the reference's 3199 MHz (ADR 0001). After a cold boot nothing
 * has asked for it, so the program would otherwise be measured at whatever
 * rate the firmware left. A refusal only costs speed, and `slm gpu` prints
 * the real rate. Asked once per boot. */
static void jb_prepare_clocks(void)
{
    static bool asked;
    if (asked) return;
    asked = true;
    uint64_t before = 0, set = 0;
    (void)bpmp_clk_get_rate(T234_CLK_EMC, &before);
    int rc = bpmp_emc_floor_khz(BPMP_BWMGR_CLIENT_GPU, 3199000u, &set);
    uart_printf("[slm-prog] EMC was %llu MHz; floor request rc=%d, now %llu MHz\n",
                (unsigned long long)(before / 1000000u), rc,
                (unsigned long long)(set / 1000000u));
}

static void jb_dump(const char *why)
{
    (void)why;
    slm_gpu_prog_dump_fault();
}

static struct slm_prog_backend g_jb;

static const struct slm_prog_backend *jetson_backend(void)
{
    if (!oplib_cold_active()) return NULL;
    const struct ga10b_gpu_ops *ops = NULL;
    struct ga10b_vm *vm = NULL;
    if (oplib_cold_vm(&ops, &vm) != 0) return NULL;
    g_jb.ops = ops;
    g_jb.vm = vm;
    g_jb.tlb_invalidate = jb_tlb;
    g_jb.kernel_va = jb_kernel_va;
    g_jb.virt_to_phys = jb_v2p;
    g_jb.is_contiguous = jb_contig;
    g_jb.submit = slm_gpu_prog_submit;
    g_jb.finished = slm_gpu_prog_finished;
    g_jb.l2_evict = slm_gpu_prog_l2_evict;
    g_jb.now_us = jb_now_us;
    g_jb.relax = jb_relax;
    g_jb.dump_fault = jb_dump;
    g_jb.irqs_enabled = jb_irqs;
    g_jb.prepare_clocks = jb_prepare_clocks;
    return &g_jb;
}

static const char *platform_reason(void)
{
    return slm_prog_reason_for(boot_source_is_uefi(), oplib_cold_library_empty(),
                               ga10b_cold_progress() >= GA10B_COLD_CHANNEL, oplib_cold_active());
}

void slm_prog_clocks(uint64_t *emc_hz, uint64_t *gpc_hz)
{
    uint64_t e = 0, g = 0;
    (void)bpmp_clk_get_rate(T234_CLK_EMC, &e);
    (void)bpmp_clk_get_rate(TEGRA234_CLK_GPC0CLK, &g);
    if (emc_hz) *emc_hz = e;
    if (gpc_hz) *gpc_hz = g;
}

#else

static const struct slm_prog_backend *jetson_backend(void)
{
    return NULL;
}

static const char *platform_reason(void)
{
    return SLM_PROG_WHY_KEXEC;
}

void slm_prog_clocks(uint64_t *emc_hz, uint64_t *gpc_hz)
{
    if (emc_hz) *emc_hz = 0;
    if (gpc_hz) *gpc_hz = 0;
}

#endif
