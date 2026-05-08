/*
 * task.c - Task management for SLM-OS
 */

#include "task.h"
#include "sched.h"
#include "pmm.h"
#include "uart.h"
#include "debug.h"
#include "smp.h"
#include "spinlock.h"
#include "cache.h"
#include "ncmem.h"
#include "arch.h"
#if !defined(PLATFORM_X86_64)
#include "vmm.h"
#include "elf.h"
#endif
#include <stddef.h>

/* Task table - NC on Pi 5, BSS fallback otherwise */
static struct task *task_table;
static struct task task_table_fallback[MAX_TASKS];
static uint32_t next_task_id = 1;       /* ID 0 reserved for idle task */

/* Lock protecting task_table and next_task_id.
 * On Pi 5, standard ldaxr/stxr spinlocks fail under cross-CPU contention
 * (L2 retains stale lock values). Use atomic test-and-set instead. */
#if defined(PLATFORM_HAS_NC_MEMORY)
/* Use standard spinlock but add DC CIVAC after release to push
 * "unlocked" state through L2 to DRAM. Secondary CPUs' first lock
 * attempt reads from DRAM (cold L2) and sees "unlocked". */
static spinlock_t task_lock __attribute__((aligned(64))) = SPINLOCK_INIT;
#define TASK_LOCK_IRQSAVE() \
    irq_flags_t _task_flags = spin_lock_irqsave(&task_lock)
#define TASK_UNLOCK_IRQRESTORE() \
    do { \
        spin_unlock_irqrestore(&task_lock, _task_flags); \
        __asm__ volatile("dc civac, %0" :: "r"(&task_lock) : "memory"); \
        __asm__ volatile("dsb sy" ::: "memory"); \
        __asm__ volatile("isb" ::: "memory"); \
    } while(0)
#else
static spinlock_t task_lock = SPINLOCK_INIT;
#define TASK_LOCK_IRQSAVE() \
    irq_flags_t _task_flags = spin_lock_irqsave(&task_lock)
#define TASK_UNLOCK_IRQRESTORE() \
    spin_unlock_irqrestore(&task_lock, _task_flags)
#endif

/* Per-CPU current running task (set by scheduler).
 *
 * On Pi 5 / Jetson (PLATFORM_HAS_NC_MEMORY), this lives in non-cacheable
 * memory so cross-CPU readers see writes immediately without cache
 * maintenance. DC CIVAC has a known failure mode on this topology
 * (writer's stale cacheline can be written back, clobbering a newer
 * cross-CPU write), so NC relocation is preferred over cache maintenance.
 *
 * On other platforms, we keep the BSS array and the existing
 * cache_clean/cache_invalidate pattern in task_current/task_set_current. */
#if defined(PLATFORM_HAS_NC_MEMORY)
static struct task **current_task;
#else
static struct task *current_task[MAX_CPUS];
#endif

void task_table_init(void)
{
#if defined(PLATFORM_HAS_NC_MEMORY)
    task_table = ncmem_alloc(MAX_TASKS * sizeof(struct task), CACHE_LINE_SIZE);
    if (!task_table) {
        task_table = task_table_fallback;
    } else {
        volatile uint8_t *p = (volatile uint8_t *)task_table;
        for (size_t i = 0; i < MAX_TASKS * sizeof(struct task); i++)
            p[i] = 0;
    }

    current_task = ncmem_alloc(MAX_CPUS * sizeof(struct task *),
                               CACHE_LINE_SIZE);
    if (!current_task) {
        /* NC exhaustion is fatal on platforms that need NC for cross-
         * CPU coherency (Pi 5, Jetson). Falling back to BSS would let
         * `task_set_current` run without `cache_clean` (the NC build
         * path elides it), so a remote CPU's `task_current_on_cpu`
         * would see stale data — silent corruption with no diagnostic.
         * Better to halt here with a clear message: NC arena sizing
         * needs to be raised in the platform header. */
        panic("current_task: NC arena exhausted — bump NC_MEM_SIZE in kernel/include/ncmem.h");
    }
    for (uint32_t i = 0; i < MAX_CPUS; i++)
        current_task[i] = NULL;
#else
    task_table = task_table_fallback;
#endif
}

/*
 * String copy helper (no libc)
 */
static void str_copy(char *dst, const char *src, size_t max)
{
    size_t i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/*
 * Find a free slot in the task table.
 */
static struct task *alloc_task_slot(void)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].id == 0) {
            return &task_table[i];
        }
    }
    return NULL;
}

/* Forward declaration — defined in assembly (context.S for x86-64, inline below for ARM64) */
extern void task_entry_wrapper(void);

/*
 * C trampoline called from assembly task_entry_wrapper.
 * Callee-saved registers hold the entry point and argument.
 */
