/*
 * coherency.c - `coherency` shell verb and litmus driver (#1193)
 *
 * Drives the patterns in coherency_litmus.h on chosen CPUs and prints rows
 * in the format shared with the Linux harness (scripts/coherency/, C1):
 *
 *   pattern cpus iters forbidden timeouts ms
 *
 * cpus is `A>B` for the pair patterns (A = writer / T0 / even side of P5),
 * `w<W>/<lo>-<hi>` for P4 (writer W) and `<lo>-<hi>` for P6. Rows come in
 * the order P1 P2 P3 P4 P5 P6 S1 S2. P4 rows append
 * ` pairs=<r1>,<r2>,... straddles=<r1>,<r2>,... coverage=ok|weak`, one
 * value per reader in ascending CPU order, and their iters is the number
 * of writes made. After the rows, one line per ordered pair:
 *
 *   SENS cpus=a>b S1=<n> S2=<n> P2_sensitive=yes|no P3_sensitive=yes|no
 *
 * then `coherency: PASS` or `coherency: FAIL (n rows)`. A P row fails on
 * any forbidden outcome or timeout, and a P4 row also when it is weak (a
 * reader saw no straddle); S1 and S2 are never judged.
 *
 * Revision 3: every value P1, P2, S1 and P4 store carries the row's tag
 * (coherency_litmus.h), the variables start at a reserved-tag value, and
 * MP rows (P1, P2, S1) append ` observed=<n>`: the reader's observations
 * of the row in progress. A P1 or P2 row whose reader observed nothing is
 * weak and fails, like a P4 row without straddles.
 *
 * Per-CPU dispatch: a worker task per remote participant, created at
 * TASK_PRIORITY_HIGH, pinned with task_set_affinity() and queued with
 * scheduler_add_task_to_cpu(). The calling task (shell or test main task)
 * is pinned to the CPU it runs on for the duration of a run and plays that
 * CPU's role inline, so the calling CPU can be one side of a pair. Workers
 * never print: each writes its result into its own cacheable slot and
 * publishes it with STLR of the run's nonce; only the caller prints.
 *
 * All pattern state is ordinary cacheable memory (.bss, or a PMM block for
 * the negative controls), never ncmem, so on hardware a stale line shows
 * up as a forbidden count, a timeout or a lost result, never as a pass.
 * The scheduler's own state (task table, run queues) is what it is on the
 * platform; only the worker's completion is read from it (task state and
 * slot generation), to tell a finished worker whose result never became
 * visible ("lost", counted as a timeout) from one still running.
 */

#if !defined(PLATFORM_X86_64)

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "coherency.h"
#include "coherency_litmus.h"
#include "config.h"
#include "pmm.h"
#include "sched.h"
#include "shell.h"
#include "smp.h"
#include "spinlock.h"
#include "string.h"
#include "task.h"
#include "uart.h"
#include "vmm.h"

/* A worker that has neither published nor finished after this long is
 * abandoned (unstarted ones are reclaimed; see coh_remote_abandon). */
#define COH_CAP_SECONDS         120u

/* Grace period between "worker finished" and "its result is lost". */
#define COH_LOST_GRACE_MS       1000u

/* ------------------------------------------------------------------------
 * Run state
 * ------------------------------------------------------------------------ */

/*
 * One participant of one run. Line 0 holds the parameters the driver
 * writes before dispatch (with a checksum, so a worker that reads a stale
 * or torn copy refuses to run instead of running the wrong pattern),
 * line 1 the outputs, line 2 the completion nonce.
 */
struct coh_part {
    alignas(COH_LINE_BYTES) uint64_t nonce;
    uint32_t pattern;
    uint32_t role;
    uint32_t cpu;
    uint32_t nparts;
    uint64_t n;
    struct coh_vars *wv;            /* writer view (all shared state) */
    struct coh_vars *rv;            /* reader view of data/flag/x */
    uint64_t p4_target;             /* P4: straddles each reader should reach */
    uint64_t p4_budget_ticks;       /* P4: writer time budget */
    uint64_t tag;                   /* row tag for P1/P2/S1/P4 values */
    uint64_t timeout_ticks;         /* no-progress budget */
    uint32_t inject;                /* enum coh_inject (self-test only) */
    uint64_t check;

    alignas(COH_LINE_BYTES) struct coh_result res;
    uint64_t t_start;
    uint64_t t_end;
    uint32_t ran_on;
    uint32_t bad;                   /* 1 = bad params, 2 = wrong CPU */
    uint64_t aux[4];                /* control c reader: p1 stale/fresh, p2 stale/fresh */

    alignas(COH_LINE_BYTES) volatile uint64_t done;
};
static_assert(offsetof(struct coh_part, done) - offsetof(struct coh_part, res) ==
              COH_LINE_BYTES, "coh_part outputs must fit one line");
static_assert(offsetof(struct coh_part, res) == COH_LINE_BYTES,
              "coh_part parameters must fit one line");

/*
 * Negative control (c) runs as an internal pattern after the public ones.
 * It is the one place in this file that uses cache maintenance (DC CVAC,
 * DC CIVAC) and touches SCTLR, by design: see coh_dcoff_* below.
 */
#define COH_PAT_DCOFF           ((uint32_t)COH_PATTERN_COUNT)

static struct coh_part g_parts[MAX_CPUS];
static struct coh_vars g_vars;          /* litmus shared state (.bss) */
static uint64_t g_nonce;
static atomic_bool g_busy;
static bool g_dirty;                    /* a worker of an earlier run never finished */
static void *g_neg_block;               /* 2 MB PMM block for the controls */

struct coh_remote {
    struct task *t;
    uint32_t id;
    uint32_t gen;
};

struct coh_row {
    enum coh_pattern pat;
    char cpus[20];
    uint64_t n;                     /* P4: writes made */
    uint64_t forbidden;
    uint64_t timeouts;
    uint64_t ms;
    uint32_t misplaced;             /* workers that ran on the wrong CPU */
    uint32_t lost;                  /* results never published */
    uint64_t aux[4];                /* control c: from the reader */
    /* P4 only: per reader (role - 1), ascending CPU order. */
    uint32_t nreaders;
    uint64_t pairs[MAX_CPUS];
    uint64_t straddles[MAX_CPUS];
    /* P1/P2/S1 only: per reader (role - 1). */
    uint32_t nobserved;
    uint64_t observed[MAX_CPUS];
    bool weak;                      /* some reader saw no straddle / observed nothing */
};

static const char *const g_pattern_names[COH_PATTERN_COUNT] = {
    "P1", "P2", "P3", "P4", "P5", "P6", "S1", "S2",
};

static const char *const g_inject_names[COH_INJECT_COUNT] = {
    "none", "P3-stale-partner", "P4-descending", "P4-no-writes", "P6-sum-offset",
    "MP-stale-flag",
};

static bool coh_is_mp(enum coh_pattern p)
{
    return p == COH_P1 || p == COH_P2 || p == COH_S1;
}

const char *coherency_pattern_name(enum coh_pattern p)
{
    return ((unsigned)p < COH_PATTERN_COUNT) ? g_pattern_names[p] : "??";
}

bool coherency_row_fails(enum coh_pattern p, uint64_t forbidden, uint64_t timeouts, bool weak)
{
    if (p == COH_S1 || p == COH_S2) {
        return false;
    }
    if (weak && (p == COH_P1 || p == COH_P2 || p == COH_P4)) {
        return true;
    }
    return forbidden != 0 || timeouts != 0;
}

uint64_t coherency_p4_target(uint32_t scale)
{
    if (scale == 0) {
        scale = 1;
    }
    uint64_t t = COH_P4_STRADDLE_TARGET / scale;
    return (t == 0) ? 1 : t;
}

uint64_t coherency_iters(enum coh_pattern p, uint32_t scale)
{
    uint64_t base;
    switch (p) {
    case COH_P3:
    case COH_P5:
    case COH_S2:
        base = COH_ITERS_ROUNDS;
        break;
    case COH_P6:
        base = COH_ITERS_SUM;
        break;
    default:
        base = COH_ITERS_MP;
        break;
    }
    if (scale == 0) {
        scale = 1;
    }
    uint64_t n = base / scale;
    return (n < COH_MIN_ITERS) ? COH_MIN_ITERS : n;
}

static uint64_t coh_ms_to_ticks(uint64_t ms)
{
    return coh_cntfrq() * ms / 1000u;
}

static uint64_t coh_part_checksum(const struct coh_part *p)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ p->nonce;
    uint64_t f[] = {
        p->pattern, p->role, p->cpu, p->nparts, p->n,
        (uint64_t)(uintptr_t)p->wv, (uint64_t)(uintptr_t)p->rv,
        p->p4_target, p->p4_budget_ticks, p->tag, p->timeout_ticks, p->inject,
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        h ^= f[i] + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h;
}

static void coh_reset_vars(struct coh_vars *v)
{
    struct coh_line *line = (struct coh_line *)v;
    for (size_t i = 0; i < sizeof(*v) / sizeof(*line); i++) {
        coh_st(&line[i].v, 0);
    }
    coh_dmb_ish();
}

/* ------------------------------------------------------------------------
 * Participant body (runs on the target CPU, worker task or inline)
 * ------------------------------------------------------------------------ */

