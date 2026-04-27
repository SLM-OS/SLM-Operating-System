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