void task_entry_trampoline(uint64_t entry_addr, uint64_t arg_addr)
{
    task_entry_t entry = (task_entry_t)entry_addr;
    void *arg = (void *)arg_addr;

    /* NC debug marker: 0xDD = trampoline reached.
     * If this appears in diagnostics, context.S restore completed. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    {
        uint64_t _mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(_mpidr));
        /* Pi 5: CPU index in Aff1 (bits[15:8]), QEMU: Aff0 (bits[7:0]).
         * OR gives correct index when only one field is non-zero. */
        uint32_t _cpu = (_mpidr & 0xFF) | ((_mpidr >> 8) & 0xFF);
        *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + _cpu * 4) = 0xDD;
    }
#endif

    /* Clear preempt_disabled for this CPU.
     * scheduler_start() sets it to 1 before switch_to(NULL, first),
     * but switch_to never returns — it jumps here.
     *
     * Note: On Pi 5, tasks run with IRQs masked (DAIF.I=1). Timer
     * preemption is not enabled — scheduling is cooperative via yield().
     * pit_ticks is advanced by the idle task which unmasks IRQs in its
     * loop. On QEMU, timer IRQs fire regardless of DAIF. */
#if !defined(PLATFORM_X86_64)
    {
        extern volatile int preempt_disabled[];
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        uint32_t hw_cpu = (mpidr & 0xFF) | ((mpidr >> 8) & 0xFF);
        if (hw_cpu < MAX_CPUS) {
            preempt_disabled[hw_cpu] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }
#else
    {
        extern volatile int preempt_disabled[];
        preempt_disabled[cpu_id()] = 0;
        __asm__ volatile("mfence" ::: "memory");
    }
#endif

    entry(arg);
    task_exit();
}

#if !defined(PLATFORM_X86_64)
/*
 * ARM64 task entry wrapper (inline assembly).
 *
 * When switch_to restores a new task, x19 = entry, x20 = arg.
 * Passes them to task_entry_trampoline as function arguments.
 */
__asm__(
    ".global task_entry_wrapper\n"
    ".type task_entry_wrapper, %function\n"
    "task_entry_wrapper:\n"
    "    mov x0, x19\n"
    "    mov x1, x20\n"
    "    b task_entry_trampoline\n"
);
#endif /* !PLATFORM_X86_64 — x86-64 wrapper is in context.S */

/*
 * Allocate a task slot without setting up a stack.
 *
 * This is used by elf.c to create tasks with custom stack setup.
 * The caller must set up stack_base, stack_top, and context.
 */
struct task *task_alloc(const char *name, uint8_t priority)
{
    struct task *task;
    uint32_t task_id;

    /* Clamp priority to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    /* Acquire lock to access task_table and next_task_id */
    TASK_LOCK_IRQSAVE();

    /* Find free task slot */
    task = alloc_task_slot();
    if (!task) {
        TASK_UNLOCK_IRQRESTORE();
        ERROR("task_alloc: no free task slots");
        return NULL;
    }

    /* Reserve task ID atomically */
    task_id = next_task_id++;

    /* Mark slot as used immediately (id != 0 means in use) */
    task->id = task_id;

    TASK_UNLOCK_IRQRESTORE();

    /* Initialize task structure (slot is ours now) */
    str_copy(task->name, name ? name : "unnamed", TASK_NAME_LEN);
    task->state = TASK_READY;
    task->next = NULL;
    task->cpu_affinity = CPU_AFFINITY_ANY;
    task->assigned_cpu = 0;
    task->priority = priority;
    task->effective_priority = priority;
    task->deadline_ns = 0;
    task->wake_time_ns = 0;
    task->sleep_next = NULL;
    task->switches = 0;

    /* Stack pointers left uninitialized - caller must set these */
    task->stack_base = NULL;
    task->stack_top = NULL;

    /* Zero out the context */
    for (size_t i = 0; i < sizeof(task->context); i++) {
        ((uint8_t *)&task->context)[i] = 0;
    }

    /* No cleanup callback by default */
    task->cleanup = NULL;
    task->cleanup_arg = NULL;

    /* User-mode defaults — task_create_user overrides is_user +
     * user_entry; #697 PR-3 will populate user_l1_pa. Explicit init
     * guards against stale-slot reuse: alloc_task_slot may return a
     * slot whose previous owner was an EL0 task. */
    task->is_user = 0;
    task->user_entry = NULL;
    task->user_l1_pa = 0;
    task->user_stack_top = 0;
    task->user_stack_phys = 0;
    task->user_va_next = 0;
    task->user_asid = 0;

    DEBUG_PRINT("Allocated task '%s' (id=%u, priority=%u)",
                task->name, task->id, task->priority);

    return task;
}

/*
 * Create a new task with specified priority.
 */
