/*
 * sched_diag.h - Per-CPU scheduler diagnostic counters (sched_diag_*)
 *
 * sched.c defines each counter as a cacheable array of MAX_CPUS; every
 * user indexes it as sched_diag_tick[cpu]. Declare them only here: a
 * local extern that disagrees with the definition (a pointer extern
 * against the array) makes the reader load the first two counters as a
 * pointer and fault (seen on Pi 5 in cpu_supervisor_task_entry, #1193).
 * sched.c includes this header (through sched.h), so a definition that
 * drifts from these declarations is a compile error.
 */

#ifndef SCHED_DIAG_H
#define SCHED_DIAG_H

#include <stdint.h>
#include "config.h"     /* MAX_CPUS */

#define SCHED_DIAG_COUNTER(name)    extern volatile uint32_t name[MAX_CPUS]

SCHED_DIAG_COUNTER(sched_diag_tick);
SCHED_DIAG_COUNTER(sched_diag_schedule);
SCHED_DIAG_COUNTER(sched_diag_picked);
SCHED_DIAG_COUNTER(sched_diag_idle_loops);              /* idle task iterations */
SCHED_DIAG_COUNTER(sched_diag_steal_attempts);          /* sched_try_steal entries */
SCHED_DIAG_COUNTER(sched_diag_steal_successes);         /* live task returned */
SCHED_DIAG_COUNTER(sched_diag_steal_stale);             /* stale pointer discarded */
SCHED_DIAG_COUNTER(sched_diag_steal_empty_victim);      /* victim had nothing to take */
SCHED_DIAG_COUNTER(sched_diag_steal_push_full);         /* push failed, deque full (#175) */
SCHED_DIAG_COUNTER(sched_diag_switch_defer);            /* switch-in deferred: previous CPU had not released next (#1234) */

#undef SCHED_DIAG_COUNTER

#endif /* SCHED_DIAG_H */