/*
 * Self-test fake for P3 T1 (COH_INJECT_P3_STALE_PARTNER): takes part in
 * every barrier of the real thread but never stores y and reports, for
 * round r, that it read x = r - 1. T0 runs the real code, reads y = 0, and
 * must count every round as forbidden.
 */
static void coh_sb_fake_partner(struct coh_vars *v, uint64_t n, uint64_t *local_sense,
                                const struct coh_env *env, struct coh_result *res)
{
    for (uint64_t r = 1; r <= n + 1; r++) {
        if (coh_barrier_wait(&v->bar_count.v, &v->bar_sense.v, 2, local_sense,
                             env, res) != COH_WAIT_OK) {
            return;
        }
        if (r <= n) {
            coh_st(&v->res1[r & 1].v, r - 1);
        }
    }
}

/* Start index of the self-test's descending P4 writer. */
#define COH_P4_DESC_FIRST       (1ull << 38)

static void coh_part_body(const struct coh_part *pp, uint64_t *local_sense,
                          const struct coh_env *env, struct coh_result *res)
{
    uint32_t role = pp->role;
    uint64_t n = pp->n;
    struct coh_vars *wv = pp->wv;
    struct coh_vars *rv = pp->rv;

    switch (pp->pattern) {
    case COH_P1:
    case COH_P2:
    case COH_S1: {
        enum coh_mp_kind kind = (pp->pattern == COH_P1) ? COH_MP_REL_ACQ :
                                (pp->pattern == COH_P2) ? COH_MP_DMB : COH_MP_NONE;
        if (role == 0) {
            /* COH_INJECT_MP_STALE_FLAG: a silent writer, so the reader only
             * ever sees the stale foreign-tag values the driver preloaded. */
            if (pp->inject != COH_INJECT_MP_STALE_FLAG) {
                coh_mp_writer(kind, &wv->data.v, &wv->flag.v, pp->tag, n, env);
            }
        } else {
            coh_mp_reader(kind, &rv->data.v, &rv->flag.v, pp->tag, n, env, res);
        }
        break;
    }
    case COH_P3:
        if (role == 1 && pp->inject == COH_INJECT_P3_STALE_PARTNER) {
            coh_sb_fake_partner(wv, n, local_sense, env, res);
        } else {
            coh_sb_thread(role, true, wv, n, local_sense, env, res);
        }
        break;
    case COH_S2:
        coh_sb_thread(role, false, wv, n, local_sense, env, res);
        break;
    case COH_P4:
        if (role == 0) {
            uint32_t nr = pp->nparts - 1u;
            if (pp->inject == COH_INJECT_P4_DESCENDING) {
                res->writes = coh_corr_writer_ex(&wv->x.v, &wv->p4_done.v, wv->strad, nr,
                                                 pp->tag, n, pp->p4_target,
                                                 pp->p4_budget_ticks, COH_P4_DESC_FIRST, true,
                                                 env);
            } else if (pp->inject == COH_INJECT_P4_NO_WRITES) {
                res->writes = coh_corr_writer(&wv->x.v, &wv->p4_done.v, wv->strad, nr,
                                              pp->tag, 0, pp->p4_target, 0, env);
            } else {
                res->writes = coh_corr_writer(&wv->x.v, &wv->p4_done.v, wv->strad, nr,
                                              pp->tag, n, pp->p4_target, pp->p4_budget_ticks,
                                              env);
            }
        } else {
            coh_corr_reader(&rv->x.v, &wv->p4_done.v, &wv->strad[role - 1u].v, pp->tag, env,
                            res);
        }
        break;
    case COH_P5:
        coh_pingpong(role, &wv->ctr.v, n, env, res);
        break;
    case COH_P6:
        coh_atomic_sum(&wv->sum.v, n, env);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * Negative control (c): a reader with its data cache off
 *
 * Arm Cortex-A76 TRM 7.2.3 and Cortex-A78AE TRM 2.6.2.3: with SCTLR.C=0 a
 * load looks up no L1/L2/L3 array, so it returns what is at the Point of
 * Coherency. The writer puts P0 at PoC (store, DC CVAC, DSB), then stores
 * P1 cacheably with no clean, so P1 stays dirty in the writer's cache. A
 * reader with C=0 must then read the stale P0 (phase 1). After the writer
 * cleans the line (DC CVAC, DSB) the same reader must read P1 (phase 2).
 * QEMU TCG models no caches, so there both phases read P1.
 * ------------------------------------------------------------------------ */

static inline void coh_dc_cvac(const volatile void *addr)
{
    __asm__ volatile("dc cvac, %0" : : "r"(addr) : "memory");
}

static inline void coh_dc_civac(const volatile void *addr)
{
    __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
}

static inline void coh_dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t coh_dcache_line_bytes(void)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return 4ull << ((ctr >> 16) & 0xFu);        /* CTR_EL0.DminLine, in words */
}

/* Clean and invalidate [addr, addr + len) to PoC. */
static void coh_dc_civac_range(const volatile void *addr, uint64_t len)
{
    uint64_t line = coh_dcache_line_bytes();
    uint64_t start = (uint64_t)(uintptr_t)addr & ~(line - 1u);
    uint64_t end = (uint64_t)(uintptr_t)addr + len;
    for (uint64_t a = start; a < end; a += line) {
        coh_dc_civac((const volatile void *)(uintptr_t)a);
    }
    coh_dsb_sy();
}

/* Wait (C on) for the step line to reach @want. */
static enum coh_wait_status coh_dcoff_wait_step(const volatile uint64_t *step, uint64_t want,
                                                const struct coh_env *env,
                                                struct coh_result *res)
{
    uint64_t v = coh_ldar(step);
    struct coh_wait w;
    coh_wait_init(&w, v);
    while (v < want) {
        enum coh_wait_status st = coh_wait_tick(&w, v, env, res);
        if (st != COH_WAIT_OK) {
            return st;
        }
        v = coh_ldar(step);
    }
    return COH_WAIT_OK;
}

/*
 * The data-cache-off window. Register-only: DAIF fully masked, SCTLR_ELx.C
 * cleared (the EL2 regime with VHE on Pi 5 and Jetson, EL1 on QEMU), @n
 * loads of @line counted against @p0 and @p1, C restored, DAIF restored.
 * Nothing but @line is read or written while C=0; the counts leave in
 * registers and reach memory only after C is back on.
 */
static void coh_dcoff_window(const volatile uint64_t *line, uint64_t n, uint64_t p0,
                             uint64_t p1, uint64_t *n0, uint64_t *n1)
{
    uint64_t c0 = 0;
    uint64_t c1 = 0;
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    if (n == 0) {
        n = 1;
    }
    if (((el >> 2) & 3u) == 2) {
        __asm__ volatile(
            "mrs    x9, daif\n"
            "msr    daifset, #0xf\n"
            "mrs    x11, sctlr_el2\n"
            "bic    x12, x11, #4\n"
            "dsb    sy\n"
            "msr    sctlr_el2, x12\n"
            "isb\n"
            "1: ldr x10, [%[line]]\n"
            "cmp    x10, %[p0]\n"
            "cinc   %[c0], %[c0], eq\n"
            "cmp    x10, %[p1]\n"
            "cinc   %[c1], %[c1], eq\n"
            "subs   %[n], %[n], #1\n"
            "b.ne   1b\n"
            "msr    sctlr_el2, x11\n"
            "isb\n"
            "msr    daif, x9\n"
            : [c0] "+r"(c0), [c1] "+r"(c1), [n] "+r"(n)
            : [line] "r"(line), [p0] "r"(p0), [p1] "r"(p1)
            : "x9", "x10", "x11", "x12", "cc", "memory");
    } else {
        __asm__ volatile(
            "mrs    x9, daif\n"
            "msr    daifset, #0xf\n"
            "mrs    x11, sctlr_el1\n"
            "bic    x12, x11, #4\n"
            "dsb    sy\n"
            "msr    sctlr_el1, x12\n"
            "isb\n"
            "1: ldr x10, [%[line]]\n"
            "cmp    x10, %[p0]\n"
            "cinc   %[c0], %[c0], eq\n"
            "cmp    x10, %[p1]\n"
            "cinc   %[c1], %[c1], eq\n"
            "subs   %[n], %[n], #1\n"
            "b.ne   1b\n"
            "msr    sctlr_el1, x11\n"
            "isb\n"
            "msr    daif, x9\n"
            : [c0] "+r"(c0), [c1] "+r"(c1), [n] "+r"(n)
            : [line] "r"(line), [p0] "r"(p0), [p1] "r"(p1)
            : "x9", "x10", "x11", "x12", "cc", "memory");
    }
    *n0 = c0;
    *n1 = c1;
}

/* One reader phase: clean+invalidate what the reader holds that it could
 * write back over (its stack page, its result slot), then the window. */
static void coh_dcoff_phase(const volatile uint64_t *line, uint64_t n, uint64_t p0, uint64_t p1,
                            struct coh_part *slot, uint64_t *n0, uint64_t *n1)
{
    uint64_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    coh_dc_civac_range((const volatile void *)(uintptr_t)(sp & ~(uint64_t)(PAGE_SIZE - 1)),
                       PAGE_SIZE);
    coh_dc_civac_range(&slot->res, COH_LINE_BYTES);
    coh_dcoff_window(line, n, p0, p1, n0, n1);
}

/*
 * Role 0 is the writer, role 1 the reader. The step line (flag) carries
 * the handshake: 1 = P1 stored dirty, 2 = phase 1 read, 3 = P1 cleaned.
 * aux[] = { phase 1 P0 reads, phase 1 P1 reads, phase 2 P0, phase 2 P1 }.
 * The data line is never touched by the reader with C on, so no coherent
 * read can pull the writer's dirty P1 out to memory before phase 1.
 */
static void coh_dcoff_role(uint32_t role, struct coh_vars *v, uint64_t n, uint64_t nonce,
                           struct coh_part *slot, const struct coh_env *env,
                           struct coh_result *res, uint64_t aux[4])
{
    volatile uint64_t *line = &v->data.v;
    volatile uint64_t *step = &v->flag.v;
    uint64_t p0 = (nonce << 8) | 0xA0u;
    uint64_t p1 = p0 + 1u;

    if (role == 0) {
        coh_st(line, p0);
        coh_dc_cvac(line);
        coh_dsb_sy();
        coh_st(line, p1);                       /* dirty here, no clean */
        coh_stlr(step, 1);
        if (coh_dcoff_wait_step(step, 2, env, res) != COH_WAIT_OK) {
            return;
        }
        coh_dc_cvac(line);
        coh_dsb_sy();
        coh_stlr(step, 3);
        return;
    }
    if (coh_dcoff_wait_step(step, 1, env, res) != COH_WAIT_OK) {
        return;
    }
    coh_dcoff_phase(line, n, p0, p1, slot, &aux[0], &aux[1]);
    coh_stlr(step, 2);
    if (coh_dcoff_wait_step(step, 3, env, res) != COH_WAIT_OK) {
        return;
    }
    coh_dcoff_phase(line, n, p0, p1, slot, &aux[2], &aux[3]);
}

static void coh_part_run(struct coh_part *p)
{
    uint32_t here = cpu_id();
    struct coh_result res = {0};
    uint64_t aux[4] = {0, 0, 0, 0};
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    uint32_t bad = 0;

    /* Copy the parameters out once, then validate the copy. */
    struct coh_part params;
    params.nonce = p->nonce;
    params.pattern = p->pattern;
    params.role = p->role;
    params.cpu = p->cpu;
    params.nparts = p->nparts;
    params.n = p->n;
    params.wv = p->wv;
    params.rv = p->rv;
    params.p4_target = p->p4_target;
    params.p4_budget_ticks = p->p4_budget_ticks;
    params.tag = p->tag;
    params.timeout_ticks = p->timeout_ticks;
    params.inject = p->inject;
    uint64_t check = p->check;

    if (coh_part_checksum(&params) != check || params.wv == NULL ||
        params.rv == NULL || params.pattern > COH_PAT_DCOFF ||
        params.role >= params.nparts || params.nparts > MAX_CPUS) {
        /* Stale or torn parameters: touch no shared state at all. */
        bad = 1;
        res.timeouts++;
    } else {
        struct coh_env env = {
            .abort = &params.wv->abort.v,
            .timeout_ticks = (params.timeout_ticks != 0) ? params.timeout_ticks
                                                         : coh_ms_to_ticks(COH_TIMEOUT_MS),
        };
        if (here != params.cpu) {
            bad = 2;
            res.timeouts++;
            coh_raise_abort(&env);
        } else {
            struct coh_env start_env = env;
            start_env.timeout_ticks = coh_ms_to_ticks(COH_START_TIMEOUT_MS);
            uint64_t local_sense = 0;
            if (coh_barrier_wait(&params.wv->bar_count.v, &params.wv->bar_sense.v,
                                 params.nparts, &local_sense, &start_env, &res) == COH_WAIT_OK) {
                t0 = coh_cntpct();
                if (params.pattern == COH_PAT_DCOFF) {
                    coh_dcoff_role(params.role, params.wv, params.n, params.nonce,
                                   p, &env, &res, aux);
                } else {
                    coh_part_body(&params, &local_sense, &env, &res);
                }
                t1 = coh_cntpct();
            }
        }
    }

    p->res = res;
    p->t_start = t0;
    p->t_end = t1;
    p->ran_on = here;
    p->bad = bad;
    for (int i = 0; i < 4; i++) {
        p->aux[i] = aux[i];
    }
    coh_stlr(&p->done, params.nonce);
}

static void coh_part_task(void *arg)
{
    coh_part_run((struct coh_part *)arg);
}

/* ------------------------------------------------------------------------
 * Remote dispatch
 * ------------------------------------------------------------------------ */

static bool coh_remote_start(struct coh_remote *r, uint32_t cpu, task_entry_t fn,
                             void *arg, char tag)
{
    char name[8] = { 'c', 'o', 'h', '_', tag, (char)('0' + (cpu % 10)), '\0', '\0' };
    struct task *t = task_create_with_priority(name, fn, arg, TASK_PRIORITY_HIGH);
    if (t == NULL) {
        r->t = NULL;
        return false;
    }
    r->t = t;
    r->id = t->id;
    r->gen = t->generation;
    task_set_affinity(t, cpu);
    scheduler_add_task_to_cpu(t, cpu);
    return true;
}

/* The worker returned (TERMINATED) or its slot was already reclaimed. */
static bool coh_remote_finished(const struct coh_remote *r)
{
    const volatile struct task *t = r->t;
    if (t->generation != r->gen || t->id != r->id) {
        return true;
    }
    return t->state == TASK_TERMINATED || t->state == TASK_DESTROYED;
}

static void coh_remote_abandon(struct coh_remote *r)
{
    struct task *t = r->t;
    if (t->generation == r->gen && t->id == r->id && t->state == TASK_READY) {
        /* Not running anywhere: dequeue and reclaim it. */
        scheduler_terminate_task(t);
        task_destroy(t);
        return;
    }
    if (!coh_remote_finished(r)) {
        g_dirty = true;     /* still running: later runs would race it */
    }
}

/* Wait for @done == @nonce; false if the result is lost or abandoned. */
static bool coh_wait_done(volatile uint64_t *done, uint64_t nonce, struct coh_remote *r)
{
    uint64_t start = coh_cntpct();
    uint64_t cap = coh_ms_to_ticks(COH_CAP_SECONDS * 1000ull);
    uint64_t grace = coh_ms_to_ticks(COH_LOST_GRACE_MS);
    uint64_t finished_at = 0;
    for (;;) {
        if (coh_ldar(done) == nonce) {
            return true;
        }
        uint64_t now = coh_cntpct();
        if (coh_remote_finished(r)) {
            if (finished_at == 0) {
                finished_at = now;
            } else if (now - finished_at > grace) {
                return false;
            }
        } else if (now - start > cap) {
            coh_remote_abandon(r);
            return false;
        }
        task_sleep_ms(1);
    }
}

/* ------------------------------------------------------------------------
 * Caller setup: pin to the current CPU, raise priority, single entry
 * ------------------------------------------------------------------------ */

struct coh_caller {
    struct task *self;
    uint32_t cpu;
    uint32_t old_affinity;
    uint8_t old_priority;
};

static int coh_enter(struct coh_caller *c, coh_out_fn out)
{
    if (atomic_exchange(&g_busy, true)) {
        if (out) {
            out("coherency: another run is in progress\r\n");
        }
        return -1;
    }
    if (g_dirty) {
        if (out) {
            out("coherency: a worker from an earlier run never finished; reboot first\r\n");
        }
        atomic_store(&g_busy, false);
        return -1;
    }
    c->self = task_current();
    c->old_affinity = task_get_affinity(c->self);
    c->old_priority = task_get_priority(c->self);
    /* Pin to wherever this task runs now, then confirm it stayed. */
    for (int i = 0; i < 8; i++) {
        uint32_t cpu = cpu_id();
        task_set_affinity(c->self, cpu);
        yield();
        if (cpu_id() == cpu) {
            break;
        }
    }
    c->cpu = cpu_id();
    task_set_affinity(c->self, c->cpu);
    task_set_priority(c->self, TASK_PRIORITY_HIGH);
    return 0;
}

static void coh_leave(struct coh_caller *c)
{
    task_set_priority(c->self, c->old_priority);
    sched_refresh_effective_priority(c->self);
    task_set_affinity(c->self, c->old_affinity);
    atomic_store(&g_busy, false);
}

/* ------------------------------------------------------------------------
 * One run of one pattern over one CPU set
 * ------------------------------------------------------------------------ */

/* Per-run settings shared by every job of one litmus or neg run. */
struct coh_job_cfg {
    uint32_t local;                 /* CPU the caller plays inline */
    uint64_t p4_target;
    uint64_t p4_budget_ticks;
    uint64_t timeout_ticks;         /* no-progress budget (spec: 1 s) */
    enum coh_inject inject;
};

static void coh_run_job(enum coh_pattern pat, uint64_t n, const uint32_t *cpus,
                        uint32_t nparts, struct coh_vars *wv, struct coh_vars *rv,
                        const struct coh_job_cfg *cfg, struct coh_row *row)
{
    struct coh_remote rem[MAX_CPUS] = {0};
    bool started[MAX_CPUS] = {false};
    int local_idx = -1;
    uint32_t local = cfg->local;

    coh_reset_vars(wv);
    if (pat == COH_P6 && cfg->inject == COH_INJECT_P6_SUM_OFFSET) {
        coh_st(&wv->sum.v, 1);      /* fault: the sum starts one too high */
        coh_dmb_ish();
    }
    uint64_t nonce = ++g_nonce;
    uint64_t tag = coh_row_tag(nonce);
    if (coh_is_mp(pat) || pat == COH_P4) {
        /* Revision 3: tagged variables start at the reserved tag, before
         * any participant reaches the start barrier. */
        coh_st(&wv->data.v, COH_TAGGED_INIT);
        coh_st(&wv->flag.v, COH_TAGGED_INIT);
        coh_st(&wv->x.v, COH_TAGGED_INIT);
        coh_st(&wv->p4_done.v, COH_TAGGED_INIT);
        for (uint32_t k = 0; k < MAX_CPUS; k++) {
            coh_st(&wv->strad[k].v, COH_TAGGED_INIT);
        }
        if (coh_is_mp(pat) && cfg->inject == COH_INJECT_MP_STALE_FLAG) {
            /* Fault: the final values of another row, as a stale line
             * would hold them; the writer stays silent. */
            uint64_t other = (tag % (COH_TAG_RESERVED - 1u)) + 1u;
            coh_st(&wv->data.v, coh_tagged(other, n));
            coh_st(&wv->flag.v, coh_tagged(other, n));
        }
        coh_dmb_ish();
    }
    for (uint32_t i = 0; i < nparts; i++) {
        struct coh_part *p = &g_parts[i];
        p->nonce = nonce;
        p->pattern = (uint32_t)pat;
        p->role = i;
        p->cpu = cpus[i];
        p->nparts = nparts;
        p->n = n;
        p->wv = wv;
        p->rv = rv;
        p->p4_target = cfg->p4_target;
        p->p4_budget_ticks = cfg->p4_budget_ticks;
        p->tag = tag;
        p->timeout_ticks = cfg->timeout_ticks;
        p->inject = (uint32_t)cfg->inject;
        p->check = coh_part_checksum(p);
        p->res = (struct coh_result){0};
        p->t_start = 0;
        p->t_end = 0;
        p->ran_on = 0;
        p->bad = 0;
        for (int k = 0; k < 4; k++) {
            p->aux[k] = 0;
        }
        coh_st(&p->done, 0);
    }
    coh_dmb_ish();

    struct coh_env env = { .abort = &wv->abort.v, .timeout_ticks = 0 };
    for (uint32_t i = 0; i < nparts; i++) {
        if (cpus[i] == local) {
            local_idx = (int)i;
            continue;
        }
        started[i] = coh_remote_start(&rem[i], cpus[i], coh_part_task, &g_parts[i], 'w');
        if (!started[i]) {
            coh_raise_abort(&env);
        }
    }
    if (local_idx >= 0) {
        coh_part_run(&g_parts[local_idx]);
    }

    row->pat = pat;
    row->n = n;
    row->forbidden = 0;
    row->timeouts = 0;
    row->ms = 0;
    row->misplaced = 0;
    row->lost = 0;
    for (int k = 0; k < 4; k++) {
        row->aux[k] = 0;
    }
    row->nreaders = (pat == COH_P4 && nparts > 0) ? nparts - 1u : 0u;
    row->nobserved = (coh_is_mp(pat) && nparts > 0) ? nparts - 1u : 0u;
    row->weak = false;
    for (uint32_t k = 0; k < MAX_CPUS; k++) {
        row->pairs[k] = 0;
        row->straddles[k] = 0;
        row->observed[k] = 0;
    }
    if (pat == COH_P4) {
        row->n = 0;                 /* writes made, from the writer */
    }
    uint64_t t_min = UINT64_MAX;
    uint64_t t_max = 0;
    for (uint32_t i = 0; i < nparts; i++) {
        bool have = ((int)i == local_idx);
        if (!have) {
            have = started[i] && coh_wait_done(&g_parts[i].done, nonce, &rem[i]);
        }
        if (!have) {
            row->lost++;
            continue;
        }
        const struct coh_part *p = &g_parts[i];
        row->forbidden += p->res.forbidden;
        row->timeouts += p->res.timeouts;
        if (p->bad == 2) {
            row->misplaced++;
        }
        for (int k = 0; k < 4; k++) {
            row->aux[k] += p->aux[k];
        }
        if (pat == COH_P4) {
            if (i == 0) {
                row->n = p->res.writes;
            } else {
                row->pairs[i - 1u] = p->res.pairs;
                row->straddles[i - 1u] = p->res.straddles;
            }
        }
        if (coh_is_mp(pat) && i > 0) {
            row->observed[i - 1u] = p->res.observed;
        }
        if (p->t_start != 0 && p->t_start < t_min) {
            t_min = p->t_start;
        }
        if (p->t_end > t_max) {
            t_max = p->t_end;
        }
    }
    row->timeouts += row->lost;
    for (uint32_t k = 0; k < row->nreaders; k++) {
        if (row->straddles[k] == 0) {
            row->weak = true;       /* includes a reader whose result was lost */
        }
    }
    for (uint32_t k = 0; k < row->nobserved; k++) {
        if (row->observed[k] == 0) {
            row->weak = true;       /* the reader never saw the row in progress */
        }
    }

    /* End-state checks, once every role finished cleanly. */
    if (row->timeouts == 0) {
        if (pat == COH_P5 && coh_final_is_forbidden(coh_ldar(&wv->ctr.v), 2 * n)) {
            row->forbidden++;
        }
        if (pat == COH_P6 &&
            coh_final_is_forbidden(coh_ldar(&wv->sum.v), (uint64_t)nparts * n)) {
            row->forbidden++;
        }
    }
    if (t_min != UINT64_MAX && t_max > t_min) {
        row->ms = (t_max - t_min) * 1000u / coh_cntfrq();
    }
}

/* ------------------------------------------------------------------------
 * Row formatting
 * ------------------------------------------------------------------------ */

static void coh_u32_append(char *buf, size_t size, size_t *pos, uint32_t v)
{
    char tmp[12];
    int len = 0;
    do {
        tmp[len++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && len < (int)sizeof(tmp));
    while (len > 0 && *pos + 1 < size) {
        buf[(*pos)++] = tmp[--len];
    }
    buf[*pos] = '\0';
}

static void coh_char_append(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos + 1 < size) {
        buf[(*pos)++] = c;
    }
    buf[*pos] = '\0';
}

static void coh_cpus_pair(char *buf, size_t size, uint32_t a, uint32_t b)
{
    size_t pos = 0;
    buf[0] = '\0';
    coh_u32_append(buf, size, &pos, a);
    coh_char_append(buf, size, &pos, '>');
    coh_u32_append(buf, size, &pos, b);
}

static void coh_cpus_range(char *buf, size_t size, int writer, uint32_t lo, uint32_t hi)
{
    size_t pos = 0;
    buf[0] = '\0';
    if (writer >= 0) {
        coh_char_append(buf, size, &pos, 'w');
        coh_u32_append(buf, size, &pos, (uint32_t)writer);
        coh_char_append(buf, size, &pos, '/');
    }
    coh_u32_append(buf, size, &pos, lo);
    coh_char_append(buf, size, &pos, '-');
    coh_u32_append(buf, size, &pos, hi);
}

static void coh_print_header(coh_out_fn out)
{
    if (out) {
        out("%-7s %-9s %9s %9s %8s %7s\r\n",
            "pattern", "cpus", "iters", "forbidden", "timeouts", "ms");
    }
}

/* Appends ` <key>=v0,v1,...` for @n values to @buf at *@pos. */
static void coh_list_append(char *buf, size_t size, size_t *pos, const char *key,
                            const uint64_t *v, uint32_t n)
{
    int len = uart_snprintf(buf + *pos, size - *pos, " %s=", key);
    *pos = (len > 0 && *pos + (size_t)len < size) ? *pos + (size_t)len : size - 1u;
    for (uint32_t k = 0; k < n; k++) {
        len = uart_snprintf(buf + *pos, size - *pos, "%s%llu", (k == 0) ? "" : ",",
                            (unsigned long long)v[k]);
        *pos = (len > 0 && *pos + (size_t)len < size) ? *pos + (size_t)len : size - 1u;
    }
}

static void coh_print_row(coh_out_fn out, const struct coh_row *r)
{
    if (!out) {
        return;
    }
    /* One out() call per row, so a capturing sink sees whole rows. */
    char line[384];
    int len = uart_snprintf(line, sizeof(line), "%-7s %-9s %9llu %9llu %8llu %7llu",
                            coherency_pattern_name(r->pat), r->cpus,
                            (unsigned long long)r->n, (unsigned long long)r->forbidden,
                            (unsigned long long)r->timeouts, (unsigned long long)r->ms);
    size_t pos = (len > 0 && (size_t)len < sizeof(line)) ? (size_t)len : sizeof(line) - 1u;
    if (r->pat == COH_P4) {
        coh_list_append(line, sizeof(line), &pos, "pairs", r->pairs, r->nreaders);
        coh_list_append(line, sizeof(line), &pos, "straddles", r->straddles, r->nreaders);
        (void)uart_snprintf(line + pos, sizeof(line) - pos, " coverage=%s",
                            r->weak ? "weak" : "ok");
    }
    if (r->nobserved != 0) {
        coh_list_append(line, sizeof(line), &pos, "observed", r->observed, r->nobserved);
    }
    out("%s\r\n", line);
    if (r->misplaced != 0) {
        out("# %s %s: %u worker(s) ran on the wrong CPU (counted as timeouts)\r\n",
            coherency_pattern_name(r->pat), r->cpus, (unsigned)r->misplaced);
    }
    if (r->lost != 0) {
        out("# %s %s: %u result(s) never became visible (counted as timeouts)\r\n",
            coherency_pattern_name(r->pat), r->cpus, (unsigned)r->lost);
    }
}

static void coh_sum_row(struct coh_summary *sum, const struct coh_row *r)
{
    sum->rows++;
    sum->forbidden[r->pat] += r->forbidden;
    sum->timeouts[r->pat] += r->timeouts;
    if (coh_is_mp(r->pat) && r->weak) {
        sum->mp_weak_rows++;
    }
    if (r->pat == COH_P4) {
        if (r->weak) {
            sum->p4_weak_rows++;
        }
        for (uint32_t k = 0; k < r->nreaders; k++) {
            if (r->straddles[k] < sum->p4_min_straddles) {
                sum->p4_min_straddles = r->straddles[k];
            }
        }
    }
    if (coherency_row_fails(r->pat, r->forbidden, r->timeouts, r->weak)) {
        sum->failed_rows++;
    }
}

/*
 * Where a row's shared variables live. The litmus run uses one fixed set
 * (g_vars) for every row. The negative controls take a fresh 4 KB slot of
 * their 2 MB block for every row (cursor != NULL) and never reuse one in
 * the same boot: a reader that went through the Non-shareable alias can
 * keep a Non-shareable copy of a line in its cache, and a later row that
 * reused the line would hit that copy (seen on jetson-nano-2, see
 * docs/archive/coherency-gauntlet/c2-hw/jetson-kexec-neg-run1-contaminated.txt).
 */
struct coh_view_src {
    struct coh_vars *wv;            /* fixed views when cursor == NULL */
    struct coh_vars *rv;
    uint8_t *wbase;                 /* slot 0, writer view */
    uint8_t *rbase;                 /* slot 0, reader view */
    uint32_t *cursor;               /* next free slot */
    uint32_t limit;                 /* one past the last slot */
    bool exhausted;
};

#define COH_NEG_SLOT_BYTES      4096u
#define COH_NEG_SLOTS_AB        256u    /* first 1 MB: controls a and b */
#define COH_NEG_SLOTS_C         256u    /* second 1 MB: control c, never aliased-read */
static_assert(sizeof(struct coh_vars) <= COH_NEG_SLOT_BYTES, "coh_vars must fit a slot");

static uint32_t g_neg_cursor_ab;        /* 0 .. COH_NEG_SLOTS_AB */
static uint32_t g_neg_cursor_c;         /* 0 .. COH_NEG_SLOTS_C, in the second MB */

static bool coh_view_next(struct coh_view_src *src, struct coh_vars **wv, struct coh_vars **rv)
{
    if (src->cursor == NULL) {
        *wv = src->wv;
        *rv = src->rv;
        return true;
    }
    if (*src->cursor >= src->limit) {
        src->exhausted = true;
        return false;
    }
    uint64_t off = (uint64_t)(*src->cursor)++ * COH_NEG_SLOT_BYTES;
    *wv = (struct coh_vars *)(void *)(src->wbase + off);
    *rv = (struct coh_vars *)(void *)(src->rbase + off);
    return true;
}

/* Pair patterns over every ordered pair (a != b), row-major. */
static void coh_run_pairs(enum coh_pattern pat, uint64_t n, struct coh_view_src *src,
                          const struct coh_job_cfg *cfg, coh_out_fn out,
                          struct coh_summary *sum)
{
    for (uint32_t a = 0; a < cpu_count; a++) {
        for (uint32_t b = 0; b < cpu_count; b++) {
            if (a == b) {
                continue;
            }
            struct coh_vars *wv;
            struct coh_vars *rv;
            if (!coh_view_next(src, &wv, &rv)) {
                return;
            }
            uint32_t cpus[2] = { a, b };
            struct coh_row row;
            coh_cpus_pair(row.cpus, sizeof(row.cpus), a, b);
            coh_run_job(pat, n, cpus, 2, wv, rv, cfg, &row);
            coh_print_row(out, &row);
            coh_sum_row(sum, &row);
            if (pat == COH_S1) {
                sum->s1_pair[a][b] += row.forbidden;
            } else if (pat == COH_S2) {
                sum->s2_pair[a][b] += row.forbidden;
            }
        }
    }
}

/* P4: one run per writer, every other online CPU reads. */
static void coh_run_corr(uint64_t n, struct coh_view_src *src,
                         const struct coh_job_cfg *cfg, coh_out_fn out,
                         struct coh_summary *sum)
{
    for (uint32_t w = 0; w < cpu_count; w++) {
        struct coh_vars *wv;
        struct coh_vars *rv;
        if (!coh_view_next(src, &wv, &rv)) {
            return;
        }
        uint32_t cpus[MAX_CPUS];
        uint32_t k = 0;
        cpus[k++] = w;
        for (uint32_t c = 0; c < cpu_count; c++) {
            if (c != w) {
                cpus[k++] = c;
            }
        }
        struct coh_row row;
        coh_cpus_range(row.cpus, sizeof(row.cpus), (int)w, 0, cpu_count - 1);
        coh_run_job(COH_P4, n, cpus, k, wv, rv, cfg, &row);
        coh_print_row(out, &row);
        coh_sum_row(sum, &row);
    }
}

/* ------------------------------------------------------------------------
 * coherency litmus
 * ------------------------------------------------------------------------ */

static void coh_print_sens(coh_out_fn out, const struct coh_summary *sum)
{
    for (uint32_t a = 0; a < cpu_count; a++) {
        for (uint32_t b = 0; b < cpu_count; b++) {
            if (a == b) {
                continue;
            }
            uint64_t s1 = sum->s1_pair[a][b];
            uint64_t s2 = sum->s2_pair[a][b];
            out("SENS cpus=%u>%u S1=%llu S2=%llu P2_sensitive=%s P3_sensitive=%s\r\n",
                (unsigned)a, (unsigned)b, (unsigned long long)s1, (unsigned long long)s2,
                s1 ? "yes" : "no", s2 ? "yes" : "no");
        }
    }
}

int coherency_litmus_run_opts(const struct coh_run_opts *opts, coh_out_fn out,
                              struct coh_summary *sum)
{
    static struct coh_summary local_sum;    /* serialized by g_busy */
    uint32_t scale = (opts->scale == 0) ? 1u : opts->scale;
    uint32_t mask = (opts->patterns == 0) ? COH_PAT_ALL : (opts->patterns & COH_PAT_ALL);
    enum coh_inject inject = ((unsigned)opts->inject < COH_INJECT_COUNT) ? opts->inject
                                                                         : COH_INJECT_NONE;
    if (cpu_count < 2 || cpu_count > MAX_CPUS) {
        if (out) {
            out("coherency: needs 2..%u online CPUs (have %u)\r\n",
                (unsigned)MAX_CPUS, (unsigned)cpu_count);
        }
        if (sum != NULL) {
            memset(sum, 0, sizeof(*sum));
        }
        return -1;
    }
    struct coh_caller caller;
    if (coh_enter(&caller, out) != 0) {
        if (sum != NULL) {
            memset(sum, 0, sizeof(*sum));
        }
        return -1;
    }
    if (sum == NULL) {
        sum = &local_sum;
    }
    memset(sum, 0, sizeof(*sum));
    sum->p4_min_straddles = UINT64_MAX;

    uint32_t timeout_ms = (opts->timeout_ms == 0) ? COH_TIMEOUT_MS : opts->timeout_ms;
    struct coh_job_cfg cfg = {
        .local = caller.cpu,
        .p4_target = (opts->p4_target != 0) ? opts->p4_target : coherency_p4_target(scale),
        .p4_budget_ticks = coh_ms_to_ticks(COH_P4_BUDGET_MS),
        .timeout_ticks = coh_ms_to_ticks(timeout_ms),
        .inject = inject,
    };
    sum->p4_target = cfg.p4_target;

    if (out) {
        out("# coherency litmus: cpus=0-%u scale=%u timeout=%ums caller=cpu%u "
            "p4_target=%llu p4_budget_ms=%u\r\n",
            (unsigned)(cpu_count - 1), (unsigned)scale, (unsigned)timeout_ms,
            (unsigned)caller.cpu, (unsigned long long)cfg.p4_target,
            (unsigned)COH_P4_BUDGET_MS);
        if (mask != COH_PAT_ALL || inject != COH_INJECT_NONE || timeout_ms != COH_TIMEOUT_MS ||
            opts->p4_target != 0) {
            out("# self-test run: patterns=0x%x inject=%s (not a spec run)\r\n",
                (unsigned)mask, g_inject_names[inject]);
        }
    }
    coh_print_header(out);

    struct coh_vars *v = &g_vars;
    struct coh_view_src src = { .wv = v, .rv = v };
    for (int p = COH_P1; p < COH_PATTERN_COUNT; p++) {
        if ((mask & COH_PAT_BIT(p)) == 0) {
            continue;
        }
        uint64_t n = coherency_iters((enum coh_pattern)p, scale);
        if (p == COH_P4) {
            coh_run_corr(n, &src, &cfg, out, sum);
        } else if (p == COH_P6) {
            uint32_t cpus[MAX_CPUS];
            for (uint32_t c = 0; c < cpu_count; c++) {
                cpus[c] = c;
            }
            struct coh_row row;
            coh_cpus_range(row.cpus, sizeof(row.cpus), -1, 0, cpu_count - 1);
            coh_run_job(COH_P6, n, cpus, cpu_count, v, v, &cfg, &row);
            coh_print_row(out, &row);
            coh_sum_row(sum, &row);
        } else {
            coh_run_pairs((enum coh_pattern)p, n, &src, &cfg, out, sum);
        }
    }

    coh_leave(&caller);
    if (sum->p4_min_straddles == UINT64_MAX) {
        sum->p4_min_straddles = 0;      /* P4 did not run */
    }

    if (out) {
        bool both = (mask & COH_PAT_BIT(COH_S1)) != 0 && (mask & COH_PAT_BIT(COH_S2)) != 0;
        if (both) {
            coh_print_sens(out, sum);
        }
        out("# S1 relaxed outcomes: %llu, S2 relaxed outcomes: %llu (reported, not judged)\r\n",
            (unsigned long long)sum->forbidden[COH_S1],
            (unsigned long long)sum->forbidden[COH_S2]);
        if ((mask & (COH_PAT_BIT(COH_P1) | COH_PAT_BIT(COH_P2) | COH_PAT_BIT(COH_S1))) != 0) {
            out("# MP weak rows (a reader observed nothing): %u\r\n",
                (unsigned)sum->mp_weak_rows);
        }
        if ((mask & COH_PAT_BIT(COH_P4)) != 0) {
            out("# P4 weak rows: %u, fewest straddles of any reader: %llu (target %llu)\r\n",
                (unsigned)sum->p4_weak_rows, (unsigned long long)sum->p4_min_straddles,
                (unsigned long long)sum->p4_target);
        }
        if (sum->failed_rows == 0) {
            out("coherency: PASS\r\n");
        } else {
            out("coherency: FAIL (%u rows)\r\n", (unsigned)sum->failed_rows);
        }
    }
    return (sum->failed_rows == 0) ? 0 : 1;
}

int coherency_litmus_run(uint32_t scale, coh_out_fn out, struct coh_summary *sum)
{
    struct coh_run_opts opts = {
        .scale = scale,
        .patterns = COH_PAT_ALL,
        .inject = COH_INJECT_NONE,
        .timeout_ms = 0,
        .p4_target = 0,
    };
    return coherency_litmus_run_opts(&opts, out, sum);
}

/* ------------------------------------------------------------------------
 * coherency neg
 * ------------------------------------------------------------------------ */

static const char *const g_neg_names[COH_NEG_COUNT] = {
    "a: reader on Normal WB Non-shareable alias (SH=00)",
    "b: reader on Normal Non-cacheable alias",
    "c: reader with its data cache off (SCTLR.C=0); writer cleans P0 to PoC, "
    "stores P1 with no clean (phase 1), then cleans P1 (phase 2)",
};

/* Control (c) over every ordered pair: a = writer, b = reader. */
static void coh_run_dcoff(struct coh_view_src *src, const struct coh_job_cfg *cfg,
                          coh_out_fn out, struct coh_neg_summary *sum)
{
    if (out) {
        out("# control %s\r\n", g_neg_names[COH_NEG_DCACHE_OFF]);
        out("# expected with real caches: p1_stale = iters, p2_fresh = iters\r\n");
        out("%-7s %-9s %9s %9s %9s %9s %9s %8s\r\n", "pattern", "cpus", "iters",
            "p1_stale", "p1_fresh", "p2_stale", "p2_fresh", "timeouts");
    }
    for (uint32_t a = 0; a < cpu_count; a++) {
        for (uint32_t b = 0; b < cpu_count; b++) {
            if (a == b) {
                continue;
            }
            struct coh_vars *wv;
            struct coh_vars *rv;
            if (!coh_view_next(src, &wv, &rv)) {
                return;
            }
            uint32_t cpus[2] = { a, b };
            struct coh_row row;
            coh_cpus_pair(row.cpus, sizeof(row.cpus), a, b);
            coh_run_job((enum coh_pattern)COH_PAT_DCOFF, COH_DCOFF_LOADS, cpus, 2, wv, rv,
                        cfg, &row);
            sum->dcoff_loads += COH_DCOFF_LOADS;
            sum->dcoff_p1_stale += row.aux[0];
            sum->dcoff_p1_fresh += row.aux[1];
            sum->dcoff_p2_stale += row.aux[2];
            sum->dcoff_p2_fresh += row.aux[3];
            sum->dcoff_timeouts += row.timeouts;
            if (out) {
                out("%-7s %-9s %9llu %9llu %9llu %9llu %9llu %8llu\r\n", "DCOFF", row.cpus,
                    (unsigned long long)COH_DCOFF_LOADS, (unsigned long long)row.aux[0],
                    (unsigned long long)row.aux[1], (unsigned long long)row.aux[2],
                    (unsigned long long)row.aux[3], (unsigned long long)row.timeouts);
            }
        }
    }
    sum->ran[COH_NEG_DCACHE_OFF] = !src->exhausted;
    if (out) {
        out("# control c totals: loads/phase=%llu p1_stale=%llu p1_fresh=%llu "
            "p2_stale=%llu p2_fresh=%llu timeouts=%llu\r\n",
            (unsigned long long)sum->dcoff_loads, (unsigned long long)sum->dcoff_p1_stale,
            (unsigned long long)sum->dcoff_p1_fresh, (unsigned long long)sum->dcoff_p2_stale,
            (unsigned long long)sum->dcoff_p2_fresh, (unsigned long long)sum->dcoff_timeouts);
    }
}

int coherency_neg_run(uint32_t scale, coh_out_fn out, struct coh_neg_summary *sum)
{
    struct coh_neg_summary local_sum;
    if (sum == NULL) {
        sum = &local_sum;
    }
    memset(sum, 0, sizeof(*sum));
    if (scale == 0) {
        scale = 1;
    }
    if (cpu_count < 2 || cpu_count > MAX_CPUS) {
        if (out) {
            out("coherency: needs 2..%u online CPUs (have %u)\r\n",
                (unsigned)MAX_CPUS, (unsigned)cpu_count);
        }
        return -1;
    }
    struct coh_caller caller;
    if (coh_enter(&caller, out) != 0) {
        return -1;
    }

    int rc = 0;
    if (g_neg_block == NULL) {
        /* One 2 MB block, kept for the life of the kernel: lines the
         * aliases pulled into a cache never reach memory anyone else
         * allocates. Buddy blocks are aligned to their size. */
        void *blk = pmm_alloc_pages(BLOCK_SIZE / PAGE_SIZE);
        if (blk != NULL && ((uintptr_t)blk & (BLOCK_SIZE - 1)) != 0) {
            pmm_free_pages(blk, BLOCK_SIZE / PAGE_SIZE);
            blk = NULL;
        }
        g_neg_block = blk;
    }
    if (g_neg_block == NULL) {
        if (out) {
            out("coherency neg: no 2 MB-aligned block available\r\n");
        }
        coh_leave(&caller);
        return -1;
    }

    uint8_t *wbase = (uint8_t *)g_neg_block;        /* identity map, WB ISH */
    uint8_t *rbase = (uint8_t *)COH_ALIAS_VA;
    uint64_t pa = (uint64_t)(uintptr_t)g_neg_block;
    /* Every control row needs a slot no earlier row (of this or an earlier
     * `coherency neg`) touched; refuse instead of reusing one. */
    uint32_t need_ab = 2u * (cpu_count * (cpu_count - 1u) + cpu_count);
    uint32_t need_c = cpu_count * (cpu_count - 1u);
    if (g_neg_cursor_ab + need_ab > COH_NEG_SLOTS_AB ||
        g_neg_cursor_c + need_c > COH_NEG_SLOTS_C) {
        if (out) {
            out("coherency neg: no unused slots left in the control block; reboot to "
                "run the controls again\r\n");
        }
        coh_leave(&caller);
        return -1;
    }
    uint64_t n = COH_ITERS_NEG / scale;
    if (n < COH_MIN_ITERS) {
        n = COH_MIN_ITERS;
    }
    struct coh_job_cfg cfg = {
        .local = caller.cpu,
        .p4_target = coherency_p4_target(scale),
        .p4_budget_ticks = coh_ms_to_ticks(COH_P4_BUDGET_MS),
        .timeout_ticks = coh_ms_to_ticks(COH_TIMEOUT_MS),
        .inject = COH_INJECT_NONE,
    };
    /*
     * Revision 3: write a sentinel (a reserved-tag value) over every slot
     * controls a and b will use and clean it to the PoC through the
     * normal mapping, before any alias exists, so what a reader finds
     * through an alias never depends on old DRAM contents.
     */
    {
        volatile uint64_t *w0 =
            (volatile uint64_t *)(void *)(wbase + (uint64_t)g_neg_cursor_ab * COH_NEG_SLOT_BYTES);
        uint64_t bytes = (uint64_t)need_ab * COH_NEG_SLOT_BYTES;
        for (uint64_t k = 0; k < bytes / sizeof(uint64_t); k++) {
            w0[k] = COH_TAGGED_INIT;
        }
        coh_dc_civac_range(w0, bytes);      /* DC CIVAC per line, then DSB SY */
    }
    static const uint32_t flags[2] = {
        VMM_FLAGS_KERNEL_DATA | VMM_FLAG_NONSHARE,
        VMM_FLAG_NOCACHE | VMM_FLAG_READ | VMM_FLAG_WRITE,
    };

    for (uint32_t c = 0; c < 2; c++) {
        if (vmm_ensure_kernel_l2_table(COH_ALIAS_VA) != 0 || vmm_is_mapped(COH_ALIAS_VA) ||
            vmm_map_block(COH_ALIAS_VA, pa, flags[c]) != 0) {
            if (out) {
                out("coherency neg: cannot map the alias at 0x%lx\r\n",
                    (unsigned long)COH_ALIAS_VA);
            }
            rc = -1;
            break;
        }
        /* Read-only look at the descriptor just installed, for the log. */
        sum->alias_pte[c] = vmm_test_get_l2_entry(COH_ALIAS_VA);
        struct coh_view_src src = {
            .wbase = wbase, .rbase = rbase, .cursor = &g_neg_cursor_ab,
            .limit = COH_NEG_SLOTS_AB,
        };
        if (out) {
            out("# control %s\r\n", g_neg_names[c]);
            out("# writer VA 0x%lx, reader VA 0x%lx, PA 0x%lx, alias descriptor 0x%016lx\r\n",
                (unsigned long)(uintptr_t)wbase, (unsigned long)COH_ALIAS_VA,
                (unsigned long)pa, (unsigned long)sum->alias_pte[c]);
            out("# fresh 4 KB slot per row, first slot %u\r\n", (unsigned)g_neg_cursor_ab);
        }
        coh_print_header(out);

        static struct coh_summary s2;       /* serialized by g_busy */
        memset(&s2, 0, sizeof(s2));
        coh_run_pairs(COH_P2, n, &src, &cfg, out, &s2);
        static struct coh_summary s4;
        memset(&s4, 0, sizeof(s4));
        s4.p4_min_straddles = UINT64_MAX;
        coh_run_corr(n, &src, &cfg, out, &s4);

        /* Tear the alias down; vmm_unmap_block broadcasts the TLBI. */
        vmm_unmap_block(COH_ALIAS_VA);

        sum->forbidden[c][0] = s2.forbidden[COH_P2];
        sum->timeouts[c][0] = s2.timeouts[COH_P2];
        sum->forbidden[c][1] = s4.forbidden[COH_P4];
        sum->timeouts[c][1] = s4.timeouts[COH_P4];
        sum->ran[c] = true;
        if (out) {
            out("# control %c totals: P2 forbidden=%llu timeouts=%llu, "
                "P4 forbidden=%llu timeouts=%llu\r\n",
                (char)('a' + c),
                (unsigned long long)sum->forbidden[c][0],
                (unsigned long long)sum->timeouts[c][0],
                (unsigned long long)sum->forbidden[c][1],
                (unsigned long long)sum->timeouts[c][1]);
        }
    }

    if (rc == 0) {
        /* Second MB of the block: the aliases cover it but no alias read
         * ever touches it, so no reader holds a Non-shareable copy. */
        uint8_t *cbase = wbase + (uint64_t)COH_NEG_SLOTS_AB * COH_NEG_SLOT_BYTES;
        struct coh_view_src src = {
            .wbase = cbase, .rbase = cbase, .cursor = &g_neg_cursor_c,
            .limit = COH_NEG_SLOTS_C,
        };
        coh_run_dcoff(&src, &cfg, out, sum);
    }

    coh_leave(&caller);
    if (out && rc == 0) {
        out("coherency neg: done (report only: control c should show stale phase-1 "
            "reads on hardware with real caches, a and b may or may not; QEMU TCG "
            "models no caches)\r\n");
    }
    return rc;
}

/* ------------------------------------------------------------------------
 * coherency regs
 * ------------------------------------------------------------------------ */

#define COH_DEFINE_MRS(fn, regname)                                     \
    static inline uint64_t fn(void)                                     \
    {                                                                   \
        uint64_t v;                                                     \
        __asm__ volatile("mrs %0, " regname : "=r"(v));                 \
        return v;                                                       \
    }

COH_DEFINE_MRS(coh_read_currentel, "CurrentEL")
COH_DEFINE_MRS(coh_read_hcr_el2, "hcr_el2")
COH_DEFINE_MRS(coh_read_sctlr_el2, "sctlr_el2")
COH_DEFINE_MRS(coh_read_sctlr_el12, "s3_5_c1_c0_0")    /* SCTLR_EL12 */
COH_DEFINE_MRS(coh_read_sctlr_el1, "sctlr_el1")
COH_DEFINE_MRS(coh_read_tcr_el2, "tcr_el2")
COH_DEFINE_MRS(coh_read_mair_el2, "mair_el2")
COH_DEFINE_MRS(coh_read_tcr_el1, "tcr_el1")
COH_DEFINE_MRS(coh_read_mair_el1, "mair_el1")
COH_DEFINE_MRS(coh_read_clidr, "clidr_el1")
COH_DEFINE_MRS(coh_read_midr, "midr_el1")
COH_DEFINE_MRS(coh_read_mpidr, "mpidr_el1")
COH_DEFINE_MRS(coh_read_ctr, "ctr_el0")
COH_DEFINE_MRS(coh_read_ccsidr, "ccsidr_el1")
COH_DEFINE_MRS(coh_read_mmfr2, "id_aa64mmfr2_el1")
#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)
/*
 * IMPLEMENTATION DEFINED, read-only here. Encodings: CPUECTLR_EL1 from the
 * Cortex-A76 TRM (100798) and A78AE TRM (101779); CLUSTERECTLR_EL1 from
 * the DSU TRM (100453) section 11.8 and the A78AE TRM register summary
 * (DSU-AE). Reads at NS-EL2 are RW-accessible per those tables; only
 * writes are gated by ACTLR_EL3/ACTLR_EL2.ECTLREN. Never write these (or
 * CPUACTLR*, CPUPWRCTLR, CLUSTERPWR*): a denied write traps to EL3, which
 * on Jetson panics BL31.
 */
COH_DEFINE_MRS(coh_read_cpuectlr, "s3_0_c15_c1_4")     /* CPUECTLR_EL1 */
COH_DEFINE_MRS(coh_read_clusterectlr, "s3_0_c15_c3_4") /* CLUSTERECTLR_EL1 */
#define COH_HAVE_IMPDEF_READS   1
#endif

#define COH_HCR_E2H             (1ull << 34)

/* Cores whose TRMs document CPUECTLR_EL1 and CLUSTERECTLR_EL1 (DSU). */
static bool coh_midr_has_cpuectlr(uint64_t midr)
{
    uint32_t implementer = (uint32_t)((midr >> 24) & 0xFF);
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFF);
    if (implementer != 0x41) {
        return false;
    }
    return part == 0xD0B ||     /* Cortex-A76 */
           part == 0xD41 ||     /* Cortex-A78 */
           part == 0xD42 ||     /* Cortex-A78AE */
           part == 0xD4B;       /* Cortex-A78C */
}

