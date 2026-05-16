/*
 * sched_trace_ai.c — AI-scheduler decision-trace ring buffer (#880).
 *
 * Design: per-CPU ring buffer of fixed-size 480-byte records. Each
 * CPU writes only its own slots; no cross-CPU lock or atomic
 * fetch-add on the hot path. The owning CPU cache_cleans every
 * write so the dump path (typically on CPU 0) sees up-to-date data
 * after cache_invalidate. This mirrors the sched_diag NC-counter
 * pattern called out in `kernel/CLAUDE.md`.
 *
 * Per-CPU vs single-global was chosen because Pi 5 / Jetson have
 * incoherent per-core L2 caches (no SMPEN) — a single global head
 * index updated via atomic fetch-add would either need explicit DC
 * CIVAC/DSB SY around every increment (expensive) or NC memory for
 * the head (the run-queue NC region is small and the ring buffer
 * itself can't fit there). Per-CPU rings sidestep both problems.
 *
 * Dump format documented in `docs/sched-trace-format.md`. The
 * sibling-repo ingester (#879 / 61c) parses the same bytes.
 */

#if defined(CONFIG_AI_SCHEDULER)

#include "sched_trace_ai.h"
#include "ai_types.h"
#include "ai_state.h"
#include "task.h"
#include "smp.h"
#include "cache.h"
#include "pmm.h"
#include "spinlock.h"   /* irq_save / irq_restore */
#include "string.h"
#include "config.h"
#include "vfs.h"
#include "littlefs_slm.h"
#include "shell.h"
#include "shell_internal.h"

extern uint64_t slm_get_time_ns(void);

/* Sanity-check the state dimension constant matches the policy
 * runtime's expectation. If a future change desyncs them the
 * compile breaks here rather than producing silently misshapen
 * trace records. */
_Static_assert(SCHED_TRACE_AI_STATE_DIM == AI_STATE_DIM,
               "trace state_dim must match AI_STATE_DIM");

/* Per-CPU ring length. Total ring = MAX_CPUS * PERCPU_ENTRIES. With
 * 8 CPUs × 512 entries × 480 B = ~1.92 MB. */
#define PERCPU_ENTRIES  (SCHED_TRACE_AI_RING_ENTRIES / MAX_CPUS)

_Static_assert(PERCPU_ENTRIES * MAX_CPUS == SCHED_TRACE_AI_RING_ENTRIES,
               "ring entries must divide evenly across MAX_CPUS");

struct sched_trace_ai_percpu {
    /* Monotonic count of records written by this CPU. Slot index
     * derives from `total_written % PERCPU_ENTRIES`; readers compute
     * the same value rather than reading a separately-stored `head`
     * field (eliminates a cross-CPU write-order race where a reader
     * could observe total_written++ before head was updated). */
    volatile uint64_t total_written;
    /* Cacheline padding so adjacent CPUs' percpu structs don't
     * false-share the counter. */
    uint8_t  _hdrpad[64 - 8];
    struct sched_trace_ai_record ring[PERCPU_ENTRIES];
};

_Static_assert(offsetof(struct sched_trace_ai_percpu, ring) == 64,
               "ring must start on the second cacheline of the percpu struct");

static struct sched_trace_ai_percpu *tracer_percpu;  /* MAX_CPUS-element array */
static volatile uint32_t tracer_enabled;             /* 0 / 1 atomic flag */
static volatile uint32_t tracer_initialized;

/* ---- Initialization ----
 *
 * Called from `sched_ai_init()` (boot, CPU 0) and from
 * `sched_trace_ai_start()` (shell, CPU 0). The check-then-allocate
 * below is intentionally NOT atomic — both call sites run on CPU 0
 * in the shell/init task. A future caller from a non-CPU-0 path
 * (e.g. a Lua/IPC binding scheduled onto a secondary) must add an
 * external lock or replace the body with a compare-and-swap. */