struct task *task_create_with_priority(const char *name, task_entry_t entry,
                                       void *arg, uint8_t priority)
{
    struct task *task;
    uint32_t task_id;

    /* Clamp priority to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    /* Allocate stack first (outside lock - pmm has its own locking) */
    size_t stack_pages = STACK_SIZE / 4096;
    void *stack = pmm_alloc_pages(stack_pages);
    if (!stack) {
        ERROR("task_create: failed to allocate stack");
        return NULL;
    }

    /* Acquire lock to access task_table and next_task_id */
    TASK_LOCK_IRQSAVE();

    /* Find free task slot */
    task = alloc_task_slot();
    if (!task) {
        TASK_UNLOCK_IRQRESTORE();
        pmm_free_pages(stack, stack_pages);
        ERROR("task_create: no free task slots");
        return NULL;
    }

    /* Reserve task ID atomically */
    task_id = next_task_id++;

    /* Mark slot as used immediately (id != 0 means in use) */
    task->id = task_id;

    TASK_UNLOCK_IRQRESTORE();

    /* Initialize rest of task structure (slot is ours now) */
    str_copy(task->name, name ? name : "unnamed", TASK_NAME_LEN);
    task->state = TASK_READY;
    task->next = NULL;
    task->cpu_affinity = CPU_AFFINITY_ANY;  /* Can run on any CPU */
    task->assigned_cpu = 0;                  /* Default to CPU 0 */
    task->priority = priority;
    task->effective_priority = priority;
    task->deadline_ns = 0;                   /* No deadline by default */
    task->wake_time_ns = 0;                  /* Not sleeping */
    task->sleep_next = NULL;
    task->switches = 0;

    /* Set up stack (grows downward on ARM64) */
    task->stack_base = stack;
    task->stack_top = (void *)((uintptr_t)stack + STACK_SIZE);

    /* Plant canary at stack bottom — diagnostic for #601 Bug B. */
    task_canary_init(task);

    /* Initialize CPU context */
    /* Zero out the context first */
    for (size_t i = 0; i < sizeof(task->context); i++) {
        ((uint8_t *)&task->context)[i] = 0;
    }

    /* Set up initial context for first switch */
#if defined(PLATFORM_X86_64)
    task->context.rsp = (uint64_t)task->stack_top;
    task->context.rip = (uint64_t)task_entry_wrapper;
    task->context.rbp = 0;                              /* Frame pointer */
    task->context.rflags = 0;                           /* IF=0: interrupts disabled */
    task->context.rbx = (uint64_t)entry;                /* Entry function */
    task->context.r12 = (uint64_t)arg;                  /* Argument */

    /* FXSAVE area (D2 / P1-6). Zero the buffer so fxrstor on first
     * switch restores an all-zero XMM/x87 state. Set FCW to the
     * Intel i387 init value (0x037F) — round-to-nearest, unmasked
     * precision/underflow/overflow/zero-divide/invalid, 53-bit
     * precision. Per Intel SDM Vol. 1 §8.1.5, this matches the
     * hardware-init state after a hard reset. */
    for (size_t i = 0; i < sizeof(task->context.fxsave); i++)
        task->context.fxsave[i] = 0;
    task->context.fxsave[0] = 0x7F;                     /* FCW low  */
    task->context.fxsave[1] = 0x03;                     /* FCW high */
    task->context.fxsave[24] = 0x80;                    /* MXCSR = 0x1F80 */
    task->context.fxsave[25] = 0x1F;                    /* (SSE masks) */
#else
    task->context.sp = (uint64_t)task->stack_top;
    task->context.x30 = (uint64_t)task_entry_wrapper;  /* Return address */
    task->context.x29 = 0;                              /* Frame pointer */
    task->context.daif = 0x080;  /* IRQ masked (DAIF I-bit set). Critical for
                                  * correctness: context.S restores DAIF early
                                  * in the switch sequence, before GP registers
                                  * and SP are fully loaded. With DAIF=0, the
                                  * timer ISR fires mid-restore and corrupts the
                                  * partially restored context. Setting 0x080
                                  * keeps IRQs masked until the task is fully
                                  * running; task code unmasks naturally via
                                  * spin_unlock_irqrestore or explicit DAIF
                                  * clear. */

    /* Store entry point and arg in callee-saved registers for wrapper */
    task->context.x19 = (uint64_t)entry;
    task->context.x20 = (uint64_t)arg;
#endif /* PLATFORM_X86_64 */

    /* No cleanup callback by default */
    task->cleanup = NULL;
    task->cleanup_arg = NULL;

    /* User-mode defaults — task_create_user overrides is_user +
     * user_entry; #697 PR-3 will populate user_l1_pa. Explicit init
     * guards against stale-slot reuse. */
    task->is_user = 0;
    task->user_entry = NULL;
    task->user_l1_pa = 0;
    task->user_stack_top = 0;
    task->user_stack_phys = 0;
    task->user_va_next = 0;
    task->user_asid = 0;

    /* Clean the context struct to PoC so a secondary CPU can read it
     * during switch_to(). Without SMPEN, task_create's writes to
     * context.sp, context.x30, etc. stay in this CPU's L1 cache.
     * scheduler_add_task_to_cpu() intentionally skips cleaning the
     * context (to avoid overwriting a running task's live state),
     * so we must clean it here at creation time. */
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean_range(&task->context, sizeof(task->context));
#endif

    /* Skip DEBUG_PRINT on secondary CPUs — uart_lock contention with
     * CPU 0's boot output causes deadlock/hang on real hardware. */
#if defined(PLATFORM_HAS_NC_MEMORY)
    {
        uint64_t _mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(_mpidr));
        if (cpu_logical_id(_mpidr) == 0) {
            DEBUG_PRINT("Created task '%s' (id=%u, stack=%p-%p, priority=%u)",
                        task->name, task->id, task->stack_base, task->stack_top,
                        task->priority);
        }
    }