static void coh_regs_read(struct coh_cpu_regs *r, bool impdef)
{
    irq_flags_t flags = irq_save();
    r->ran_on = cpu_id();
    r->current_el = (coh_read_currentel() >> 2) & 3u;
    r->sctlr_el2 = 0;
    r->hcr_el2 = 0;
    if (r->current_el == 2) {
        r->hcr_el2 = coh_read_hcr_el2();
        r->sctlr_el2 = coh_read_sctlr_el2();
        /* With E2H=1 the _EL1 name means SCTLR_EL2; read the EL1 copy. */
        r->sctlr_el1 = (r->hcr_el2 & COH_HCR_E2H) ? coh_read_sctlr_el12()
                                                   : coh_read_sctlr_el1();
        r->tcr = coh_read_tcr_el2();
        r->mair = coh_read_mair_el2();
    } else {
        r->sctlr_el1 = coh_read_sctlr_el1();
        r->tcr = coh_read_tcr_el1();
        r->mair = coh_read_mair_el1();
    }
    r->clidr = coh_read_clidr();
    r->midr = coh_read_midr();
    r->mpidr = coh_read_mpidr();
    r->ctr = coh_read_ctr();
    r->ncache = 0;
    for (uint32_t level = 1; level <= COH_MAX_CACHE_LEVELS; level++) {
        uint32_t ctype = (uint32_t)((r->clidr >> (3u * (level - 1u))) & 7u);
        if (ctype == 0) {
            break;
        }
        if (ctype < 2) {
            continue;       /* instruction cache only */
        }
        uint64_t sel = (uint64_t)(level - 1u) << 1;     /* InD = 0: data/unified */
        __asm__ volatile("msr csselr_el1, %0\n isb" : : "r"(sel) : "memory");
        r->level[r->ncache] = (uint8_t)level;
        r->ctype[r->ncache] = (uint8_t)ctype;
        r->ccsidr[r->ncache] = coh_read_ccsidr();
        r->ncache++;
    }
    r->impdef_read = false;
    r->cpuectlr = 0;
    r->clusterectlr = 0;
#if defined(COH_HAVE_IMPDEF_READS)
    if (impdef && coh_midr_has_cpuectlr(r->midr)) {
        r->cpuectlr = coh_read_cpuectlr();
        r->clusterectlr = coh_read_clusterectlr();
        r->impdef_read = true;
    }
#else
    (void)impdef;
    (void)coh_midr_has_cpuectlr;
#endif
    r->valid = true;
    irq_restore(flags);
}