int sched_trace_ai_init(void)
{
    if (tracer_initialized) {
        return 0;
    }
    size_t total = sizeof(struct sched_trace_ai_percpu) * MAX_CPUS;
    size_t pages = (total + 4095u) / 4096u;
    tracer_percpu = pmm_alloc_pages(pages);
    if (!tracer_percpu) {
        return -1;
    }
    memset(tracer_percpu, 0, total);
    /* Initial cache_clean so secondary CPUs reading their own counter
     * see 0 rather than uninitialized garbage. */
    cache_clean_range(tracer_percpu, total);
    tracer_initialized = 1;
    return 0;
}

/* ---- Control plane ---- */

/* Zero every per-CPU ring + counter. NOT synchronized against
 * concurrent hot-path writers — the caller must pause writes (via
 * the `tracer_enabled` atomic) before invoking. `sched_trace_ai_start`
 * and `sched_trace_ai_clear` both follow this contract. */
static void clear_rings_unsynchronized(void)
{
    if (!tracer_initialized || !tracer_percpu) return;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        tracer_percpu[i].total_written = 0;
        /* Zero the ring so a dump of a freshly-cleared trace
         * doesn't leak prior recording's contents. */
        memset(tracer_percpu[i].ring, 0, sizeof(tracer_percpu[i].ring));
    }
    cache_clean_range(tracer_percpu,
                      sizeof(struct sched_trace_ai_percpu) * MAX_CPUS);
}

void sched_trace_ai_start(void)
{
    if (!tracer_initialized && sched_trace_ai_init() != 0) {
        return;
    }
    /* Disable first so the clear below isn't racing concurrent
     * hot-path writers on other CPUs. A narrow residual window still
     * exists: a writer that already passed the `tracer_enabled`
     * load in record_decision/completion can be mid-record when the
     * atomic store flips the flag to 0. That window is bounded by
     * the IRQ-disabled critical section length (~hundreds of cycles),
     * so at most one partially-zeroed record per CPU can survive the
     * clear — acceptable for a trace ring whose ingester tolerates
     * occasional dropped records. */
    __atomic_store_n(&tracer_enabled, 0u, __ATOMIC_RELEASE);
    clear_rings_unsynchronized();
    __atomic_store_n(&tracer_enabled, 1u, __ATOMIC_RELEASE);
}

void sched_trace_ai_stop(void)
{
    __atomic_store_n(&tracer_enabled, 0u, __ATOMIC_RELEASE);
}

void sched_trace_ai_clear(void)
{
    /* Briefly suspend writes so we don't race the clear. Restored
     * to caller's prior state on exit. Same residual-window caveat
     * as `sched_trace_ai_start` applies. */
    uint32_t prev = __atomic_exchange_n(&tracer_enabled, 0u, __ATOMIC_ACQ_REL);
    clear_rings_unsynchronized();
    __atomic_store_n(&tracer_enabled, prev, __ATOMIC_RELEASE);
}

bool sched_trace_ai_is_enabled(void)
{
    return __atomic_load_n(&tracer_enabled, __ATOMIC_ACQUIRE) != 0;
}

static uint64_t total_written_sum(void)
{
    if (!tracer_initialized || !tracer_percpu) return 0;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cache_invalidate_range(&tracer_percpu[i],
                               offsetof(struct sched_trace_ai_percpu, ring));
        sum += tracer_percpu[i].total_written;
    }
    return sum;
}

uint32_t sched_trace_ai_records_used(void)
{
    if (!tracer_initialized || !tracer_percpu) return 0;
    uint32_t used = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cache_invalidate_range(&tracer_percpu[i],
                               offsetof(struct sched_trace_ai_percpu, ring));
        uint64_t w = tracer_percpu[i].total_written;
        used += (w > PERCPU_ENTRIES) ? PERCPU_ENTRIES : (uint32_t)w;
    }
    return used;
}

uint64_t sched_trace_ai_total_events(void)
{
    return total_written_sum();
}

uint64_t sched_trace_ai_dropped(void)
{
    if (!tracer_initialized || !tracer_percpu) return 0;
    uint64_t dropped = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cache_invalidate_range(&tracer_percpu[i],
                               offsetof(struct sched_trace_ai_percpu, ring));
        uint64_t w = tracer_percpu[i].total_written;
        if (w > PERCPU_ENTRIES) {
            dropped += (w - PERCPU_ENTRIES);
        }
    }
    return dropped;
}

