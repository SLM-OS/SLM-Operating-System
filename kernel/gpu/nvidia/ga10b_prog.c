/*
 * ga10b_prog.c - Prebuilt compute programs. See ga10b_prog.h.
 *
 * Method encodings match ga10b_bringup.c's launch and semaphore
 * builders, which are verified on hardware; the difference is that the
 * state setup is emitted once, not per launch.
 */
#include "ga10b_prog.h"

#include "ga10b_channel_handoff.h"
#include "ga10b_qmd.h"

#include <string.h>

#define SUBCH_COMPUTE 1u

/* Host class method. */
#define M_SET_OBJECT 0x0000u
/* AMPERE_COMPUTE_B (clc7c0.h). */
#define M_WAIT_FOR_IDLE                     0x0110u
#define M_REPORT_SEMAPHORE_PAYLOAD_LOWER    0x0158u  /* ...UPPER, ADDR_LO, ADDR_HI, EXECUTE */
#define M_INVALIDATE_SHADER_CACHES          0x021cu
#define M_INVALIDATE_TEX_HEADER_NO_WFI      0x0244u
#define M_INVALIDATE_SKED_CACHES            0x0298u
#define M_SHARED_MEM_WINDOW_A               0x02a0u
#define M_SEND_PCAS_A                       0x02b4u
#define M_SEND_SIGNALING_PCAS2_B            0x02c0u
#define M_LOCAL_MEM_WINDOW_A                0x07b0u

#define COMPUTE_CLASS 0xC7C0u
#define PCAS2_INVALIDATE_COPY_SCHEDULE 0xAu
#define SHARED_WINDOW_BASE 0xfe000000u
#define LOCAL_WINDOW_BASE  0xff000000u
#define INVALIDATE_ALL     0x1017u  /* instruction, locks, flush, data, constant */
#define INVALIDATE_DATA_FLUSH 0x0014u /* DATA | FLUSH_DATA */

/* REPORT_SEMAPHORE_EXECUTE: OPERATION=RELEASE, STRUCTURE_SIZE=FOUR_WORDS. */
#define SEM_RELEASE_FOUR_WORDS 0x0u
#define SEM_FLUSH_DISABLE      (1u << 5)

/* Dwords: prologue is 6 + 3 + 3 + 3 (windows, immediates) + start sema. */
#define PB_PROLOGUE_DWORDS 20u
#define PB_LAUNCH_DWORDS    3u  /* SEND_PCAS_A pair + PCAS2_B immediate */
#define PB_BARRIER_DWORDS   2u  /* two immediates at most */
#define PB_SEMA_DWORDS      6u  /* 5-method incrementing run */
#define PB_DRAIN_DWORDS     2u  /* WFI + flush before a timestamp means anything */
#define PB_MARK_DWORDS      (1u + PB_SEMA_DWORDS)  /* WFI only if the barrier has none */

static inline uint32_t hdr_inc(uint32_t count, uint32_t byte_off)
{
    return (1u << 29) | (count << 16) | (SUBCH_COMPUTE << 13) | ((byte_off >> 2) & 0x1FFFu);
}

static inline uint32_t hdr_immd(uint32_t byte_off, uint32_t data)
{
    return (4u << 29) | ((data & 0x1FFFu) << 16) | (SUBCH_COMPUTE << 13) |
           ((byte_off >> 2) & 0x1FFFu);
}

void ga10b_prog_sizes(uint32_t max_launches, uint32_t flags, uint32_t *pb_bytes,
                      uint32_t *qmd_bytes, uint32_t *cbuf_bytes, uint32_t *sema_bytes)
{
    /* Worst case: a barrier after every launch. */
    uint32_t per = PB_LAUNCH_DWORDS + PB_BARRIER_DWORDS;
    uint32_t slots = GA10B_PROG_SEMA_OP0;
    if (flags & GA10B_PROG_F_PROFILE) {
        per += PB_DRAIN_DWORDS + PB_SEMA_DWORDS;
        slots += max_launches;
    }
    uint32_t plain = PB_PROLOGUE_DWORDS + PB_DRAIN_DWORDS + PB_SEMA_DWORDS + per * max_launches;
    uint32_t total = plain;
    if (flags & GA10B_PROG_F_MARKS) {
        total += plain + PB_MARK_DWORDS * max_launches;
        slots += max_launches;
    }
    if (pb_bytes) {
        *pb_bytes = total * 4u;
    }
    if (qmd_bytes) *qmd_bytes = max_launches * GA10B_QMD_SIZE_BYTES;
    if (cbuf_bytes) *cbuf_bytes = max_launches * GA10B_PROG_CBUF_STRIDE;
    if (sema_bytes) *sema_bytes = slots * GA10B_PROG_SEMA_SLOT_BYTES;
}

