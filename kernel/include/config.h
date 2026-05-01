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
 * History (#581 throughput stack):
 *   - 1024: original. Capped framed `xput chunk` binary at ~497 B,
 *     forcing ~2.16M round-trips per 1 GB upload.
 *   - 1024 → 8192 (PR #592): raised binary chunk to ~4 KB,
 *     cutting round-trips 8× to ~270K.
 *   - 8192 → 32768 (this bump): raises binary chunk to ~16 KB,
 *     cutting round-trips 4× more to ~65K. Combined with the
 *     persistent fd (#591), echo-skip (#593), and net_poll drain
 *     loop (#594), 1 GB upload should land in tens of minutes.
 *
 * Wire form for the bulk-upload command is
 * `xput chunk OFFSET HEXDATA\n`, with HEXDATA at 2× the binary
 * chunk size. The line-budget arithmetic in slm-put.py
 * (`max_framed_chunk_bytes`) leaves a 16-char headroom and
 * subtracts `xput chunk ` (11 chars) + 10-digit offset + space
 * (12 chars) + newline (1) = 24 chars, then halves the rest for
 * hex. With SHELL_MAX_LINE = 32768 the effective binary ceiling
 * is `(32768 - 1 - 16 - 24) / 2 = 16363` bytes per chunk.
 *
 * Stack cost is 32 KB on the stack-local `line_buffer` in
 * shell_run() and on the `buf` in shell_execute(). On a 64 KB
 * STACK_SIZE that's 50% per buffer — only one is live at a time
 * (shell_run's REPL is the only caller of shell_execute via
 * dispatch_cmd), and the deepest call chain underneath
 * (shell_run → cmd_xput → littlefs_file_write → COW metadata
 * helpers) typically uses < 4 KB more. Headroom remains.
 *
 * BSS cost lands in TCP_SHELL_RING_SIZE (16 KB / session × 16
 * sessions = 256 KB; see shell_io_tcp.c) and the static `data[]`
 * decode buffer in cmd_xput chunk (16 KB now).
 *
 * Both sides of the protocol must agree on this value:
 * `SHELL_MAX_LINE` in scripts/tools/slm-put.py mirrors it.
 * Mismatch → either truncated commands (kernel < client) or
 * wasted slm-put.py headroom (kernel > client). */
#define SHELL_MAX_LINE      32768
#define SHELL_MAX_ARGS      16              /* Maximum arguments per command */

#endif /* CONFIG_H */
