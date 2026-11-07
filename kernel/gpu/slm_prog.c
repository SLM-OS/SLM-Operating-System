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
#include "operator_dispatch.h"
#include "nvidia/ga10b_chan.h"
#include "nvidia/ga10b_prog.h"
#include "nvidia/ga10b_qmd.h"
#include "string.h"
#include "uart.h"

/* ---- Kernel table ----
 *
 * Existing kernels reuse operator_dispatch.c's cbuf and launch-shape
 * code, so their ABI stays in one place. The #475 kernels come from
 * scripts/cuda/slm/ (C3K); the ABI comments there are the source. */

#define MM_BLOCK_X 32u
#define MM_TILE_ROWS 15u

/* slm_mm_*: 0x160 x, 0x168 W, 0x170 out, 0x178 bias, 0x180 K, 0x184 N,
 * 0x188 B, 0x18C flags. */
static int encode_mm(const struct slm_prog_op *op, struct ga10b_prog_launch *l, uint8_t *args)
{
    uint32_t k = op->dim[0], n = op->dim[1], b = op->rows;
    if (k == 0u || n == 0u || b == 0u || b > 64u || op->ptr[0] == 0u || op->ptr[1] == 0u ||
        op->ptr[2] == 0u) {
        return -1;
    }
    memcpy(args + 0x00, &op->ptr[0], 8);
    memcpy(args + 0x08, &op->ptr[1], 8);
    memcpy(args + 0x10, &op->ptr[2], 8);
    memcpy(args + 0x18, &op->ptr[3], 8);
    memcpy(args + 0x20, &k, 4);
    memcpy(args + 0x24, &n, 4);
    memcpy(args + 0x28, &b, 4);
    memcpy(args + 0x2C, &op->scal[0], 4);
    l->args_len = 0x30;
    l->grid_x = (n + MM_TILE_ROWS - 1u) / MM_TILE_ROWS;
    l->grid_y = 1;
    l->grid_z = 1;
    l->block_x = MM_BLOCK_X;
    l->block_y = 1;
    l->block_z = 1;
    return 0;
}

/* slm_add: 0x160 a, 0x168 b, 0x170 out, 0x178 n (total elements). */
static int encode_add(const struct slm_prog_op *op, struct ga10b_prog_launch *l, uint8_t *args)
{
    uint64_t total = (uint64_t)op->rows * op->dim[0];
    if (total == 0u || total > 0xFFFFFFFFu || !op->ptr[0] || !op->ptr[1] || !op->ptr[2]) return -1;
    uint32_t n = (uint32_t)total;
    memcpy(args + 0x00, &op->ptr[0], 8);
    memcpy(args + 0x08, &op->ptr[1], 8);
    memcpy(args + 0x10, &op->ptr[2], 8);
    memcpy(args + 0x18, &n, 4);
    l->args_len = 0x1C;
    l->grid_x = (n + 255u) / 256u;
    l->grid_y = 1;
    l->grid_z = 1;
    l->block_x = 256;
    l->block_y = 1;
    l->block_z = 1;
    return 0;
}

/* Legacy kernels: build through the registry into a scratch cbuf. */
static int encode_registry(const struct operator_dispatch_args *a, struct ga10b_prog_launch *l,
                           uint8_t *args, uint32_t args_len)
{
    static_assert(OPERATOR_CBUF0_BASE == GA10B_PROG_CBUF_ARGS_BASE, "same cbuf layout");
    uint8_t cbuf[OPERATOR_CBUF0_BASE + GA10B_PROG_CBUF_ARGS_MAX];
    struct operator_launch_shape s;
    if (operator_dispatch_build_cbuf(cbuf, a) != 0 || operator_dispatch_launch_shape(a, &s) != 0) {
        return -1;
    }
    memcpy(args, cbuf + OPERATOR_CBUF0_BASE, args_len);
    l->args_len = args_len;
    l->grid_x = s.grid_x;
    l->grid_y = s.grid_y;
    l->grid_z = s.grid_z;
    l->block_x = s.block_x;
    l->block_y = s.block_y;
    l->block_z = s.block_z;
    l->register_count_v = s.register_count_v;
    l->smem_size_bytes = s.smem_size_bytes;
    l->slm_size_bytes = s.slm_size_bytes;
    l->barrier_count = s.barrier_count;
    return 0;
}