/* Append to the plain pushbuffer and, when there is one, the marked copy. */
static void pb_put(struct ga10b_prog *p, const uint32_t *w, uint32_t n)
{
    uint32_t *base = (uint32_t *)(void *)p->pb.cpu;
    memcpy(base + p->pb_dwords, w, n * 4u);
    p->pb_dwords += n;
    if (p->flags & GA10B_PROG_F_MARKS) {
        memcpy(base + p->marks_base + p->marks_dwords, w, n * 4u);
        p->marks_dwords += n;
    }
}

static void sema_words(const struct ga10b_prog *p, uint32_t *w, uint32_t slot, uint32_t payload,
                       uint32_t bits)
{
    uint64_t va = p->sema.gpu_va + (uint64_t)slot * GA10B_PROG_SEMA_SLOT_BYTES;
    w[0] = hdr_inc(5u, M_REPORT_SEMAPHORE_PAYLOAD_LOWER);
    w[1] = payload;
    w[2] = 0u;
    w[3] = (uint32_t)va;
    w[4] = (uint32_t)(va >> 32) & 0xFFu;
    w[5] = SEM_RELEASE_FOUR_WORDS | bits;
}

static void emit_sema(struct ga10b_prog *p, uint32_t slot, uint32_t payload, uint32_t bits)
{
    uint32_t w[PB_SEMA_DWORDS];
    sema_words(p, w, slot, payload, bits);
    pb_put(p, w, PB_SEMA_DWORDS);
}

int ga10b_prog_init(struct ga10b_prog *p, const struct ga10b_prog_mem *pb,
                    const struct ga10b_prog_mem *qmd, const struct ga10b_prog_mem *cbuf,
                    const struct ga10b_prog_mem *sema, uint32_t max_launches,
                    uint32_t flags, enum ga10b_prog_barrier barrier)
{
    if (!p || !pb || !qmd || !cbuf || !sema || max_launches == 0u ||
        max_launches > GA10B_PROG_MAX_LAUNCHES || barrier >= GA10B_PROG_BARRIER_COUNT) {
        return GA10B_PROG_ERR_ARG;
    }
    uint32_t need_pb, need_q, need_c, need_s;
    ga10b_prog_sizes(max_launches, flags, &need_pb, &need_q, &need_c, &need_s);
    if (!pb->cpu || !qmd->cpu || !cbuf->cpu || !sema->cpu || pb->size < need_pb ||
        qmd->size < need_q || cbuf->size < need_c || sema->size < need_s) {
        return GA10B_PROG_ERR_ARG;
    }
    if ((pb->gpu_va & 3u) || (qmd->gpu_va & 0xFFu) || (cbuf->gpu_va & 0xFFu) ||
        (sema->gpu_va & 0xFu)) {
        return GA10B_PROG_ERR_ARG;
    }
    memset(p, 0, sizeof(*p));
    p->pb = *pb;
    p->qmd = *qmd;
    p->cbuf = *cbuf;
    p->sema = *sema;
    p->max_launches = max_launches;
    p->flags = flags;
    p->barrier = barrier;
    if (flags & GA10B_PROG_F_MARKS) {
        /* The marked copy starts where the largest plain program would end. */
        uint32_t plain;
        ga10b_prog_sizes(max_launches, flags & ~(uint32_t)GA10B_PROG_F_MARKS, &plain, NULL, NULL, NULL);
        p->marks_base = plain / 4u;
    }

    uint32_t w[PB_PROLOGUE_DWORDS];
    uint32_t n = 0;
    w[n++] = hdr_inc(1u, M_SET_OBJECT);
    w[n++] = COMPUTE_CLASS;
    w[n++] = hdr_inc(2u, M_SHARED_MEM_WINDOW_A);
    w[n++] = 0u;
    w[n++] = SHARED_WINDOW_BASE;
    w[n++] = hdr_inc(2u, M_LOCAL_MEM_WINDOW_A);
    w[n++] = 0u;
    w[n++] = LOCAL_WINDOW_BASE;
    w[n++] = hdr_immd(M_INVALIDATE_SKED_CACHES, 0u);
    w[n++] = hdr_immd(M_INVALIDATE_TEX_HEADER_NO_WFI, 0u);
    w[n++] = hdr_immd(M_INVALIDATE_SHADER_CACHES, INVALIDATE_ALL);
    pb_put(p, w, n);
    emit_sema(p, GA10B_PROG_SEMA_START, GA10B_PROG_PAYLOAD_START, SEM_FLUSH_DISABLE);
    return GA10B_PROG_OK;
}