#else
    DEBUG_PRINT("Created task '%s' (id=%u, stack=%p-%p, priority=%u)",
                task->name, task->id, task->stack_base, task->stack_top,
                task->priority);
#endif

    return task;
}

/*
 * Create a new task with default priority.
 */
struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    return task_create_with_priority(name, entry, arg, TASK_PRIORITY_DEFAULT);
}

#if !defined(PLATFORM_X86_64)
/*
 * Assembly trampoline that transitions from EL1 to EL0 via ERET.
 * Defined in user_entry.S.
 */
extern void user_task_enter(void *entry, void *stack_top, void *arg);

/*
 * Kernel-mode wrapper for user tasks.
 *
 * When switch_to() restores this task for the first time, x19=entry, x20=arg.
 * task_entry_trampoline calls this function, which then ERETsm into EL0.
 *
 * Note: At this point we are at EL1 with interrupts masked. user_task_enter
 * sets SPSR to EL0t with interrupts enabled, so ERET unmasks them.
 */
static void user_task_wrapper(void *arg)
{
    struct task *t = task_current();
    if (!t || !t->user_entry || !t->user_stack_top) {
        task_exit();
        return;
    }

    /* ERET to EL0 — does not return. SP_EL0 = the per-task EL0 stack
     * top (mapped into the per-task L1 with VMM_FLAG_USER), NOT the
     * kernel stack at t->stack_top — EL0 cannot access kernel VAs. */
    user_task_enter((void *)(uintptr_t)t->user_entry,
                    (void *)(uintptr_t)t->user_stack_top,
                    arg);

    /* Should never reach here */
    task_exit();
}

/*
 * Create a new user-mode (EL0) task.
 *
 * The task starts in kernel mode (via task_entry_wrapper) then
 * transitions to EL0 via ERET. Syscalls (SVC #0) return to EL1.
 */
/* Linker-defined .text.user range (#697 PR-4). The section holds the
 * EL0-runnable code+rodata pages; task_create_user maps PA→user VA at
 * USER_TEXT_VA so the ERET target lands inside the user window. */
extern char __text_user_start[];
extern char __text_user_end[];

