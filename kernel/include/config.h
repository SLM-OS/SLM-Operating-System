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

/* 256 KB per stack — needed for SLM forward (Qwen2.5-1.5B) on top
 * of ONNX parsing. Worst-case PMM cost is `MAX_TASKS * STACK_SIZE`
 * = 64 * 256 KB = 16 MB; comfortably within budget on every shipping
 * platform (8 GB Jetson, 8 GB Pi 5, 16 GB x86-64, 1 GB QEMU virt).
 * Re-check this if MAX_TASKS grows. */
#define STACK_SIZE          0x40000UL

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
 * **Hard ceiling: 1024 MB per pool.** `model_mem_init` asks PMM for a
 * single contiguous block per pool; the buddy allocator's max order
 * is 18 (1 GiB, see `kernel/CLAUDE.md` §"Buddy Allocator"). A request
 * larger than 1 GiB returns `PMM_ERR_OUT_OF_RANGE` and leaves the pool
 * uninitialized — `slm load` via the M5.3.3 PMM-bypass path still
 * works, but `mm::alloc_weights` callers (eviction integration,
 * future shared-weight refcounting) silently no-op. See #578 for the
 * regression history and #550 for the multi-block follow-up that
 * lifts the ceiling.
 *
 * Jetson sizes the weight pool to the buddy ceiling so Qwen2.5-1.5B-
 * Q4_K_M (~1.0 GB resident) fits in a single block; workspace covers
 * per-layer activations and the 512 MB KV-cache sub-pool that M5
 * carves out (see docs/specs/slm-integration.md "Memory Plan").
 *
 * Pi 5 hosts smaller vision-class models. QEMU and x86-64 keep the
 * original Phase-5 defaults so the test kernel boots inside
 * `make test`'s 1 GB systemd MemoryMax cap.
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define MODEL_MEM_WEIGHT_MB     1024u
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
/* The 1024 MB cap below is two ceilings stacked:
 *   1. PMM buddy max-order (18 = 1 GiB, see kernel/CLAUDE.md
 *      §"Buddy Allocator") rejects single-block allocations larger
 *      than 1 GiB.
 *   2. The Rust pool's `MAX_BLOCKS_PER_POOL = 512` in
 *      `runtime/src/mm/model_mem.rs` makes 512 × 2 MB = 1 GiB the
 *      effective per-pool cap. `model_mem_init` returns
 *      `AllocError::Oversized` if either request exceeds this.
 * Bumping this assert above 1024 requires bumping BOTH ceilings —
 * #550 tracks the multi-block PMM follow-up. */
_Static_assert(MODEL_MEM_WEIGHT_MB    <= 1024u,
               "MODEL_MEM_WEIGHT_MB cannot exceed 1024 (PMM buddy + MAX_BLOCKS_PER_POOL ceiling; see #578)");
_Static_assert(MODEL_MEM_WORKSPACE_MB <= 1024u,
               "MODEL_MEM_WORKSPACE_MB cannot exceed 1024 (PMM buddy + MAX_BLOCKS_PER_POOL ceiling; see #578)");

/* ============================================================================
 * Rust heap — sized by SLM working-set demand
 * ============================================================================
 *
 * The Rust runtime's `linked_list_allocator` lives entirely inside the
 * region passed to `rust_heap_init`. `Vec`/`Box` allocations from the
 * SLM forward path land here, plus the msg_router publish queue and
 * the component runtime. KV cache (the dominant consumer) =
 *   2 (k+v) × n_layers × n_kv_heads × head_dim × ctx × 2 B (FP16):
 *
 *   Model           |  ctx=1K  |  ctx=4K  |  ctx=8K  | + scratch
 *   ----------------|----------|----------|----------|----------
 *   SmolLM2-135M    |    22 MB |    90 MB |   180 MB | +   7 MB
 *   Qwen2.5-1.5B    |    28 MB |   112 MB |   224 MB | +  23 MB
 *   Qwen2.5-3B      |    36 MB |   144 MB |   288 MB | +  24 MB
 *   Qwen2.5-7B      |    58 MB |   230 MB |   470 MB | +  24 MB
 *   Llama-3.1-8B    |   128 MB |   512 MB |  1024 MB | +  16 MB
 *   Qwen2.5-14B     |   192 MB |   768 MB |  1536 MB | +  24 MB
 *
 * Jetson sized to comfortably hold:
 *   - Two concurrent Qwen2.5-1.5B sessions at ctx=8K (~500 MB)
 *   - One Qwen2.5-7B at ctx=4K (~250 MB) once the 1 GiB model_mem /
 *     ramdisk PMM cap is lifted (separate work; see #550).
 *   - Headroom for fragmentation under msg_router publish churn.
 *     Telemetry pumps publish on every net_pump tick; without
 *     enough heap room those small allocs interleave with mid-
 *     sized matmul scratch and starve the inference path. Tracked
 *     as a longer-term slab-allocator follow-up.
 *
 * Pi 5 carries 1/4 the heap (one Qwen2.5-1.5B at ctx=4K). QEMU and
 * x86-64 stay small because their tests only use a 256-element
 * synthetic fixture.
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define RUST_HEAP_MB            1024u
#elif defined(PLATFORM_RASPI5)
#define RUST_HEAP_MB            256u
#else /* PLATFORM_QEMU_VIRT, PLATFORM_X86_64, host harness */
#define RUST_HEAP_MB            4u
#endif