/* Fill the QMD: ga10b_qmd_pool_prepare's field set, plus the lean option. */
static void write_qmd(const struct ga10b_prog *p, uint32_t idx, const struct ga10b_prog_launch *l)
{
    uint32_t *q = ga10b_prog_qmd(p, idx);
    uint64_t cbuf_va = p->cbuf.gpu_va + (uint64_t)idx * GA10B_PROG_CBUF_STRIDE;
    ga10b_qmd_populate(q, l->shader_gpu_va, cbuf_va, l->register_count_v,
                       l->grid_x, l->grid_y, l->grid_z,
                       l->block_x, l->block_y, l->block_z);
    ga10b_qmd_set_bits(q, GA10B_QMD_SHARED_MEMORY_SIZE_HI, GA10B_QMD_SHARED_MEMORY_SIZE_LO,
                       l->smem_size_bytes);
    ga10b_qmd_set_bits(q, GA10B_QMD_SHADER_LOCAL_MEM_LOW_SIZE_HI,
                       GA10B_QMD_SHADER_LOCAL_MEM_LOW_SIZE_LO, l->slm_size_bytes);
    ga10b_qmd_set_bits(q, GA10B_QMD_BARRIER_COUNT_HI, GA10B_QMD_BARRIER_COUNT_LO,
                       l->barrier_count);
    if (p->flags & GA10B_PROG_F_LEAN_QMD) {
        static const unsigned off[] = {
            GA10B_QMD_INVALIDATE_TEXTURE_HEADER_CACHE_BIT,
            GA10B_QMD_INVALIDATE_TEXTURE_SAMPLER_CACHE_BIT,
            GA10B_QMD_INVALIDATE_TEXTURE_DATA_CACHE_BIT,
            GA10B_QMD_INVALIDATE_INSTRUCTION_CACHE_BIT,
        };
        for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
            ga10b_qmd_set_bits(q, off[i], off[i], 0u);
        }
        ga10b_qmd_set_bits(q, GA10B_QMD_CWD_MEMBAR_TYPE_HI_BIT, GA10B_QMD_CWD_MEMBAR_TYPE_LO_BIT,
                           GA10B_QMD_CWD_MEMBAR_TYPE_L1_NONE);
    }
}

static void write_cbuf(const struct ga10b_prog *p, uint32_t idx, const struct ga10b_prog_launch *l)
{
    uint8_t *c = p->cbuf.cpu + (size_t)idx * GA10B_PROG_CBUF_STRIDE;
    memset(c, 0, GA10B_PROG_CBUF_STRIDE);
    memcpy(c + GA10B_PROG_CBUF_ARGS_BASE, l->args, l->args_len);
}

static bool launch_valid(const struct ga10b_prog_launch *l)
{
    return l && l->shader_gpu_va != 0u && l->args && l->args_len <= GA10B_PROG_CBUF_ARGS_MAX &&
           l->grid_x != 0u && l->grid_y != 0u && l->grid_z != 0u &&
           l->block_x != 0u && l->block_y != 0u && l->block_z != 0u;
}

/* WAIT_FOR_IDLE + flush of shader data, whatever kind of barrier the program
 * uses between launches. */