struct task *task_create_user(const char *name, task_entry_t user_entry,
                              void *arg, uint8_t priority)
{
    /* Allocate the per-task L1 page table BEFORE the task slot. If the
     * allocation fails we never publish a half-initialized task; if the
     * task allocation fails we tear the L1 back down on the same path.
     * vmm_create_user_l1 mirrors the boot L1's kernel entries (L1[0..255])
     * and zeros the user window (L1[256..511]); subsequent user mappings
     * land in this L1 without touching the kernel's. */
    uint64_t user_l1_pa = 0;
    if (vmm_create_user_l1(&user_l1_pa) != 0) {
        ERROR("task_create_user: vmm_create_user_l1 failed");
        return NULL;
    }

    uint16_t user_asid = vmm_alloc_asid();
    if (!user_asid) {
        ERROR("task_create_user: ASID pool exhausted");
        vmm_destroy_user_l1(user_l1_pa);
        return NULL;
    }

    /* Map every 4 KB page of .text.user into the per-task L1 at
     * USER_TEXT_VA, RX user. Multiple user tasks share the same backing
     * pages (the section is read-only and execute-only at EL0), so no
     * copy is needed. */
    uintptr_t text_user_kva = (uintptr_t)__text_user_start;
    size_t text_user_bytes = (size_t)(__text_user_end - __text_user_start);
    if ((text_user_kva & (PAGE_SIZE - 1)) != 0 ||
        (text_user_bytes & (PAGE_SIZE - 1)) != 0 ||
        text_user_bytes == 0) {
        ERROR("task_create_user: .text.user is not page-aligned/sized "
              "(start=0x%lx, size=0x%zx)", text_user_kva, text_user_bytes);
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }
    for (size_t off = 0; off < text_user_bytes; off += PAGE_SIZE) {
        uint64_t pa = (uint64_t)text_user_kva + off;
        uint64_t va = USER_TEXT_VA + off;
        if (vmm_user_map_page(user_l1_pa, va, pa,
                              VMM_FLAGS_USER_CODE) != 0) {
            ERROR("task_create_user: failed to map .text.user page "
                  "VA=0x%lx PA=0x%lx", va, pa);
            vmm_destroy_user_l1(user_l1_pa);
            vmm_free_asid(user_asid);
            return NULL;
        }
    }

    /* Allocate a single 4 KB EL0 stack page from PMM and map it RW
     * user at USER_STACK_PAGE_VA. Per-task; not shared. The PMM_OWNED
     * flag tells vmm_destroy_user_l1 to free this leaf when the task
     * is destroyed — no explicit user_stack_phys tracking needed. */
    void *user_stack_page = pmm_alloc_pages(1);
    if (!user_stack_page) {
        ERROR("task_create_user: failed to allocate user stack page");
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }
    if (vmm_user_map_page(user_l1_pa, USER_STACK_PAGE_VA,
                          (uint64_t)(uintptr_t)user_stack_page,
                          VMM_FLAGS_USER_DATA | VMM_FLAG_PMM_OWNED) != 0) {
        ERROR("task_create_user: failed to map user stack page");
        pmm_free_pages(user_stack_page, 1);
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }

    /* Translate the linker-resolved kernel VA of `user_entry` to its
     * user VA inside the mapped .text.user window. Out-of-range entries
     * fall through unchanged — that supports unit tests that pass a
     * kernel-only stub (e.g. task_exit) and immediately TERMINATE the
     * task without ever ERETing to EL0. */
    uintptr_t entry_kva = (uintptr_t)user_entry;
    uint64_t entry_va;
    if (entry_kva >= text_user_kva && entry_kva < text_user_kva + text_user_bytes) {
        entry_va = USER_TEXT_VA + (entry_kva - text_user_kva);
    } else {
        entry_va = entry_kva;
    }

    /* Create the kernel-side task slot last. If alloc fails we tear
     * down everything we've built up to this point. */
    struct task *task = task_create_with_priority(name, user_task_wrapper,
                                                   arg, priority);
    if (!task) {
        pmm_free_pages(user_stack_page, 1);
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }

    /* Mark as user-mode and stash the per-task address-space state.
     * No scheduler hand-off has happened yet (caller is responsible
     * for `scheduler_add_task`), so these stores can't race with
     * schedule() picking the task. */
    task->is_user = 1;
    task->user_entry = (void (*)(void *))(uintptr_t)entry_va;
    task->user_l1_pa = user_l1_pa;
    task->user_stack_top = USER_STACK_TOP;
    task->user_stack_phys = (uint64_t)(uintptr_t)user_stack_page;
    task->user_va_next = USER_MMAP_VA_START;
    task->user_asid = user_asid;

    return task;
}

/*
 * Create a user-mode task from a static ARM64 ELF blob.
 *
 * Sibling of task_create_user. Differences:
 *   - text/rodata/data come from PT_LOAD segments mapped by
 *     elf_load_user (each page PMM_OWNED), not the linker's
 *     `.text.user` window.
 *   - the user stack lives at USER_ELF_STACK_PAGE_VA (just below
 *     the mmap window) so it cannot collide with multi-page ELF
 *     segments. user_stack_top mirrors that VA.
 *   - the entry point is whatever ELF e_entry pointed at, used as
 *     the ERET target directly (no kernel→user VA translation).
 */