/* ---- Hot path ---- */

/* Copy `src` into `dst[0..max-1]`, zero-padding the remainder.
 * Always null-terminates. Used for the fixed 16-byte policy name
 * field so the on-disk record never has trailing garbage. */
static void copy_policy_name(char *dst, size_t max, const char *src)
{
    if (max == 0) return;
    memset(dst, 0, max);
    if (!src) return;
    size_t i = 0;
    while (i < max - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    /* Last byte already zero from memset — guarantees null term. */
}

void sched_trace_ai_record_decision(struct task *task,
                                    const char *policy_name,
                                    uint32_t action_core)
{
    if (!__atomic_load_n(&tracer_enabled, __ATOMIC_ACQUIRE)) return;
    if (!tracer_percpu) return;

    /* IRQ-disable for the per-CPU critical section. Cheap (single
     * DAIF write on ARM64; STI/CLI pair on x86) and clearly correct:
     * keeps a timer IRQ that re-enters the scheduler path from
     * overwriting the slot we just claimed before `total_written++`
     * commits. The hook itself doesn't yield. */
    irq_flags_t irq_flags = irq_save();

    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) {  /* defensive */
        irq_restore(irq_flags);
        return;
    }

    struct sched_trace_ai_percpu *pc = &tracer_percpu[cpu];
    uint32_t slot = (uint32_t)(pc->total_written % PERCPU_ENTRIES);
    struct sched_trace_ai_record *r = &pc->ring[slot];

    /* Fill the header. */
    r->kind = (uint8_t)SCHED_TRACE_AI_KIND_DECISION;
    r->cpu_recorded = (uint8_t)cpu;
    r->_hdr_pad = 0;
    r->task_id = task ? task->id : 0u;
    r->timestamp_ns = slm_get_time_ns();

    /* Decision body. */
    copy_policy_name(r->u.decision.policy_name,
                     sizeof(r->u.decision.policy_name), policy_name);
    r->u.decision.action_core = (int32_t)action_core;
    r->u.decision.action_priority = task ? (int32_t)task->effective_priority : -1;
    r->u.decision.action_preempt = 0;  /* reserved */
    r->u.decision._decision_pad = 0;

    /* State vector — full 108-dim FP32 snapshot. ai_extract_state
     * reads global scheduler state; it doesn't touch the running
     * task. Stage in an aligned local buffer then memcpy: the
     * record is `__attribute__((packed))` so taking the address of
     * the embedded state[] array yields an "unaligned pointer"
     * warning, but a memcpy from a properly-aligned source is fine. */
    float state_buf[AI_STATE_DIM] __attribute__((aligned(8)));
    ai_extract_state(state_buf);
    memcpy(r->u.decision.state, state_buf, sizeof(state_buf));

    /* Push the record to PoC so dump (on a possibly different CPU)
     * sees it without depending on an L2 coherency event. */
    cache_clean_range(r, sizeof(*r));

    /* Bump the per-CPU counter and push it. Slot index is derived
     * from `total_written % PERCPU_ENTRIES` everywhere it's needed
     * (both writer and dump reader) — no separately-stored `head`
     * field, no cross-CPU write-order race between two counter
     * stores. */
    pc->total_written++;
    cache_clean_range(pc,
                      offsetof(struct sched_trace_ai_percpu, ring));

    irq_restore(irq_flags);
}