static void emit_drain(struct ga10b_prog *p)
{
    const uint32_t w[PB_DRAIN_DWORDS] = {
        hdr_immd(M_WAIT_FOR_IDLE, 0u),
        hdr_immd(M_INVALIDATE_SHADER_CACHES, INVALIDATE_DATA_FLUSH),
    };
    pb_put(p, w, PB_DRAIN_DWORDS);
}

int ga10b_prog_add_launch(struct ga10b_prog *p, const struct ga10b_prog_launch *l)
{
    if (!p || !launch_valid(l)) return GA10B_PROG_ERR_ARG;
    if (p->finished) return GA10B_PROG_ERR_STATE;
    if (p->n_launches >= p->max_launches) return GA10B_PROG_ERR_FULL;
    uint32_t idx = p->n_launches++;
    write_qmd(p, idx, l);
    write_cbuf(p, idx, l);

    uint64_t qva = p->qmd.gpu_va + (uint64_t)idx * GA10B_QMD_SIZE_BYTES;
    const uint32_t w[PB_LAUNCH_DWORDS] = {
        hdr_inc(1u, M_SEND_PCAS_A),
        (uint32_t)(qva >> 8),
        hdr_immd(M_SEND_SIGNALING_PCAS2_B, PCAS2_INVALIDATE_COPY_SCHEDULE),
    };
    pb_put(p, w, PB_LAUNCH_DWORDS);
    if (p->flags & GA10B_PROG_F_PROFILE) {
        emit_drain(p);
        emit_sema(p, GA10B_PROG_SEMA_OP0 + idx, GA10B_PROG_PAYLOAD_OP, 0u);
    }
    return (int)idx;
}

int ga10b_prog_update_launch(struct ga10b_prog *p, uint32_t idx,
                             const struct ga10b_prog_launch *l)
{
    if (!p || !launch_valid(l) || idx >= p->n_launches) return GA10B_PROG_ERR_ARG;
    write_qmd(p, idx, l);
    write_cbuf(p, idx, l);
    return GA10B_PROG_OK;
}

int ga10b_prog_add_barrier(struct ga10b_prog *p)
{
    if (!p) return GA10B_PROG_ERR_ARG;
    if (p->finished) return GA10B_PROG_ERR_STATE;
    uint32_t w[PB_BARRIER_DWORDS];
    uint32_t n = 0;
    if (p->barrier == GA10B_PROG_BARRIER_WFI_FLUSH || p->barrier == GA10B_PROG_BARRIER_WFI) {
        w[n++] = hdr_immd(M_WAIT_FOR_IDLE, 0u);
    }
    if (p->barrier == GA10B_PROG_BARRIER_WFI_FLUSH || p->barrier == GA10B_PROG_BARRIER_FLUSH) {
        w[n++] = hdr_immd(M_INVALIDATE_SHADER_CACHES, INVALIDATE_DATA_FLUSH);
    }
    pb_put(p, w, n);
    p->n_barriers++;
    return GA10B_PROG_OK;
}

int ga10b_prog_add_mark(struct ga10b_prog *p)
{
    if (!p || !(p->flags & GA10B_PROG_F_MARKS)) return GA10B_PROG_ERR_ARG;
    if (p->finished) return GA10B_PROG_ERR_STATE;
    if (p->n_marks >= p->max_launches) return GA10B_PROG_ERR_FULL;
    uint32_t *w = (uint32_t *)(void *)p->pb.cpu + p->marks_base + p->marks_dwords;
    uint32_t n = 0;
    /* A release doesn't wait for launched work, so a barrier without an idle
     * needs one here. That only happens in `slm gpu opts flush`. */
    if (p->barrier == GA10B_PROG_BARRIER_FLUSH) {
        w[n++] = hdr_immd(M_WAIT_FOR_IDLE, 0u);
    }
    sema_words(p, w + n, ga10b_prog_mark_slot0(p) + p->n_marks, GA10B_PROG_PAYLOAD_MARK,
               SEM_FLUSH_DISABLE);
    p->marks_dwords += n + PB_SEMA_DWORDS;
    return (int)p->n_marks++;
}