struct task *task_create_user_elf(const char *name,
                                  const void *blob, size_t blob_len,
                                  uint8_t priority)
{
    if (!blob || blob_len == 0) {
        return NULL;
    }

    uint64_t user_l1_pa = 0;
    if (vmm_create_user_l1(&user_l1_pa) != 0) {
        ERROR("task_create_user_elf: vmm_create_user_l1 failed");
        return NULL;
    }

    uint16_t user_asid = vmm_alloc_asid();
    if (!user_asid) {
        ERROR("task_create_user_elf: ASID pool exhausted");
        vmm_destroy_user_l1(user_l1_pa);
        return NULL;
    }

    uint64_t entry_va = 0;
    int rc = elf_load_user(blob, blob_len, user_l1_pa, &entry_va);
    if (rc != ELF_OK) {
        ERROR("task_create_user_elf: elf_load_user failed: %s",
              elf_strerror(rc));
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }

    /* Allocate the EL0 stack page and map it RW-user at the ELF
     * stack VA. PMM_OWNED so vmm_destroy_user_l1 reclaims it
     * alongside the ELF segments at task teardown. */
    void *user_stack_page = pmm_alloc_pages(1);
    if (!user_stack_page) {
        ERROR("task_create_user_elf: failed to allocate user stack page");
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }
    if (vmm_user_map_page(user_l1_pa, USER_ELF_STACK_PAGE_VA,
                          (uint64_t)(uintptr_t)user_stack_page,
                          VMM_FLAGS_USER_DATA | VMM_FLAG_PMM_OWNED) != 0) {
        ERROR("task_create_user_elf: failed to map user stack page");
        pmm_free_pages(user_stack_page, 1);
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }

    struct task *task = task_create_with_priority(name, user_task_wrapper,
                                                   NULL, priority);
    if (!task) {
        pmm_free_pages(user_stack_page, 1);
        vmm_destroy_user_l1(user_l1_pa);
        vmm_free_asid(user_asid);
        return NULL;
    }

    task->is_user = 1;
    task->user_entry = (void (*)(void *))(uintptr_t)entry_va;
    task->user_l1_pa = user_l1_pa;
    task->user_stack_top = USER_ELF_STACK_TOP;
    task->user_stack_phys = (uint64_t)(uintptr_t)user_stack_page;
    task->user_va_next = USER_MMAP_VA_START;
    task->user_asid = user_asid;

    return task;
}
#endif /* !PLATFORM_X86_64 */

/*
 * Terminate the current task.
 */
void task_exit(void)
{
    struct task *task = task_current();

    /* Only print from CPU 0 — secondary CPUs don't have a cross-CPU
     * UART lock, so printing from multiple CPUs causes garbled output
     * and potential hangs. */
    if (cpu_id() == 0) {
        INFO("Task '%s' (id=%u) exiting", task->name, task->id);
    }

    /* Mask IRQs to prevent a timer-driven schedule() from racing with
     * the state change below. Without this, the timer can fire between
     * setting TASK_TERMINATED and scheduler_terminate_task(), causing
     * schedule() to find a terminated task still in the run queue
     * (pick_next_task returns it, next == current → panic). */
    arch_irq_disable();

#ifdef CONFIG_AI_SCHEDULER
    {
        extern void sched_ai_record_completion(struct task *task);
        sched_ai_record_completion(task);
    }
#endif

    /* Atomically set TASK_TERMINATED and dequeue under rq_lock.
     * Without this, a cross-CPU scheduler on Pi 5 (no SMPEN, per-core L2)
     * could observe the task still in the queue after the state flip but
     * before dequeue runs, picking a terminated task and panicking. */
    scheduler_terminate_task(task);

    /* Context-switch away. schedule() -> spin_lock_irqsave saves our
     * masked DAIF state. The context switch to the next task restores
     * that task's DAIF. */
    schedule();

    /* Should never reach here */
    panic("task_exit: schedule returned!");
}

/*
 * Get current running task (for this CPU).
 *
 * On PLATFORM_HAS_NC_MEMORY, current_task is in non-cacheable memory —
 * every read bypasses L1/L2 and hits DRAM directly, so writes from other
 * CPUs are instantly visible without cache maintenance.
 */
struct task *task_current(void)
{
    uint32_t cpu = cpu_id();
    return task_current_on_cpu(cpu);
}

struct task *task_current_on_cpu(uint32_t cpu)
{
    if (cpu >= MAX_CPUS) {
        return NULL;
    }
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_invalidate(&current_task[cpu]);
#endif
    return current_task[cpu];
}

/*
 * Set current running task (called by scheduler).
 */
void task_set_current(struct task *task)
{
    uint32_t cpu = cpu_id();
    current_task[cpu] = task;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&current_task[cpu]);
#endif
}

/*
 * Get task by ID.
 */
struct task *task_get(uint32_t id)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (task_table[i].id == id) {
            return &task_table[i];
        }
    }
    return NULL;
}

/*
 * Get task by table slot index.
 */
struct task *task_slot(uint32_t idx)
{
    if (idx >= MAX_TASKS) {
        return NULL;
    }
    return &task_table[idx];
}

/*
 * Free a terminated task's resources.
 *
 * This function reclaims the task's stack memory and frees the task slot
 * for reuse. Must only be called after the task has been removed from
 * all run queues and is no longer running.
 */