void sched_trace_ai_record_completion(struct task *task)
{
    if (!__atomic_load_n(&tracer_enabled, __ATOMIC_ACQUIRE)) return;
    if (!tracer_percpu || !task) return;

    irq_flags_t irq_flags = irq_save();

    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) {
        irq_restore(irq_flags);
        return;
    }

    struct sched_trace_ai_percpu *pc = &tracer_percpu[cpu];
    uint32_t slot = (uint32_t)(pc->total_written % PERCPU_ENTRIES);
    struct sched_trace_ai_record *r = &pc->ring[slot];

    r->kind = (uint8_t)SCHED_TRACE_AI_KIND_COMPLETION;
    r->cpu_recorded = (uint8_t)cpu;
    r->_hdr_pad = 0;
    r->task_id = task->id;
    r->timestamp_ns = slm_get_time_ns();

    uint64_t dispatch_ns = 0;  /* not currently tracked per-task; left 0 */
    uint64_t deadline_ns = task->deadline_ns;
    uint64_t completion_ns = r->timestamp_ns;
    r->u.completion.dispatch_ns = dispatch_ns;
    r->u.completion.completion_ns = completion_ns;
    r->u.completion.deadline_ns = deadline_ns;
    r->u.completion.latency_to_complete_us =
        (dispatch_ns != 0 && completion_ns > dispatch_ns)
            ? (uint32_t)((completion_ns - dispatch_ns) / 1000ULL)
            : 0u;
    r->u.completion.ran_on_cpu = (uint8_t)task->assigned_cpu;
    r->u.completion.deadline_met =
        (deadline_ns == 0 || completion_ns <= deadline_ns) ? 1u : 0u;
    r->u.completion._completion_pad[0] = 0;
    r->u.completion._completion_pad[1] = 0;
    memset(r->u.completion._tail_pad, 0, sizeof(r->u.completion._tail_pad));

    cache_clean_range(r, sizeof(*r));
    pc->total_written++;
    cache_clean_range(pc,
                      offsetof(struct sched_trace_ai_percpu, ring));

    irq_restore(irq_flags);
}

/* ---- Dump ---- */

size_t sched_trace_ai_dump_size(void)
{
    uint32_t used = sched_trace_ai_records_used();
    return sizeof(struct sched_trace_ai_file_header) +
           (size_t)used * SCHED_TRACE_AI_RECORD_SIZE;
}

/* Snapshot each CPU's (total_written, head) tuple into a local
 * array. Used by the dump path so the file header and the records
 * emitted derive from the same observation point — without the
 * snapshot, a hot-path write that lands between header computation
 * and the record-iteration loop produces a file whose
 * `record_count` disagrees with the byte stream. */
struct trace_snapshot {
    uint64_t total;
    uint32_t count;
    uint32_t start;
};

static void take_snapshot(struct trace_snapshot snap[MAX_CPUS],
                          uint32_t *used_out,
                          uint64_t *total_out,
                          uint64_t *dropped_out)
{
    uint32_t used = 0;
    uint64_t total = 0;
    uint64_t dropped = 0;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        cache_invalidate_range(&tracer_percpu[i],
                               offsetof(struct sched_trace_ai_percpu, ring));
        uint64_t w = tracer_percpu[i].total_written;
        snap[i].total = w;
        snap[i].count = (w > PERCPU_ENTRIES) ? PERCPU_ENTRIES : (uint32_t)w;
        /* Oldest record sits at the next-write slot, which is
         * (total_written % PERCPU_ENTRIES) for a wrapped ring. Derive
         * here rather than reading a separately-stored head field —
         * keeps the writer's two counter updates collapsed into a
         * single monotonic counter and removes the cross-CPU write-
         * order race that would otherwise put `start` one slot
         * behind `total`. */
        snap[i].start = (w > PERCPU_ENTRIES)
            ? (uint32_t)(w % PERCPU_ENTRIES) : 0u;
        used += snap[i].count;
        total += w;
        if (w > PERCPU_ENTRIES) dropped += (w - PERCPU_ENTRIES);
    }
    *used_out = used;
    *total_out = total;
    *dropped_out = dropped;
}

