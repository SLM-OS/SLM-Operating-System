/*
 * config.h - System Configuration for SLM-OS
 *
 * Central location for all compile-time tunables.
 * Modify these values to adjust system limits and behavior.
 */

#ifndef CONFIG_H
#define CONFIG_H

/* ============================================================================
 * System Limits
 * ============================================================================ */

#define MAX_CPUS            8               /* Maximum supported CPU cores */
#define MAX_TASKS           64              /* Maximum concurrent tasks */

/* ============================================================================
 * Memory Configuration
 * ============================================================================ */

#define STACK_SIZE          0x10000UL       /* 64 KB per stack — needed for ONNX parsing + inference */

/* ============================================================================
 * Scheduler Configuration
 * ============================================================================ */

#define TIMER_HZ            100             /* Timer frequency: 100 Hz = 10ms tick */
#define TIME_SLICE_MS       (1000 / TIMER_HZ)   /* Time slice in milliseconds */

/*
 * Work-stealing scheduler (#59 Phase B).
 *
 * When set to 1, idle CPUs try to pull unpinned tasks from other CPUs'
 * run queues via kernel/sched/steal_deque. Default off until Phase C
 * benchmarks justify enabling by default (requires #57 preemption on
 * all platforms).
 */
#ifndef CONFIG_WORK_STEALING
#define CONFIG_WORK_STEALING 0
#endif

/* ============================================================================
 * Task Configuration
 * ============================================================================ */

#define TASK_NAME_LEN       16              /* Maximum task name length */

/* ============================================================================
 * IPC Configuration
 * ============================================================================ */

#define MSG_SIZE_DEFAULT    64              /* Default message size in bytes */
#define MSG_QUEUE_CAPACITY  16              /* Messages per queue */
#define MSG_QUEUE_MAX       32              /* Maximum message queues */

#define SHM_BUFFER_MAX      64              /* Maximum shared buffers */
#define SHM_MAPPING_MAX     16              /* Maximum mappings per buffer */

/* ============================================================================
 * Shell Configuration
 * ============================================================================ */

#define SHELL_MAX_LINE      1024            /* Maximum command line length */
#define SHELL_MAX_ARGS      16              /* Maximum arguments per command */

#endif /* CONFIG_H */
