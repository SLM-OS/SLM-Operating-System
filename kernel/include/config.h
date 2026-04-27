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
 * chunk size. The effective per-chunk binary ceiling is
 * `(SHELL_MAX_LINE - 1 - 16 - 22) / 2 ≈ 16 KB` at SHELL_MAX_LINE
 * = 32768, where the 22 covers `xput chunk OFFSET ` at a
 * worst-case 10-digit offset plus the trailing space, the -1
 * reserves a byte for the newline that `run_command` appends,
 * the -16 is the SHELL_LINE_HEADROOM safety margin in slm-put.py,
 * and the /2 accounts for hex's 2× expansion. See
 * `max_framed_chunk_bytes` in scripts/tools/slm-put.py for the
 * live arithmetic.
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

/* ============================================================================
 * Model Memory (Phase 3) — pool sizes consumed by rust_model_mem_init
 * ============================================================================
 *
 * Per-platform default sizes for the model-memory weight and workspace
 * pools. Both values are megabytes and must be multiples of 2 (the
 * model-memory block size is 2 MB; the Rust pool allocator rejects
 * misaligned sizes).
 *
 * Jetson hosts the SLM weight pool sized for Qwen2.5-1.5B-Q4_K_M
 * (~1.0 GB resident) plus a second-slot headroom; workspace covers
 * per-layer activations and the 512 MB KV-cache sub-pool that M5
 * carves out (see docs/specs/slm-integration.md "Memory Plan").
 *
 * Pi 5 hosts smaller vision-class models. QEMU and x86-64 keep the
 * original Phase-5 defaults so the test kernel boots inside
 * `make test`'s 1 GB systemd MemoryMax cap.
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define MODEL_MEM_WEIGHT_MB     2048u
#define MODEL_MEM_WORKSPACE_MB  256u
#elif defined(PLATFORM_RASPI5)
#define MODEL_MEM_WEIGHT_MB     512u
#define MODEL_MEM_WORKSPACE_MB  256u
#else /* PLATFORM_QEMU_VIRT, PLATFORM_X86_64, host harness */
#define MODEL_MEM_WEIGHT_MB     256u
#define MODEL_MEM_WORKSPACE_MB  128u
#endif

/* `_Static_assert` works in both C11+ and C23 without `<assert.h>` —
 * config.h is also pulled in by the bundled Lua build (`lua_stubs.c`),
 * which compiles with a pre-C23 standard, so the bare `static_assert`
 * spelling is not portable here. */
_Static_assert((MODEL_MEM_WEIGHT_MB    % 2u) == 0u,
               "MODEL_MEM_WEIGHT_MB must be a multiple of 2 (pool block = 2 MB)");
_Static_assert((MODEL_MEM_WORKSPACE_MB % 2u) == 0u,
               "MODEL_MEM_WORKSPACE_MB must be a multiple of 2 (pool block = 2 MB)");

#endif /* CONFIG_H */
