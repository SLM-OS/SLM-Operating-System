/*
 * sched_trace.c - Scheduler trace buffer implementation (#195)
 *
 * Single-producer-per-CPU circular buffer. The buffer head is advanced
 * with a compare-exchange so concurrent producers on different CPUs
 * serialize on the head slot without a spinlock. Tracing overhead when
 * enabled is ~one atomic FAA plus four stores per event; when disabled
 * it's a single relaxed load and a branch.
 *
 * No allocation: the storage is a static array sized at compile time.
 */

#include "sched_trace.h"
#include "task.h"
#include "slm_ffi.h"
#include <stdatomic.h>
#include <string.h>

/* ---------- State ------------------------------------------------ */

static atomic_bool g_trace_enabled = false;
static atomic_uint_fast64_t g_trace_total_events;

/* Monotonically-advancing head. `head % CAPACITY` is the write slot.
 * Using a wrap-friendly 64-bit counter makes the readout trivially
 * correct: the last N events are indexed by the head value modulo
 * capacity. */
static atomic_uint_fast64_t g_trace_head;

static struct sched_trace_record g_trace_buf[SCHED_TRACE_CAPACITY];

/* ---------- Enable / disable ------------------------------------- */

void sched_trace_start(void)
{
    /* Clear first, then flip the flag. New events record into a fresh
     * buffer and the total-events counter resets so the "dropped"
     * math in the shell output reflects only the new session. */
    sched_trace_clear();
    atomic_store_explicit(&g_trace_enabled, true, memory_order_release);
}

void sched_trace_stop(void)
{
    atomic_store_explicit(&g_trace_enabled, false, memory_order_release);
}

void sched_trace_clear(void)
{
    atomic_store_explicit(&g_trace_head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_trace_total_events, 0, memory_order_relaxed);
}

bool sched_trace_is_enabled(void)
{
    return atomic_load_explicit(&g_trace_enabled, memory_order_acquire);
}

uint64_t sched_trace_total_events(void)
{
    return atomic_load_explicit(&g_trace_total_events, memory_order_relaxed);
}

/* ---------- Record hooks ----------------------------------------- */

static inline uint16_t task_id_or_none(struct task *t)
{
    if (!t) return (uint16_t)0xFFFF;
    uint32_t id = t->id;
    return (uint16_t)(id > 0xFFFE ? 0xFFFE : id);
}

static void trace_push(struct sched_trace_record *ev)
{
    /* Atomically reserve a slot. The 64-bit head wraps in ~58 years
     * at 10M events/sec so the monotonic assumption holds. */
    uint64_t slot = atomic_fetch_add_explicit(&g_trace_head, 1,
                                              memory_order_acq_rel);
    uint32_t idx = (uint32_t)(slot % SCHED_TRACE_CAPACITY);
    g_trace_buf[idx] = *ev;
    /* Ensure the event data is globally visible before a consumer on
     * another CPU observes the incremented head via an acquire load.
     * Without this fence the plain store above can be reordered past
     * the total_events bump on weakly-ordered cores (Pi 5 / Jetson). */
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&g_trace_total_events, 1,
                              memory_order_relaxed);
}

void sched_trace_record_switch(uint32_t cpu, struct task *prev,
                                struct task *next)
{
    if (!atomic_load_explicit(&g_trace_enabled, memory_order_acquire)) {
        return;
    }
    struct sched_trace_record ev = {
        .timestamp_ns = slm_get_time_ns(),
        .prev_task_id = task_id_or_none(prev),
        .next_task_id = task_id_or_none(next),
        .cpu = (uint8_t)cpu,
        .prev_cpu = (uint8_t)cpu,
        .event = (uint8_t)SCHED_TRACE_SCHED,
        ._pad = 0,
    };
    trace_push(&ev);
}

void sched_trace_record_migrate(struct task *task, uint32_t from_cpu,
                                 uint32_t to_cpu)
{
    if (!atomic_load_explicit(&g_trace_enabled, memory_order_acquire)) {
        return;
    }
    struct sched_trace_record ev = {
        .timestamp_ns = slm_get_time_ns(),
        .prev_task_id = task_id_or_none(task),
        .next_task_id = task_id_or_none(task),
        .cpu = (uint8_t)to_cpu,
        .prev_cpu = (uint8_t)from_cpu,
        .event = (uint8_t)SCHED_TRACE_MIGRATE,
        ._pad = 0,
    };
    trace_push(&ev);
}

/* ---------- Snapshot --------------------------------------------- */

uint32_t sched_trace_snapshot(struct sched_trace_record *out, uint32_t max)
{
    if (!out || max == 0) return 0;

    uint64_t head = atomic_load_explicit(&g_trace_total_events, memory_order_acquire);
    uint64_t available = head < SCHED_TRACE_CAPACITY ? head : SCHED_TRACE_CAPACITY;
    uint64_t to_copy = available < max ? available : max;

    /* Copy the oldest `to_copy` retained events, in order. The oldest
     * retained slot is (head - available), which may wrap; iterate
     * from there modulo capacity. */
    uint64_t start = head - available;
    for (uint32_t i = 0; i < (uint32_t)to_copy; i++) {
        uint32_t idx = (uint32_t)((start + i) % SCHED_TRACE_CAPACITY);
        out[i] = g_trace_buf[idx];
    }
    return (uint32_t)to_copy;
}