size_t sched_trace_ai_dump_stream(sched_trace_ai_dump_cb cb, void *ctx)
{
    if (!cb || !tracer_initialized || !tracer_percpu) return 0;

    struct trace_snapshot snap[MAX_CPUS];
    uint32_t used;
    uint64_t total;
    uint64_t dropped;
    take_snapshot(snap, &used, &total, &dropped);

    struct sched_trace_ai_file_header hdr = {
        .magic = SCHED_TRACE_AI_MAGIC,
        .version = (uint16_t)SCHED_TRACE_AI_VERSION,
        .record_size = (uint16_t)SCHED_TRACE_AI_RECORD_SIZE,
        .record_count = used,
        .state_dim = SCHED_TRACE_AI_STATE_DIM,
        .total_events_since_start = total,
        .dropped_events = dropped,
    };
    if (cb(ctx, &hdr, sizeof(hdr)) < 0) return 0;
    size_t emitted = sizeof(hdr);

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (snap[i].count == 0) continue;
        /* Re-invalidate the ring portion only; the header was
         * already invalidated by take_snapshot. */
        cache_invalidate_range(tracer_percpu[i].ring,
                               sizeof(tracer_percpu[i].ring));
        for (uint32_t k = 0; k < snap[i].count; k++) {
            uint32_t idx = (snap[i].start + k) % PERCPU_ENTRIES;
            if (cb(ctx, &tracer_percpu[i].ring[idx],
                   SCHED_TRACE_AI_RECORD_SIZE) < 0) {
                return 0;
            }
            emitted += SCHED_TRACE_AI_RECORD_SIZE;
        }
    }
    return emitted;
}

size_t sched_trace_ai_dump_to_buf(uint8_t *buf, size_t buflen)
{
    if (!buf || !tracer_initialized || !tracer_percpu) return 0;

    /* Snapshot first so the header's record_count and the iterated
     * bytes derive from the same observation. Without this, a
     * tight `buflen == dump_size()` caller would overflow when a
     * concurrent hot-path write advances total_written between the
     * size check and the loop. */
    struct trace_snapshot snap[MAX_CPUS];
    uint32_t used;
    uint64_t total;
    uint64_t dropped;
    take_snapshot(snap, &used, &total, &dropped);

    size_t need = sizeof(struct sched_trace_ai_file_header) +
                  (size_t)used * SCHED_TRACE_AI_RECORD_SIZE;
    if (buflen < need) return 0;

    struct sched_trace_ai_file_header hdr = {
        .magic = SCHED_TRACE_AI_MAGIC,
        .version = (uint16_t)SCHED_TRACE_AI_VERSION,
        .record_size = (uint16_t)SCHED_TRACE_AI_RECORD_SIZE,
        .record_count = used,
        .state_dim = SCHED_TRACE_AI_STATE_DIM,
        .total_events_since_start = total,
        .dropped_events = dropped,
    };
    memcpy(buf, &hdr, sizeof(hdr));

    /* Walk each per-CPU ring in oldest→newest order using snapshot
     * counts. Records from different CPUs are interleaved by
     * insertion order within each CPU; the sibling-repo ingester
     * merge-sorts by timestamp_ns if a strict global order is needed. */
    uint8_t *out = buf + sizeof(hdr);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (snap[i].count == 0) continue;
        cache_invalidate_range(tracer_percpu[i].ring,
                               sizeof(tracer_percpu[i].ring));
        for (uint32_t k = 0; k < snap[i].count; k++) {
            uint32_t idx = (snap[i].start + k) % PERCPU_ENTRIES;
            memcpy(out, &tracer_percpu[i].ring[idx],
                   SCHED_TRACE_AI_RECORD_SIZE);
            out += SCHED_TRACE_AI_RECORD_SIZE;
        }
    }
    return (size_t)(out - buf);
}

/* ---- Shell CLI ----
 *
 * Lives here (vs in shell_sys.c) so the file-write dependency on
 * LittleFS / VFS is co-located with the trace API rather than
 * scattered across the shell. Called from shell_sys.c via extern.
 * (VFS / LittleFS / shell headers are pulled in at the top of this
 * translation unit.) */

struct aitrace_dump_ctx {
    struct lfs_mount *mnt;
    int fd;
    int err;
    size_t bytes_written;
};

