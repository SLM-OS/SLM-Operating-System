/*
 * slot_trace.c - Per-CPU task-slot lifecycle trace (#1213). See slot_trace.h.
 *
 * Compiles to an empty unit unless CONFIG_SLOT_TRACE is defined.
 */

#include "fault_dump.h"
#include "slot_trace.h"

#if defined(CONFIG_SLOT_TRACE)

#include "config.h"
#include "task.h"
#include "sched.h"
#include "smp.h"
#include "uart.h"
#include "semihosting.h"
#include <stddef.h>

#define SLOT_TRACE_LEN 512U     /* entries per CPU (power of two) */

struct slot_trace_ent {
    uint64_t ts;        /* CNTVCT_EL0 */
    uint32_t seq;       /* global order */
    uint16_t ev;
    uint8_t  cpu;
    uint8_t  pad0;
    int16_t  slot;      /* task_table index, -1 if not a table task */
    uint16_t state;
    uint32_t gen;
    uint32_t id;
    uint32_t holders;   /* rq lock holder per CPU, 4 bits each (0 = free, n+1 = CPU n) */
    uint32_t rep;       /* identical consecutive records folded into this one */
    uint64_t a, b, c;
};

static struct slot_trace_ent slot_trace_ring[MAX_CPUS][SLOT_TRACE_LEN];
static volatile uint32_t slot_trace_idx[MAX_CPUS];
static volatile uint32_t slot_trace_seq;
static volatile uint32_t slot_trace_frozen;
volatile uint8_t slot_trace_rq_holder[MAX_CPUS];

/* Emergency stacks for the bad-SP vector path (vectors.S). */
#define SLOT_TRACE_EMERG_STACK 16384
__attribute__((aligned(16)))
uint8_t slot_trace_emerg[4][SLOT_TRACE_EMERG_STACK];

static inline uint64_t st_cntvct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static int st_slot(const struct task *t)
{
    const struct task *base = task_slot(0);
    if (!t || !base) {
        return -1;
    }
    if (t < base || t >= base + MAX_TASKS) {
        return -1;
    }
    return (int)(t - base);
}

uint64_t slot_trace_pack(const struct task *t)
{
    if (!t) {
        return 0xFFFFULL << 48;
    }
    int s = st_slot(t);
    return ((uint64_t)(uint16_t)(int16_t)s << 48) |
           ((uint64_t)t->generation << 16) |
           (uint64_t)((uint16_t)t->state);
}

