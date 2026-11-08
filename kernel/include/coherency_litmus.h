/*
 * coherency_litmus.h - Freestanding cache-coherency litmus patterns (#1193)
 *
 * Implements P1-P6 and S1 from docs/archive/coherency-gauntlet/litmus-spec.md
 * with the same code shapes, barriers and forbidden-outcome definitions as
 * the Linux harness (scripts/coherency/, C1), so the two produce rows that
 * compare line for line.
 *
 * Rules every pattern here follows:
 *   - Shared variables live in ordinary cacheable memory (Normal WB, Inner
 *     Shareable), one variable per 128-byte line (struct coh_line). The
 *     callers place them in .bss or PMM pages, never in ncmem.
 *   - No DC or IC cache maintenance anywhere. Only plain LDR/STR (volatile
 *     accesses), LDAR, STLR, DMB ISH / ISHST / ISHLD and LDXR/STXR
 *     (LDAXR/STLXR for the barrier counter).
 *   - Every wait loop has a no-progress timeout measured with CNTPCT_EL0
 *     against a tick budget the caller derives from CNTFRQ_EL0 (1 s in the
 *     spec). A loop that sees no change in its watched value for the whole
 *     budget counts one timeout and raises the shared abort flag so the
 *     other roles stop instead of waiting out their own budgets.
 *
 * The forbidden-outcome predicates are separate inline functions so the
 * test suite can feed them synthetic observations and prove a forbidden
 * outcome is counted (kernel/tests/test_coherency.c).
 *
 * The negative control (c) in kernel/src/coherency.c (data cache off on
 * the reader) is not a pattern and does use DC CVAC / DC CIVAC by design.
 *
 * ARM64 only: the whole header is empty on x86-64.
 */

#ifndef COHERENCY_LITMUS_H
#define COHERENCY_LITMUS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#if !defined(PLATFORM_X86_64)

/* One shared variable per line. 128 bytes covers the 64-byte lines of
 * Cortex-A76 / A78AE and any 128-byte prefetch pairing. */
#define COH_LINE_BYTES          128u

/* Wait loops read CNTPCT_EL0 and the abort flag once per this many spins. */
#define COH_CHECK_SPINS         64u

/* Writers poll the abort flag once per this many iterations. */
#define COH_WRITER_ABORT_MASK   0xFFFu

struct coh_line {
    alignas(COH_LINE_BYTES) volatile uint64_t v;
};
static_assert(sizeof(struct coh_line) == COH_LINE_BYTES,
              "coh_line must fill exactly one 128-byte line");

/*
 * Every variable any pattern touches. The SLM-OS driver keeps one copy in
 * .bss for the litmus run and one at the base of a PMM 2 MB block for the
 * negative controls (which alias that block with other attributes).
 */
struct coh_vars {
    struct coh_line data;       /* P1/P2/S1 payload */
    struct coh_line flag;       /* P1/P2/S1 flag */
    struct coh_line x;          /* P3 T0 store, P4 counter */
    struct coh_line y;          /* P3 T1 store */
    struct coh_line ctr;        /* P5 ping-pong counter */
    struct coh_line sum;        /* P6 atomic sum */
    struct coh_line res0[2];    /* P3 T0 per-round reads (double buffered) */
    struct coh_line res1[2];    /* P3 T1 per-round reads (double buffered) */
    struct coh_line bar_count;  /* sense-reversing barrier: arrivals */
    struct coh_line bar_sense;  /* sense-reversing barrier: sense */
    struct coh_line abort;      /* non-zero once any role timed out */
};

/* ------------------------------------------------------------------------
 * Access primitives
 * ------------------------------------------------------------------------ */

static inline uint64_t coh_ld(const volatile uint64_t *p)
{
    return *p;                                      /* plain LDR */
}

static inline void coh_st(volatile uint64_t *p, uint64_t v)
{
    *p = v;                                         /* plain STR */
}

