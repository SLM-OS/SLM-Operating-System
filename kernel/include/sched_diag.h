/*
 * sched_diag.h - Per-CPU scheduler diagnostic counters (sched_diag_*)
 *
 * sched.c defines these counters in one of two ways:
 *
 *   PLATFORM_HAS_NC_MEMORY (Pi 5, Jetson, default build): pointers into
 *     the NC block, assigned in scheduler_init().
 *   Otherwise (QEMU, x86-64, and Pi 5 / Jetson built with NC_AXIS=OFF):
 *     cacheable arrays of MAX_CPUS.
 *
 * Every user indexes them the same way (sched_diag_tick[cpu]), but an
 * extern declaration must match the definition: a pointer extern against
 * an array definition makes the reader load the first two counters as a
 * pointer and fault (seen on Pi 5 under NC_AXIS=OFF in
 * cpu_supervisor_task_entry, #1193). Declare them only here. sched.c
 * includes this header (through sched.h), so a definition that drifts
 * from these declarations is a compile error in both configurations.
 */

#ifndef SCHED_DIAG_H
#define SCHED_DIAG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"     /* MAX_CPUS */
#include "ncmem.h"      /* PLATFORM_HAS_NC_MEMORY */

#if defined(PLATFORM_HAS_NC_MEMORY)
#define SCHED_DIAG_COUNTER(name)    extern volatile uint32_t *name
#else
#define SCHED_DIAG_COUNTER(name)    extern volatile uint32_t name[MAX_CPUS]
#endif

SCHED_DIAG_COUNTER(sched_diag_tick);
SCHED_DIAG_COUNTER(sched_diag_schedule);
SCHED_DIAG_COUNTER(sched_diag_picked);
SCHED_DIAG_COUNTER(sched_diag_idle_loops);              /* idle task iterations */
SCHED_DIAG_COUNTER(sched_diag_steal_attempts);          /* sched_try_steal entries */
SCHED_DIAG_COUNTER(sched_diag_steal_successes);         /* live task returned */
SCHED_DIAG_COUNTER(sched_diag_steal_stale);             /* stale pointer discarded */
SCHED_DIAG_COUNTER(sched_diag_steal_empty_victim);      /* victim had nothing to take */
SCHED_DIAG_COUNTER(sched_diag_steal_push_full);         /* push failed, deque full (#175) */

#undef SCHED_DIAG_COUNTER

/*
 * True once counter @name can be indexed. The NC pointers are NULL until
 * scheduler_init() assigns them; the arrays always exist, and testing an
 * array's address is a -Werror=address error, so code that may run
 * before scheduler_init uses this instead of `if (name)`.
 */
#if defined(PLATFORM_HAS_NC_MEMORY)
#define SCHED_DIAG_READY(name)      ((name) != NULL)
#else
#define SCHED_DIAG_READY(name)      true
#endif

#endif /* SCHED_DIAG_H */
