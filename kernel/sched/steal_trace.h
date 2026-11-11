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

/* Phase bins (#1216). An attempt's or a window's time relative to a
 * tick is binned at [0,10us) [10,20) [20,50) [50,100) [100,200)
 * [200,500) [500us,1ms) [1,2ms) [2,5ms) [5,10ms) [>=10ms]. The bench
 * computes the bounds in counter ticks at arm time. */
#define STEAL_PH_BINS 11
/* Offset of an attempt into the watched CPU's open preempt_disabled
 * window: [0,1us) [1,2) [2,4) [4,8) [8,16) [16,32) [32,64) [>=64us]. */
#define STEAL_WOFF_BINS 8
/* Windows logged per CPU per run (the first ones of the run). */
#define STEAL_WIN_LOG_LEN 128

/* preempt_disabled window reasons (#1216). */
#define STEAL_WIN_TICK   0  /* 'T': SECONDARY_PREEMPT tick trampoline   */
#define STEAL_WIN_COOP   1  /* 'C': coop tick inside schedule()         */
#define STEAL_WIN_PUSH   2  /* 'P': direct schedule() after a push onto
                               this CPU's deque since its last window  */
#define STEAL_WIN_OTHER  3  /* 'O': any other direct schedule()         */
#define STEAL_WIN_BOOT   4  /* 'B': scheduler_start                     */
#define STEAL_WIN_NREASON 5

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

    /* Per run, against the watched CPU w (steal_trace_watch, CPU 1;
     * #1216). Read at the start of each attempt with plain loads of
     * w's flag and w's window and tick blocks; nothing is written to
     * w's memory. */
    volatile uint32_t w_in[STEAL_WIN_NREASON]; /* attempts that started
                                      with preempt_disabled[w] set, by
                                      w's open window reason             */
    volatile uint32_t w_in_unk;    /* ... flag set, no window open in w's
                                      tracker (set by an untracked site)  */
    volatile uint32_t w_off[STEAL_WOFF_BINS]; /* offset into w's window   */
    volatile uint32_t ph_w[STEAL_PH_BINS];    /* now - w's last tick      */
    volatile uint32_t ph_w_pd[STEAL_PH_BINS]; /* ... attempts ending 'P'  */
    volatile uint32_t ph_own[STEAL_PH_BINS];  /* now - this CPU's tick    */
} __attribute__((aligned(64)));

/*
 * Victim-side preempt_disabled window trace (#1216).
 *
 * Every store to preempt_disabled[c] in the ARM64 scheduler (schedule(),
 * the coop tick, scheduler_start, task_entry_wrapper and the
 * SECONDARY_PREEMPT trampoline) is paired with a call that mirrors the
 * flag: 0 -> 1 opens a window, 1 -> 0 closes it, nested sets are
 * no-ops. Each block is written only by its own CPU, from those sites
 * and from sched_note_hw_tick() in the timer ISR; thieves read win_open, cur_reason,
 * cur_entry and last_tick with plain loads. Times are raw CNTPCT; the
 * log stores signed offsets from the bench's arm time. Per-run fields
 * reset lazily at the CPU's first event of a new run, keeping the
 * currently open window.
 */
struct steal_pdwin_cpu {
    volatile uint32_t run;
    volatile uint32_t win_open;    /* mirrors preempt_disabled[c]        */
    volatile uint32_t cur_reason;  /* STEAL_WIN_* of the open window     */
    volatile uint32_t pushes_at_close; /* steal_trace_dq[c].pushes when the
                                      last window closed                 */
    volatile uint64_t cur_entry;   /* CNTPCT the open window opened      */
    volatile uint64_t last_tick;   /* CNTPCT of the last hardware tick   */