static inline uint64_t coh_ldar(const volatile uint64_t *p)
{
    uint64_t v;
    __asm__ volatile("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline void coh_stlr(volatile uint64_t *p, uint64_t v)
{
    __asm__ volatile("stlr %1, [%0]" : : "r"(p), "r"(v) : "memory");
}

static inline void coh_dmb_ish(void)
{
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline void coh_dmb_ishst(void)
{
    __asm__ volatile("dmb ishst" ::: "memory");
}

static inline void coh_dmb_ishld(void)
{
    __asm__ volatile("dmb ishld" ::: "memory");
}

/* Fetch-and-add with acquire + release (LDAXR/STLXR), for the barrier. */
static inline uint64_t coh_fetch_add_acqrel(volatile uint64_t *p, uint64_t inc)
{
    uint64_t old;
    uint64_t tmp;
    uint32_t fail;
    __asm__ volatile(
        "1: ldaxr  %0, [%3]\n"
        "   add    %1, %0, %4\n"
        "   stlxr  %w2, %1, [%3]\n"
        "   cbnz   %w2, 1b\n"
        : "=&r"(old), "=&r"(tmp), "=&r"(fail)
        : "r"(p), "r"(inc)
        : "memory");
    return old;
}

/* Relaxed atomic increment (LDXR/STXR), for P6. */
static inline void coh_atomic_inc(volatile uint64_t *p)
{
    uint64_t tmp;
    uint32_t fail;
    __asm__ volatile(
        "1: ldxr   %0, [%2]\n"
        "   add    %0, %0, #1\n"
        "   stxr   %w1, %0, [%2]\n"
        "   cbnz   %w1, 1b\n"
        : "=&r"(tmp), "=&r"(fail)
        : "r"(p)
        : "memory");
}

static inline uint64_t coh_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t coh_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* ------------------------------------------------------------------------
 * Forbidden-outcome predicates (spec table, last column)
 * ------------------------------------------------------------------------ */

/* P1, P2 (and S1, where the outcome is allowed): flag seen ahead of data. */
static inline bool coh_mp_is_forbidden(uint64_t f, uint64_t d)
{
    return d < f;
}

/* P3: in round r both threads read the other's variable from before r. */
static inline bool coh_sb_is_forbidden(uint64_t r, uint64_t r0, uint64_t r1)
{
    return r0 < r && r1 < r;
}

/* P4: the second of two back-to-back reads returned an older value. */
static inline bool coh_corr_is_forbidden(uint64_t first, uint64_t second)
{
    return second < first;
}

/* P5 / P6 end state: a lost or duplicated step. */
static inline bool coh_final_is_forbidden(uint64_t final, uint64_t expected)
{
    return final != expected;
}

/* ------------------------------------------------------------------------
 * Per-role environment, result and no-progress tracking
 * ------------------------------------------------------------------------ */

struct coh_env {
    volatile uint64_t *abort;       /* shared abort flag (struct coh_vars.abort) */
    uint64_t timeout_ticks;         /* no-progress budget in CNTPCT ticks */
};

struct coh_result {
    uint64_t forbidden;             /* forbidden outcomes observed by this role */
    uint64_t timeouts;              /* wait loops that made no progress */
};

enum coh_wait_status {
    COH_WAIT_OK = 0,
    COH_WAIT_TIMEOUT = 1,           /* this role saw no progress; counted */
    COH_WAIT_ABORTED = 2,           /* another role timed out; not counted */
};

struct coh_wait {
    uint64_t last;                  /* last value of the watched variable */
    uint64_t since;                 /* CNTPCT when it last changed */
    uint32_t spins;
    bool changed;
};

static inline void coh_wait_init(struct coh_wait *w, uint64_t watched)
{
    w->last = watched;
    w->since = coh_cntpct();
    w->spins = 0;
    w->changed = false;
}

static inline void coh_raise_abort(const struct coh_env *env)
{
    coh_stlr(env->abort, 1);
}

/*
 * Called once per spin of a wait loop with the current watched value.
 * Every COH_CHECK_SPINS spins it reads the counter and the abort flag.
 * On a timeout it counts the timeout in @res and raises the abort flag.
 */
static inline enum coh_wait_status coh_wait_tick(struct coh_wait *w, uint64_t watched,
                                                 const struct coh_env *env,
                                                 struct coh_result *res)
{
    if (watched != w->last) {
        w->last = watched;
        w->changed = true;
    }
    if (++w->spins < COH_CHECK_SPINS) {
        return COH_WAIT_OK;
    }
    w->spins = 0;
    if (coh_ld(env->abort) != 0) {
        return COH_WAIT_ABORTED;
    }
    uint64_t now = coh_cntpct();
    if (w->changed) {
        w->changed = false;
        w->since = now;
        return COH_WAIT_OK;
    }
    if (now - w->since > env->timeout_ticks) {
        res->timeouts++;
        coh_raise_abort(env);
        return COH_WAIT_TIMEOUT;
    }
    return COH_WAIT_OK;
}

/* ------------------------------------------------------------------------
 * Sense-reversing spin barrier (P3 rounds, start of every run)
 * ------------------------------------------------------------------------ */

/*
 * @local_sense is the caller's private sense word, 0 before the first
 * wait. Arrivals on the counter count as progress, so a slow participant
 * only times the others out once nobody has arrived for the whole budget.
 */
static inline enum coh_wait_status coh_barrier_wait(volatile uint64_t *count,
                                                    volatile uint64_t *sense,
                                                    uint64_t nparts,
                                                    uint64_t *local_sense,
                                                    const struct coh_env *env,
                                                    struct coh_result *res)
{
    *local_sense ^= 1u;
    uint64_t old = coh_fetch_add_acqrel(count, 1);
    if (old == nparts - 1) {
        coh_st(count, 0);
        coh_stlr(sense, *local_sense);
        return COH_WAIT_OK;
    }
    struct coh_wait w;
    coh_wait_init(&w, coh_ld(count));
    while (coh_ldar(sense) != *local_sense) {
        enum coh_wait_status st = coh_wait_tick(&w, coh_ld(count), env, res);
        if (st != COH_WAIT_OK) {
            return st;
        }
    }
    return COH_WAIT_OK;
}

/* ------------------------------------------------------------------------
 * P1 / P2 / S1: message passing
 * ------------------------------------------------------------------------ */

enum coh_mp_kind {
    COH_MP_REL_ACQ = 0,     /* P1: STLR flag / LDAR flag */
    COH_MP_DMB = 1,         /* P2: DMB ISHST / DMB ISHLD */
    COH_MP_NONE = 2,        /* S1: no barriers (relaxed outcome allowed) */
};

static inline void coh_mp_writer(enum coh_mp_kind kind, volatile uint64_t *data,
                                 volatile uint64_t *flag, uint64_t n,
                                 const struct coh_env *env)
{
    switch (kind) {
    case COH_MP_REL_ACQ:
        for (uint64_t i = 1; i <= n; i++) {
            coh_st(data, i);
            coh_stlr(flag, i);
            if ((i & COH_WRITER_ABORT_MASK) == 0 && coh_ld(env->abort) != 0) {
                return;
            }
        }
        break;
    case COH_MP_DMB:
        for (uint64_t i = 1; i <= n; i++) {
            coh_st(data, i);
            coh_dmb_ishst();
            coh_st(flag, i);
            if ((i & COH_WRITER_ABORT_MASK) == 0 && coh_ld(env->abort) != 0) {
                return;
            }
        }
        break;
    case COH_MP_NONE:
        for (uint64_t i = 1; i <= n; i++) {
            coh_st(data, i);
            coh_st(flag, i);
            if ((i & COH_WRITER_ABORT_MASK) == 0 && coh_ld(env->abort) != 0) {
                return;
            }
        }
        break;
    }
}

/* One reader observation in the shape of @kind: f = flag, then d = data. */
static inline void coh_mp_read_once(enum coh_mp_kind kind, const volatile uint64_t *data,
                                    const volatile uint64_t *flag,
                                    uint64_t *f_out, uint64_t *d_out)
{
    uint64_t f;
    uint64_t d;
    switch (kind) {
    case COH_MP_REL_ACQ:
        f = coh_ldar(flag);
        d = coh_ld(data);
        break;
    case COH_MP_DMB:
        f = coh_ld(flag);
        coh_dmb_ishld();
        d = coh_ld(data);
        break;
    default:
        f = coh_ld(flag);
        d = coh_ld(data);
        break;
    }
    *f_out = f;
    *d_out = d;
}

/* Reads until flag reaches @n; counts every d < f observation. */
static inline void coh_mp_reader(enum coh_mp_kind kind, const volatile uint64_t *data,
                                 const volatile uint64_t *flag, uint64_t n,
                                 const struct coh_env *env, struct coh_result *res)
{
    uint64_t f;
    uint64_t d;
    coh_mp_read_once(kind, data, flag, &f, &d);
    struct coh_wait w;
    coh_wait_init(&w, f);
    for (;;) {
        if (coh_mp_is_forbidden(f, d)) {
            res->forbidden++;
        }
        if (f >= n) {
            return;
        }
        if (coh_wait_tick(&w, f, env, res) != COH_WAIT_OK) {
            return;
        }
        coh_mp_read_once(kind, data, flag, &f, &d);
    }
}

/* ------------------------------------------------------------------------
 * P3: store buffering with DMB ISH, rounds separated by the barrier
 * ------------------------------------------------------------------------ */

/*
 * @me is 0 for T0 (stores x, reads y) and 1 for T1 (stores y, reads x).
 * Each round's read goes to res<me>[r & 1]. T0 judges round r - 1 right
 * after the barrier that opens round r (and round n after a final
 * barrier); T1 cannot overwrite that slot until round r + 1, which needs
 * T0 at the next barrier first.
 */
static inline void coh_sb_thread(uint32_t me, struct coh_vars *v, uint64_t n,
                                 uint64_t *local_sense, const struct coh_env *env,
                                 struct coh_result *res)
{
    volatile uint64_t *mine = (me == 0) ? &v->x.v : &v->y.v;
    const volatile uint64_t *other = (me == 0) ? &v->y.v : &v->x.v;
    struct coh_line *my_res = (me == 0) ? v->res0 : v->res1;

    for (uint64_t r = 1; r <= n; r++) {
        if (coh_barrier_wait(&v->bar_count.v, &v->bar_sense.v, 2, local_sense,
                             env, res) != COH_WAIT_OK) {
            return;
        }
        if (me == 0 && r > 1) {
            uint64_t pr = r - 1;
            if (coh_sb_is_forbidden(pr, coh_ld(&v->res0[pr & 1].v),
                                    coh_ld(&v->res1[pr & 1].v))) {
                res->forbidden++;
            }
        }
        coh_st(mine, r);
        coh_dmb_ish();
        uint64_t got = coh_ld(other);
        coh_st(&my_res[r & 1].v, got);
    }
    if (coh_barrier_wait(&v->bar_count.v, &v->bar_sense.v, 2, local_sense,
                         env, res) != COH_WAIT_OK) {
        return;
    }
    if (me == 0 && n > 0) {
        if (coh_sb_is_forbidden(n, coh_ld(&v->res0[n & 1].v),
                                coh_ld(&v->res1[n & 1].v))) {
            res->forbidden++;
        }
    }
}

/* ------------------------------------------------------------------------
 * P4: read-read coherence (CoRR) with plain accesses
 * ------------------------------------------------------------------------ */

static inline void coh_corr_writer(volatile uint64_t *x, uint64_t n,
                                   const struct coh_env *env)
{
    for (uint64_t i = 1; i <= n; i++) {
        coh_st(x, i);
        if ((i & COH_WRITER_ABORT_MASK) == 0 && coh_ld(env->abort) != 0) {
            return;
        }
    }
}

static inline void coh_corr_reader(const volatile uint64_t *x, uint64_t n,
                                   const struct coh_env *env, struct coh_result *res)
{
    uint64_t a = coh_ld(x);
    uint64_t b = coh_ld(x);
    struct coh_wait w;
    coh_wait_init(&w, b);
    for (;;) {
        if (coh_corr_is_forbidden(a, b)) {
            res->forbidden++;
        }
        if (b >= n) {
            return;
        }
        if (coh_wait_tick(&w, b, env, res) != COH_WAIT_OK) {
            return;
        }
        a = coh_ld(x);
        b = coh_ld(x);
    }
}

/* ------------------------------------------------------------------------
 * P5: ping-pong on one counter, plain loads, STLR stores
 * ------------------------------------------------------------------------ */

/*
 * @parity 0 is CPU A (acts on even values), 1 is CPU B (odd values).
 * Over n round trips A writes 1, 3, ..., 2n-1 and B writes 2, 4, ..., 2n,
 * so the final value is 2n. A value of this role's parity other than the
 * expected one is a lost (ahead) or duplicated (behind) step and counts as
 * forbidden; the role resyncs on an ahead value and ignores a behind one.
 */
static inline void coh_pingpong(uint32_t parity, volatile uint64_t *ctr, uint64_t n,
                                const struct coh_env *env, struct coh_result *res)
{
    if (n == 0) {
        return;
    }
    uint64_t last_mine = 2 * n - 2 + parity;    /* last value this role acts on */
    uint64_t expect = parity;
    uint64_t last_stale = UINT64_MAX;
    uint64_t v = coh_ld(ctr);
    struct coh_wait w;
    coh_wait_init(&w, v);
    while (expect <= last_mine) {
        if ((v & 1u) == parity) {
            if (v == expect) {
                coh_stlr(ctr, v + 1);
                expect = v + 2;
                v = coh_ld(ctr);
                continue;
            }
            if (v > expect) {
                res->forbidden++;               /* a step was lost */
                if (v > last_mine) {
                    return;
                }
                coh_stlr(ctr, v + 1);
                expect = v + 2;
                v = coh_ld(ctr);
                continue;
            }
            /* v < expect: an older value after this role's own newer
             * store is a coherence violation; count it once per stale
             * value and keep waiting. */
            if (v != last_stale) {
                res->forbidden++;
                last_stale = v;
            }
        }
        if (coh_wait_tick(&w, v, env, res) != COH_WAIT_OK) {
            return;
        }
        v = coh_ld(ctr);
    }
}

/* ------------------------------------------------------------------------
 * P6: atomic sum with exclusives
 * ------------------------------------------------------------------------ */

static inline void coh_atomic_sum(volatile uint64_t *sum, uint64_t n,
                                  const struct coh_env *env)
{
    for (uint64_t i = 1; i <= n; i++) {
        coh_atomic_inc(sum);
        if ((i & COH_WRITER_ABORT_MASK) == 0 && coh_ld(env->abort) != 0) {
            return;
        }
    }
}

#endif /* !PLATFORM_X86_64 */

#endif /* COHERENCY_LITMUS_H */