static int aitrace_dump_cb(void *vctx, const void *bytes, size_t len)
{
    struct aitrace_dump_ctx *ctx = (struct aitrace_dump_ctx *)vctx;
    if (ctx->err) return ctx->err;
    int rc = littlefs_file_write(ctx->mnt, ctx->fd, bytes, len);
    if (rc < 0) {
        ctx->err = rc;
        return rc;
    }
    if ((size_t)rc != len) {
        ctx->err = -1;
        return -1;
    }
    ctx->bytes_written += len;
    return 0;
}

size_t sched_trace_ai_dump_to_path(const char *path)
{
    if (!path) return 0;
    char resolved[256];
    if (shell_resolve_path(path, resolved, sizeof(resolved)) < 0) {
        return 0;
    }
    const char *subpath = 0;
    struct lfs_mount *mnt = vfs_get_mount_ctx(resolved, &subpath);
    if (!mnt) return 0;
    int fd = littlefs_file_open(mnt, subpath,
                                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
    if (fd < 0) return 0;
    struct aitrace_dump_ctx ctx = { .mnt = mnt, .fd = fd,
                                     .err = 0, .bytes_written = 0 };
    size_t total = sched_trace_ai_dump_stream(aitrace_dump_cb, &ctx);
    littlefs_file_close(mnt, fd);
    if (ctx.err) return 0;
    return total;
}

int sched_aitrace_cli(int argc, char *argv[])
{
    const char *sub = (argc >= 3) ? argv[2] : "stats";

    if (strcmp(sub, "start") == 0) {
        sched_trace_ai_start();
        shell_printf("AI decision trace started (%u slots, %u B/record, "
                     "ring = %u KB total)\r\n",
                     (unsigned)SCHED_TRACE_AI_RING_ENTRIES,
                     (unsigned)SCHED_TRACE_AI_RECORD_SIZE,
                     (unsigned)((SCHED_TRACE_AI_RING_ENTRIES *
                                 SCHED_TRACE_AI_RECORD_SIZE) / 1024u));
        return 0;
    }
    if (strcmp(sub, "stop") == 0) {
        sched_trace_ai_stop();
        shell_printf("AI decision trace stopped (%u records used, "
                     "%lu events total, %lu dropped)\r\n",
                     (unsigned)sched_trace_ai_records_used(),
                     (unsigned long)sched_trace_ai_total_events(),
                     (unsigned long)sched_trace_ai_dropped());
        return 0;
    }
    if (strcmp(sub, "clear") == 0) {
        sched_trace_ai_clear();
        shell_puts("AI decision trace cleared\r\n");
        return 0;
    }
    if (strcmp(sub, "stats") == 0) {
        shell_printf("AI decision trace: %s\r\n",
                     sched_trace_ai_is_enabled() ? "ON" : "OFF");
        shell_printf("  records used   : %u / %u\r\n",
                     (unsigned)sched_trace_ai_records_used(),
                     (unsigned)SCHED_TRACE_AI_RING_ENTRIES);
        shell_printf("  total events   : %lu\r\n",
                     (unsigned long)sched_trace_ai_total_events());
        shell_printf("  dropped events : %lu\r\n",
                     (unsigned long)sched_trace_ai_dropped());
        shell_printf("  dump size      : %lu bytes\r\n",
                     (unsigned long)sched_trace_ai_dump_size());
        return 0;
    }
    if (strcmp(sub, "dump") == 0) {
        if (argc < 4) {
            shell_puts("Usage: sched aitrace dump <path>\r\n");
            shell_puts("  e.g. sched aitrace dump /mnt/files/sched_trace.bin\r\n");
            return 1;
        }
        size_t total = sched_trace_ai_dump_to_path(argv[3]);
        if (total == 0) {
            shell_printf("aitrace: dump to %s failed (bad path, "
                         "unmounted FS, or write error)\r\n", argv[3]);
            return 1;
        }
        shell_printf("aitrace: dumped %lu bytes to %s\r\n",
                     (unsigned long)total, argv[3]);
        return 0;
    }
    shell_puts("Usage: sched aitrace [start|stop|stats|clear|dump <path>]\r\n");
    return 1;
}

#endif /* CONFIG_AI_SCHEDULER */