void task_destroy(struct task *task)
{
    if (!task) {
        return;
    }

    /* Idle-task guard (#200/#606 investigation, 2026-05-02 / 03).
     * Refuse to destroy an idle task — destroying one would leave
     * the owning CPU with no fallback for `pick_next_task`, leading
     * to the `terminated/destroyed task selected as next` panic
     * seen on pre-PR-598 baseline runs. Uses the O(1) flag-bit
     * predicate `is_idle_task()` (Linux PF_IDLE pattern) — replaces
     * an earlier loop-over-cpu_rq pointer comparison. */
    if (is_idle_task(task)) {
        panic("task_destroy: refusing to destroy idle task "
              "(task='%s', cpu_affinity=%u)", task->name, task->cpu_affinity);
    }

    TASK_LOCK_IRQSAVE();

    /* Verify task is terminated */
    if (task->state != TASK_TERMINATED) {
        if (cpu_id() == 0) {
            WARN("task_destroy: task '%s' not terminated (state=%d)",
                 task->name, task->state);
        }
        TASK_UNLOCK_IRQRESTORE();
        return;
    }

    /* Capture info before clearing */
    void *stack = task->stack_base;
    uint32_t task_id = task->id;
    char task_name[TASK_NAME_LEN];
    str_copy(task_name, task->name, TASK_NAME_LEN);
    task_cleanup_t cleanup = task->cleanup;
    void *cleanup_arg = task->cleanup_arg;
    uint64_t user_l1_pa = task->user_l1_pa;
    uint64_t user_stack_phys = task->user_stack_phys;
    uint16_t user_asid = task->user_asid;

    /* Clear task slot (marks as free: id == 0) */
    task->id = 0;
    task->name[0] = '\0';
    task->stack_base = NULL;
    task->stack_top = NULL;
    task->cleanup = NULL;
    task->cleanup_arg = NULL;
    task->is_user = 0;
    task->user_entry = NULL;
    task->user_l1_pa = 0;
    task->user_stack_top = 0;
    task->user_stack_phys = 0;
    task->user_va_next = 0;
    task->user_asid = 0;

    /* Bump the slot generation (#139) so any still-cached captures in
     * per-CPU steal deques from the previous life of this slot will
     * fail the validator when a thief tries to accept them. Wraps
     * naturally — collisions require 2^32 reuses of the same slot
     * between a push and a still-outstanding steal probe, which is
     * not reachable by a realistic workload.
     *
     * On non-NC platforms the bumped generation must be flushed to
     * PoC for cross-CPU thieves to observe it; otherwise a thief on
     * another CPU validating a captured generation against
     * `task->generation` reads a stale cacheline and accepts a slot
     * that was just recycled. NC platforms (Pi 5, Jetson) keep
     * task_table in non-cacheable memory so the write is visible
     * without maintenance. */
    task->generation++;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->generation);
#endif

    TASK_UNLOCK_IRQRESTORE();

    /* Call cleanup callback first (e.g., to free ELF segment memory) */
    if (cleanup) {
        cleanup(cleanup_arg);
    }

    /* Free stack outside lock - pmm has its own locking */
    if (stack) {
        size_t stack_pages = STACK_SIZE / 4096;
        pmm_free_pages(stack, stack_pages);
    }

#if !defined(PLATFORM_X86_64)
    /* Free the per-task L1, all user-region L2/L3 sub-tables, and
     * any L3 leaf pages flagged VMM_FLAG_PMM_OWNED — that's the EL0
     * stack page allocated in task_create_user, plus any pages the
     * task mapped via mmap-style helpers. .text.user pages (kernel-
     * image PA, not PMM-owned) are left alone. */
    if (user_l1_pa) {
        vmm_destroy_user_l1(user_l1_pa);
    }
    /* Free the ASID after destroying the L1 — vmm_destroy_user_l1
     * doesn't touch TTBR0 (it just walks the L1 tree freeing pages),
     * so any other CPU that might still be running this task is the
     * scheduler's concern, not ours. The ASID-recycle TLB flush
     * happens at vmm_alloc_asid time when the slot is reused. */
    if (user_asid) {
        vmm_free_asid(user_asid);
    }
    (void)user_stack_phys;  /* now reclaimed by vmm_destroy_user_l1 */
#else
    (void)user_l1_pa;
    (void)user_stack_phys;
#endif

    /* Note: DEBUG_PRINT removed here to avoid output interleaving issues
     * during test runs with concurrent task destruction across multiple CPUs.
     * Task lifecycle is well-tested; enable if debugging task issues. */
    (void)task_name;
    (void)task_id;
}

/*
 * Set task CPU affinity.
 */
void task_set_affinity(struct task *task, uint32_t cpu)
{
    if (!task) return;

    task->cpu_affinity = cpu;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->cpu_affinity);
#endif

    /* If pinning to a specific CPU, update assigned_cpu */
    if (cpu != CPU_AFFINITY_ANY && cpu < cpu_count) {
        task->assigned_cpu = cpu;
#if !defined(PLATFORM_HAS_NC_MEMORY)
        cache_clean(&task->assigned_cpu);
#endif
    }
}

/*
 * Get task CPU affinity.
 */
uint32_t task_get_affinity(struct task *task)
{
    if (!task) return CPU_AFFINITY_ANY;
    return task->cpu_affinity;
}

/*
 * Set task priority.
 */