struct coh_regs_slot {
    alignas(COH_LINE_BYTES) struct coh_cpu_regs r;
    uint64_t nonce;
    bool impdef;
    alignas(COH_LINE_BYTES) volatile uint64_t done;
};

static struct coh_regs_slot g_regs_slots[MAX_CPUS];

static void coh_regs_task(void *arg)
{
    struct coh_regs_slot *s = (struct coh_regs_slot *)arg;
    uint64_t nonce = s->nonce;
    coh_regs_read(&s->r, s->impdef);
    coh_stlr(&s->done, nonce);
}

int coherency_regs_collect(struct coh_cpu_regs regs[MAX_CPUS], bool impdef)
{
    struct coh_caller caller;
    if (coh_enter(&caller, NULL) != 0) {
        return -1;
    }
    uint32_t ncpu = (cpu_count > MAX_CPUS) ? MAX_CPUS : cpu_count;
    struct coh_remote rem[MAX_CPUS] = {0};
    bool started[MAX_CPUS] = {false};
    uint64_t nonce = ++g_nonce;

    for (uint32_t c = 0; c < ncpu; c++) {
        memset(&g_regs_slots[c].r, 0, sizeof(g_regs_slots[c].r));
        g_regs_slots[c].nonce = nonce;
        g_regs_slots[c].impdef = impdef;
        coh_st(&g_regs_slots[c].done, 0);
    }
    coh_dmb_ish();
    for (uint32_t c = 0; c < ncpu; c++) {
        if (c != caller.cpu) {
            started[c] = coh_remote_start(&rem[c], c, coh_regs_task, &g_regs_slots[c], 'r');
        }
    }
    coh_regs_read(&g_regs_slots[caller.cpu].r, impdef);

    int reported = 0;
    for (uint32_t c = 0; c < ncpu; c++) {
        bool have = (c == caller.cpu) ||
                    (started[c] && coh_wait_done(&g_regs_slots[c].done, nonce, &rem[c]));
        if (have) {
            regs[c] = g_regs_slots[c].r;
            reported++;
        } else {
            memset(&regs[c], 0, sizeof(regs[c]));
        }
    }
    coh_leave(&caller);
    return reported;
}

