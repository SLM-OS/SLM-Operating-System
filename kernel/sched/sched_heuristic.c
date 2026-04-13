/*
 * sched_heuristic.c - Built-in heuristic scheduling policy for SLM-OS
 *
 * Round-robin load balancing with deadline pressure awareness.
 * Deadline-constrained tasks are routed to "performance" cores (CPU > 0)
 * to keep CPU 0 available for system tasks. On big.LITTLE hardware,
 * performance cores would be the big cores.
 *
 * This was the original inline CPU assignment logic in sched.c, extracted
 * into the pluggable policy interface.
 */

#include "sched_policy.h"
#include "sched.h"
#include "task.h"
#include "smp.h"
#include "slm_ffi.h"
#include "debug.h"
#include <stdint.h>

/* Deadline boost thresholds (must match sched.c) */
#define DEADLINE_CRITICAL_NS    (10 * 1000000ULL)   /* 10ms */
#define DEADLINE_HIGH_NS        (50 * 1000000ULL)   /* 50ms */
#define DEADLINE_BOOST_NS       (100 * 1000000ULL)  /* 100ms */

/* sched_cpu_rq() and sched_get_isolated_cores() are declared in sched.h */

/*
 * Calculate deadline pressure for a CPU's run queue.
 *
 * Returns a score based on the sum of urgency of deadline-constrained tasks.
 * Higher score = more deadline pressure = avoid placing more work here.
 */
static uint32_t calculate_deadline_pressure(uint32_t cpu)
{
    struct cpu_runqueue *rq = sched_cpu_rq(cpu);
    uint32_t pressure = 0;
    uint64_t now = slm_get_time_ns();

    struct task *t = rq->head;
    while (t) {
        if (t->deadline_ns > 0) {
            if (now >= t->deadline_ns) {
                pressure += 8;  /* Deadline missed */
            } else {
                uint64_t remaining = t->deadline_ns - now;
                if (remaining < DEADLINE_CRITICAL_NS) {
                    pressure += 8;  /* < 10ms */
                } else if (remaining < DEADLINE_HIGH_NS) {
                    pressure += 4;  /* < 50ms */
                } else if (remaining < DEADLINE_BOOST_NS) {
                    pressure += 2;  /* < 100ms */
                } else {
                    pressure += 1;  /* distant deadline */
                }
            }
        }
        t = t->next;
    }

    return pressure;
}

/*
 * Find a non-isolated CPU with the lowest combined load.
 *
 * Uses a combined metric: ready_count + deadline_pressure.
 * Round-robin starting point breaks ties (avoids always picking CPU 0).
 */
static uint32_t find_target_cpu(void)
{
    static uint32_t rr_next;
    uint32_t isolated = sched_get_isolated_cores();

    uint32_t best_cpu = 0;  /* fallback */
    uint32_t best_score = UINT32_MAX;

    for (uint32_t i = 0; i < cpu_count; i++) {
        uint32_t cpu = (rr_next + i) % cpu_count;

        if (isolated & (1U << cpu))
            continue;

        uint32_t score = sched_cpu_rq(cpu)->ready_count
                       + calculate_deadline_pressure(cpu);

        if (score < best_score) {
            best_cpu = cpu;
            best_score = score;
        }
    }

    rr_next = best_cpu + 1;
    return best_cpu;
}

/*
 * Find a "performance" core for deadline-critical tasks.
 *
 * In a big.LITTLE system, this would return a big core.
 * On homogeneous systems, CPU 1+ are treated as "performance" cores
 * to keep CPU 0 available for system tasks.
 */
static uint32_t find_performance_cpu(void)
{
    if (cpu_count <= 1) {
        return 0;
    }

    uint32_t isolated = sched_get_isolated_cores();
    uint32_t best_cpu = 0;
    uint32_t best_score = UINT32_MAX;
    int found_perf_core = 0;

    for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
        if (isolated & (1U << cpu))
            continue;

        uint32_t score = sched_cpu_rq(cpu)->ready_count
                       + calculate_deadline_pressure(cpu);
        if (score < best_score) {
            best_cpu = cpu;
            best_score = score;
            found_perf_core = 1;
        }
    }

    if (!found_perf_core) {
        return find_target_cpu();
    }

    return best_cpu;
}

/*
 * Heuristic CPU assignment: deadline tasks go to performance cores,
 * others go to the least-loaded non-isolated core.
 */
static uint32_t heuristic_assign_cpu(struct task *task)
{
    if (task->deadline_ns > 0) {
        uint32_t cpu = find_performance_cpu();
        DEBUG_PRINT("Deadline task '%s' -> CPU %u (performance core)",
                    task->name, cpu);
        return cpu;
    }

    return find_target_cpu();
}

/* Periodic rebalance hook (D1 / P2-4). Implemented in
 * kernel/sched/sched_rebalance.c — only runs on BSP and only every
 * REBALANCE_INTERVAL_TICKS timer ticks, so it's cheap to wire into
 * every tick. */
extern void sched_rebalance_tick(uint32_t cpu);

static void heuristic_tick(uint32_t cpu)
{
    sched_rebalance_tick(cpu);
}

const struct sched_policy_ops sched_policy_heuristic = {
    .name       = "heuristic",
    .init       = NULL,
    .shutdown   = NULL,
    .assign_cpu = heuristic_assign_cpu,
    .tick       = heuristic_tick,
};