void slot_trace_rec(uint32_t ev, const struct task *t,
                    uint64_t a, uint64_t b, uint64_t c)
{
    uint32_t cpu = cpu_id();
    uint32_t fz = __atomic_load_n(&slot_trace_frozen, __ATOMIC_RELAXED);
    if (fz) {
        /* A dump is in progress on another CPU: park this one at its next
         * trace point, so its console output does not interleave with the
         * dump and its rings stay as captured. */
        if (fz != cpu + 1) {
            __asm__ volatile("msr daifset, #0xF" ::: "memory");
            for (;;) {
                __asm__ volatile("wfe");
            }
        }
        return;
    }
    if (cpu >= MAX_CPUS) {
        return;
    }
    uint64_t daif;
    __asm__ volatile("mrs %0, daif\n\tmsr daifset, #3" : "=r"(daif) :: "memory");

    uint32_t i = slot_trace_idx[cpu];
    /* Fold an identical consecutive record (a yield loop's requeue ADDs,
     * a thief's repeated preempt_disabled skips) into the previous one,
     * so floods do not push the history out of the ring. */
    if (i > 0) {
        struct slot_trace_ent *p =
            &slot_trace_ring[cpu][(i - 1) & (SLOT_TRACE_LEN - 1)];
        if (p->ev == ev && p->slot == (int16_t)st_slot(t) &&
            p->gen == (t ? t->generation : 0) && p->a == a && p->b == b &&
            p->c == c && (ev == ST_ADD || ev == ST_STEAL_PD ||
                          ev == ST_PD_SET || ev == ST_PD_CLEAR)) {
            p->rep++;
            __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
            return;
        }
    }
    struct slot_trace_ent *e = &slot_trace_ring[cpu][i & (SLOT_TRACE_LEN - 1)];
    e->ts = st_cntvct();
    e->seq = __atomic_fetch_add(&slot_trace_seq, 1, __ATOMIC_RELAXED);
    e->ev = (uint16_t)ev;
    e->cpu = (uint8_t)cpu;
    e->slot = (int16_t)st_slot(t);
    e->state = t ? (uint16_t)t->state : 0xFFFF;
    e->gen = t ? t->generation : 0;
    e->id = t ? t->id : 0;
    uint32_t h = 0;
    for (uint32_t k = 0; k < 4 && k < MAX_CPUS; k++) {
        h |= ((uint32_t)slot_trace_rq_holder[k] & 0xF) << (4 * k);
    }
    e->holders = h;
    e->rep = 0;
    e->a = a;
    e->b = b;
    e->c = c;
    slot_trace_idx[cpu] = i + 1;

    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static const char *st_name(uint32_t ev)
{
    switch (ev) {
    case ST_CREATE_ALLOC:    return "CREATE_ALLOC";
    case ST_CREATE_CTX_DONE: return "CREATE_CTX_DONE";
    case ST_DESTROY:         return "DESTROY";
    case ST_DESTROY_DONE:    return "DESTROY_DONE";
    case ST_EXIT:            return "EXIT";
    case ST_TERMINATE:       return "TERMINATE";
    case ST_ADD:             return "ADD";
    case ST_ZOMBIE_SET:      return "ZOMBIE_SET";
    case ST_ZOMBIE_REAP:     return "ZOMBIE_REAP";
    case ST_SWITCH:          return "SWITCH";
    case ST_SWITCH_RET:      return "SWITCH_RET";
    case ST_FIRST_RUN:       return "FIRST_RUN";
    case ST_PICK_SAME:       return "PICK_SAME";
    case ST_TEST:            return "TEST";
    case ST_PD_SET:          return "PD_SET";
    case ST_PD_CLEAR:        return "PD_CLEAR";
    case ST_STEAL_PD:        return "STEAL_PD";
    case ST_STEAL_TRY:       return "STEAL_TRY";
    case ST_PD_STUCK:        return "PD_STUCK";
    default:                 return "?";
    }
}

static void st_print_task(const char *label, const struct task *t)
{
    if (!t) {
        uart_printf_unlocked("    %s: (null)\n", label);
        return;
    }
    /* Only dereference pointers into the task table: a corrupted run
     * queue must not fault the dump itself. */
    if (st_slot(t) < 0) {
        uart_printf_unlocked("    %s: %p (not a task_table slot)\n", label,
                             (const void *)t);
        return;
    }
    uart_printf_unlocked("    %s: slot=%d id=%u gen=%u state=%u name='%s' "
                         "ctx.sp=0x%lx ctx.x19=0x%lx ctx.x30=0x%lx stack=[0x%lx..0x%lx)\n",
                         label, st_slot(t), t->id, t->generation,
                         (unsigned)t->state, t->name,
                         (unsigned long)t->context.sp,
                         (unsigned long)t->context.x19,
                         (unsigned long)t->context.x30,
                         (unsigned long)(uintptr_t)t->stack_base,
                         (unsigned long)(uintptr_t)t->stack_top);
}

void slot_trace_dump(const char *why, uint64_t a, uint64_t b, uint64_t c)
{
    __asm__ volatile("msr daifset, #0xF" ::: "memory");
    uint32_t me = cpu_id();
    uint32_t was = __atomic_exchange_n(&slot_trace_frozen, me + 1,
                                       __ATOMIC_ACQ_REL);
    if (was) {
        for (;;) {
            __asm__ volatile("wfe");
        }
    }
    /* Let in-flight records on other CPUs finish their stores. */
    for (volatile int d = 0; d < 100000; d++) {
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    uart_printf_unlocked("\nSLOTTRACE-DUMP-BEGIN why=%s cpu=%u a=0x%lx b=0x%lx c=0x%lx "
                         "seq=%u cntvct=0x%lx\n",
                         why, me, (unsigned long)a, (unsigned long)b,
                         (unsigned long)c, slot_trace_seq,
                         (unsigned long)st_cntvct());
    uint32_t ncpu = cpu_count < 4 ? cpu_count : 4;
    uart_printf_unlocked("  format: ST c<cpu> #<seq> t=<cntvct> <ev> slot gen id st a b c rq=<holder nibbles cpu3..cpu0>\n");
    for (uint32_t k = 0; k < ncpu; k++) {
        uint32_t n = slot_trace_idx[k];
        uint32_t first = n > SLOT_TRACE_LEN ? n - SLOT_TRACE_LEN : 0;
        uart_printf_unlocked("SLOTTRACE-CPU %u entries=%u shown=%u\n",
                             k, n, n - first);
        for (uint32_t i = first; i < n; i++) {
            const struct slot_trace_ent *e =
                &slot_trace_ring[k][i & (SLOT_TRACE_LEN - 1)];
            uart_printf_unlocked("ST c%u #%u t=0x%lx %s slot=%d gen=%u id=%u st=%u "
                                 "a=0x%lx b=0x%lx c=0x%lx rq=%x rep=%u\n",
                                 (unsigned)e->cpu, e->seq,
                                 (unsigned long)e->ts, st_name(e->ev),
                                 (int)e->slot, e->gen, e->id,
                                 (unsigned)e->state,
                                 (unsigned long)e->a, (unsigned long)e->b,
                                 (unsigned long)e->c, e->holders, e->rep);
        }
    }
    uart_printf_unlocked("  state at dump:\n");
    for (uint32_t k = 0; k < ncpu; k++) {
        struct cpu_runqueue *rq = sched_cpu_rq(k);
        uart_printf_unlocked("  cpu%u: rq_holder=%u\n", k,
                             (unsigned)slot_trace_rq_holder[k]);
        st_print_task("current", task_current_on_cpu(k));
        if (rq) {
            st_print_task("rq.head", rq->head);
            st_print_task("rq.zombie", rq->zombie);
        }
    }
    uart_printf_unlocked("SLOTTRACE-DUMP-END\n");
    semihosting_exit(3);
    for (;;) {
        __asm__ volatile("wfe");
    }
}

#define SLOT_TRACE_PD_STUCK_TICKS 300U   /* 3 s at 100 Hz */
static uint32_t slot_trace_pd_ticks[MAX_CPUS];

void slot_trace_pd_tick(uint32_t cpu, int pd)
{
    if (cpu >= MAX_CPUS) {
        return;
    }
    if (!pd) {
        slot_trace_pd_ticks[cpu] = 0;
        return;
    }
    if (++slot_trace_pd_ticks[cpu] == SLOT_TRACE_PD_STUCK_TICKS) {
        extern volatile int preempt_disabled[];
        SLOT_TRACE(ST_PD_STUCK, task_current_on_cpu(cpu), cpu,
                   slot_trace_pd_ticks[cpu], 0);
        uart_printf_unlocked("\nSLOTTRACE-FAULT preempt_disabled[%u] stuck for %u ticks "
                             "(pd=%d/%d/%d/%d)\n", cpu, slot_trace_pd_ticks[cpu],
                             preempt_disabled[0], preempt_disabled[1],
                             preempt_disabled[2], preempt_disabled[3]);
        slot_trace_dump("pd_stuck", cpu, slot_trace_pd_ticks[cpu], 0);
    }
}

void slot_trace_bad_sp_fault(void *tf, uint64_t orig_sp)
{
    /* Serialized dump, every line tagged with this CPU (fault_dump.h). */
    (void)fault_dump_begin();
    uint64_t esr, far, elr;
    const uint64_t *x = (const uint64_t *)tf;  /* x0..x30 at 0..240 (trap.h) */
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));
    __asm__ volatile("mrs %0, elr_el1" : "=r"(elr));
    uart_printf_unlocked("\nSLOTTRACE-FAULT bad SP at EL1 sync entry: cpu=%u sp=0x%lx "
                         "esr=0x%lx far=0x%lx elr=0x%lx\n",
                         cpu_id(), (unsigned long)orig_sp, (unsigned long)esr,
                         (unsigned long)far, (unsigned long)elr);
    for (int r = 0; r < 31; r += 2) {
        if (r == 30) {
            uart_printf_unlocked("  x30=0x%lx\n", (unsigned long)x[30]);
        } else {
            uart_printf_unlocked("  x%d=0x%lx x%d=0x%lx\n", r,
                                 (unsigned long)x[r], r + 1,
                                 (unsigned long)x[r + 1]);
        }
    }
    slot_trace_dump("el1_sync_bad_sp", orig_sp, esr, elr);
}

#endif /* CONFIG_SLOT_TRACE */