    /* Per run. */
    volatile uint64_t first_tick;
    volatile uint32_t tick_n;      /* hardware timer ticks this run      */
    volatile uint32_t n[STEAL_WIN_NREASON];      /* windows closed       */
    volatile uint64_t sum[STEAL_WIN_NREASON];    /* total open counts    */
    volatile uint32_t max[STEAL_WIN_NREASON];    /* longest, counts      */
    volatile uint32_t wph[STEAL_PH_BINS];        /* entry - last_tick    */
    volatile uint32_t nlog;
    volatile int32_t  log_entry[STEAL_WIN_LOG_LEN]; /* entry - arm time  */
    volatile uint32_t log_dur[STEAL_WIN_LOG_LEN];   /* counts            */
    volatile uint8_t  log_reason[STEAL_WIN_LOG_LEN];
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
const struct steal_pdwin_cpu *sched_steal_pdwin(uint32_t cpu);

/* The CPU thieves record windows and tick phase against, and the arm
 * time window logs are relative to (set by sched_steal_trace_arm). */
uint32_t sched_steal_trace_watch(void);
uint64_t sched_steal_trace_t0(void);

/* Window hooks (#1216). Called on CPU `cpu` right after it stores
 * preempt_disabled[cpu]; defined as no-ops without work stealing. The
 * _asm variants take the CPU in x0 and are called from vectors.S with
 * IRQs masked. */
void steal_pdwin_set(uint32_t cpu, uint32_t reason);
void steal_pdwin_set_sched(uint32_t cpu);
void steal_pdwin_clear(uint32_t cpu);
void steal_pdwin_tick(uint32_t cpu);
void steal_pdwin_set_tick_asm(uint32_t cpu);
void steal_pdwin_clear_asm(uint32_t cpu);

/*
 * `bench stealing --sustain --assert-shares` decision (#1216).
 *
 * exec[c] is the number of tasks CPU c executed in the run, for the
 * ncpu online CPUs. The owner is the CPU every task was pushed to;
 * every other online CPU is a thief. A thief's share is exec[c] over
 * the sum of exec[] over all CPUs, so comparing two thieves' shares is
 * comparing their exec counts. The run fails when:
 *   - it timed out (STEAL_SHARES_TIMEOUT), or
 *   - there is no thief, or no thief executed a task
 *     (STEAL_SHARES_NO_THIEF), or
 *   - some thief's share is below half the best thief's share, that
 *     is 2 * exec[c] < exec[best] (STEAL_SHARES_LOW). Exactly half
 *     passes.
 * The owner is never compared. best_cpu is the thief with the most
 * tasks and worst_cpu the thief with the fewest (ties: the lowest CPU
 * number). below_mask has bit c set for every thief below half the
 * best (CPUs 0 to 31). Timeout takes precedence over the other
 * failures; the CPU fields are filled in either way so the caller can
 * print them. Pure: no globals, no side effects.
 */
#define STEAL_SHARES_PASS      0u
#define STEAL_SHARES_LOW       1u
#define STEAL_SHARES_TIMEOUT   2u
#define STEAL_SHARES_NO_THIEF  3u
#define STEAL_SHARES_MAX_CPUS  32u

struct steal_shares_verdict {
    uint32_t result;      /* STEAL_SHARES_*                          */
    uint32_t best_cpu;    /* thief with the highest exec             */
    uint32_t worst_cpu;   /* thief with the lowest exec              */
    uint32_t below_mask;  /* bit c: thief c below half of best_cpu   */
    uint32_t nthieves;    /* thieves compared                        */
    uint32_t total;       /* sum of exec over all CPUs (the share
                             denominator)                            */
};

static inline struct steal_shares_verdict
steal_shares_check(const uint32_t *exec, uint32_t ncpu, uint32_t owner,
                   int timed_out)
{
    struct steal_shares_verdict v = {
        STEAL_SHARES_NO_THIEF, owner, owner, 0, 0, 0
    };
    if (ncpu > STEAL_SHARES_MAX_CPUS) {
        ncpu = STEAL_SHARES_MAX_CPUS;
    }
    for (uint32_t c = 0; c < ncpu; c++) {
        v.total += exec[c];
        if (c == owner) {
            continue;
        }
        if (v.nthieves == 0 || exec[c] > exec[v.best_cpu]) {
            v.best_cpu = c;
        }
        if (v.nthieves == 0 || exec[c] < exec[v.worst_cpu]) {
            v.worst_cpu = c;
        }
        v.nthieves++;
    }
    if (v.nthieves != 0) {
        uint64_t best = exec[v.best_cpu];
        for (uint32_t c = 0; c < ncpu; c++) {
            if (c != owner && 2ULL * exec[c] < best) {
                v.below_mask |= 1u << c;
            }
        }
    }
    if (timed_out) {
        v.result = STEAL_SHARES_TIMEOUT;
    } else if (v.nthieves == 0 || exec[v.best_cpu] == 0) {
        v.result = STEAL_SHARES_NO_THIEF;
    } else if (v.below_mask != 0) {
        v.result = STEAL_SHARES_LOW;
    } else {
        v.result = STEAL_SHARES_PASS;
    }
    return v;
}

#endif /* SCHED_STEAL_TRACE_H */