static void coh_print_sctlr(coh_out_fn out, const char *name, uint64_t v)
{
    out("  %-10s 0x%016llx (M=%u C=%u I=%u)\r\n", name, (unsigned long long)v,
        (unsigned)(v & 1u), (unsigned)((v >> 2) & 1u), (unsigned)((v >> 12) & 1u));
}

void coherency_regs_print(coh_out_fn out, const struct coh_cpu_regs regs[MAX_CPUS],
                          uint32_t ncpus)
{
    bool ccidx = ((coh_read_mmfr2() >> 20) & 0xFu) != 0;
    for (uint32_t c = 0; c < ncpus && c < MAX_CPUS; c++) {
        const struct coh_cpu_regs *r = &regs[c];
        if (!r->valid) {
            out("cpu%u: no report\r\n", (unsigned)c);
            continue;
        }
        out("cpu%u: CurrentEL=EL%llu MIDR=0x%08llx MPIDR=0x%llx%s\r\n", (unsigned)c,
            (unsigned long long)r->current_el, (unsigned long long)r->midr,
            (unsigned long long)r->mpidr, (r->ran_on == c) ? "" : " (ran on another CPU)");
        if (r->current_el == 2) {
            coh_print_sctlr(out, "SCTLR_EL2", r->sctlr_el2);
            out("  %-10s 0x%016llx (E2H=%u TGE=%u)\r\n", "HCR_EL2",
                (unsigned long long)r->hcr_el2, (unsigned)((r->hcr_el2 >> 34) & 1u),
                (unsigned)((r->hcr_el2 >> 27) & 1u));
        }
        coh_print_sctlr(out, "SCTLR_EL1", r->sctlr_el1);
        out("  %-10s 0x%016llx\r\n", (r->current_el == 2) ? "TCR_EL2" : "TCR_EL1",
            (unsigned long long)r->tcr);
        out("  %-10s 0x%016llx\r\n", (r->current_el == 2) ? "MAIR_EL2" : "MAIR_EL1",
            (unsigned long long)r->mair);
        out("  %-10s 0x%016llx\r\n", "CLIDR_EL1", (unsigned long long)r->clidr);
        out("  %-10s 0x%016llx\r\n", "CTR_EL0", (unsigned long long)r->ctr);
        for (uint32_t i = 0; i < r->ncache; i++) {
            uint64_t s = r->ccsidr[i];
            uint64_t line = 1ull << ((s & 7u) + 4u);
            uint64_t ways;
            uint64_t sets;
            if (ccidx) {
                ways = ((s >> 3) & 0x1FFFFFu) + 1u;
                sets = ((s >> 32) & 0xFFFFFFu) + 1u;
            } else {
                ways = ((s >> 3) & 0x3FFu) + 1u;
                sets = ((s >> 13) & 0x7FFFu) + 1u;
            }
            out("  L%u%s CCSIDR 0x%016llx (%llu B lines, %llu-way, %llu sets, %llu KB)\r\n",
                (unsigned)r->level[i], (r->ctype[i] == 4) ? " " : "D",
                (unsigned long long)s, (unsigned long long)line,
                (unsigned long long)ways, (unsigned long long)sets,
                (unsigned long long)(line * ways * sets / 1024u));
        }
        if (r->impdef_read) {
            /* Raw values only: bit meanings differ per core (bit 6 is RES0
             * on A76 and PF_MODE[0] on A78AE), so nothing is decoded. */
            out("  %-10s 0x%016llx (raw)\r\n", "CPUECTLR", (unsigned long long)r->cpuectlr);
            out("  %-10s 0x%016llx (raw)\r\n", "CLUSTERECTLR",
                (unsigned long long)r->clusterectlr);
        } else {
            out("  CPUECTLR / CLUSTERECTLR not read on this platform or core\r\n");
        }
    }
}