int ga10b_prog_finish(struct ga10b_prog *p)
{
    if (!p) return GA10B_PROG_ERR_ARG;
    if (p->finished) return GA10B_PROG_ERR_STATE;
    /* The release alone doesn't wait for launched work: on hardware it fired
     * about 1 us after a launch whose kernel was still running, so logits read
     * back half-computed. Idle first, then write everything to memory. */
    emit_drain(p);
    emit_sema(p, GA10B_PROG_SEMA_END, GA10B_PROG_PAYLOAD_END, 0u);
    p->finished = true;
    return GA10B_PROG_OK;
}

uint64_t ga10b_prog_gp_entry(const struct ga10b_prog *p)
{
    uint32_t e0 = (uint32_t)(p->pb.gpu_va & 0xFFFFFFFCu);
    uint32_t e1 = (uint32_t)((p->pb.gpu_va >> 32) & 0xFFu) | (p->pb_dwords << 10);
    return ((uint64_t)e1 << 32) | e0;
}

uint64_t ga10b_prog_gp_entry_marked(const struct ga10b_prog *p)
{
    if (!(p->flags & GA10B_PROG_F_MARKS)) return 0u;
    uint64_t va = p->pb.gpu_va + (uint64_t)p->marks_base * 4u;
    uint32_t e0 = (uint32_t)(va & 0xFFFFFFFCu);
    uint32_t e1 = (uint32_t)((va >> 32) & 0xFFu) | (p->marks_dwords << 10);
    return ((uint64_t)e1 << 32) | e0;
}

uint8_t *ga10b_prog_cbuf_args(const struct ga10b_prog *p, uint32_t idx)
{
    if (!p || idx >= p->n_launches) return NULL;
    return p->cbuf.cpu + (size_t)idx * GA10B_PROG_CBUF_STRIDE + GA10B_PROG_CBUF_ARGS_BASE;
}

uint32_t *ga10b_prog_qmd(const struct ga10b_prog *p, uint32_t idx)
{
    if (!p || idx >= p->max_launches) return NULL;
    return (uint32_t *)(void *)(p->qmd.cpu + (size_t)idx * GA10B_QMD_SIZE_BYTES);
}

uint32_t ga10b_prog_sema_bytes(const struct ga10b_prog *p)
{
    uint32_t n = GA10B_PROG_SEMA_OP0;
    if (p->flags & GA10B_PROG_F_PROFILE) n += p->max_launches;
    return n * GA10B_PROG_SEMA_SLOT_BYTES;
}

void ga10b_prog_sema_arm(const struct ga10b_prog *p)
{
    memset(p->sema.cpu, 0, ga10b_prog_sema_bytes(p));
}

uint32_t ga10b_prog_mark_slot0(const struct ga10b_prog *p)
{
    return ga10b_prog_sema_bytes(p) / GA10B_PROG_SEMA_SLOT_BYTES;
}

uint32_t ga10b_prog_marks_bytes(const struct ga10b_prog *p)
{
    return p->n_marks * GA10B_PROG_SEMA_SLOT_BYTES;
}

uint32_t ga10b_prog_marks_ns(const struct ga10b_prog *p, uint64_t *ns, uint32_t cap)
{
    uint32_t count = p->n_marks + 1u;
    if (!ns || cap < count) return 0u;
    uint32_t slot0 = ga10b_prog_mark_slot0(p);
    uint64_t prev = ga10b_prog_sema_ts(p, GA10B_PROG_SEMA_START);
    bool prev_ok = true;
    for (uint32_t i = 0; i < count; i++) {
        bool last = i == p->n_marks;
        uint32_t slot = last ? GA10B_PROG_SEMA_END : slot0 + i;
        uint32_t want = last ? GA10B_PROG_PAYLOAD_END : GA10B_PROG_PAYLOAD_MARK;
        bool ok = ga10b_prog_sema_payload(p, slot) == want;
        uint64_t t = ga10b_prog_sema_ts(p, slot);
        ns[i] = (ok && prev_ok && t >= prev) ? t - prev : 0u;
        if (ok) prev = t;
        prev_ok = ok;
    }
    return count;
}
