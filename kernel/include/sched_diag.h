/*
 * sched_diag.h - Per-CPU scheduler diagnostic counters (sched_diag[cpu])
 *
 * sched.c defines one cacheable `struct sched_diag_cpu` per CPU; every
 * user reads sched_diag[cpu].tick and so on. Declare it only here: a
 * local extern that disagrees with the definition makes the reader load
 * the wrong words (a pointer extern against the array faulted on Pi 5 in
 * cpu_supervisor_task_entry, #1193). sched.c includes this header (through
 * sched.h), so a definition that drifts from this declaration is a compile
 * error.
 *
 * Layout (#1263): the counters used to be ten packed uint32_t arrays, so
 * the counters of all CPUs shared four cache lines and every increment on
 * one CPU (a tick, a schedule(), an idle-loop pass) took a line away from
 * the others. Each CPU now has its counters in its own cache line
 * (cacheline.h). A CPU writes only its own struct, except steal_push_full,
 * which the CPU adding a task to another CPU's queue increments.
 */

#ifndef SCHED_DIAG_H
#define SCHED_DIAG_H

#include <stdint.h>
#include "cacheline.h"
#include "config.h"     /* MAX_CPUS */

struct sched_diag_cpu {
    volatile uint32_t tick;                 /* scheduler_tick calls */
    volatile uint32_t switch_defer;         /* switch-in deferred: previous CPU had not released next (#1234) */
    volatile uint32_t schedule;             /* schedule() calls */
    volatile uint32_t picked;               /* non-idle task picked */
    volatile uint32_t idle_loops;           /* idle task iterations */
    volatile uint32_t steal_attempts;       /* sched_try_steal entries */
    volatile uint32_t steal_successes;      /* live task returned */
    volatile uint32_t steal_stale;          /* stale pointer discarded */
    volatile uint32_t steal_empty_victim;   /* victim had nothing to take */
    volatile uint32_t steal_push_full;      /* push failed, deque full (#175) */
} CACHE_LINE_ALIGNED;

CACHE_LINE_ASSERT_PADDED(struct sched_diag_cpu);

extern struct sched_diag_cpu sched_diag[MAX_CPUS];

#endif /* SCHED_DIAG_H */