_Static_assert(RUST_HEAP_MB > 0u, "RUST_HEAP_MB must be positive");
/* 4 GiB upper bound mirrors the realistic per-platform RAM ceiling
 * (Jetson 8 GB, Pi 5 8 GB, x86-64 16 GB) — anything larger almost
 * certainly indicates a typo (e.g. RUST_HEAP_MB 12800 instead of
 * 128) and would also overflow the `unsigned int` arithmetic in
 * `kernel/src/main.c::pmm_alloc_pages` callers if the size_t casts
 * were ever stripped. */
_Static_assert(RUST_HEAP_MB <= 4096u, "RUST_HEAP_MB cannot exceed 4096 (4 GiB sanity ceiling)");

/* ============================================================================
 * RAM disk — sized to hold staged model files
 * ============================================================================
 *
 * /mnt/files is backed by `ramdisk_create_default` (kernel/drivers/ramdisk.c)
 * at 4 KB blocks. The driver does a single `pmm_alloc_pages(data_pages)`
 * call, so the cap is the PMM buddy max-order: 2 GiB on this kernel
 * (`PMM_MAX_ORDER = 19`, bumped 2026-05-02 from 18 to fit a
 * Q4_K_M GGUF for Qwen2.5-1.5B which is 1.04 GB on disk).
 *
 * Jetson sizes to 64 MB — `slm xload` streams the GGUF straight
 * into a PMM buffer (no LittleFS intermediate), so the ramdisk only
 * needs to hold the boot-seeded demo scripts, preload.conf, and the
 * help tree (~70 KB total). 64 MB leaves plenty of headroom for
 * future small blobs without claiming any of the order-18+ buddy
 * blocks the SLM load buffer needs. The previous 1280 MB sizing
 * (kept for the LittleFS-based `slm load` path) ate one of the
 * 8 GB system's order-19-aligned buddies, which combined with the
 * 1 GB Rust heap left no contiguous order-19 free for a Q4_K_M
 * Qwen2.5-1.5B GGUF (1.04 GB → rounds up to order 19 in the buddy
 * allocator).
 *
 * Pi 5 keeps the original 32 MB cap (Hailo HEFs are ~20 MB; SLM on
 * Pi 5 is not the demo target). QEMU and host-harness builds must
 * stay small enough to boot inside `make test`'s 1 GB systemd
 * MemoryMax cap.
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
#define RAMDISK_DEFAULT_MB      64u
#elif defined(PLATFORM_RASPI5)
#define RAMDISK_DEFAULT_MB      32u
#else /* PLATFORM_QEMU_VIRT, PLATFORM_X86_64, host harness */
#define RAMDISK_DEFAULT_MB      32u
#endif

_Static_assert(RAMDISK_DEFAULT_MB > 0u, "RAMDISK_DEFAULT_MB must be positive");
/* 2 GiB hard cap — `ramdisk_create` does a single `pmm_alloc_pages`
 * call. The PMM buddy max-order (`PMM_MAX_ORDER` in pmm.h) is the
 * ceiling. A multi-chunk ramdisk driver would lift this; deferred
 * until a workload actually needs > 2 GiB of staging. */
_Static_assert(RAMDISK_DEFAULT_MB <= 2048u, "RAMDISK_DEFAULT_MB cannot exceed 2048 (PMM buddy max-order = 2 GiB)");

#endif /* CONFIG_H */
