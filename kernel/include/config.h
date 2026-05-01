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

/* Maximum command line length.
 *
 * Bumped 1024 → 8192 (#581 throughput follow-up) so framed `xput
 * chunk` commands can carry larger binary chunks. The wire form is
 * `xput chunk OFFSET HEXDATA\n`, where HEXDATA is 2× the binary
 * chunk size; with the previous 1024-char ceiling and a ~25-char
 * prefix (`xput chunk ` + 10-digit offset + space + newline) plus
 * slm-put.py's 16-char headroom, the binary chunk capped at
 * ~497 B. A 1 GB upload at 497 B/chunk over a strictly synchronous
 * request/response shell protocol takes hours regardless of LAN
 * speed (~2.16M round-trips × ~3 ms RTT ≈ 1.8 h, 6 h+ on slower
 * paths). 8192 raises the binary chunk to ~4 KB, cutting round-
 * trips 8× and the 1 GB transfer to ~15-25 minutes.
 *
 * Stack cost is 8 KB on the stack-local `line_buffer` in
 * shell_run() and on the `buf` in shell_execute(), well under the
 * 64 KB STACK_SIZE budget. BSS cost lands in TCP_SHELL_RING_SIZE
 * (16 KB / session × 16 sessions = 256 KB extra; see
 * shell_io_tcp.c) and the static `data[]` decode buffer in
 * cmd_xput chunk (4 KB).
 *
 * Both sides of the protocol must agree on this value:
 * `SHELL_MAX_LINE` in scripts/tools/slm-put.py mirrors it.
 * Mismatch → either truncated commands (kernel < client) or
 * wasted slm-put.py headroom (kernel > client). */
#define SHELL_MAX_LINE      8192
#define SHELL_MAX_ARGS      16              /* Maximum arguments per command */

#endif /* CONFIG_H */
