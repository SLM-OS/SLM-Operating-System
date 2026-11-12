/*
 * slot_trace.h - Per-CPU task-slot lifecycle trace (#1213)
 *
 * Debug-only. Built with `make ... SLOT_TRACE=ON` (CMake -DSLOT_TRACE=ON,
 * which defines CONFIG_SLOT_TRACE). With the option off every macro below
 * expands to nothing and slot_trace.c compiles to an empty unit, so the
 * kernel's .text is unchanged.
 *
 * What it records, per task_table slot: task_create (slot claimed, context
 * written), task_destroy (entry and the generation bump), task_exit,
 * scheduler_terminate_task, run-queue inserts, zombie set and reap,
 * switch_to entry (outgoing and incoming slot, generation, the incoming
 * context's saved sp/x19 as read just before the switch), switch_to return,
 * a task's first instruction in C (task_entry_trampoline), and test markers.
 * Every entry carries the CPU, CNTVCT, a global sequence number (one relaxed
 * atomic increment, for a total order across CPUs) and the four run-queue
 * lock holders at the time of the record.
 *
 * Writers: ring c is written only by CPU c, with local IRQs masked for the
 * few stores of one entry. No locks and no prints on the record path.
 *
 * Dump: the first EL1 synchronous exception (including the bad-SP case,
 * which the el1_sync vector moves onto a per-CPU emergency stack before
 * anything touches memory) or the first panic freezes every ring and prints
 * all of them, then exits QEMU through semihosting.
 */

#ifndef SLOT_TRACE_H
#define SLOT_TRACE_H

#include <stdint.h>

/* Event codes. */
#define ST_CREATE_ALLOC     1   /* a=stack base, b=caller                     */
#define ST_CREATE_CTX_DONE  2   /* a=ctx.sp, b=ctx.x19, c=ctx.x30 (read back)  */
#define ST_DESTROY          3   /* a=caller, b=stack base, c=ctx.sp            */
#define ST_DESTROY_DONE     4   /* gen = new generation                       */
#define ST_EXIT             5   /* task_exit on the running task               */
#define ST_TERMINATE        6   /* a=rq cpu, b=removed from queue (0/1)        */
#define ST_ADD              7   /* a=target cpu, b=caller                      */
#define ST_ZOMBIE_SET       8   /* rq->zombie = task, a=cpu                    */
#define ST_ZOMBIE_REAP      9   /* about to task_destroy(rq->zombie)          */
#define ST_SWITCH           10  /* task=next; a=pack(current), b=next ctx.sp, c=next ctx.x19 */
#define ST_SWITCH_RET       11  /* switch_to returned; task=current, a=live sp */
#define ST_FIRST_RUN        12  /* task_entry_trampoline; a=live sp, b=ctx.sp  */
#define ST_PICK_SAME        13  /* schedule() kept current; a=state at entry   */
#define ST_TEST             14  /* test marker; a=tag, b, c                    */
/* #1229 / #1219 events (lifecycle run 4, C1). */
#define ST_PD_SET           15  /* preempt_disabled[a] = 1; task=next, b=cpu_id() */
#define ST_PD_CLEAR         16  /* preempt_disabled[a] = 0; task=current, b=cpu_id() actually running */
#define ST_STEAL_PD         17  /* thief skipped victim a on preempt_disabled (task NULL) */
#define ST_STEAL_TRY        18  /* thief popped task from victim a; b=preempt_disabled[a] under rq_lock[a],
                                   c=outcome | (task was victim's current)<<8;
                                   outcome 1 stolen, 2 gen, 3 state, 4 cpu,
                                   5 affinity, 6 not in queue                 */
#define ST_PD_STUCK         19  /* scheduler_tick: preempt_disabled[a] set for b ticks */

#if defined(CONFIG_SLOT_TRACE)

struct task;

void slot_trace_rec(uint32_t ev, const struct task *t,
                    uint64_t a, uint64_t b, uint64_t c);
/* slot(16) | generation(32) | state(16), for one task in an entry field. */
uint64_t slot_trace_pack(const struct task *t);
/* Freeze all rings and print them; first caller only (later callers spin).
 * Exits QEMU through semihosting; does not return. */
void slot_trace_dump(const char *why, uint64_t a, uint64_t b, uint64_t c)
    __attribute__((noreturn));
/* Called from the el1_sync vector on a bad SP (already on the emergency
 * stack); orig_sp is the SP at exception entry. */
void slot_trace_bad_sp_fault(void *tf, uint64_t orig_sp)
    __attribute__((noreturn));

extern volatile uint8_t slot_trace_rq_holder[];

#define SLOT_TRACE(ev, t, a, b, c) \
    slot_trace_rec((ev), (t), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c))
#define SLOT_TRACE_PACK(t)          slot_trace_pack(t)
#define SLOT_TRACE_ONLY(...)        __VA_ARGS__
#define SLOT_TRACE_RQ_LOCKED(cpu)   (slot_trace_rq_holder[(cpu)] = (uint8_t)(cpu_id() + 1))
#define SLOT_TRACE_RQ_UNLOCKING(cpu) (slot_trace_rq_holder[(cpu)] = 0)
/* Tick-side watchdog: dumps the rings once preempt_disabled[cpu] has
 * stayed 1 for SLOT_TRACE_PD_STUCK_TICKS consecutive ticks (#1229). */
void slot_trace_pd_tick(uint32_t cpu, int pd);

#else

#define SLOT_TRACE(ev, t, a, b, c)  ((void)0)
#define SLOT_TRACE_PACK(t)          0
#define SLOT_TRACE_ONLY(...)
#define SLOT_TRACE_RQ_LOCKED(cpu)   ((void)0)
#define SLOT_TRACE_RQ_UNLOCKING(cpu) ((void)0)
#define slot_trace_pd_tick(cpu, pd) ((void)0)

#endif /* CONFIG_SLOT_TRACE */

#endif /* SLOT_TRACE_H */
