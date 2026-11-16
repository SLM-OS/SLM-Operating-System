/*
 * switch_check.h - switch-in window test instruments (#1234, #1238).
 *
 * Two build options, both default OFF; with both off this header defines
 * only empty macros and the kernel's .text is unchanged.
 *
 * SWITCH_CHECK=ON (CONFIG_SWITCH_CHECK): a per-task-slot generation pair.
 *   At switch-in (schedule()/scheduler_start(), under the incoming CPU's
 *   rq_lock, before switch_to restores the context) the incoming CPU
 *   checks that the save of the task's previous run has completed
 *   (saved_gen == restored_gen, read with a load-acquire), then bumps
 *   restored_gen. At switch-out completion (sched_finish_switch, on the
 *   CPU that ran the task, before the on_cpu store-release) saved_gen is
 *   set to restored_gen with a store-release. It also claims a per-slot
 *   owner word with a compare-and-swap (0 -> cpu + 1) at switch-in and
 *   releases it at switch-out, so a task switched in while another CPU
 *   still owns it is counted as running on two CPUs. Counters are read
 *   by test_migrate_ready_during_switch_out (R3).
 *
 * SWITCH_WINDOW_TEST=ON (CONFIG_SWITCH_WINDOW_TEST): the run-4 critic's
 *   window patch as a test hook (R1). schedule() holds the outgoing
 *   switch window (after rq_unlock, before switch_to) open for 25 ms for
 *   one chosen outgoing task, with the IRQ state of the caller, so that
 *   a timer tick lands inside it. Driven by test_switch_window_held.
 */
#ifndef SWITCH_CHECK_H
#define SWITCH_CHECK_H

#include <stdint.h>

struct task;

#if defined(CONFIG_SWITCH_CHECK)
struct switch_check_stats {
    uint32_t switch_ins;        /* checked switch-ins */
    uint32_t bad_gen;           /* restored before the previous save completed */
    uint32_t bad_dual;          /* switched in while another CPU owned the task */
    uint32_t first_task_id;     /* first failure: task id */
    uint32_t first_cpu;         /* first failure: incoming CPU */
    uint32_t first_owner;       /* first failure: owner word (cpu + 1), 0 if none */
    uint32_t first_kind;        /* 1 = gen, 2 = dual, 3 = both */
    uint32_t stale_cpu;         /* schedule() holds rq_lock[c] but runs on another CPU (#1242) */
    uint32_t stale_first_task;  /* first stale: task id, held id, executing CPU, site */
    uint32_t stale_first_held;
    uint32_t stale_first_real;
    uint32_t stale_first_where; /* 1 first lock, 2 after the zombie relock */
};
void switch_check_in(struct task *next, uint32_t cpu);
void switch_check_out(struct task *prev);
void switch_check_snapshot(struct switch_check_stats *out);
void switch_check_cpu(uint32_t held_cpu, uint32_t where);
#define SWITCH_CHECK_IN(next, cpu)  switch_check_in((next), (cpu))
#define SWITCH_CHECK_OUT(prev)      switch_check_out((prev))
#define SWITCH_CHECK_CPU(cpu, w)    switch_check_cpu((cpu), (w))
#else
#define SWITCH_CHECK_IN(next, cpu)  do { } while (0)
#define SWITCH_CHECK_OUT(prev)      do { } while (0)
#define SWITCH_CHECK_CPU(cpu, w)    do { (void)(cpu); (void)(w); } while (0)
#endif

#if defined(CONFIG_SWITCH_WINDOW_TEST)
extern volatile int switch_window_on;
extern struct task *volatile switch_window_task;
extern volatile int switch_window_in;
/* Forced-tick mode (no-deadlock bar): when nonzero, after the test sets
 * switch_window_moved the hold continues until this many hardware ticks
 * have run on the holding CPU (bounded at 200 ms), so a tick lands in the
 * window after the moved task's new CPU has picked it. */
extern volatile int switch_window_ticks_after_move;
extern volatile int switch_window_moved;
extern volatile int switch_window_ticks_seen;
void switch_window_hold(struct task *current);
#define SWITCH_WINDOW_HOLD(cur)     switch_window_hold((cur))
#else
#define SWITCH_WINDOW_HOLD(cur)     do { } while (0)
#endif

#endif /* SWITCH_CHECK_H */