/* ------------------------------------------------------------------------
 * Shell verb
 * ------------------------------------------------------------------------ */

static int coh_parse_scale(int argc, char **argv, uint32_t *scale)
{
    *scale = 1;
    if (argc < 3) {
        return 0;
    }
    uint32_t v = 0;
    for (const char *p = argv[2]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9' || v > COH_MAX_SCALE) {
            return -1;
        }
        v = v * 10u + (uint32_t)(*p - '0');
    }
    if (v == 0 || v > COH_MAX_SCALE) {
        return -1;
    }
    *scale = v;
    return 0;
}

static void coh_usage(void)
{
    shell_puts("usage: coherency regs | litmus [scale] | neg [scale]\r\n");
    shell_puts("  scale divides the spec iteration counts (1 = spec, default)\r\n");
}

int cmd_coherency(int argc, char **argv)
{
    if (argc < 2) {
        coh_usage();
        return -1;
    }
    if (strcmp(argv[1], "regs") == 0) {
        if (argc >= 3) {
            coh_usage();
            return -1;
        }
        static struct coh_cpu_regs regs[MAX_CPUS];
        int n = coherency_regs_collect(regs, true);
        if (n < 0) {
            shell_puts("coherency: another run is in progress\r\n");
            return -1;
        }
        coherency_regs_print(shell_printf, regs, cpu_count);
        shell_printf("coherency regs: %d of %u CPUs reported\r\n", n, (unsigned)cpu_count);
        return (n == (int)cpu_count) ? 0 : 1;
    }
    if (strcmp(argv[1], "litmus") == 0 || strcmp(argv[1], "neg") == 0) {
        uint32_t scale;
        if (coh_parse_scale(argc, argv, &scale) != 0) {
            shell_printf("coherency: scale is 1..%u\r\n", (unsigned)COH_MAX_SCALE);
            return -1;
        }
        if (argv[1][0] == 'l') {
            return coherency_litmus_run(scale, shell_printf, NULL);
        }
        return coherency_neg_run(scale, shell_printf, NULL);
    }
    coh_usage();
    return -1;
}

#endif /* !PLATFORM_X86_64 */