int slm_prog_encode(const struct slm_prog_op *op, uint64_t shader_va,
                    struct ga10b_prog_launch *l, uint8_t *args)
{
    if (!op || !l || !args || shader_va == 0u) return -1;
    memset(l, 0, sizeof(*l));
    memset(args, 0, GA10B_PROG_CBUF_ARGS_MAX);
    l->shader_gpu_va = shader_va;
    l->args = args;
    /* Default until the kernel's resource usage says otherwise. */
    l->register_count_v = 64;

    struct operator_dispatch_args a;
    memset(&a, 0, sizeof(a));
    switch (op->kernel) {
    case SLM_PROG_K_RMSNORM:
        a.op_kind = SLM_GPU_OP_RMSNORM;
        a.u.rmsnorm.x_gpu_va = op->ptr[0];
        a.u.rmsnorm.gamma_gpu_va = op->ptr[1];
        a.u.rmsnorm.out_gpu_va = op->ptr[2];
        a.u.rmsnorm.n_rows = op->rows;
        a.u.rmsnorm.n = op->dim[0];
        a.u.rmsnorm.eps_bits = op->scal[0];
        return encode_registry(&a, l, args, 0x20);
    case SLM_PROG_K_ROPE:
        a.op_kind = SLM_GPU_OP_ROPE;
        a.u.rope.vec_gpu_va = op->ptr[0];
        a.u.rope.positions_gpu_va = op->ptr[1];
        a.u.rope.cos_sin_gpu_va = op->ptr[2];
        a.u.rope.batch = op->rows;
        a.u.rope.num_heads = op->dim[0];
        a.u.rope.head_dim = op->dim[1];
        return encode_registry(&a, l, args, 0x24);
    case SLM_PROG_K_SWIGLU: {
        uint64_t n = (uint64_t)op->rows * op->dim[0];
        if (n > 0xFFFFFFFFu) return -1;
        a.op_kind = SLM_GPU_OP_SWIGLU;
        a.u.swiglu.gate_gpu_va = op->ptr[0];
        a.u.swiglu.up_gpu_va = op->ptr[1];
        a.u.swiglu.out_gpu_va = op->ptr[2];
        a.u.swiglu.n = (uint32_t)n;
        return encode_registry(&a, l, args, 0x1C);
    }
    case SLM_PROG_K_GQA:
        if (op->rows != 1u || op->scal[0] > GQA_ATTN_MAX_SEQ_LEN) return -1;
        a.op_kind = SLM_GPU_OP_GQA_ATTN;
        a.u.gqa_attn.q_gpu_va = op->ptr[0];
        a.u.gqa_attn.k_gpu_va = op->ptr[1];
        a.u.gqa_attn.v_gpu_va = op->ptr[2];
        a.u.gqa_attn.out_gpu_va = op->ptr[3];
        a.u.gqa_attn.n_head_q = op->dim[0];
        a.u.gqa_attn.n_head_kv = op->dim[1];
        a.u.gqa_attn.head_dim = op->dim[2];
        a.u.gqa_attn.seq_len = op->scal[0];
        return encode_registry(&a, l, args, 0x30);
    case SLM_PROG_K_GQA_BATCH: {
        /* One block per (row, query head); the kernel takes the row from
         * blockIdx. Same layout as GQA_ATTN with seq_len as the base
         * position, plus the row count. Not final until scripts/cuda/slm/
         * has this kernel. */
        uint32_t rows = op->rows;
        uint64_t grid = (uint64_t)rows * op->dim[0];
        if (rows == 0u || rows > 64u || grid == 0u || grid > 0xFFFFFFFFu ||
            op->scal[0] + rows > GQA_ATTN_MAX_SEQ_LEN) {
            return -1;
        }
        memcpy(args + 0x00, &op->ptr[0], 8);
        memcpy(args + 0x08, &op->ptr[1], 8);
        memcpy(args + 0x10, &op->ptr[2], 8);
        memcpy(args + 0x18, &op->ptr[3], 8);
        memcpy(args + 0x20, &op->dim[0], 12);
        memcpy(args + 0x2C, &op->scal[0], 4);
        memcpy(args + 0x30, &rows, 4);
        l->args_len = 0x34;
        l->grid_x = (uint32_t)grid;
        l->grid_y = 1;
        l->grid_z = 1;
        l->block_x = GQA_ATTN_BLOCK_DIM;
        l->block_y = 1;
        l->block_z = 1;
        l->smem_size_bytes = GQA_ATTN_SMEM_BYTES;
        l->barrier_count = 1;
        return 0;
    }
    case SLM_PROG_K_MM_Q4K:
    case SLM_PROG_K_MM_Q6K:
        return encode_mm(op, l, args);
    case SLM_PROG_K_ADD:
        return encode_add(op, l, args);
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
};

static struct prog_session g_sessions[SLM_PROG_MAX_SESSIONS];
static struct prog_wmap g_wmaps[WMAP_MAX];
static struct prog_va_range g_va_free[VA_FREE_MAX];
static const struct slm_prog_backend *g_backend_override;

static const struct slm_prog_backend *jetson_backend(void);

static const struct slm_prog_backend *backend(void)
{
    return g_backend_override ? g_backend_override : jetson_backend();
}

void slm_prog_backend_set(const struct slm_prog_backend *b)
{
    g_backend_override = b;
    /* Tests run against several fake VMs: forget what the last one held. */
    memset(g_wmaps, 0, sizeof(g_wmaps));
    memset(g_va_free, 0, sizeof(g_va_free));
    memset(g_sessions, 0, sizeof(g_sessions));
}

/* All big-page spans are 2 MB aligned and whole, so a reused range can
 * hold either page size only if it stays that way: every range here is
 * 2 MB aligned and never shares a PDE0 span with small-page data, and
 * ga10b_vm_map refuses a span with big PTEs. Control regions use small
 * pages, data and weights big, so a free range keeps its kind. */
static uint64_t va_get(const struct slm_prog_backend *be, uint64_t size, uint64_t *got)
{
    for (unsigned i = 0; i < VA_FREE_MAX; i++) {
        if (g_va_free[i].va != 0u && g_va_free[i].size == size) {
            uint64_t va = g_va_free[i].va;
            g_va_free[i].va = 0u;
            *got = size;
            return va;
        }
    }
    *got = size;
    return ga10b_vm_reserve(be->vm, size, GA10B_VM_PDE0_SPAN, false);
}

static void va_put(uint64_t va, uint64_t size)
{
    for (unsigned i = 0; i < VA_FREE_MAX; i++) {
        if (g_va_free[i].va == 0u) {
            g_va_free[i].va = va;
            g_va_free[i].size = size;
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
    uint64_t got = 0;
    uint64_t va = va_get(be, va_size, &got);
    if (va == 0u) return SLM_PROG_ERR_MAP;
    uint32_t flags = GA10B_MAP_CACHEABLE | GA10B_MAP_RO;
    uint64_t phys = be->virt_to_phys(w->cpu);
    int rc;
    if (phys != 0u && be->is_contiguous(w->cpu, bytes) &&
        (phys & (GA10B_BIG_PAGE_SIZE - 1u)) == 0u) {
        rc = ga10b_vm_map_big(be->ops, be->vm, va, phys, big_round(bytes), flags);
        w->big = true;
    } else {
        rc = GA10B_CHAN_OK;
        w->big = false;
        const uint8_t *c = (const uint8_t *)w->cpu;
        if (((uintptr_t)c & (GA10B_PAGE_SIZE - 1u)) != 0u) return SLM_PROG_ERR_ARG;
        for (uint64_t off = 0; off < bytes && rc == GA10B_CHAN_OK; off += GA10B_PAGE_SIZE) {
            uint64_t ph = be->virt_to_phys(c + off);
            rc = ph ? ga10b_vm_map(be->ops, be->vm, va + off, ph, GA10B_PAGE_SIZE, flags)
                    : GA10B_CHAN_ERR_VA;
        }
    }
    if (rc != GA10B_CHAN_OK) {
        /* Pull down whatever part was written. */
        ga10b_vm_unmap(be->ops, be->vm, va, w->big ? big_round(bytes) : bytes);
        va_put(va, va_size);
        return rc == GA10B_CHAN_ERR_NOMEM ? SLM_PROG_ERR_NOMEM : SLM_PROG_ERR_MAP;
    }
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
                va_put(w->va, w->va_size);
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
        va_put(w->va, w->va_size);
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

static uint64_t control_bytes(const struct slm_prog_open_args *a)
{
    uint64_t total = align256(a->step_bytes);
    uint32_t f = (a->flags & SLM_PROG_FLAG_PROFILE) ? GA10B_PROG_F_PROFILE : 0u;
    for (unsigned s = 0; s < SLM_PROG_SHAPES; s++) {
        if (a->max_launches[s] == 0u) continue;
        uint32_t pb, q, c, se;
        ga10b_prog_sizes(a->max_launches[s], f, &pb, &q, &c, &se);
        total += (uint64_t)align256(pb) + align256(q) + align256(c) + align256(se);
    }
    return (total + GA10B_PAGE_SIZE - 1u) & ~(uint64_t)(GA10B_PAGE_SIZE - 1u);
}

static void session_free(struct prog_session *s)
{
    const struct slm_prog_backend *be = s->be;
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
    } else {
        if (s->data.cpu) ga10b_dma_free(be->ops, &s->data);
        if (s->control.cpu) ga10b_dma_free(be->ops, &s->control);
        if (s->data_va) va_put(s->data_va, s->data_va_size);
        if (s->control_va) va_put(s->control_va, s->control_va_size);
    }
    memset(s, 0, sizeof(*s));
}

int slm_prog_available(void)
{
    return backend() != NULL;
}

int slm_prog_open(const struct slm_prog_open_args *a, struct slm_prog_open_out *out)
{
    const struct slm_prog_backend *be = backend();
    if (!be) return SLM_PROG_ERR_NO_BACKEND;
    if (!a || !out || !a->weights || a->weights_bytes == 0u || a->data_bytes == 0u ||
        a->data_bytes > 0xFFFF0000ull || a->barrier >= GA10B_PROG_BARRIER_COUNT) {
        return SLM_PROG_ERR_ARG;
    }
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
    rc = wmap_get(be, a->weights, a->weights_bytes, &s->wmap);
    if (rc != 0) goto fail;
    out->weights_map_us = be->now_us() - t0;
    out->weights_gpu_va = g_wmaps[s->wmap].va;
    out->weights_big = g_wmaps[s->wmap].big;

    /* Data: big pages when PMM gives 64 KB alignment (blocks are aligned
     * to their size, so any block this large is). Zeroed by the allocator,
     * cleaned so no dirty CPU line lands on GPU data later. */
    uint64_t data_bytes = big_round(a->data_bytes);
    if (ga10b_dma_alloc(be->ops, (uint32_t)data_bytes, &s->data) != GA10B_CHAN_OK) {
        rc = SLM_PROG_ERR_NOMEM;
        goto fail;
    }
    ga10b_dma_clean(be->ops, &s->data);
    uint64_t got = 0;
    s->data_va_size = span_round(data_bytes);
    s->data_va = va_get(be, s->data_va_size, &got);
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
    s->control_va = va_get(be, s->control_va_size, &got);
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
    for (unsigned i = 0; i < SLM_PROG_SHAPES; i++) {
        if (a->max_launches[i] == 0u) continue;
        uint32_t pb, q, c, se;
        ga10b_prog_sizes(a->max_launches[i], pf, &pb, &q, &c, &se);
        struct ga10b_prog_mem m_pb = { (uint8_t *)s->control.cpu + off, s->control_va + off, pb };
        off += align256(pb);
        struct ga10b_prog_mem m_q = { (uint8_t *)s->control.cpu + off, s->control_va + off, q };
        off += align256(q);
        struct ga10b_prog_mem m_c = { (uint8_t *)s->control.cpu + off, s->control_va + off, c };
        off += align256(c);
        struct ga10b_prog_mem m_s = { (uint8_t *)s->control.cpu + off, s->control_va + off, se };
        off += align256(se);
        if (ga10b_prog_init(&s->shapes[i].p, &m_pb, &m_q, &m_c, &m_s, a->max_launches[i], pf,
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
    struct prog_session *s = session(h);
    if (s) session_free(s);
}

static struct prog_shape *shape_of(struct prog_session *s, uint32_t shape)
{
    if (!s || shape >= SLM_PROG_SHAPES || !s->shapes[shape].used) return NULL;
    return &s->shapes[shape];
}

static int encode_for(const struct prog_session *s, const struct slm_prog_op *op,
                      struct ga10b_prog_launch *l, uint8_t *args)
{
    uint64_t va = 0;
    if (s->be->kernel_va(op->kernel, op->rows, &va) != 0) return SLM_PROG_ERR_NO_KERNEL;
    return slm_prog_encode(op, va, l, args) == 0 ? 0 : SLM_PROG_ERR_ARG;
}

int slm_prog_add(int32_t h, uint32_t shape, const struct slm_prog_op *op)
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

int slm_prog_barrier(int32_t h, uint32_t shape)
{
    struct prog_shape *ps = shape_of(session(h), shape);
    if (!ps) return SLM_PROG_ERR_ARG;
    return ga10b_prog_add_barrier(&ps->p) == GA10B_PROG_OK ? 0 : SLM_PROG_ERR_ARG;
}

int slm_prog_finish(int32_t h, uint32_t shape)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps) return SLM_PROG_ERR_ARG;
    if (ga10b_prog_finish(&ps->p) != GA10B_PROG_OK) return SLM_PROG_ERR_ARG;
    ga10b_dma_clean(s->be->ops, &s->control);
    return 0;
}

int slm_prog_patch(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op)
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

int slm_prog_update(int32_t h, uint32_t shape, uint32_t idx, const struct slm_prog_op *op)
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

void slm_prog_clean(int32_t h, const void *p, size_t len)
{
    struct prog_session *s = session(h);
    if (s) s->be->ops->cache_clean(s->be->ops->ctx, p, len);
}

void slm_prog_invalidate(int32_t h, const void *p, size_t len)
{
    struct prog_session *s = session(h);
    if (s) s->be->ops->cache_inval(s->be->ops->ctx, p, len);
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

int slm_prog_run(int32_t h, uint32_t shape, struct slm_prog_run_stats *stats)
{
    struct prog_session *s = session(h);
    struct prog_shape *ps = shape_of(s, shape);
    if (!ps || !ps->p.finished) return SLM_PROG_ERR_ARG;
    if (s->dead) return SLM_PROG_ERR_DEAD;
    const struct slm_prog_backend *be = s->be;
    const struct ga10b_gpu_ops *ops = be->ops;
    /* A wait under masked IRQs is the #1093 hang, so refuse outright. */
    if (be->irqs_enabled && !be->irqs_enabled()) return SLM_PROG_ERR_IRQS_OFF;

    const struct ga10b_prog *p = &ps->p;
    uint32_t sema_len = ga10b_prog_sema_bytes(p);
    ga10b_prog_sema_arm(p);
    ops->cache_clean(ops->ctx, p->sema.cpu, sema_len);

    uint64_t t0 = be->now_us();
    int rc = be->submit(ga10b_prog_gp_entry(p),
                        p->sema.cpu + GA10B_PROG_SEMA_END * GA10B_PROG_SEMA_SLOT_BYTES,
                        GA10B_PROG_PAYLOAD_END, SLM_PROG_STEP_TIMEOUT_MS);
    if (rc != 0) {
        /* Nothing reached the GPU: not fatal to the session's state, but
         * the session can't run this step; Rust moves it to the CPU. */
        return SLM_PROG_ERR_SUBMIT;
    }
    uint64_t deadline = t0 + (uint64_t)SLM_PROG_STEP_TIMEOUT_MS * 1000u;
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
    return 0;
}

uint64_t slm_prog_profile_ns(int32_t h, uint32_t shape, uint32_t idx)
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

int slm_prog_kernel_available(uint32_t kernel, uint32_t rows)
{
    const struct slm_prog_backend *be = backend();
    uint64_t va = 0;
    return be && be->kernel_va(kernel, rows, &va) == 0 && va != 0u;
}

uint32_t slm_prog_max_batch(void)
{
    if (!slm_prog_kernel_available(SLM_PROG_K_GQA_BATCH, 8u) ||
        !slm_prog_kernel_available(SLM_PROG_K_MM_Q4K, 8u) ||
        !slm_prog_kernel_available(SLM_PROG_K_MM_Q6K, 8u)) {
        return 1u;
    }
    if (slm_prog_kernel_available(SLM_PROG_K_MM_Q4K, 64u) &&
        slm_prog_kernel_available(SLM_PROG_K_MM_Q6K, 64u)) {
        return 64u;
    }
    return 8u;
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
    default: return "unknown error";
    }
}

/* ---- Jetson backend ---- */

#if defined(PLATFORM_JETSON_ORIN_NANO)

#include "bpmp.h"
#include "kbuf.h"
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

/* (op_kind, tier, dtype) in the operator library, from MANIFEST.json. */
static int jb_kernel_va(uint32_t kernel, uint32_t rows, uint64_t *va)
{
    uint32_t kind = 0, tier = SLM_GPU_TIER_SIMT, dtype = SLM_GPU_DTYPE_FP16;
    switch (kernel) {
    case SLM_PROG_K_RMSNORM: kind = SLM_GPU_OP_RMSNORM; break;
    case SLM_PROG_K_ROPE:    kind = SLM_GPU_OP_ROPE; break;
    case SLM_PROG_K_SWIGLU:  kind = SLM_GPU_OP_SWIGLU; break;
    case SLM_PROG_K_GQA:     kind = SLM_GPU_OP_GQA_ATTN; break;
    case SLM_PROG_K_GQA_BATCH:
        kind = SLM_GPU_OP_GQA_ATTN;
        tier = SLM_GPU_TIER_HMMA;
        break;
    case SLM_PROG_K_MM_Q4K:
    case SLM_PROG_K_MM_Q6K:
        kind = kernel == SLM_PROG_K_MM_Q4K ? SLM_GPU_OP_MM_Q4K : SLM_GPU_OP_MM_Q6K;
        tier = SLM_GPU_TIER_HMMA;
        dtype = rows <= 8u ? SLM_GPU_DTYPE_MM_B8 : SLM_GPU_DTYPE_MM_B64;
        break;
    case SLM_PROG_K_ADD:     kind = SLM_GPU_OP_ADD_F16; break;
    default: return -1;
    }
    size_t size = 0;
    return oplib_pool_get_sass_gpu_va(kind, tier, dtype, va, &size);
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
    return &g_jb;
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

void slm_prog_clocks(uint64_t *emc_hz, uint64_t *gpc_hz)
{
    if (emc_hz) *emc_hz = 0;
    if (gpc_hz) *gpc_hz = 0;
}

#endif
