/*
 * tss.c - Per-CPU TSS + IST1 stack setup for x86-64 (A1 / P1-1).
 *
 * On x86-64 the IDT gate "IST" field selects one of 7 per-CPU
 * interrupt-stack-table (IST) slots. When an interrupt vector with
 * IST > 0 fires, the CPU switches to the TSS's istN field before
 * pushing the exception frame, regardless of the current privilege
 * level. This makes the ISR stack independent of whatever task was
 * running when the interrupt arrived — a task-stack overflow can no
 * longer corrupt the ISR's frame.
 *
 * We use IST1 for the LAPIC timer vector (48) and for the reschedule
 * IPI vector (0xF1 — installed in B1). All other vectors keep IST=0
 * so that synchronous exceptions (#PF, #GP, …) run on the current
 * task's kernel stack where callers expect them.
 *
 * Design:
 *   - Each CPU has its own GDT (copied from the boot GDT at
 *     kernel/arch/x86_64/trampoline32.S) with a TSS descriptor
 *     appended at selector 0x18. The extra space for TSS high
 *     qword lands at 0x20; selector loaded into TR is 0x18.
 *   - Each CPU has its own TSS (104 bytes) and IST1 stack (4 KiB,
 *     16-byte aligned).
 *   - tss_install_current_cpu() is called from each CPU after LAPIC
 *     init but before timer_start() / before STI.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stddef.h>
#include "smp.h"

/* x86-64 64-bit TSS (Intel SDM Vol. 3, §8.7). */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;          /* Stack for CPL transitions to ring 0 */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;          /* IST1: timer + reschedule IPI */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss64) == 104, "TSS64 must be 104 bytes");

/* Per-CPU GDT: null, code64, data64, TSS low, TSS high. 5 entries. */
#define CPU_GDT_ENTRIES 5
#define CPU_TSS_SELECTOR 0x18   /* index 3 — TSS descriptor (16 bytes) */

struct cpu_gdt {
    uint64_t entries[CPU_GDT_ENTRIES];
} __attribute__((aligned(16)));

struct cpu_gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* One per CPU. Put in BSS (cache-coherent on x86-64). */
static struct cpu_gdt    cpu_gdts[MAX_CPUS];
static struct tss64      cpu_tsses[MAX_CPUS] __attribute__((aligned(16)));
static uint8_t           cpu_ist1_stacks[MAX_CPUS][4096] __attribute__((aligned(16)));

/* TSS descriptor layout (Intel SDM Vol. 3, §8.2.3 — 16-byte
 * system descriptor in IA-32e mode).
 *
 * Low qword:
 *   bits 0..15:  limit[15..0]
 *   bits 16..39: base[23..0]
 *   bits 40..47: type_attr  — 0x89 (present, DPL=0, type=9 = available TSS64)
 *   bits 48..51: limit[19..16]
 *   bits 52..55: flags      — 0 (byte granularity, no AVL)
 *   bits 56..63: base[31..24]
 * High qword:
 *   bits 0..31:  base[63..32]
 *   bits 32..63: reserved (0)
 */
static void tss_descriptor_write(uint64_t *slot, uint64_t base,
                                 uint32_t limit)
{
    uint64_t low = 0;
    low |= (limit & 0xFFFFULL);
    low |= ((base  & 0xFFFFFFULL) << 16);
    low |= ((uint64_t)0x89ULL << 40);
    low |= (((uint64_t)(limit >> 16) & 0xFULL) << 48);
    low |= (((base  >> 24) & 0xFFULL) << 56);
    slot[0] = low;

    slot[1] = (base >> 32) & 0xFFFFFFFFULL;
}

/*
 * Install the per-CPU TSS for @cpu_id: fills the TSS, writes the
 * per-CPU GDT, LGDTs it, and loads TR.
 *
 * Must be called from @cpu_id itself (CS-affine — LGDT / LTR act on
 * the calling CPU's descriptor registers).
 */
void tss_install_current_cpu(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS)
        return;

    struct tss64 *tss = &cpu_tsses[cpu_id];
    struct cpu_gdt *gdt = &cpu_gdts[cpu_id];

    /* Populate TSS: point IST1 at the top of this CPU's 4 KiB IST
     * stack (x86-64 stack grows down). All other IST slots and the
     * legacy ring-0 rsp0/1/2 fields are zero — unused today. */
    for (size_t i = 0; i < sizeof(*tss); i++)
        ((uint8_t *)tss)[i] = 0;
    tss->ist1 = (uint64_t)&cpu_ist1_stacks[cpu_id][sizeof(cpu_ist1_stacks[cpu_id])];
    tss->io_map_base = sizeof(*tss);    /* no I/O bitmap */

    /* Populate GDT: copy boot entries, then build the 16-byte TSS
     * descriptor at selector 0x18. */
    gdt->entries[0] = 0x0000000000000000ULL;              /* null */
    gdt->entries[1] = 0x00AF9A000000FFFFULL;              /* code64 (CS=0x08) */
    gdt->entries[2] = 0x00CF92000000FFFFULL;              /* data64 (DS=0x10) */
    tss_descriptor_write(&gdt->entries[3], (uint64_t)tss,
                         (uint32_t)(sizeof(*tss) - 1));

    /* Load per-CPU GDT. Segment selectors (CS=0x08, DS=0x10) keep
     * the same numeric values so no far-return / segment reload is
     * needed — the CPU just resolves future selector loads through
     * the new GDT. */
    struct cpu_gdt_ptr ptr;
    ptr.limit = sizeof(*gdt) - 1;
    ptr.base = (uint64_t)gdt;
    __asm__ volatile("lgdt (%0)" :: "r"(&ptr) : "memory");

    /* Load Task Register with the TSS selector. */
    uint16_t tr_sel = CPU_TSS_SELECTOR;
    __asm__ volatile("ltr %0" :: "r"(tr_sel));
}

/*
 * tss_get_ist1_for_cpu — diagnostic accessor used by tests to verify
 * that the active IST1 stack is the per-CPU one, not the task stack.
 */
uintptr_t tss_get_ist1_for_cpu(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS)
        return 0;
    return (uintptr_t)&cpu_ist1_stacks[cpu_id][sizeof(cpu_ist1_stacks[cpu_id])];
}

uintptr_t tss_get_ist1_stack_base_for_cpu(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS)
        return 0;
    return (uintptr_t)&cpu_ist1_stacks[cpu_id][0];
}

uintptr_t tss_get_tss_base_for_cpu(uint32_t cpu_id)
{
    if (cpu_id >= MAX_CPUS)
        return 0;
    return (uintptr_t)&cpu_tsses[cpu_id];
}

#endif /* PLATFORM_X86_64 */