void task_set_priority(struct task *task, uint8_t priority)
{
    if (!task) return;

    /* Clamp to valid range */
    if (priority > TASK_PRIORITY_MAX) {
        priority = TASK_PRIORITY_MAX;
    }

    task->priority = priority;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->priority);
#endif

    /* Update effective priority (may be boosted by deadline) */
    if (task->effective_priority < priority) {
        task->effective_priority = priority;
    }
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->effective_priority);
#endif
}

/*
 * Get task priority.
 */
uint8_t task_get_priority(struct task *task)
{
    if (!task) return TASK_PRIORITY_NORMAL;
    return task->priority;
}

/*
 * Get effective task priority (includes deadline boost).
 */
uint8_t task_get_effective_priority(struct task *task)
{
    if (!task) return TASK_PRIORITY_NORMAL;
    return task->effective_priority;
}

/*
 * Set task deadline.
 */
void task_set_deadline(struct task *task, uint64_t deadline_ns)
{
    if (!task) return;
    task->deadline_ns = deadline_ns;
#if !defined(PLATFORM_HAS_NC_MEMORY)
    cache_clean(&task->deadline_ns);
#endif
}

/*
 * Get task deadline.
 */
uint64_t task_get_deadline(struct task *task)
{
    if (!task) return 0;
    return task->deadline_ns;
}

/*
 * Set task cleanup callback.
 */
void task_set_cleanup(struct task *task, task_cleanup_t cleanup, void *cleanup_arg)
{
    if (!task) return;
    task->cleanup = cleanup;
    task->cleanup_arg = cleanup_arg;
}

/* ========================================================================
 * Stack canary diagnostic — #601 Bug B investigation
 * ====================================================================== */

void task_canary_init(struct task *task)
{
    if (!task || !task->stack_base) return;
    uint64_t *p = (uint64_t *)task->stack_base;
    const uint32_t n = TASK_STACK_CANARY_BYTES / sizeof(uint64_t);
    for (uint32_t i = 0; i < n; i++) {
        p[i] = TASK_STACK_CANARY_PATTERN;
    }
}

static int task_canary_check_impl(struct task *task, bool unlocked)
{
    if (!task || !task->stack_base || task->id == 0) return 0;
    const uint64_t *p = (const uint64_t *)task->stack_base;
    const uint32_t n = TASK_STACK_CANARY_BYTES / sizeof(uint64_t);
    int broken = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == TASK_STACK_CANARY_PATTERN) continue;
        broken = 1;
        /* Log offset + actual value + ASCII interpretation. */
        uint64_t v = p[i];
        char ascii[9];
        for (int j = 0; j < 8; j++) {
            uint8_t b = (uint8_t)(v >> (j * 8));
            ascii[j] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
        }
        ascii[8] = '\0';
        if (unlocked) {
            uart_printf_unlocked(
                "[canary] BROKEN task='%s' id=%u stack_base=%p "
                "+0x%lx: 0x%lx  \"%s\"\n",
                task->name, task->id, task->stack_base,
                (unsigned long)(i * sizeof(uint64_t)),
                (unsigned long)v, ascii);
        } else {
            uart_printf("[canary] BROKEN task='%s' id=%u stack_base=%p "
                        "+0x%lx: 0x%lx  \"%s\"\n",
                        task->name, task->id, task->stack_base,
                        (unsigned long)(i * sizeof(uint64_t)),
                        (unsigned long)v, ascii);
        }
    }
    return broken;
}

int task_canary_check(struct task *task)
{
    return task_canary_check_impl(task, false);
}

/* Shared implementation. `unlocked` selects between the locked
 * uart_printf path (safe from normal task context) and the
 * uart_printf_unlocked path (safe from panic context where the
 * UART lock cannot be held). See task.h for the public callers. */
static int task_canary_check_all_impl(bool unlocked)
{
    int broken_count = 0;
    /* Iterate without taking the task_lock — this is observation only,
     * a torn read of `id` just means we miss a transient zero or new
     * task, which is acceptable for diagnostic purposes. */
    if (unlocked) {
        uart_printf_unlocked("[canary] Task stack inventory:\n");
    } else {
        uart_printf("[canary] Task stack inventory:\n");
    }
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct task *t = &task_table[i];
        if (t->id == 0) continue;
        if (!t->stack_base) continue;
        if (unlocked) {
            uart_printf_unlocked("  id=%u name='%s' stack=[%p..%p)\n",
                                 t->id, t->name, t->stack_base, t->stack_top);
        } else {
            uart_printf("  id=%u name='%s' stack=[%p..%p)\n",
                        t->id, t->name, t->stack_base, t->stack_top);
        }
        if (task_canary_check_impl(t, unlocked)) {
            broken_count++;
        }
    }
    return broken_count;
}

int task_canary_check_all(void)
{
    return task_canary_check_all_impl(false);
}

int task_canary_check_all_unlocked(void)
{
    return task_canary_check_all_impl(true);
}
