/*
 * steal_trace.h - Per-CPU work-stealing failure-reason trace (#1203)
 *
 * `bench stealing` on jetson-nano-2 showed a non-owner CPU making 10 to
 * 16 steal attempts and 0 successes in cacheable builds. The existing
 * sched_diag_steal_* counters say an attempt failed but not why. This
 * trace records, for every sched_try_steal() call, how each victim
 * visit ended and how the attempt as a whole ended, plus a few
 * timestamps, so a stalled thief can be told apart from a thief that
 * lost a race to drain.
 *
 * Writers and memory:
 *   - struct steal_trace_cpu[c] is written only by CPU c, inside
 *     sched_try_steal() (plain stores to its own 64-byte-aligned
 *     block; no locks, no atomics, no prints).
 *   - struct steal_trace_dq[v] is written only under
 *     steal_deque_lock[v] (by the owner's push or a thief's steal).
 *   - Both live in ordinary cacheable BSS on every platform. The bench
 *     reads them on CPU 0 after every task has completed. Cross-CPU
 *     visibility relies on the hardware coherence ADR 0007 measured on
 *     Pi 5 and Jetson; `att` duplicates sched_diag_steal_attempts so a
 *     reader can check one against the other.
 *
 * Counters run forever; the bench reports deltas. Timestamps and the
 * attempt log are per run: sched_steal_trace_arm() bumps a run number
 * and each CPU resets its own timestamp fields the first time it
 * attempts a steal in the new run.
 */

#ifndef SCHED_STEAL_TRACE_H
#define SCHED_STEAL_TRACE_H

#include <stdint.h>

#define STEAL_TRACE_LOG_LEN 16

/* Attempt outcome codes (one per sched_try_steal call). */
#define STEAL_OUT_OK     'S'  /* returned a live task                    */
#define STEAL_OUT_ISO    'I'  /* this CPU is isolated: no victim probed  */
#define STEAL_OUT_STALE  'X'  /* some victim gave an entry, none valid   */
#define STEAL_OUT_PD     'P'  /* no stale entry; some victim skipped on
                                 preempt_disabled[victim]               */
#define STEAL_OUT_EMPTY  'E'  /* every probed victim's deque was empty   */
#define STEAL_OUT_OTHER  'O'  /* none of the above (no victim probed)    */

struct steal_trace_cpu {
    /* Attempt level: att = ok + iso + f_stale + f_pd + f_empty + f_other */
    volatile uint32_t att;
    volatile uint32_t ok;
    volatile uint32_t iso;
    volatile uint32_t f_stale;
    volatile uint32_t f_pd;
    volatile uint32_t f_empty;
    volatile uint32_t f_other;

    /* Victim-visit level, summed over victims. */
    volatile uint32_t v_pd;        /* skipped: preempt_disabled[victim] != 0 */
    volatile uint32_t v_pd_work;   /* ... while victim deque looked non-empty
                                      (unlocked read of top/bottom)          */
    volatile uint32_t v_busy;      /* steal_deque_lock[victim] read held just
                                      before spin_lock_irqsave              */
    volatile uint32_t v_empty;     /* deque empty under the lock            */
    volatile uint32_t v_gen;       /* entry popped, generation mismatch     */
    volatile uint32_t v_state;     /* entry popped, task state != READY     */
    volatile uint32_t v_cpu;       /* entry popped, assigned_cpu != victim  */
    volatile uint32_t v_aff;       /* entry popped, affinity pinned         */
    volatile uint32_t v_rm;        /* entry popped, not on victim's run
                                      queue (owner or rebalancer had it)    */

    /* Per run (valid when run == steal_trace_run). Raw CNTPCT. */
    volatile uint32_t run;
    volatile uint32_t lk_hw;       /* spinlock_hw_enabled as this CPU saw it
                                      at its first attempt of the run
                                      (2 = no such flag on this platform)   */
    volatile uint64_t first_att;
    volatile uint64_t last_att;
    volatile uint64_t first_ok;
    volatile uint32_t nlog;        /* attempts logged this run (saturates)  */
    volatile uint32_t log_dt[STEAL_TRACE_LOG_LEN];  /* CNTPCT - first_att  */
    volatile uint8_t  log_out[STEAL_TRACE_LOG_LEN]; /* STEAL_OUT_*         */
} __attribute__((aligned(64)));

struct steal_trace_dq {
    volatile uint32_t pushes;      /* successful pushes (counter)           */
    volatile uint32_t drains;      /* steals that left it empty (counter)   */
    volatile uint32_t drain_run;   /* run number of drain_ts                */
    volatile uint32_t push_run;    /* run number of last_push               */
    volatile uint64_t drain_ts;    /* CNTPCT when a steal left it empty:
                                      the deque was last non-empty here     */
    volatile uint64_t last_push;   /* CNTPCT of the last push               */
} __attribute__((aligned(64)));

/* Start a new trace run; returns the run number. Call from the bench
 * before dispatching work. */
uint32_t sched_steal_trace_arm(void);

/* Read-only views for the bench (cpu < MAX_CPUS, else NULL). */
const struct steal_trace_cpu *sched_steal_trace_cpu(uint32_t cpu);
const struct steal_trace_dq *sched_steal_trace_dq(uint32_t cpu);

#endif /* SCHED_STEAL_TRACE_H */
