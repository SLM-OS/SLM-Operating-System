/*
 * shell_sys.c - System and debug commands for SLM-OS shell
 *
 * Commands: help, mem, tasks, cpu, uptime, clear, reboot, vmm, ipc, model, dtb
 */

#include "shell.h"
#include "shell_internal.h"
#include "uart.h"
#include "task.h"
#include "sched.h"
#include "sched_policy.h"
#include "sched_trace.h"
#ifdef CONFIG_AI_SCHEDULER
#include "ai_types.h"
#endif
#include "pmm.h"
#include "vmm.h"
#include "smp.h"
#include "ipc.h"
#include "timer.h"
#include "slm_ffi.h"
#include "platform.h"
#include "ncmem.h"
#include "dtb.h"
#include "help.h"
#include "string.h"
#include "../gpu/gpu.h"
#include <stdint.h>

/* Command table access (defined in shell.c) */
extern const shell_cmd_t builtin_commands[];
extern const int NUM_BUILTIN_COMMANDS;
extern shell_cmd_t external_commands[];
extern int num_external_commands;

/*
 * help - List available commands or show detailed help
 *
 * Usage:
 *   help          - List all commands with brief descriptions
 *   help <cmd>    - Show detailed help for a specific command
 */
int cmd_help(int argc, char *argv[])
{
    /* If a command name is given, show detailed help from file */
    if (argc >= 2) {
        return help_show(argv[1]);
    }

    /* Otherwise, list all commands with brief descriptions */
    shell_puts("Available commands:\r\n");
    shell_puts("\r\n");

    /* Built-in commands */
    for (int i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
        shell_printf("  %-10s %s\r\n",
                    builtin_commands[i].name,
                    builtin_commands[i].help);
    }

    /* External commands */
    for (int i = 0; i < num_external_commands; i++) {
        shell_printf("  %-10s %s\r\n",
                    external_commands[i].name,
                    external_commands[i].help);
    }

    shell_puts("\r\n");
    shell_puts("Use 'help <cmd>' for detailed help on a command.\r\n");
    return 0;
}

/*
 * mem - Show memory statistics
 */
int cmd_mem(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    size_t total_pages = pmm_get_total_pages();
    size_t free_pages = pmm_get_free_pages();
    size_t used_pages = total_pages - free_pages;

    size_t page_size = 4096;
    size_t total_kb = (total_pages * page_size) / 1024;
    size_t free_kb = (free_pages * page_size) / 1024;
    size_t used_kb = (used_pages * page_size) / 1024;

    shell_puts("Memory Statistics:\r\n");
    shell_puts("\r\n");
    shell_printf("  Total:     %lu KB (%lu pages)\r\n", total_kb, total_pages);
    shell_printf("  Used:      %lu KB (%lu pages)\r\n", used_kb, used_pages);
    shell_printf("  Free:      %lu KB (%lu pages)\r\n", free_kb, free_pages);
    shell_puts("\r\n");

    return 0;
}

/*
 * State name lookup
 */
static const char *state_name(task_state_t state)
{
    switch (state) {
        case TASK_READY:      return "READY";
        case TASK_RUNNING:    return "RUNNING";
        case TASK_BLOCKED:    return "BLOCKED";
        case TASK_TERMINATED: return "TERMINATED";
        default:              return "UNKNOWN";
    }
}

/*
 * tasks - List all tasks
 */
int cmd_tasks(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    shell_puts("Task List:\r\n");
    shell_puts("\r\n");
    shell_puts("  ID  Name             State       Pri  CPU  Switches\r\n");
    shell_puts("  --  ---------------  ----------  ---  ---  --------\r\n");

    /* Iterate slots, not task IDs — IDs come from a monotonic counter
     * and can exceed MAX_TASKS (#321). */
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        struct task *t = task_slot(i);
        if (t != NULL && t->id != 0 && t->state != TASK_TERMINATED) {
            shell_printf("  %2lu  %-15s  %-10s  %3u  %3lu  %lu\r\n",
                        t->id,
                        t->name,
                        state_name(t->state),
                        t->effective_priority,
                        t->assigned_cpu,
                        t->switches);
        }
    }

    shell_puts("\r\n");
    return 0;
}

/*
 * cpu - Show CPU status
 */
int cmd_cpu(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    shell_puts("CPU Status:\r\n");
    shell_puts("\r\n");
    shell_printf("  Platform:    %s\r\n", PLATFORM_NAME);
    shell_printf("  CPUs:        %lu online / %lu total\r\n", cpus_online, cpu_count);
    shell_puts("\r\n");

    shell_puts("  CPU  Status   Isolated  Current Task\r\n");
    shell_puts("  ---  ------   --------  ------------\r\n");

    for (uint32_t i = 0; i < cpu_count; i++) {
        bool online = (i < cpus_online);
        bool isolated = sched_is_core_isolated(i);
        struct task *current = NULL;

        /* Get current task for this CPU (if we can) */
        /* Note: This is a snapshot and may be racy */
        if (online && i == cpu_id()) {
            current = task_current();
        }

        shell_printf("  %3lu  %-6s   %-8s  %s\r\n",
                    i,
                    online ? "online" : "offline",
                    isolated ? "yes" : "no",
                    current ? current->name : "-");
    }

    /* Per-CPU scheduler diagnostics */
    {
#if defined(PLATFORM_HAS_NC_MEMORY)
        extern volatile uint32_t *sched_diag_tick;
        extern volatile uint32_t *sched_diag_schedule;
        extern volatile uint32_t *sched_diag_picked;
#else
        extern volatile uint32_t sched_diag_tick[];
        extern volatile uint32_t sched_diag_schedule[];
        extern volatile uint32_t sched_diag_picked[];
#endif
        extern volatile uint32_t timer_handler_count;
        shell_printf("\r\n  Per-CPU scheduler diagnostics:\r\n");
        shell_printf("  CPU  Ticks     Schedule  Picked    IdleLoops\r\n");
        shell_printf("  ---  --------  --------  ------    ---------\r\n");
#if defined(PLATFORM_HAS_NC_MEMORY)
        extern volatile uint32_t *sched_diag_idle_loops;
#else
        extern volatile uint32_t sched_diag_idle_loops[];
#endif
        shell_printf("  diag_tick ptr=0x%lx val[0]=0x%x\r\n",
                    (unsigned long)(uintptr_t)sched_diag_tick,
                    *(volatile uint32_t *)sched_diag_tick);
        for (uint32_t i = 0; i < cpu_count; i++) {
            uint32_t t = sched_diag_tick[i];
            uint32_t s = sched_diag_schedule[i];
            uint32_t p = sched_diag_picked[i];
            uint32_t il = sched_diag_idle_loops[i];
            shell_printf("  %3lu  %8u  %8u  %6u    %9u\r\n", i, t, s, p, il);
        }
        shell_printf("  timer_handler_count: %u\r\n", timer_handler_count);

#if CONFIG_WORK_STEALING
        /* Work-stealing per-CPU counters (#105). Written by the
         * thief in sched_try_steal; helpful for tuning Phase C
         * benchmarks and deciding whether CONFIG_WORK_STEALING should
         * default to ON. */
#if defined(PLATFORM_HAS_NC_MEMORY)
        extern volatile uint32_t *sched_diag_steal_attempts;
        extern volatile uint32_t *sched_diag_steal_successes;
        extern volatile uint32_t *sched_diag_steal_stale;
        extern volatile uint32_t *sched_diag_steal_empty_victim;
        extern volatile uint32_t *sched_diag_steal_push_full;
#else
        extern volatile uint32_t sched_diag_steal_attempts[];
        extern volatile uint32_t sched_diag_steal_successes[];
        extern volatile uint32_t sched_diag_steal_stale[];
        extern volatile uint32_t sched_diag_steal_empty_victim[];
        extern volatile uint32_t sched_diag_steal_push_full[];
#endif
        shell_printf("\r\n  Per-CPU work-stealing counters:\r\n");
        shell_printf("  CPU  Attempts  Success   Stale     EmptyVic  PushFull\r\n");
        shell_printf("  ---  --------  --------  --------  --------  --------\r\n");
        for (uint32_t i = 0; i < cpu_count; i++) {
            shell_printf("  %3lu  %8u  %8u  %8u  %8u  %8u\r\n",
                        i,
                        sched_diag_steal_attempts[i],
                        sched_diag_steal_successes[i],
                        sched_diag_steal_stale[i],
                        sched_diag_steal_empty_victim[i],
                        sched_diag_steal_push_full[i]);
        }
#endif /* CONFIG_WORK_STEALING */

#if !defined(PLATFORM_X86_64)
        /* Show secondary CPU TTBR0 values (stored in boot_flag slots) */
        {
            extern volatile uint32_t cpu_boot_flag[];
            uint64_t my_ttbr0;
            __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(my_ttbr0));
            shell_printf("  CPU 0 TTBR0: 0x%lx\r\n", (unsigned long)my_ttbr0);
            for (uint32_t i = 1; i < cpu_count; i++) {
                uint32_t val = __atomic_load_n(&cpu_boot_flag[i], __ATOMIC_ACQUIRE);
#if defined(PLATFORM_HAS_NC_MEMORY)
                volatile uint32_t *nc_dbg_addr = (volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + i * 4);
                uint32_t nc_dbg = *nc_dbg_addr;
                uint32_t nc_flag_read = *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 192 + i * 4);
                shell_printf("  CPU %u boot: 0x%x  NC_DBG@%lx: 0x%x  NC_FLAG_READ: 0x%x\r\n",
                            i, val, (unsigned long)nc_dbg_addr, nc_dbg, nc_flag_read);
#else
                shell_printf("  CPU %u boot: 0x%x\r\n", i, val);
#endif
            }
#if defined(PLATFORM_HAS_NC_MEMORY)
            /* Write-readback test: CPU 0 writes to NC_DBG[0], reads back */
            {
                volatile uint32_t *test_addr = (volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256);
                uint32_t before = *test_addr;
                *test_addr = 0xDEAD;
                uint32_t after = *test_addr;
                *test_addr = before;  /* restore */
                /* Also read nc_flag directly */
                uint32_t nc_flag_val = *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 64);
                shell_printf("  NC test@%lx: before=0x%x wrote=0xDEAD read=0x%x nc_flag=%u\r\n",
                            (unsigned long)test_addr, before, after, nc_flag_val);
            }
#endif
        }
#endif
    }

#if defined(PLATFORM_RASPI5)
    {
        extern int uart_is_irq_mode(void);
        extern volatile uint32_t uart_irq_count;
        shell_printf("\r\n  UART RX:     %s (irq_count=%u)\r\n",
                    uart_is_irq_mode() ? "interrupt-driven" : "polling",
                    uart_irq_count);

        /* GIC pending check for UART IRQ */
        uint32_t pend_reg = UART_IRQ / 32;
        uint32_t pend_bit = UART_IRQ % 32;
        volatile uint32_t *ispendr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x200 + 4 * pend_reg);
        int pending = (*ispendr >> pend_bit) & 1;

        /* MIP status */
        volatile uint32_t *mip_status = (volatile uint32_t *)(MIP0_BASE + 0x80);  /* STATUSL_HOST */
        uint32_t mip_st = *mip_status;

        /* RP1 INTSTAT */
        volatile uint32_t *intstatl = (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_INTSTATL);
        uint32_t rp1_st = *intstatl;

        /* PL011 RIS/MIS */
        volatile uint32_t *ris = (volatile uint32_t *)(UART_BASE + 0x3C);
        volatile uint32_t *mis = (volatile uint32_t *)(UART_BASE + 0x40);

        shell_printf("  GIC pend:    %d  MIP status: 0x%x  RP1 INTSTAT: 0x%x\r\n",
                    pending, mip_st, rp1_st);
        shell_printf("  PL011 RIS:   0x%x  MIS: 0x%x\r\n", *ris, *mis);

        /* MSIX_CFG register for UART0 vector */
        volatile uint32_t *msix_cfg = (volatile uint32_t *)(RP1_INTC_BASE + RP1_MSIX_CFG(RP1_INT_UART0));
        shell_printf("  MSIX_CFG[25]:0x%x\r\n", *msix_cfg);

        /* Read MSI-X capability from config space (at 0x8000+0xB0) */
        volatile uint32_t *msix_cap = (volatile uint32_t *)(PCIE_RC_BASE + 0x8000 + 0xB0);
        shell_printf("  MSI-X cap:   0x%x (Enable=%d FuncMask=%d)\r\n",
                    *msix_cap, (*msix_cap >> 31) & 1, (*msix_cap >> 30) & 1);

        /* Read MSI-X table entry 25 */
        volatile uint32_t *e25 = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + 25 * 16);
        shell_printf("  MSIX tbl[25]:addr=0x%x_%08x data=0x%x ctrl=0x%x\r\n",
                    e25[1], e25[0], e25[2], e25[3]);

        /* GIC target and priority for UART IRQ */
        uint32_t tgt_reg = UART_IRQ / 4;
        uint32_t tgt_off = (UART_IRQ % 4) * 8;
        volatile uint32_t *itargets = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x800 + 4 * tgt_reg);
        uint8_t target = (*itargets >> tgt_off) & 0xFF;
        uint32_t pri_reg = UART_IRQ / 4;
        uint32_t pri_off = (UART_IRQ % 4) * 8;
        volatile uint32_t *ipriority = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x400 + 4 * pri_reg);
        uint8_t priority = (*ipriority >> pri_off) & 0xFF;
        /* Active state */
        uint32_t act_reg = UART_IRQ / 32;
        uint32_t act_bit = UART_IRQ % 32;
        volatile uint32_t *isactiver = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x300 + 4 * act_reg);
        int active = (*isactiver >> act_bit) & 1;
        /* Enable state */
        volatile uint32_t *isenabler = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x100 + 4 * act_reg);
        int enabled = (*isenabler >> act_bit) & 1;

        shell_printf("  GIC IRQ %d: target=0x%x pri=0x%x active=%d enabled=%d\r\n",
                    UART_IRQ, target, priority, active, enabled);

        /* GICC state from EL1 */
        volatile uint32_t *gicc_ctlr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE);
        volatile uint32_t *gicc_pmr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x4);
        volatile uint32_t *gicc_bpr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x8);
        volatile uint32_t *gicc_rpr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x14);
        volatile uint32_t *gicc_hppir = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x18);
        volatile uint32_t *gicc_ahppir = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x28);
        shell_printf("  GICC: CTLR=0x%x PMR=0x%x BPR=0x%x RPR=0x%x HPPIR=%u AHPPIR=%u\r\n",
                    *gicc_ctlr, *gicc_pmr, *gicc_bpr, *gicc_rpr, *gicc_hppir, *gicc_ahppir);

        /* IGROUPR for our SPI */
        uint32_t grp_reg = UART_IRQ / 32;
        uint32_t grp_bit = UART_IRQ % 32;
        volatile uint32_t *igroupr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x080 + 4 * grp_reg);
        int grp1 = (*igroupr >> grp_bit) & 1;
        /* Also check GICD_CTLR */
        volatile uint32_t *gicd_ctlr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE);
        shell_printf("  GICD: CTLR=0x%x IGROUPR[%d] bit %d=%d\r\n",
                    *gicd_ctlr, grp_reg, grp_bit, grp1);
        /* Dump all IGROUPR registers to see which are Group 0 vs Group 1 */
        for (uint32_t g = 0; g < 10; g++) {
            volatile uint32_t *igr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x080 + 4 * g);
            shell_printf("  IGROUPR[%u]=0x%08x\r\n", g, *igr);
        }

        /* Check SPI priority at various IRQ numbers to find the boundary */
        {
            uint32_t test_irqs[] = {64, 128, 185, 192, 224, 256, 261, 30};
            for (int t = 0; t < 8; t++) {
                uint32_t irq = test_irqs[t];
                volatile uint32_t *preg = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x400 + (irq & ~3));
                uint8_t pri = (*preg >> ((irq & 3) * 8)) & 0xFF;
                shell_printf("  IRQ%u pri=0x%x%s", irq, pri, (t < 7) ? "  " : "\r\n");
            }
        }

        /* BAR3 → MIP0 verification (BAR1 is firmware's RP1 MMIO mapping) */
        {
            volatile uint32_t *b3lo = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_LO);
            volatile uint32_t *b3hi = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_HI);
            volatile uint32_t *rlo  = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP);
            volatile uint32_t *rhi  = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP_HI);
            shell_printf("  BAR3: %x_%08x remap=%x_%08x\r\n", *b3hi, *b3lo, *rhi, *rlo);
        }
    }
#endif

    shell_puts("\r\n");
    return 0;
}

/*
 * uptime - Show system uptime
 */
int cmd_uptime(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Get time from ARM generic timer */
    extern uint64_t timer_get_count(void);
    extern uint64_t timer_get_frequency(void);

    uint64_t count = timer_get_count();
    uint64_t freq = timer_get_frequency();

    if (freq == 0) freq = 1;  /* Avoid divide by zero */

    uint64_t total_seconds = count / freq;
    uint64_t hours = total_seconds / 3600;
    uint64_t minutes = (total_seconds % 3600) / 60;
    uint64_t seconds = total_seconds % 60;

    shell_printf("Uptime: %lu:%02lu:%02lu\r\n", hours, minutes, seconds);

    return 0;
}

/*
 * clear - Clear screen using ANSI escape codes
 */
int cmd_clear(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* ANSI escape: clear screen and move cursor to home */
    shell_puts("\033[2J\033[H");

    return 0;
}

/*
 * reboot - Restart the system
 */
/*
 * sleep - Sleep for N milliseconds
 */
int cmd_sleep(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: sleep <ms>\r\n");
        return 1;
    }

    uint32_t ms;
    if (shell_parse_uint(argv[1], &ms) < 0 || ms == 0) {
        shell_puts("sleep: duration must be a positive integer\r\n");
        return 1;
    }

    uint64_t before = slm_get_time_ns();
    sleep_ms(ms);
    uint64_t after = slm_get_time_ns();
    uint64_t elapsed_ms = (after - before) / 1000000;

    shell_printf("Slept %lu ms (requested %u ms)\r\n", elapsed_ms, ms);
    return 0;
}

/* ============================================================================
 * bench - Performance benchmarking
 *
 * Subcommands:
 *   bench context  — Context switch latency (create/switch/destroy)
 *   bench irq      — Interrupt latency (timer tick interval accuracy)
 *   bench ipc      — IPC message round-trip latency
 *   bench all      — Run all benchmarks
 * ============================================================================ */

#define BENCH_ITERATIONS 100
#define SCHED_COMPARE_ITERATIONS 50

/* ============================================================================
 * Latency histogram (#196)
 *
 * Log-scale buckets covering 0 to >1 ms. Each bucket upper bound is
 * 2× the previous, starting at 500 ns. Sample recording is O(1); the
 * bar-chart renderer walks the fixed-size array.
 * ============================================================================ */

enum { HIST_N_BUCKETS = 12 };

struct latency_histogram {
    uint32_t count;
    uint64_t sum_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    uint32_t buckets[HIST_N_BUCKETS];
    uint64_t samples[BENCH_ITERATIONS]; /* raw samples for percentiles */
};

static const uint64_t HIST_BOUNDS[HIST_N_BUCKETS] = {
    /* 0 */ 500,        /*   0 -   500 ns */
    /* 1 */ 1000,       /* 500 -  1000 ns */
    /* 2 */ 2000,       /*  1  -   2 us   */
    /* 3 */ 5000,       /*  2  -   5 us   */
    /* 4 */ 10000,      /*  5  -  10 us   */
    /* 5 */ 20000,      /* 10  -  20 us   */
    /* 6 */ 50000,      /* 20  -  50 us   */
    /* 7 */ 100000,     /* 50  - 100 us   */
    /* 8 */ 200000,     /*100  - 200 us   */
    /* 9 */ 500000,     /*200  - 500 us   */
    /*10 */ 1000000,    /*500us-   1 ms   */
    /*11 */ UINT64_MAX, /* >1 ms          */
};

static const char *HIST_LABELS[HIST_N_BUCKETS] = {
    "   0-  500ns",
    " 500- 1000ns",
    "   1-    2us",
    "   2-    5us",
    "   5-   10us",
    "  10-   20us",
    "  20-   50us",
    "  50-  100us",
    " 100-  200us",
    " 200-  500us",
    " 500- 1000us",
    "1000+    us ",
};

static void hist_init(struct latency_histogram *h)
{
    h->count = 0;
    h->sum_ns = 0;
    h->min_ns = UINT64_MAX;
    h->max_ns = 0;
    for (int i = 0; i < HIST_N_BUCKETS; i++) h->buckets[i] = 0;
}

static void hist_record(struct latency_histogram *h, uint64_t ns)
{
    if (ns < h->min_ns) h->min_ns = ns;
    if (ns > h->max_ns) h->max_ns = ns;
    h->sum_ns += ns;
    if (h->count < BENCH_ITERATIONS) {
        h->samples[h->count] = ns;
    }
    h->count++;
    for (int i = 0; i < HIST_N_BUCKETS; i++) {
        if (ns <= HIST_BOUNDS[i]) {
            h->buckets[i]++;
            return;
        }
    }
    h->buckets[HIST_N_BUCKETS - 1]++;
}

static void hist_sort_samples(struct latency_histogram *h)
{
    uint32_t n = h->count < BENCH_ITERATIONS ? h->count : BENCH_ITERATIONS;
    for (uint32_t i = 1; i < n; i++) {
        uint64_t key = h->samples[i];
        uint32_t j = i;
        while (j > 0 && h->samples[j - 1] > key) {
            h->samples[j] = h->samples[j - 1];
            j--;
        }
        h->samples[j] = key;
    }
}

static void hist_print(struct latency_histogram *h)
{
    if (h->count == 0) {
        shell_puts("  (no samples)\r\n");
        return;
    }
    /* Find max bucket for bar scaling. */
    uint32_t peak = 0;
    for (int i = 0; i < HIST_N_BUCKETS; i++) {
        if (h->buckets[i] > peak) peak = h->buckets[i];
    }
    if (peak == 0) peak = 1;

    shell_puts("  Latency distribution:\r\n");
    for (int i = 0; i < HIST_N_BUCKETS; i++) {
        uint32_t c = h->buckets[i];
        /* Bar: up to 40 '#' characters, proportional to peak. */
        uint32_t bar_len = (c * 40u + peak - 1) / peak;
        shell_printf("    %s: %5u ", HIST_LABELS[i], c);
        for (uint32_t b = 0; b < bar_len; b++) shell_putc('#');
        shell_puts("\r\n");
    }

    hist_sort_samples(h);
    uint32_t n = h->count < BENCH_ITERATIONS ? h->count : BENCH_ITERATIONS;
    uint64_t p50 = h->samples[n * 50 / 100];
    uint64_t p95 = h->samples[n * 95 / 100];
    uint64_t p99 = h->samples[n * 99 / 100];
    uint64_t mean = h->sum_ns / h->count;

    shell_printf("\r\n  Min: %lu ns   Max: %lu ns   Mean: %lu ns\r\n",
                (unsigned long)h->min_ns, (unsigned long)h->max_ns,
                (unsigned long)mean);
    shell_printf("  p50: %lu ns   p95: %lu ns   p99: %lu ns\r\n",
                (unsigned long)p50, (unsigned long)p95, (unsigned long)p99);
}

/* --- Context switch benchmark --- */

static volatile int bench_ctx_done;
static volatile uint64_t bench_ctx_start;
static volatile uint64_t bench_ctx_total;
static volatile int bench_ctx_count;
static struct latency_histogram bench_ctx_hist;

static void bench_ctx_task(void *arg)
{
    (void)arg;
    while (bench_ctx_count < BENCH_ITERATIONS) {
        uint64_t now = slm_get_time_ns();
        if (bench_ctx_start > 0) {
            uint64_t dt = now - bench_ctx_start;
            bench_ctx_total += dt;
            hist_record(&bench_ctx_hist, dt);
        }
        bench_ctx_count++;
        bench_ctx_start = slm_get_time_ns();
        yield();
    }
    bench_ctx_done = 1;
}

/* When true, bench_context_switch suppresses its verbose per-run
 * output (sched compare uses it for a quiet measurement pass and
 * prints a consolidated table instead). */
static bool bench_ctx_quiet = false;

static void bench_context_switch(void)
{
    bench_ctx_done = 0;
    bench_ctx_start = 0;
    bench_ctx_total = 0;
    bench_ctx_count = 0;
    hist_init(&bench_ctx_hist);

    struct task *t = task_create_with_priority("bench_ctx",
        bench_ctx_task, NULL, TASK_PRIORITY_HIGH);
    if (!t) {
        if (!bench_ctx_quiet) {
            shell_puts("  Failed to create benchmark task\r\n");
        }
        return;
    }
    scheduler_add_task_to_cpu(t, 0);

    /* Wait for completion */
    int timeout = 500;
    while (!bench_ctx_done && timeout > 0) {
        yield();
        timeout--;
    }

    if (bench_ctx_quiet) {
        return;
    }

    if (bench_ctx_count > 1) {
        uint64_t avg_ns = bench_ctx_total / (uint64_t)(bench_ctx_count - 1);
        shell_printf("  Context switch: %d round-trips\r\n", bench_ctx_count - 1);
        shell_printf("    Average: %lu ns (%lu us)\r\n",
                    (unsigned long)avg_ns, (unsigned long)(avg_ns / 1000));
        if (avg_ns < 10000)
            shell_puts("    Rating:  Excellent (< 10 us)\r\n");
        else if (avg_ns < 50000)
            shell_puts("    Rating:  Good (< 50 us)\r\n");
        else if (avg_ns < 100000)
            shell_puts("    Rating:  Acceptable (< 100 us)\r\n");
        else
            shell_puts("    Rating:  Needs optimization (> 100 us)\r\n");
        hist_print(&bench_ctx_hist);
    } else {
        shell_puts("  Context switch: insufficient data\r\n");
    }
}

/* --- Interrupt latency benchmark --- */

static void bench_irq_latency(void)
{
    uint64_t freq = timer_get_frequency();
    uint64_t expected_interval_ns = 10000000ULL; /* 100 Hz = 10ms */
    uint64_t samples[20];
    int count = 0;

    shell_printf("  Timer frequency: %lu Hz (expected 10 ms ticks)\r\n",
                (unsigned long)freq);

    /* Measure actual tick intervals by reading counter across yields */
    uint64_t prev = slm_get_time_ns();
    for (int i = 0; i < 20; i++) {
        /* yield() allows timer tick to preempt, then we resume */
        yield();
        uint64_t now = slm_get_time_ns();
        samples[count++] = now - prev;
        prev = now;
    }

    /* Calculate stats */
    uint64_t min_ns = UINT64_MAX, max_ns = 0, sum_ns = 0;
    for (int i = 0; i < count; i++) {
        if (samples[i] < min_ns) min_ns = samples[i];
        if (samples[i] > max_ns) max_ns = samples[i];
        sum_ns += samples[i];
    }
    uint64_t avg_ns = sum_ns / (uint64_t)count;

    shell_printf("  Timer tick jitter (%d samples):\r\n", count);
    shell_printf("    Min: %lu ns (%lu us)\r\n",
                (unsigned long)min_ns, (unsigned long)(min_ns / 1000));
    shell_printf("    Avg: %lu ns (%lu us)\r\n",
                (unsigned long)avg_ns, (unsigned long)(avg_ns / 1000));
    shell_printf("    Max: %lu ns (%lu us)\r\n",
                (unsigned long)max_ns, (unsigned long)(max_ns / 1000));

    /* Jitter = max deviation from expected interval */
    int64_t jitter = (int64_t)max_ns - (int64_t)min_ns;
    shell_printf("    Jitter (max-min): %lu us\r\n",
                (unsigned long)(jitter > 0 ? (uint64_t)jitter / 1000 : 0));
    (void)expected_interval_ns;
}

/* --- IPC benchmark --- */

static void bench_ipc_latency(void)
{
    struct msg_queue *q = msg_queue_create(16, 8);
    if (!q) {
        shell_puts("  Failed to create message queue\r\n");
        return;
    }

    uint8_t msg[8] = {0};
    uint8_t buf[8];
    uint64_t total_ns = 0;
    int ipc_iters = BENCH_ITERATIONS;

    /* Measure send+recv round-trip on same CPU (no cross-CPU overhead) */
    irq_flags_t flags = irq_save();
    for (int i = 0; i < ipc_iters; i++) {
        uint64_t start = slm_get_time_ns();
        msg_send(q, msg, 0);
        msg_recv(q, buf, 0);
        uint64_t end = slm_get_time_ns();
        total_ns += end - start;
    }
    irq_restore(flags);

    uint64_t avg_ns = total_ns / (uint64_t)ipc_iters;
    shell_printf("  IPC send+recv round-trip (%d iterations):\r\n", ipc_iters);
    shell_printf("    Total: %lu ns\r\n", (unsigned long)total_ns);
    shell_printf("    Average: %lu ns (%lu us)\r\n",
                (unsigned long)avg_ns, (unsigned long)(avg_ns / 1000));

    msg_queue_destroy(q);
}

/* --- Scheduler policy inference-latency micro-benchmark (Phase 6.2d) ---
 *
 * Measures how fast each registered inference-device backend can run
 * a scheduler-shaped forward pass:
 *   - "cpu-mlp": FP32 state_in / logits_out on NEON (or SSE on x86)
 *   - "hailo-8" (Pi 5 / Jetson): INT8 state_in / logits_out on the NPU,
 *     ONLY if a model has been loaded via `hailo load`. Otherwise skips.
 *
 * Reports decisions/sec and per-decision ns. Does NOT actually switch
 * the scheduler policy — the kernel keeps running on its active one
 * throughout. This isolates model inference latency from full
 * assign_cpu overhead (which also pays state-extraction + heuristic
 * fallback); we report a pure forward-pass number.
 *
 * Round-robin ("heuristic") isn't inference-bound so it's deliberately
 * skipped here — a simulated state extraction + per-CPU counter update
 * is sub-microsecond and doesn't belong on this chart. To compare
 * scheduler throughput end-to-end, use `bench stats` while running a
 * workload under each policy in turn.
 */
#if defined(CONFIG_AI_SCHEDULER)
#include "inference_device.h"
#include "ai_types.h"

static void bench_policy_one(const char *dev_name,
                             enum inference_dtype dtype,
                             uint32_t in_n, uint32_t out_n,
                             inference_model_handle_t h,
                             uint32_t iters)
{
    struct inference_device *dev = inference_device_find(dev_name);
    if (!dev) {
        shell_printf("  %-10s  (backend not registered)\r\n", dev_name);
        return;
    }

    /* Scratch buffers. Stack budget: 432 B (fp32 108-elem) + 168 B
     * (fp32 42-elem) = 600 B worst case. INT8 paths are smaller. */
    uint32_t bpel = (dtype == INF_DTYPE_FP32) ? 4 :
                    (dtype == INF_DTYPE_FP16) ? 2 : 1;
    uint32_t in_bytes  = in_n  * bpel;
    uint32_t out_bytes = out_n * bpel;
    if (in_bytes > 1024 || out_bytes > 1024) {
        shell_printf("  %-10s  (in/out too large for stack bench)\r\n", dev_name);
        return;
    }
    alignas(8) uint8_t in_buf[1024];
    alignas(8) uint8_t out_buf[1024];
    memset(in_buf, 0, in_bytes);

    inference_tensor_t in = {
        .data = in_buf, .n_elems = in_n, .dtype = (uint8_t)dtype,
        .rank = 1, .shape = { (uint16_t)in_n, 0, 0, 0 },
    };
    inference_tensor_t out = {
        .data = out_buf, .n_elems = out_n, .dtype = (uint8_t)dtype,
        .rank = 1, .shape = { (uint16_t)out_n, 0, 0, 0 },
    };

    uint64_t t0 = timer_get_count();
    uint32_t ok = 0;
    for (uint32_t i = 0; i < iters; i++) {
        if (inference_run(dev, h, &in, &out) == INF_OK) ok++;
    }
    uint64_t t1 = timer_get_count();
    uint64_t freq = timer_get_frequency();
    uint64_t total_ns = (t1 - t0) * 1000000000ULL / freq;
    uint64_t per_ns = ok > 0 ? total_ns / ok : 0;
    uint64_t per_sec = per_ns > 0 ? 1000000000ULL / per_ns : 0;

    shell_printf("  %-10s  %u/%u ok   %lu ns/decision   %lu decisions/sec\r\n",
                 dev_name, ok, iters,
                 (unsigned long)per_ns, (unsigned long)per_sec);
}

static void bench_sched_policy(void)
{
    /* CPU-MLP is built-in; use INF_BUILTIN_HANDLE. Hailo needs a
     * shell-side loaded model — skip if ai_policy_hailo has no model. */
    shell_puts("Scheduler Policy Inference Benchmark\r\n");
    shell_puts("====================================\r\n");
    shell_printf("  Input dim: %u, Output dim: %u (AI_SCHED_N_ACTIONS)\r\n",
                 (unsigned)AI_STATE_DIM, (unsigned)AI_SCHED_N_ACTIONS);
    shell_printf("  %u iterations per backend\r\n", 1000u);

    bench_policy_one("cpu-mlp", INF_DTYPE_FP32,
                     AI_STATE_DIM, (uint32_t)AI_SCHED_N_ACTIONS,
                     INF_BUILTIN_HANDLE, 1000);

    /* Hailo-8 runs only if a .hef has been loaded (handle != INVALID).
     * The policy stashes the handle via ai_policy_hailo_set_model; we
     * reach into that via an external query below when available. */
    extern inference_model_handle_t ai_policy_hailo_get_model_handle(void);
    inference_model_handle_t hailo_h = ai_policy_hailo_get_model_handle();
    if (hailo_h == INF_INVALID_HANDLE) {
        shell_puts("  hailo-8     (no model loaded — `hailo load <hef>` first)\r\n");
    } else {
        /* hailo backend expects raw INT8 byte counts, not element counts
         * in the fp32 sense — input/output sizes come from HEF pads. For
         * the scheduler MLP today those are 108 and 24 bytes. */
        bench_policy_one("hailo-8", INF_DTYPE_INT8,
                         AI_STATE_DIM, (uint32_t)AI_SCHED_N_ACTIONS,
                         hailo_h, 1000);
    }
}
#endif /* CONFIG_AI_SCHEDULER */

/* --- Scheduler stats snapshot --- */

static void bench_sched_stats(void)
{
    struct sched_stats stats;
    scheduler_get_stats(&stats);

    uint64_t uptime_ns = slm_get_time_ns();
    uint64_t uptime_s = uptime_ns / 1000000000ULL;

    shell_printf("  Scheduler stats (uptime %lu s):\r\n", (unsigned long)uptime_s);
    shell_printf("    Tasks:            %lu\r\n", (unsigned long)stats.task_count);
    shell_printf("    Context switches: %lu\r\n", (unsigned long)stats.context_switches);
    shell_printf("    Timer ticks:      %lu\r\n", (unsigned long)stats.timer_ticks);
    if (uptime_s > 0) {
        shell_printf("    Switches/sec:     %lu\r\n",
                    (unsigned long)(stats.context_switches / uptime_s));
    }
}

/* --- Deadline accuracy benchmark --- */

/* NC offsets for deadline bench: -384 from end of NC region */
#define DL_NC_DONE   (NC_MEM_BASE + NC_MEM_SIZE - 384)
#define DL_NC_TIME   (NC_MEM_BASE + NC_MEM_SIZE - 376)  /* 8 bytes for uint64_t */

#if defined(PLATFORM_HAS_NC_MEMORY)
static void deadline_bench_task(void *arg)
{
    (void)arg;
    *(volatile uint64_t *)DL_NC_TIME = slm_get_time_ns();
    *(volatile uint32_t *)DL_NC_DONE = 1;
}
#endif

static void bench_deadline_accuracy(void)
{
    uint64_t deadlines_ms[] = {100, 50, 20, 10};
    int num_deadlines = 4;

    shell_puts("  Testing deadline-boosted task dispatch latency:\r\n");

    for (int d = 0; d < num_deadlines; d++) {
#if defined(PLATFORM_HAS_NC_MEMORY)
        *(volatile uint32_t *)DL_NC_DONE = 0;
        uint64_t start_ns = slm_get_time_ns();

        struct task *t = task_create("dl_bench", deadline_bench_task, NULL);
        if (!t) { shell_puts("    Failed to create task\r\n"); continue; }

        task_set_deadline(t, slm_get_time_ns() + deadlines_ms[d] * 1000000ULL);
        scheduler_add_task(t);

        for (int wait = 0; wait < 1000000; wait++) {
            if (*(volatile uint32_t *)DL_NC_DONE) break;
            for (volatile int x = 0; x < 100; x++) {}
        }

        if (*(volatile uint32_t *)DL_NC_DONE) {
            uint64_t end_ns = *(volatile uint64_t *)DL_NC_TIME;
            uint64_t latency_us = (end_ns - start_ns) / 1000;
            uint64_t deadline_us = deadlines_ms[d] * 1000;
            const char *met = (latency_us < deadline_us) ? "MET" : "MISSED";
            shell_printf("    Deadline %3lu ms: dispatched in %lu us — %s\r\n",
                        (unsigned long)deadlines_ms[d], (unsigned long)latency_us, met);
        } else {
            shell_printf("    Deadline %3lu ms: TIMEOUT\r\n",
                        (unsigned long)deadlines_ms[d]);
        }
#else
        shell_printf("    Deadline %3lu ms: (NC memory required for cross-CPU)\r\n",
                    (unsigned long)deadlines_ms[d]);
#endif
    }
}

/* --- Core isolation benchmark --- */

/* NC offsets for isolation bench: -400 from end of NC region */
#define ISO_NC_DONE  (NC_MEM_BASE + NC_MEM_SIZE - 400)
#define ISO_NC_CPU   (NC_MEM_BASE + NC_MEM_SIZE - 404)

#if defined(PLATFORM_HAS_NC_MEMORY)
static void isolation_bench_task(void *arg)
{
    (void)arg;
    *(volatile uint32_t *)ISO_NC_CPU = cpu_id();
    *(volatile uint32_t *)ISO_NC_DONE = 1;
}
#endif

static void bench_core_isolation(void)
{
    extern uint32_t cpu_count;
    if (cpu_count < 3) {
        shell_puts("  Need 3+ CPUs for isolation test\r\n");
        return;
    }

    shell_puts("  Isolating CPU 2, dispatching 8 tasks:\r\n");
    sched_isolate_core(2);

#if defined(PLATFORM_HAS_NC_MEMORY)
    int cpu_hits[4] = {0, 0, 0, 0};
    for (int i = 0; i < 8; i++) {
        *(volatile uint32_t *)ISO_NC_DONE = 0;
        struct task *t = task_create("iso_test", isolation_bench_task, NULL);
        if (!t) continue;
        scheduler_add_task(t);

        for (int wait = 0; wait < 500000; wait++) {
            if (*(volatile uint32_t *)ISO_NC_DONE) break;
            for (volatile int x = 0; x < 100; x++) {}
        }
        if (*(volatile uint32_t *)ISO_NC_DONE) {
            uint32_t ran_on = *(volatile uint32_t *)ISO_NC_CPU;
            if (ran_on < 4) cpu_hits[ran_on]++;
        }
    }

    shell_printf("    CPU 0: %d tasks  CPU 1: %d tasks  CPU 2: %d tasks  CPU 3: %d tasks\r\n",
                cpu_hits[0], cpu_hits[1], cpu_hits[2], cpu_hits[3]);
    shell_printf("    CPU 2 (isolated): %s\r\n",
                cpu_hits[2] == 0 ? "PASS — no tasks dispatched" : "FAIL — tasks reached isolated core");
#else
    shell_puts("    (NC memory required for cross-CPU isolation test)\r\n");
#endif

    sched_unisolate_core(2);
    shell_puts("  CPU 2 un-isolated.\r\n");
}

/* --- Shared buffer throughput benchmark --- */

static void bench_shared_buffer(void)
{
    /* Create a shared buffer and measure write+read throughput */
    struct shared_buffer *buf = shared_buffer_create(4096, 0);
    if (!buf) {
        shell_puts("  Failed to create shared buffer\r\n");
        return;
    }

    void *ptr = shared_buffer_map(buf, task_current(), 0x3); /* RW */
    if (!ptr) {
        shell_puts("  Failed to map shared buffer\r\n");
        shared_buffer_destroy(buf);
        return;
    }

    /* Write 4KB, 1000 iterations */
    uint64_t start = slm_get_time_ns();
    for (int i = 0; i < 1000; i++) {
        volatile uint8_t *p = (volatile uint8_t *)ptr;
        for (int j = 0; j < 4096; j += 64)
            p[j] = (uint8_t)i;
    }
    uint64_t write_ns = slm_get_time_ns() - start;

    /* Read 4KB, 1000 iterations */
    start = slm_get_time_ns();
    volatile uint8_t sink = 0;
    for (int i = 0; i < 1000; i++) {
        volatile uint8_t *p = (volatile uint8_t *)ptr;
        for (int j = 0; j < 4096; j += 64)
            sink = p[j];
    }
    (void)sink;
    uint64_t read_ns = slm_get_time_ns() - start;

    uint64_t write_mbps = (4096ULL * 1000 * 1000000000ULL) / (write_ns * 1024 * 1024);
    uint64_t read_mbps = (4096ULL * 1000 * 1000000000ULL) / (read_ns * 1024 * 1024);

    shell_printf("  Shared buffer (4 KB, 1000 iterations, 64B stride):\r\n");
    shell_printf("    Write: %lu MB/s (%lu ns total)\r\n",
                (unsigned long)write_mbps, (unsigned long)write_ns);
    shell_printf("    Read:  %lu MB/s (%lu ns total)\r\n",
                (unsigned long)read_mbps, (unsigned long)read_ns);

    shared_buffer_unmap(buf, task_current());
    shared_buffer_destroy(buf);
}

/* SMP cross-CPU dispatch test: task runs on target CPU, writes NC done flag */
void smp_test_task(void *arg)
{
    (void)arg;
#if defined(PLATFORM_HAS_NC_MEMORY)
    uint32_t target_cpu = (uint32_t)(uintptr_t)arg;
    /* Write to NC memory — instantly visible to CPU 0 without CIVAC.
     * Offset -320 from end of NC region: per-CPU done flags. */
    *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 320 + target_cpu * 4) = cpu_id() + 1;
#endif
}

/* S3 (Phase C): work-stealing load-imbalance benchmark task.
 *
 * All tasks in the benchmark are dispatched to CPU 1 ("victim"). Each
 * task runs a fixed amount of arithmetic work, then records which
 * CPU actually executed it by writing cpu_id()+1 into its own slot
 * (0 = not yet run). If CONFIG_WORK_STEALING is ON, idle CPUs
 * (2/3/...) will pull tasks off CPU 1's deque and wall-clock time
 * drops toward N/P of the single-CPU number. If OFF, the N tasks
 * run sequentially on CPU 1.
 *
 * Completion detection uses a *per-task* non-zero write — the driver
 * scans all N slots and counts non-zero entries. This avoids a
 * central atomic counter, which is unreliable on Pi 5's NC memory:
 * per ARM ARM and kernel/CLAUDE.md "NC memory atomic ops may fault
 * (implementation-defined per ARM ARM)". A per-slot single-writer
 * u32 store is guaranteed-visible on NC (no atomic needed) and is
 * benign under regular stores on cacheable BSS.
 *
 * Shared state is per-platform:
 *   - PLATFORM_HAS_NC_MEMORY (Pi 5, Jetson): NC memory at
 *     NC_MEM_BASE + NC_MEM_SIZE - 512, no cache maintenance needed.
 *   - Other (QEMU ARM64, x86-64): cacheable BSS arrays. Caches are
 *     coherent on these targets so cross-CPU visibility is
 *     automatic.
 */
#define S3_MAX_TASKS 64

#if defined(PLATFORM_HAS_NC_MEMORY)
#define S3_SLOT_BASE_OFFSET   (NC_MEM_SIZE - 512)
#define S3_SLOT_ADDR(i) ((volatile uint32_t *)(NC_MEM_BASE + S3_SLOT_BASE_OFFSET + (i) * 4))
#else
static volatile uint32_t s3_slots[S3_MAX_TASKS];
#define S3_SLOT_ADDR(i) (&s3_slots[(i)])
#endif

static void s3_steal_work_task(void *arg)
{
    uintptr_t slot = (uintptr_t)arg;
    /* Fixed-size chunk of arithmetic work — chosen so one task takes
     * on the order of a few hundred microseconds on Pi 5 / Jetson.
     * A CPU-bound loop (not just a timer busy-wait) lets work
     * genuinely parallelize when stealing is on. */
    volatile uint64_t x = 1;
    for (uint64_t i = 1; i < 400000; i++) {
        x = x * 1103515245 + 12345;
    }
    (void)x;

    /* Single-writer release store: cpu_id()+1 into this task's slot.
     * The driver treats non-zero == done. No atomic; NC memory on
     * Pi 5 / Jetson would potentially fault an LSE op (per
     * kernel/CLAUDE.md). */
    *S3_SLOT_ADDR(slot) = cpu_id() + 1;
}

int cmd_bench(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("Usage: bench <context|irq|ipc|eviction|deadline|isolate|shared|smp|stealing|matmul|conv|quant|gpu|stats|all>\r\n");
        return 1;
    }

    shell_puts("\r\n");

    if (strcmp(argv[1], "context") == 0) {
        shell_puts("Context Switch Benchmark\r\n");
        shell_puts("========================\r\n");
        bench_context_switch();
    } else if (strcmp(argv[1], "irq") == 0) {
        shell_puts("Interrupt Latency Benchmark\r\n");
        shell_puts("===========================\r\n");
        bench_irq_latency();
    } else if (strcmp(argv[1], "ipc") == 0) {
        shell_puts("IPC Latency Benchmark\r\n");
        shell_puts("=====================\r\n");
        bench_ipc_latency();
    } else if (strcmp(argv[1], "eviction") == 0) {
        shell_puts("Eviction Policy Fault-Rate Comparison (#117)\r\n");
        shell_puts("=============================================\r\n");
        shell_puts("Workload: single_inference (8-slot cache, 16-block WS, 10 cycles)\r\n\r\n");
        static RustEvictionCompareResult results[8];
        int32_t n = rust_eviction_workload_compare(results, 8);
        if (n <= 0) {
            shell_puts("  (AI eviction disabled or error)\r\n");
        } else {
            shell_puts("Policy           Faults  Hits   Total   Fault rate\r\n");
            shell_puts("---------------  ------  -----  ------  ----------\r\n");
            uint32_t best_faults = UINT32_MAX;
            const char *best_name = NULL;
            for (int32_t i = 0; i < n; i++) {
                uint32_t f = results[i].faults;
                uint32_t h = results[i].hits;
                uint32_t t = results[i].total_accesses;
                uint32_t pct = t > 0 ? (f * 100 / t) : 0;
                shell_printf("%-15s  %6u  %5u  %6u  %8u%%\r\n",
                            results[i].policy_name, f, h, t, pct);
                if (f < best_faults) {
                    best_faults = f;
                    best_name = (const char *)results[i].policy_name;
                }
            }
            if (best_name) {
                shell_printf("\r\nBest: %s (%u faults)\r\n", best_name, best_faults);
            }
        }
    } else if (strcmp(argv[1], "stats") == 0) {
        shell_puts("Scheduler Statistics\r\n");
        shell_puts("====================\r\n");
        bench_sched_stats();
    } else if (strcmp(argv[1], "smp") == 0) {
        shell_puts("SMP Cross-CPU Dispatch Test\r\n");
        shell_puts("===========================\r\n");
        /* Dispatch a task to each secondary CPU and verify it completes.
         * Uses NC memory for done flags — instantly visible cross-CPU. */
#if defined(PLATFORM_HAS_NC_MEMORY)
        /* Zero done flags in NC memory — one slot per CPU (skip slot 0, unused). */
        for (uint32_t i = 0; i < cpu_count; i++)
            *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 320 + i * 4) = 0;
#endif
        for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
            extern void smp_test_task(void *arg);
            char name[16];
            name[0] = 's'; name[1] = 'm'; name[2] = 'p';
            name[3] = '0' + cpu; name[4] = '\0';
            struct task *t = task_create(name, smp_test_task, (void *)(uintptr_t)cpu);
            if (t) {
                scheduler_add_task_to_cpu(t, cpu);
                shell_printf("  Dispatched '%s' to CPU %u\r\n", name, cpu);
            }
        }
        /* Wait for all to complete (NC read — no CIVAC needed) */
        for (int wait = 0; wait < 500000; wait++) {
            int all_done = 1;
            for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
#if defined(PLATFORM_HAS_NC_MEMORY)
                if (!*(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 320 + cpu * 4))
                    { all_done = 0; break; }
#endif
            }
            if (all_done) break;
            for (volatile int d = 0; d < 1000; d++) {}
        }
        for (uint32_t cpu = 1; cpu < cpu_count; cpu++) {
#if defined(PLATFORM_HAS_NC_MEMORY)
            uint32_t result = *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 320 + cpu * 4);
            shell_printf("  CPU %u: %s (ran on CPU %u)\r\n", cpu,
                        result ? "COMPLETED" : "TIMEOUT", result ? result - 1 : 0);
#else
            shell_printf("  CPU %u: (NC memory required)\r\n", cpu);
#endif
        }
    } else if (strcmp(argv[1], "deadline") == 0) {
        shell_puts("Deadline Accuracy Benchmark\r\n");
        shell_puts("===========================\r\n");
        bench_deadline_accuracy();
    } else if (strcmp(argv[1], "isolate") == 0) {
        shell_puts("Core Isolation Benchmark\r\n");
        shell_puts("========================\r\n");
        bench_core_isolation();
    } else if (strcmp(argv[1], "shared") == 0) {
        shell_puts("Shared Buffer Throughput Benchmark\r\n");
        shell_puts("==================================\r\n");
        bench_shared_buffer();
    } else if (strcmp(argv[1], "matmul") == 0) {
        shell_puts("NEON FP32 MatMul Benchmark\r\n");
        shell_puts("==========================\r\n");
        /* Default 20 iterations — enough for a stable min/avg/max
         * without dominating the shell session. User can override by
         * passing a count: `bench matmul 100`. */
        uint32_t iters = 20;
        if (argc >= 3) {
            uint32_t n;
            if (shell_parse_uint(argv[2], &n) == 0 && n > 0 && n <= 10000) {
                iters = n;
            }
        }
        rust_matmul_bench_fp32(iters);
    } else if (strcmp(argv[1], "conv") == 0) {
        shell_puts("NEON FP32 Conv2D Benchmark\r\n");
        shell_puts("==========================\r\n");
        uint32_t iters = 50;
        if (argc >= 3) {
            uint32_t n;
            if (shell_parse_uint(argv[2], &n) == 0 && n > 0 && n <= 10000) {
                iters = n;
            }
        }
        rust_conv_bench_fp32(iters);
    } else if (strcmp(argv[1], "stealing") == 0) {
        /* S3: work-stealing Phase C load-imbalance benchmark.
         *
         * Dispatches N identical CPU-bound tasks all to CPU 1 (the
         * "victim"). Wall-clock time to N completions is the headline
         * number. Also reports:
         *   - per-CPU counts (which CPU actually ran each task), to
         *     show how stealing balanced the load
         *   - steal counter deltas for the thief CPUs
         *   - steal-counter totals before/after so repeated runs are
         *     comparable
         *
         * Compare between builds:
         *   make kernel                       (CONFIG_WORK_STEALING=OFF)
         *   make kernel WORK_STEALING=ON      (ON)
         * and diff the wall-clock numbers. Input to S4 (default flip).
         */
        uint32_t n_tasks = 16;
        if (argc >= 3) {
            uint32_t n;
            if (shell_parse_uint(argv[2], &n) == 0 && n > 0 && n <= S3_MAX_TASKS) {
                n_tasks = n;
            }
        }
        if (cpu_count < 2) {
            shell_puts("  Need at least 2 CPUs for load-imbalance test\r\n");
            return 0;
        }
        shell_puts("Work-Stealing Load-Imbalance Benchmark (S3 / Phase C)\r\n");
        shell_puts("=====================================================\r\n");
        shell_printf("  Tasks dispatched:  %lu (all to CPU 1)\r\n",
                    (unsigned long)n_tasks);
        shell_printf("  Online CPUs:       %lu\r\n", (unsigned long)cpu_count);
#if CONFIG_WORK_STEALING
        shell_puts("  CONFIG_WORK_STEALING: ON  (expect parallel completion)\r\n");
#else
        shell_puts("  CONFIG_WORK_STEALING: OFF (expect sequential completion)\r\n");
#endif

        /* Zero slot array. Each task writes its slot when done; the
         * driver scans for all-non-zero to detect completion. */
        for (uint32_t i = 0; i < n_tasks; i++) {
            *S3_SLOT_ADDR(i) = 0;
        }

#if CONFIG_WORK_STEALING
        /* Snapshot steal counters so we report deltas, not absolute
         * values that include prior shell activity. */
        uint32_t pre_attempts[MAX_CPUS] = {0};
        uint32_t pre_successes[MAX_CPUS] = {0};
        uint32_t pre_push_full[MAX_CPUS] = {0};
#if defined(PLATFORM_HAS_NC_MEMORY)
        extern volatile uint32_t *sched_diag_steal_attempts;
        extern volatile uint32_t *sched_diag_steal_successes;
        extern volatile uint32_t *sched_diag_steal_push_full;
#else
        extern volatile uint32_t sched_diag_steal_attempts[];
        extern volatile uint32_t sched_diag_steal_successes[];
        extern volatile uint32_t sched_diag_steal_push_full[];
#endif
        for (uint32_t c = 0; c < cpu_count; c++) {
            pre_attempts[c] = sched_diag_steal_attempts[c];
            pre_successes[c] = sched_diag_steal_successes[c];
            pre_push_full[c] = sched_diag_steal_push_full[c];
        }
#endif

        uint64_t t0 = timer_get_count();
        for (uint32_t i = 0; i < n_tasks; i++) {
            char name[16];
            name[0] = 's'; name[1] = '3'; name[2] = '_';
            uint32_t idx = i;
            if (idx >= 10) {
                name[3] = '0' + (idx / 10);
                name[4] = '0' + (idx % 10);
                name[5] = '\0';
            } else {
                name[3] = '0' + idx;
                name[4] = '\0';
            }
            struct task *t = task_create(name, s3_steal_work_task,
                                         (void *)(uintptr_t)i);
            if (t) {
                scheduler_add_task_to_cpu(t, 1);
            }
        }

        /* Poll done counter. Timeout at 30 seconds converted to timer
         * ticks. */
        uint64_t freq = timer_get_frequency();
        uint64_t deadline = t0 + 30ULL * freq;
        uint32_t done_now = 0;
        while (1) {
            /* Count non-zero slots — one per completed task. */
            uint32_t done = 0;
            for (uint32_t i = 0; i < n_tasks; i++) {
                if (*S3_SLOT_ADDR(i) != 0) done++;
            }
            if (done >= n_tasks) { done_now = done; break; }
            if (timer_get_count() > deadline) {
                done_now = done;
                shell_printf("  TIMEOUT after 30s — %u / %u tasks done\r\n",
                            done_now, n_tasks);
                break;
            }
            /* Cooperative yield so CPU 0 doesn't spin at 100%. */
            yield();
        }
        uint64_t t1 = timer_get_count();
        uint64_t elapsed_ns = (t1 - t0) * 1000000000ULL / freq;

        /* Per-CPU execution distribution. */
        uint32_t cpu_count_exec[MAX_CPUS] = {0};
        for (uint32_t i = 0; i < n_tasks; i++) {
            uint32_t slot = *S3_SLOT_ADDR(i);
            if (slot > 0 && slot <= MAX_CPUS) {
                cpu_count_exec[slot - 1]++;
            }
        }

        shell_printf("\r\n  Results:\r\n");
        shell_printf("    Completed:    %u / %u\r\n", done_now, n_tasks);
        shell_printf("    Wall-clock:   %lu ms (%lu us)\r\n",
                    (unsigned long)(elapsed_ns / 1000000),
                    (unsigned long)(elapsed_ns / 1000));
        if (done_now > 0) {
            shell_printf("    Per-task avg: %lu us\r\n",
                        (unsigned long)(elapsed_ns / 1000 / done_now));
        }
        shell_puts("\r\n    Per-CPU execution distribution:\r\n");
        for (uint32_t c = 0; c < cpu_count; c++) {
            shell_printf("      CPU %lu: %u task%s\r\n",
                        (unsigned long)c, cpu_count_exec[c],
                        cpu_count_exec[c] == 1 ? "" : "s");
        }
#if CONFIG_WORK_STEALING
        shell_puts("\r\n    Steal counter deltas this run:\r\n");
        shell_puts("    CPU  Attempts  Successes  PushFull\r\n");
        shell_puts("    ---  --------  ---------  --------\r\n");
        for (uint32_t c = 0; c < cpu_count; c++) {
            uint32_t da = sched_diag_steal_attempts[c] - pre_attempts[c];
            uint32_t ds = sched_diag_steal_successes[c] - pre_successes[c];
            uint32_t dpf = sched_diag_steal_push_full[c] - pre_push_full[c];
            shell_printf("    %3lu  %8u  %9u  %8u\r\n",
                        (unsigned long)c, da, ds, dpf);
        }
#endif
    } else if (strcmp(argv[1], "quant") == 0) {
        /* G4: run FP32, FP16, and INT8 matmuls at the same shape so the
         * FP32 line is the baseline for comparing dequant / INT8 cost. */
        uint32_t iters = 20;
        if (argc >= 3) {
            uint32_t n;
            if (shell_parse_uint(argv[2], &n) == 0 && n > 0 && n <= 10000) {
                iters = n;
            }
        }
        shell_puts("Quantization MatMul Benchmark (FP32 / FP16 / INT8)\r\n");
        shell_puts("==================================================\r\n");
        shell_puts("--- FP32 baseline ---\r\n");
        rust_matmul_bench_fp32(iters);
        shell_puts("--- FP16 (B matrix half-precision) ---\r\n");
        rust_matmul_bench_fp16(iters);
        shell_puts("--- INT8 (A and B quantized, FP32 output) ---\r\n");
        rust_matmul_bench_int8(iters);
    } else if (strcmp(argv[1], "gpu") == 0) {
        shell_puts("GPU Cache Sync Benchmark\r\n");
        shell_puts("========================\r\n");
        if (!gpu_available()) {
            shell_puts("  GPU not available\r\n");
        } else {
            gpu_buffer_t buf;
            uint64_t t0, t1;

            /* Measure alloc */
            t0 = timer_get_count();
            int ret = gpu_alloc(4096, GPU_MEM_READWRITE, &buf);
            t1 = timer_get_count();
            if (ret != GPU_OK) {
                shell_puts("  gpu_alloc failed\r\n");
            } else {
                uint64_t freq = timer_get_frequency();
                uint64_t alloc_ns = (t1 - t0) * 1000000000ULL / freq;

                /* Write pattern */
                volatile uint32_t *p = (volatile uint32_t *)buf.cpu_addr;
                for (int i = 0; i < 1024; i++) p[i] = 0xDEADBEEF;

                /* Measure sync_for_gpu (cache clean) */
                t0 = timer_get_count();
                gpu_sync_for_gpu(&buf);
                t1 = timer_get_count();
                uint64_t clean_ns = (t1 - t0) * 1000000000ULL / freq;

                /* Measure sync_for_cpu (cache invalidate) */
                t0 = timer_get_count();
                gpu_sync_for_cpu(&buf);
                t1 = timer_get_count();
                uint64_t inv_ns = (t1 - t0) * 1000000000ULL / freq;

                /* Verify data survived round-trip */
                int ok = (p[0] == 0xDEADBEEF && p[1023] == 0xDEADBEEF);

                /* Measure free */
                t0 = timer_get_count();
                gpu_free(&buf);
                t1 = timer_get_count();
                uint64_t free_ns = (t1 - t0) * 1000000000ULL / freq;

                shell_printf("  4KB buffer (1024 uint32):\r\n");
                shell_printf("    Alloc:         %lu ns\r\n", (unsigned long)alloc_ns);
                shell_printf("    sync_for_gpu:  %lu ns (DC CVAC clean)\r\n", (unsigned long)clean_ns);
                shell_printf("    sync_for_cpu:  %lu ns (DC IVAC invalidate)\r\n", (unsigned long)inv_ns);
                shell_printf("    Free:          %lu ns\r\n", (unsigned long)free_ns);
                shell_printf("    Data integrity: %s\r\n", ok ? "PASS" : "FAIL");
            }
        }
#if defined(CONFIG_AI_SCHEDULER)
    } else if (strcmp(argv[1], "sched-policy") == 0) {
        bench_sched_policy();
#endif
    } else if (strcmp(argv[1], "all") == 0) {
        shell_puts("SLM-OS Performance Benchmarks\r\n");
        shell_puts("=============================\r\n\r\n");
        bench_context_switch();
        shell_puts("\r\n");
        bench_irq_latency();
        shell_puts("\r\n");
        bench_ipc_latency();
        shell_puts("\r\n");
        bench_deadline_accuracy();
        shell_puts("\r\n");
        bench_core_isolation();
        shell_puts("\r\n");
        bench_shared_buffer();
        shell_puts("\r\n");
        bench_sched_stats();
    } else {
        shell_printf("Unknown benchmark: %s\r\n", argv[1]);
        shell_puts("Available: context, irq, ipc, deadline, isolate, shared, smp, gpu, sched-policy, stats, all\r\n");
        return 1;
    }

    shell_puts("\r\n");
    return 0;
}

#if defined(PI5_IRQ_DIAG)
#include "diag_pi5.h"

/*
 * diag - Dump Pi 5 IRQ-delivery diagnostics (issue #99 Phase 1).
 *
 * Sub-commands:
 *   diag el2   — EL2 register snapshot captured by boot.S.
 *   diag vec   — per-CPU per-vector exception counters.
 *   diag fiq   — last FIQ source IRQ seen per CPU.
 *   diag all   — all of the above (default).
 */
static void diag_print_el2(void)
{
    struct diag_el2_snapshot *s = diag_el2_snap();
    shell_puts("\r\nEL2 register snapshot (boot.S, issue #99):\r\n");
    if (s->magic != DIAG_EL2_MAGIC) {
        shell_printf("  magic=0x%lx  INVALID — EL2 block did not run "
                    "(firmware entered at EL%lu?)\r\n",
                    (unsigned long)s->magic,
                    (unsigned long)((s->current_el_entry >> 2) & 3));
        return;
    }
    shell_printf("  CurrentEL at entry:     0x%lx (EL%lu)\r\n",
                (unsigned long)s->current_el_entry,
                (unsigned long)((s->current_el_entry >> 2) & 3));
    shell_printf("  MIDR_EL1:               0x%lx\r\n",
                (unsigned long)s->midr_el1);
    shell_printf("  ID_AA64PFR0_EL1:        0x%lx\r\n",
                (unsigned long)s->id_aa64pfr0_el1);
    shell_printf("  HCR_EL2 pre:            0x%lx\r\n",
                (unsigned long)s->hcr_el2_pre);
    shell_printf("  HCR_EL2 post (written): 0x%lx  (IMO=%lu FMO=%lu AMO=%lu RW=%lu)\r\n",
                (unsigned long)s->hcr_el2_post,
                (unsigned long)((s->hcr_el2_post >> 4) & 1),
                (unsigned long)((s->hcr_el2_post >> 3) & 1),
                (unsigned long)((s->hcr_el2_post >> 5) & 1),
                (unsigned long)((s->hcr_el2_post >> 31) & 1));
    shell_printf("  CNTHCTL_EL2:            0x%lx\r\n",
                (unsigned long)s->cnthctl_el2);
    shell_printf("  GICD_CTLR pre:          0x%lx\r\n",
                (unsigned long)s->gicd_ctlr_pre);
    shell_printf("  GICC_CTLR pre:          0x%lx\r\n",
                (unsigned long)s->gicc_ctlr_pre);
    shell_printf("  GICD_IGROUPR[0] pre:    0x%lx\r\n",
                (unsigned long)s->gicd_igroupr0_pre);
    shell_printf("  GICD_IGROUPR[0] post:   0x%lx  %s\r\n",
                (unsigned long)s->gicd_igroupr0_post,
                (s->gicd_igroupr0_post == 0xFFFFFFFFul)
                    ? "(writes held)"
                    : (s->gicd_igroupr0_post == 0ul)
                          ? "(writes SILENTLY DISCARDED — FIQ hypothesis likely)"
                          : "(partial)");
}

static void diag_print_vec(void)
{
    extern uint32_t cpu_count;
    shell_puts("\r\nPer-CPU exception counters (Phase 1b):\r\n");
    shell_puts("  CPU   Sync       IRQ        FIQ        SError\r\n");
    shell_puts("  ---   --------   --------   --------   --------\r\n");
    for (uint32_t c = 0; c < cpu_count && c < 4; c++) {
        struct diag_vec_counts *v = diag_vec_counts_cpu(c);
        shell_printf("  %3lu   %8lu   %8lu   %8lu   %8lu\r\n",
                    (unsigned long)c,
                    (unsigned long)v->sync,
                    (unsigned long)v->irq,
                    (unsigned long)v->fiq,
                    (unsigned long)v->serror);
    }
}

static void diag_print_gic_runtime(void)
{
#if defined(PLATFORM_RASPI5)
    volatile uint32_t *gicd_isenabler0 =
        (volatile uint32_t *)(GIC_DIST_BASE + 0x100UL);
    volatile uint32_t *gicd_ispendr0 =
        (volatile uint32_t *)(GIC_DIST_BASE + 0x200UL);
    volatile uint32_t *gicd_iactiver0 =
        (volatile uint32_t *)(GIC_DIST_BASE + 0x300UL);
    volatile uint32_t *gicc_ctlr =
        (volatile uint32_t *)(GIC_CPU_BASE + 0x000UL);
    volatile uint32_t *gicc_pmr =
        (volatile uint32_t *)(GIC_CPU_BASE + 0x004UL);
    uint32_t iser = *gicd_isenabler0;
    uint32_t ispr = *gicd_ispendr0;
    uint32_t iacr = *gicd_iactiver0;

    /* ICC_SRE_EL2 snapshots from boot.S would go here, but reading
     * ICC_SRE_EL2 from EL2 traps to EL3 on this platform (TF-A set
     * ICC_SRE_EL3.Enable=0). Confirmed empirically — the read mrs
     * instruction caused EL3 firmware to halt the CPU. SRE state is
     * therefore opaque from below EL3, and any fix that needs to
     * change it must run at EL3 (armstub). */
    uint64_t sre_pre  = 0;
    uint64_t sre_post = 0;
    (void)sre_pre; (void)sre_post;

    shell_puts("\r\nGIC runtime state (read from shell task):\r\n");
    shell_printf("  GICC_CTLR = 0x%x  (bit0=EnGrp1 bit4=FIQByp!disG1 "
                "bit5=IRQByp!disG1 bit9=EOImodeNS)\r\n",
                *gicc_ctlr);
    shell_printf("  GICC_PMR  = 0x%x\r\n", *gicc_pmr);
    shell_printf("  GICD_ISENABLER0 = 0x%x  (bit 30 [timer PPI] = %u)\r\n",
                iser, (iser >> 30) & 1);
    shell_printf("  GICD_ISPENDR0   = 0x%x  (bit 30 [timer pending] = %u)\r\n",
                ispr, (ispr >> 30) & 1);
    shell_printf("  GICD_IACTIVER0  = 0x%x  (bit 30 [timer active] = %u)\r\n",
                iacr, (iacr >> 30) & 1);
    shell_puts("  ICC_SRE_EL2     = (not probed — access from EL2 "
              "traps to EL3 on this platform)\r\n");
#endif
}

static void diag_print_fiq(void)
{
    extern uint32_t cpu_count;
    shell_puts("\r\nLast FIQ source per CPU (el1_fiq_handler GICC_AIAR):\r\n");
    for (uint32_t c = 0; c < cpu_count && c < 4; c++) {
        uint32_t v = diag_fiq_last(c);
        if ((v & 0xF0000000u) == 0xD0000000u) {
            shell_printf("  CPU %lu  IRQ=%lu\r\n",
                        (unsigned long)c,
                        (unsigned long)(v & 0x3FFu));
        } else {
            shell_printf("  CPU %lu  (no FIQ observed)\r\n",
                        (unsigned long)c);
        }
    }
}

int cmd_diag(int argc, char *argv[])
{
    const char *what = (argc >= 2) ? argv[1] : "all";

    if (strcmp(what, "el2") == 0) {
        diag_print_el2();
    } else if (strcmp(what, "vec") == 0) {
        diag_print_vec();
    } else if (strcmp(what, "fiq") == 0) {
        diag_print_fiq();
    } else if (strcmp(what, "gic") == 0) {
        diag_print_gic_runtime();
    } else if (strcmp(what, "all") == 0) {
        diag_print_el2();
        diag_print_vec();
        diag_print_fiq();
        diag_print_gic_runtime();
    } else {
        shell_puts("Usage: diag <el2|vec|fiq|all>\r\n");
        return 1;
    }
    return 0;
}
#endif /* PI5_IRQ_DIAG */

int cmd_reboot(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    shell_puts("Rebooting...\r\n");

    psci_system_reset();

    /* If reset fails, spin */
    shell_puts("Reboot failed!\r\n");
    while (1) {
#if defined(PLATFORM_X86_64)
        __asm__ volatile("hlt");
#else
        __asm__ volatile("wfi");
#endif
    }

    return 0;
}

/*
 * vmm - Show virtual memory statistics
 */
int cmd_vmm(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    struct vmm_stats stats;
    vmm_get_stats(&stats);

    shell_puts("Virtual Memory Statistics:\r\n");
    shell_puts("\r\n");
    shell_printf("  L1 tables:       %lu\r\n", (unsigned long)stats.l1_tables);
    shell_printf("  L2 tables:       %lu\r\n", (unsigned long)stats.l2_tables);
    shell_printf("  Blocks mapped:   %lu (2MB each)\r\n", (unsigned long)stats.blocks_mapped);
    shell_printf("  Bytes mapped:    %lu MB\r\n", (unsigned long)(stats.bytes_mapped / (1024 * 1024)));
    shell_puts("\r\n");

    shell_puts("Memory Regions:\r\n");
    shell_puts("  Region           Start            End              Flags\r\n");
    shell_puts("  ---------------  ---------------  ---------------  -----\r\n");

    /* Kernel code/data region */
    shell_printf("  Kernel           0x%08lx       0x%08lx       RWX\r\n",
                (unsigned long)0x40000000, (unsigned long)0x40200000);

    /* Device MMIO region */
    shell_printf("  MMIO (Devices)   0x%08lx       0x%08lx       RW-\r\n",
                (unsigned long)0x08000000, (unsigned long)0x10000000);

    shell_puts("\r\n");
    return 0;
}

/*
 * ipc - Show IPC statistics
 */
int cmd_ipc(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    struct ipc_stats stats;
    ipc_get_stats(&stats);

    shell_puts("IPC Statistics:\r\n");
    shell_puts("\r\n");

    shell_puts("  Message Queues:\r\n");
    shell_printf("    Active queues:     %lu\r\n", (unsigned long)stats.queue_count);
    shell_printf("    Total msgs sent:   %lu\r\n", (unsigned long)stats.total_msgs_sent);
    shell_printf("    Total msgs recv:   %lu\r\n", (unsigned long)stats.total_msgs_recv);
    shell_puts("\r\n");

    shell_puts("  Shared Buffers:\r\n");
    shell_printf("    Active buffers:    %lu\r\n", (unsigned long)stats.buffer_count);
    shell_puts("\r\n");

    return 0;
}

/*
 * model - Show model memory pool statistics
 */
static void model_show_pools(void)
{
    const size_t block_size_kb = 2048;
    RustPoolStats weight_stats = rust_weight_pool_stats();
    RustPoolStats workspace_stats = rust_workspace_pool_stats();

    shell_puts("Model Memory Pools:\r\n");
    shell_puts("\r\n");

    shell_puts("  Weight Pool (read-only model parameters):\r\n");
    shell_printf("    Block size:      %lu KB\r\n", (unsigned long)block_size_kb);
    shell_printf("    Total blocks:    %lu\r\n", (unsigned long)weight_stats.total_blocks);
    shell_printf("    Free blocks:     %lu\r\n", (unsigned long)weight_stats.free_blocks);
    shell_printf("    Allocated:       %lu\r\n", (unsigned long)weight_stats.allocated_blocks);
    shell_printf("    Shared:          %lu\r\n", (unsigned long)weight_stats.shared_blocks);
    shell_printf("    Peak usage:      %lu\r\n", (unsigned long)weight_stats.peak_usage);
    shell_puts("\r\n");

    shell_puts("  Workspace Pool (inference scratch space):\r\n");
    shell_printf("    Block size:      %lu KB\r\n", (unsigned long)block_size_kb);
    shell_printf("    Total blocks:    %lu\r\n", (unsigned long)workspace_stats.total_blocks);
    shell_printf("    Free blocks:     %lu\r\n", (unsigned long)workspace_stats.free_blocks);
    shell_printf("    Allocated:       %lu\r\n", (unsigned long)workspace_stats.allocated_blocks);
    shell_printf("    Shared:          %lu\r\n", (unsigned long)workspace_stats.shared_blocks);
    shell_printf("    Peak usage:      %lu\r\n", (unsigned long)workspace_stats.peak_usage);
    shell_puts("\r\n");

    size_t total_blocks = weight_stats.total_blocks + workspace_stats.total_blocks;
    size_t total_mb = total_blocks * block_size_kb / 1024;
    size_t free_blocks = weight_stats.free_blocks + workspace_stats.free_blocks;
    size_t free_mb = free_blocks * block_size_kb / 1024;

    shell_printf("  Total: %lu blocks (%lu MB), %lu free (%lu MB)\r\n",
                (unsigned long)total_blocks, (unsigned long)total_mb,
                (unsigned long)free_blocks, (unsigned long)free_mb);
}

/* ============================================================================
 * Async model preloading (#64)
 *
 * model preload <name> launches a background task that loads the model
 * asynchronously. The shell returns immediately; `model list` shows the
 * model once loading completes. A simple status word tracks progress.
 * ============================================================================ */

enum { MODEL_PRELOAD_MAX = 4 };
enum { PRELOAD_NAME_LEN = 32 };

/* Per-slot state for the async preload infrastructure. */
static volatile int preload_status[MODEL_PRELOAD_MAX]; /* 0=free, 1=loading, 2=done, -1=fail */
static volatile int preload_result[MODEL_PRELOAD_MAX]; /* model index or -1 */
static char preload_name[MODEL_PRELOAD_MAX][PRELOAD_NAME_LEN]; /* model name being loaded */
/* VFS path (empty for built-in models). When non-empty the preload
 * task reads the file into a PMM buffer and passes it to
 * rust_model_load. */
static char preload_path[MODEL_PRELOAD_MAX][VFS_MAX_PATH];

static void preload_task_entry(void *arg)
{
    int slot = (int)(uintptr_t)arg;

    int idx;
    if (preload_path[slot][0] == '\0') {
        /* Built-in model (currently MNIST only). */
        idx = rust_model_load_builtin_mnist();
    } else {
        /* VFS-backed model: read file into PMM buffer, load, free. */
        struct vfs_entry_info finfo;
        if (vfs_stat_path(preload_path[slot], &finfo) != 0 || finfo.size == 0) {
            preload_result[slot] = -1;
            preload_status[slot] = -1;
            return;
        }
        size_t pages = (finfo.size + 4095) / 4096;
        uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
        if (!buf) {
            preload_result[slot] = -1;
            preload_status[slot] = -1;
            return;
        }
        int bytes = vfs_read_path(preload_path[slot], (char *)buf, finfo.size, 0);
        if (bytes <= 0) {
            pmm_free_pages(buf, pages);
            preload_result[slot] = -1;
            preload_status[slot] = -1;
            return;
        }
        /* Use the slot's name if non-empty, else filename. */
        const char *mname = preload_name[slot][0] ? preload_name[slot] : "model";
        idx = rust_model_load(mname, buf, (size_t)bytes);
        pmm_free_pages(buf, pages);
    }

    preload_result[slot] = idx;
    preload_status[slot] = (idx >= 0) ? 2 : -1;
}

/*
 * Wait for an in-progress preload of `name` to complete. Returns the
 * model index (>=0) on success, -1 if no preload is in-flight for that
 * name, -2 on timeout, -3 on preload failure.
 *
 * Yields while waiting so other tasks can run. Timeout is in ms.
 */
int model_preload_wait(const char *name, uint32_t timeout_ms)
{
    /* Find the slot loading this name. */
    int slot = -1;
    for (int i = 0; i < MODEL_PRELOAD_MAX; i++) {
        if (preload_status[i] == 1 &&
            strcmp(preload_name[i], name) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Not in-flight — check if already loaded. */
        int idx = rust_model_find(name);
        return idx >= 0 ? idx : -1;
    }

    uint64_t deadline = slm_get_time_ns() + (uint64_t)timeout_ms * 1000000ULL;
    while (preload_status[slot] == 1) {
        if (slm_get_time_ns() >= deadline) {
            return -2;
        }
        yield();
    }
    if (preload_status[slot] == 2) {
        return preload_result[slot];
    }
    return -3;
}

static int model_preload_start(const char *name)
{
    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < MODEL_PRELOAD_MAX; i++) {
        if (preload_status[i] == 0 || preload_status[i] == 2 ||
            preload_status[i] == -1) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        shell_puts("model preload: all preload slots busy\r\n");
        return -1;
    }

    /* Check if already loaded. */
    if (rust_model_find(name) >= 0) {
        shell_printf("model preload: '%s' already loaded\r\n", name);
        return 0;
    }

    /* Store the model name for wait/status queries. */
    size_t nlen = 0;
    while (name[nlen] && nlen < PRELOAD_NAME_LEN - 1) nlen++;
    for (size_t i = 0; i < nlen; i++) preload_name[slot][i] = name[i];
    preload_name[slot][nlen] = '\0';
    preload_path[slot][0] = '\0';

    /* Determine load method: built-in shortcut or VFS path. */
    bool is_mnist = (name[0]=='m' && name[1]=='n' && name[2]=='i' &&
                     name[3]=='s' && name[4]=='t' && name[5]=='\0');
    if (!is_mnist) {
        /* Treat the name as a VFS path for general model preloading. */
        char resolved[VFS_MAX_PATH];
        if (shell_resolve_path(name, resolved, sizeof(resolved)) < 0) {
            shell_printf("model preload: path too long: '%s'\r\n", name);
            return -1;
        }
        struct vfs_entry_info finfo;
        if (vfs_stat_path(resolved, &finfo) != 0) {
            shell_printf("model preload: '%s' not found (not a built-in "
                        "or VFS path)\r\n", name);
            return -1;
        }
        /* Copy resolved path for the background task. */
        size_t plen = 0;
        while (resolved[plen] && plen < VFS_MAX_PATH - 1) plen++;
        for (size_t i = 0; i < plen; i++) preload_path[slot][i] = resolved[i];
        preload_path[slot][plen] = '\0';
    }

    preload_status[slot] = 1;
    preload_result[slot] = -1;

    char task_name[16];
    uart_snprintf(task_name, sizeof(task_name), "preload_%d", slot);
    struct task *t = task_create(task_name, preload_task_entry,
                                 (void *)(uintptr_t)slot);
    if (!t) {
        preload_status[slot] = -1;
        shell_puts("model preload: failed to create background task\r\n");
        return -1;
    }
    scheduler_add_task(t);
    shell_printf("Preloading '%s' in background (slot %d, task '%s')\r\n",
                preload_name[slot], slot, task_name);
    return 0;
}

/*
 * Boot-time model preloading (#64). Reads /mnt/files/preload.conf and
 * spawns a background preload task for each non-comment, non-empty
 * line. Called from shell_init after the scheduler is running.
 *
 * Config format: one model name per line. Lines starting with '#' are
 * comments. Blank lines are ignored. "mnist" triggers the built-in
 * shortcut; anything else is treated as a VFS path.
 */
void model_boot_preload(void)
{
    /* 512 bytes holds ~15 model paths at ~30 chars each; increase if
     * MAX_MODELS grows past 8. */
    static char conf_buf[512];
    int bytes = vfs_read_path("/mnt/files/preload.conf", conf_buf,
                              sizeof(conf_buf) - 1, 0);
    if (bytes <= 0) {
        return;  /* No config file or empty — skip. */
    }
    conf_buf[bytes] = '\0';

    /* Parse line by line. */
    int started = 0;
    char *p = conf_buf;
    while (*p) {
        /* Skip leading whitespace. */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* Find end of line. */
        char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;

        /* NUL-terminate this line. */
        char saved = *eol;
        *eol = '\0';

        /* Skip comments and empty lines. */
        if (*p != '#' && *p != '\0') {
            /* Trim trailing whitespace. */
            char *end = eol - 1;
            while (end > p && (*end == ' ' || *end == '\t')) {
                *end = '\0';
                end--;
            }
            if (*p != '\0') {
                model_preload_start(p);
                started++;
            }
        }

        /* Advance past the line terminator. */
        *eol = saved;
        if (*eol == '\r') eol++;
        if (*eol == '\n') eol++;
        p = eol;
    }

    if (started > 0) {
        shell_printf("[boot] Started %d model preload(s) from /mnt/files/preload.conf\r\n",
                    started);
    }
}

static int model_load(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: model load <path|mnist>\r\n");
        return -1;
    }

    /* Built-in model shortcut: `model load mnist` loads the embedded
     * MNIST without needing a file path. */
    if (strcmp(argv[2], "mnist") == 0) {
        int idx = rust_model_load_builtin_mnist();
        if (idx < 0) {
            shell_puts("model load: built-in MNIST not available\r\n");
            return -1;
        }
        shell_printf("Loaded built-in MNIST (slot %d)\r\n", idx);
        return 0;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[2], resolved, sizeof(resolved)) < 0) {
        shell_puts("model load: path too long\r\n");
        return -1;
    }

    /* Get file size */
    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0) {
        shell_printf("model load: %s: file not found\r\n", resolved);
        return -1;
    }
    if (info.type != 0) {
        shell_printf("model load: %s: not a file\r\n", resolved);
        return -1;
    }
    if (info.size == 0) {
        shell_puts("model load: file is empty\r\n");
        return -1;
    }

    /* Allocate buffer for file contents */
    size_t pages_needed = (info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        shell_puts("model load: out of memory for read buffer\r\n");
        return -1;
    }

    /* Read the file */
    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        shell_printf("model load: failed to read %s\r\n", resolved);
        pmm_free_pages(buf, pages_needed);
        return -1;
    }

    /* Extract filename as model name (strip path) */
    const char *name = resolved;
    for (const char *p = resolved; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    /* Strip .onnx extension if present */
    char model_name[32];
    size_t name_len = 0;
    for (const char *p = name; *p && *p != '.' && name_len < 31; p++) {
        model_name[name_len++] = *p;
    }
    model_name[name_len] = '\0';

    /* Load via Rust FFI */
    int result = rust_model_load(model_name, buf, (size_t)bytes_read);
    pmm_free_pages(buf, pages_needed);

    if (result < 0) {
        shell_printf("model load: failed to load %s (parse error)\r\n", model_name);
        return -1;
    }

    /* Show result */
    RustModelInfo minfo;
    if (rust_model_get_info((uint32_t)result, &minfo) == 0) {
        shell_printf("Loaded model '%s' (slot %d)\r\n", model_name, result);
        shell_printf("  Format:     ONNX\r\n");
        shell_printf("  Parameters: %lu\r\n", (unsigned long)minfo.param_count);
        shell_printf("  Weights:    %lu bytes\r\n", (unsigned long)minfo.weight_size);
        shell_printf("  Nodes:      %lu\r\n", (unsigned long)minfo.node_count);
        shell_printf("  Inputs:     %lu\r\n", (unsigned long)minfo.input_count);
        shell_printf("  Outputs:    %lu\r\n", (unsigned long)minfo.output_count);
    } else {
        shell_printf("Loaded model '%s' (slot %d)\r\n", model_name, result);
    }

    return 0;
}

static int model_list(void)
{
    uint32_t count = rust_model_count();
    if (count == 0) {
        shell_puts("No models loaded.\r\n");
        return 0;
    }

    shell_printf("Loaded models (%lu):\r\n", (unsigned long)count);
    shell_puts("  Idx  Name                     Weights   Uses  Pin  Last(ms)\r\n");
    shell_puts("  ---  ----                     -------   ----  ---  --------\r\n");

    for (uint32_t i = 0; i < 8; i++) {
        RustModelInfo info;
        if (rust_model_get_info(i, &info) == 0) {
            shell_printf("  %lu    %-24s %-9lu %-5lu %-3s  %lu\r\n",
                        (unsigned long)i,
                        (const char *)info.name,
                        (unsigned long)info.weight_size,
                        (unsigned long)info.use_count,
                        info.pinned ? "yes" : "no",
                        (unsigned long)info.last_used_ms);
        }
    }
    return 0;
}

static int model_info(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: model info <name|idx>\r\n");
        return -1;
    }

    /* Try as index first */
    int idx = -1;
    uint32_t parsed_idx;
    if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
        idx = (int)parsed_idx;
    } else {
        idx = rust_model_find(argv[2]);
    }

    if (idx < 0) {
        shell_printf("model info: '%s' not found\r\n", argv[2]);
        return -1;
    }

    RustModelInfo info;
    if (rust_model_get_info((uint32_t)idx, &info) != 0) {
        shell_printf("model info: slot %d is empty\r\n", idx);
        return -1;
    }

    shell_printf("Model: %s (slot %d)\r\n", (const char *)info.name, idx);
    shell_printf("  Format:      %s\r\n",
                info.format == 1 ? "ONNX" :
                info.format == 0 ? "GGUF" : "Raw");
    shell_printf("  Parameters:  %lu\r\n", (unsigned long)info.param_count);
    shell_printf("  Weight size: %lu bytes\r\n", (unsigned long)info.weight_size);
    shell_printf("  Workspace:   %lu bytes\r\n", (unsigned long)info.workspace_size);
    shell_printf("  Nodes:       %lu\r\n", (unsigned long)info.node_count);
    shell_printf("  Inputs:      %lu\r\n", (unsigned long)info.input_count);
    shell_printf("  Outputs:     %lu\r\n", (unsigned long)info.output_count);

    return 0;
}

static int model_unload(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: model unload <name|idx>\r\n");
        return -1;
    }

    int idx = -1;
    uint32_t parsed_idx;
    if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
        idx = (int)parsed_idx;
    } else {
        idx = rust_model_find(argv[2]);
    }

    if (idx < 0) {
        shell_printf("model unload: '%s' not found\r\n", argv[2]);
        return -1;
    }

    if (rust_model_unload((uint32_t)idx) == 0) {
        shell_printf("Unloaded model from slot %d\r\n", idx);
        return 0;
    } else {
        shell_printf("model unload: failed for slot %d\r\n", idx);
        return -1;
    }
}

/*
 * Run inference and print results — implemented in Rust to avoid FP
 * operations in -mgeneral-regs-only kernel C code.
 */
extern int rust_infer_and_print(uint32_t model_index);

static int model_infer(int argc, char *argv[])
{
    if (argc < 3) {
        shell_puts("Usage: model infer <name|idx>\r\n");
        return -1;
    }

    /* Find model */
    int idx = -1;
    uint32_t parsed_idx;
    if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
        idx = (int)parsed_idx;
    } else {
        idx = rust_model_find(argv[2]);
    }

    if (idx < 0) {
        shell_printf("model infer: '%s' not found\r\n", argv[2]);
        return -1;
    }

    int result = rust_infer_and_print((uint32_t)idx);
    if (result < 0) {
        shell_printf("model infer: failed (error %d)\r\n", result);
        return -1;
    }

    return 0;
}

int cmd_model(int argc, char *argv[])
{
    if (argc < 2) {
        /* No subcommand — show pools + loaded model summary */
        model_show_pools();
        shell_puts("\r\n");
        model_list();
        return 0;
    }

    const char *subcmd = argv[1];

    if (strcmp(subcmd, "load") == 0) {
        return model_load(argc, argv);
    }
    if (strcmp(subcmd, "list") == 0) {
        return model_list();
    }
    if (strcmp(subcmd, "info") == 0) {
        return model_info(argc, argv);
    }
    if (strcmp(subcmd, "unload") == 0) {
        return model_unload(argc, argv);
    }
    if (strcmp(subcmd, "pools") == 0) {
        model_show_pools();
        return 0;
    }
    if (strcmp(subcmd, "infer") == 0) {
        return model_infer(argc, argv);
    }
    if (strcmp(subcmd, "gpu") == 0) {
        rust_gpu_print_status();
        return 0;
    }
    if (strcmp(subcmd, "stats") == 0) {
        RustInferStats stats;
        if (rust_infer_stats(&stats) == 0) {
            shell_puts("Inference Statistics:\r\n");
            shell_printf("  Total inferences: %lu\r\n", (unsigned long)stats.total_inferences);
            if (stats.total_inferences > 0) {
                unsigned long avg_us = (unsigned long)(stats.total_time_ns / stats.total_inferences / 1000);
                shell_printf("  Avg latency:      %lu us\r\n", avg_us);
                shell_printf("  Min latency:      %lu us\r\n", (unsigned long)(stats.min_time_ns / 1000));
                shell_printf("  Max latency:      %lu us\r\n", (unsigned long)(stats.max_time_ns / 1000));
                shell_printf("  Last latency:     %lu us\r\n", (unsigned long)(stats.last_time_ns / 1000));
            }
            shell_printf("  Errors:           %lu\r\n", (unsigned long)stats.errors);
        }
        return 0;
    }
    if (strcmp(subcmd, "bench") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model bench <name|idx> [iterations]\r\n");
            return -1;
        }
        int idx = -1;
        uint32_t parsed_idx;
        if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
            idx = (int)parsed_idx;
        } else {
            idx = rust_model_find(argv[2]);
        }
        if (idx < 0) {
            shell_printf("model bench: '%s' not found\r\n", argv[2]);
            return -1;
        }
        uint32_t iters = 10;  /* Default 10 iterations */
        if (argc >= 4) {
            uint32_t parsed_iters;
            if (shell_parse_uint(argv[3], &parsed_iters) == 0 && parsed_iters > 0) {
                iters = parsed_iters;
            }
        }
        shell_printf("Benchmarking model '%s' (%lu iterations)...\r\n",
                    argv[2], (unsigned long)iters);
        return rust_infer_bench((uint32_t)idx, iters);
    }

    if (strcmp(subcmd, "pin") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model pin <name|idx>\r\n");
            return -1;
        }
        int idx = -1;
        uint32_t parsed_idx;
        if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
            idx = (int)parsed_idx;
        } else {
            idx = rust_model_find(argv[2]);
        }
        if (idx < 0) {
            shell_printf("model pin: '%s' not found\r\n", argv[2]);
            return -1;
        }
        int rc = rust_model_pin((uint32_t)idx);
        if (rc < 0) {
            shell_printf("model pin: failed (slot %d)\r\n", idx);
            return -1;
        }
        shell_printf("Pinned model at slot %d (protected from LRU eviction)\r\n", idx);
        return 0;
    }
    if (strcmp(subcmd, "unpin") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model unpin <name|idx>\r\n");
            return -1;
        }
        int idx = -1;
        uint32_t parsed_idx;
        if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
            idx = (int)parsed_idx;
        } else {
            idx = rust_model_find(argv[2]);
        }
        if (idx < 0) {
            shell_printf("model unpin: '%s' not found\r\n", argv[2]);
            return -1;
        }
        int rc = rust_model_unpin((uint32_t)idx);
        if (rc < 0) {
            shell_printf("model unpin: failed (slot %d)\r\n", idx);
            return -1;
        }
        shell_printf("Unpinned model at slot %d (eligible for LRU eviction)\r\n", idx);
        return 0;
    }

    if (strcmp(subcmd, "preload") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model preload <name>\r\n");
            shell_puts("  Loads a model in a background task. Currently only 'mnist' supported.\r\n");
            shell_puts("  Check progress with 'model preload-status'.\r\n");
            return -1;
        }
        return model_preload_start(argv[2]);
    }
    if (strcmp(subcmd, "preload-wait") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model preload-wait <name> [timeout_ms]\r\n");
            return -1;
        }
        uint32_t timeout = 5000;
        if (argc >= 4) {
            uint32_t parsed;
            if (shell_parse_uint(argv[3], &parsed) == 0 && parsed > 0) {
                timeout = parsed;
            }
        }
        int rc = model_preload_wait(argv[2], timeout);
        if (rc >= 0) {
            shell_printf("Model '%s' ready (slot %d)\r\n", argv[2], rc);
        } else if (rc == -1) {
            shell_printf("No preload in-flight for '%s'\r\n", argv[2]);
        } else if (rc == -2) {
            shell_printf("Timeout waiting for '%s'\r\n", argv[2]);
        } else {
            shell_printf("Preload of '%s' failed\r\n", argv[2]);
        }
        return (rc >= 0) ? 0 : -1;
    }
    if (strcmp(subcmd, "preload-status") == 0) {
        shell_puts("Preload slots:\r\n");
        shell_puts("  Slot  Status    Result\r\n");
        shell_puts("  ----  --------  ------\r\n");
        for (int i = 0; i < MODEL_PRELOAD_MAX; i++) {
            int st = preload_status[i];
            const char *label = st == 0 ? "free" :
                                st == 1 ? "loading" :
                                st == 2 ? "done" : "failed";
            shell_printf("  %4d  %-8s  %d\r\n", i, label, preload_result[i]);
        }
        return 0;
    }

    shell_puts("Usage: model [load|list|info|unload|pin|unpin|preload|preload-status|"
              "infer|bench|stats|pools|gpu]\r\n");
    return -1;
}

/*
 * dtb - Show device tree information
 */
int cmd_dtb(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    const fdt_info_t *info = dtb_get_info();

    shell_puts("Device Tree Information:\r\n");
    shell_puts("\r\n");
    shell_printf("  Status:       %s\r\n", info->valid ? "parsed from DTB" : "using defaults");
    shell_puts("\r\n");

    shell_puts("  Memory:\r\n");
    shell_printf("    Base:       0x%lx\r\n", (unsigned long)info->ram_base);
    shell_printf("    Size:       %lu MB\r\n", (unsigned long)(info->ram_size / (1024 * 1024)));
    shell_puts("\r\n");

    shell_puts("  UART:\r\n");
    shell_printf("    Base:       0x%lx\r\n", (unsigned long)info->uart_base);
    shell_printf("    IRQ:        %lu\r\n", (unsigned long)info->uart_irq);
    shell_puts("\r\n");

    shell_puts("  GIC:\r\n");
    shell_printf("    Dist base:  0x%lx\r\n", (unsigned long)info->gic_dist_base);
    shell_printf("    CPU base:   0x%lx\r\n", (unsigned long)info->gic_cpu_base);
    shell_puts("\r\n");

    shell_puts("  CPUs:\r\n");
    shell_printf("    Count:      %lu\r\n", (unsigned long)info->cpu_count);
    shell_puts("\r\n");

    shell_puts("  Timer:\r\n");
    shell_printf("    IRQ:        %lu\r\n", (unsigned long)info->timer_irq);
    shell_puts("\r\n");

    return 0;
}

/*
 * peek - Read 32-bit words from arbitrary physical memory addresses.
 *
 *   peek <phys-hex>           Read one 32-bit word
 *   peek <phys-hex> <count>   Read count consecutive 32-bit words
 *
 * WARNING: reading from unmapped or device-memory regions may cause a
 * synchronous exception. Only use for addresses known to be in DRAM
 * or memory-mapped I/O that the VMM has identity-mapped.
 */
int cmd_peek(int argc, char *argv[])
{
    if (argc < 2) {
        shell_puts("usage: peek <phys-hex> [count]\r\n");
        return -1;
    }

    /* Parse hex address */
    const char *s = argv[1];
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t addr = 0;
    while (*s) {
        uint64_t d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = 10 + (*s - 'a');
        else if (*s >= 'A' && *s <= 'F') d = 10 + (*s - 'A');
        else { shell_puts("bad hex address\r\n"); return -1; }
        addr = (addr << 4) | d;
        s++;
    }

    uint32_t count = 1;
    if (argc >= 3) {
        count = 0;
        for (const char *p = argv[2]; *p; p++) {
            if (*p < '0' || *p > '9') { shell_puts("bad count\r\n"); return -1; }
            count = count * 10 + (*p - '0');
        }
        if (count == 0 || count > 256) {
            shell_puts("count must be 1-256\r\n");
            return -1;
        }
    }

    for (uint32_t i = 0; i < count; i++) {
        uint64_t a = addr + i * 4;
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)a;
        uint32_t val = *p;
        if (count == 1) {
            shell_printf("[0x%lx] = 0x%08lx\r\n",
                        (unsigned long)a, (unsigned long)val);
        } else {
            if (i % 4 == 0) shell_printf("[0x%lx]", (unsigned long)a);
            shell_printf(" %08lx", (unsigned long)val);
            if (i % 4 == 3 || i == count - 1) shell_puts("\r\n");
        }
    }
    return 0;
}

/*
 * poke - Write a 32-bit word to an arbitrary physical memory address.
 *
 *   poke <phys-hex> <val-hex>
 *
 * WARNING: writes to device memory can have side effects (doorbell
 * kicks, resets, etc.). Used for diagnosing MMIO firewall behavior.
 */
int cmd_poke(int argc, char *argv[])
{
    if (argc < 3) {
        uart_puts("usage: poke <phys-hex> <val-hex>\r\n");
        return -1;
    }

    uint64_t fields[2] = {0, 0};
    for (int f = 0; f < 2; f++) {
        const char *s = argv[1 + f];
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        if (!*s) { uart_puts("bad hex\r\n"); return -1; }
        /* Bound digit count: a uint64_t accumulator silently drops bits
         * past 16 hex digits. Reject explicitly so typos surface. */
        int ndigits = 0;
        while (*s) {
            uint64_t d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = 10 + (*s - 'a');
            else if (*s >= 'A' && *s <= 'F') d = 10 + (*s - 'A');
            else { uart_puts("bad hex\r\n"); return -1; }
            if (++ndigits > 16) { uart_puts("hex too long\r\n"); return -1; }
            fields[f] = (fields[f] << 4) | d;
            s++;
        }
    }
    uint64_t addr = fields[0];
    uint32_t val = (uint32_t)fields[1];

    /* Unaligned MMIO writes take a synchronous data abort on ARM64.
     * Reject with a readable message instead of crashing the shell. */
    if (addr & 0x3) {
        uart_puts("addr not 4-byte aligned\r\n");
        return -1;
    }

    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    *p = val;
    /* Full barrier so the MMIO write commits to the interconnect
     * before we print: dsb sy on ARM64, mfence on x86-64. mfence is
     * a full fence on x86; the trailing uart_printf can't observe a
     * stale store buffer. */
#if defined(PLATFORM_X86_64)
    __asm__ volatile("mfence" ::: "memory");
#else
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    uart_printf("[0x%lx] <- 0x%08lx\r\n",
                (unsigned long)addr, (unsigned long)val);
    return 0;
}

#if defined(PLATFORM_JETSON_ORIN_NANO)
/*
 * xhci - Show Tegra XHCI host-controller info (#266 Phase 3A). No
 * arguments. Dumps parsed capabilities + live USBCMD / USBSTS /
 * PAGESIZE. Reports "not live" if xhci_init bailed (typically because
 * the xusb clocks weren't held through kexec).
 */
int cmd_xhci(int argc, char *argv[])
{
    (void)argc; (void)argv;
    extern bool xhci_dump_info(void);
    if (!xhci_dump_info())
        shell_puts("xhci: not live (clocks gated? check slmos-kexec)\r\n");
    return 0;
}
#endif

/*
 * gpu - Show GPU driver status and optionally read a BAR0 register.
 *
 *   gpu                     Show GPU info via the registered driver
 *   gpu read <hex-offset>   Read 32-bit BAR0 register (Jetson-only —
 *                           uses the fixed 0x17000000 GPU MMIO base)
 */
int cmd_gpu(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "read") == 0) {
#if defined(PLATFORM_JETSON_ORIN_NANO)
        if (argc < 3) {
            shell_puts("usage: gpu read <hex-offset>\r\n");
            return -1;
        }
        /* Parse hex offset. Accepts "0x1200" or "1200". */
        const char *s = argv[2];
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
        uint32_t off = 0;
        while (*s) {
            uint32_t d;
            if (*s >= '0' && *s <= '9') d = *s - '0';
            else if (*s >= 'a' && *s <= 'f') d = 10 + (*s - 'a');
            else if (*s >= 'A' && *s <= 'F') d = 10 + (*s - 'A');
            else { shell_puts("bad hex offset\r\n"); return -1; }
            off = (off << 4) | d;
            s++;
        }
        volatile uint32_t *reg = (volatile uint32_t *)((uintptr_t)GPU_BASE + off);
        uint32_t val = *reg;
        shell_printf("GPU[0x%lx] = 0x%08lx\r\n",
                    (unsigned long)off, (unsigned long)val);
        return 0;
#else
        shell_puts("gpu read: only supported on JETSON_ORIN_NANO\r\n");
        return -1;
#endif
    }

    gpu_info_t info = {0};
    int rc = gpu_get_info(&info);
    if (rc != GPU_OK) {
        shell_printf("GPU info unavailable (rc=%d)\r\n", rc);
        return rc;
    }
    shell_puts("GPU Information:\r\n\r\n");
    shell_printf("  Driver:         %s\r\n", info.name ? info.name : "(null)");
    shell_printf("  Device:         %s\r\n", info.device ? info.device : "(null)");
    shell_printf("  Capabilities:   0x%08lx\r\n", (unsigned long)info.capabilities);
    shell_printf("  CUDA cores:     %lu\r\n", (unsigned long)info.cuda_cores);
    shell_printf("  Tensor cores:   %lu\r\n", (unsigned long)info.tensor_cores);
    shell_printf("  Unified mem:    %s\r\n", info.unified_memory ? "yes" : "no");
    shell_printf("  Memory size:    %lu\r\n", (unsigned long)info.memory_size);
    return 0;
}

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../gpu/nvidia/ga10b_bringup.h"

/*
 * nvgpu - Jetson GA10B nvgpu-native bringup driver (ACR → FECS → GPCCS
 *        → PMU → channel → first method). Alternative to gsp_init()
 *        for the integrated GPU.
 *
 *   nvgpu                Show current bringup state + firmware inventory
 *   nvgpu prepare        Run phase 0 (Falcon probes + firmware check)
 *   nvgpu run            Attempt all phases end-to-end
 *   nvgpu info           Show firmware inventory only
 */
int cmd_nvgpu(int argc, char *argv[])
{
    static struct ga10b_bringup b;

    if (argc < 2 || strcmp(argv[1], "info") == 0) {
        shell_puts("GA10B firmware inventory:\r\n");
        static const char *NAMES[GA10B_FW_KIND_COUNT] = {
            "acr_text", "acr_data", "acr_manifest",
            "fecs", "fecs_sig",
            "gpccs", "gpccs_sig",
            "pmu_image", "pmu_desc", "pmu_sig",
            "net_a", "net_b", "net_c", "net_d",
            "safety_text", "safety_data", "safety_manifest",
        };
        for (int k = 0; k < GA10B_FW_KIND_COUNT; k++) {
            struct ga10b_firmware_blob blob;
            int rc = ga10b_firmware_get((enum ga10b_firmware_kind)k, &blob);
            if (rc < 0) {
                shell_printf("  %-20s  (not embedded)\r\n", NAMES[k]);
            } else {
                shell_printf("  %-20s  %8lu bytes\r\n",
                            NAMES[k], (unsigned long)blob.size);
            }
        }
        if (argc < 2) {
            shell_printf("\r\nState: %d  Last error phase: %d\r\n",
                        (int)b.state, b.last_error_phase);
        }
        return 0;
    }

    if (strcmp(argv[1], "prepare") == 0) {
        int rc = ga10b_bringup_prepare(&b);
        shell_printf("prepare: rc=%d\r\n", rc);
        return rc;
    }

    if (strcmp(argv[1], "acr") == 0) {
        /* Always re-run prepare — it's cheap (memset + probe) and
         * guarantees the Falcon context is populated (imem_size etc.).
         * Without this, fresh shell invocations run acr against a
         * zeroed `struct falcon` and fail size checks. */
        int rc = ga10b_bringup_prepare(&b);
        if (rc < 0) {
            shell_printf("prepare failed: rc=%d\r\n", rc);
            return rc;
        }
        rc = ga10b_bringup_acr(&b);
        shell_printf("acr: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }

    if (strcmp(argv[1], "inherit") == 0) {
        /* Path 3 (#190): detect Linux's already-bootstrapped Falcon
         * state after a --no-gpu-suspend kexec. Skips phases 1-4. */
        int rc = ga10b_bringup_inherit(&b);
        shell_printf("inherit: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }

    if (strcmp(argv[1], "run") == 0) {
        int rc = ga10b_bringup_run(&b);
        shell_printf("run: rc=%d, state=%d, last_err_phase=%d\r\n",
                    rc, (int)b.state, b.last_error_phase);
        return rc;
    }

    /* Per-phase invocations for step-by-step debugging. Each runs the
     * corresponding phase against the persistent `b` — preceding phases
     * must have completed so `b->state` satisfies the phase precondition. */
    if (strcmp(argv[1], "fecs") == 0) {
        int rc = ga10b_bringup_fecs(&b);
        shell_printf("fecs: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "gpccs") == 0) {
        int rc = ga10b_bringup_gpccs(&b);
        shell_printf("gpccs: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "pmu") == 0) {
        int rc = ga10b_bringup_pmu(&b);
        shell_printf("pmu: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "test") == 0) {
        /* Phase 5: FECS method gateway smoke test. */
        int rc = ga10b_bringup_address_space(&b);
        shell_printf("test: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }

    if (strcmp(argv[1], "channel") == 0) {
        /* Phase 6: inherit channel from Linux handoff block. */
        int rc = ga10b_bringup_channel(&b);
        shell_printf("channel: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "submit") == 0) {
        /* Phase 7: pushbuffer smoke test. */
        int rc = ga10b_bringup_smoke_test(&b);
        shell_printf("submit: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }

    shell_puts("usage: nvgpu [info | prepare | inherit | acr | test | "
              "channel | submit | fecs | gpccs | pmu | run]\r\n");
    return -1;
}
#endif /* PLATFORM_JETSON_ORIN_NANO */

/*
 * sched - Show or change scheduler policy.
 *
 *   sched              Show current policy name
 *   sched policy       List all registered policies
 *   sched policy <name> Switch to named policy
 */
int cmd_sched(int argc, char *argv[])
{
    if (argc < 2) {
        shell_printf("Scheduler policy: %s\r\n", sched_get_policy());
        return 0;
    }

    if (strcmp(argv[1], "policy") == 0) {
        if (argc < 3) {
            /* List available policies */
            int count = sched_policy_count();
            const char *current = sched_get_policy();
            shell_printf("Available policies (%d):\r\n", count);
            for (int i = 0; i < count; i++) {
                const struct sched_policy_ops *p = sched_policy_get(i);
                if (p) {
                    shell_printf("  %s%s\r\n", p->name,
                                strcmp(p->name, current) == 0 ? " (active)" : "");
                }
            }
            return 0;
        }

        /* Switch to named policy */
        const struct sched_policy_ops *policy = sched_find_policy(argv[2]);
        if (!policy) {
            shell_printf("Unknown policy: '%s'\r\n", argv[2]);
            shell_puts("Use 'sched policy' to list available policies.\r\n");
            return 1;
        }

        int ret = sched_set_policy(policy);
        if (ret < 0) {
            shell_printf("Failed to switch to policy '%s'\r\n", argv[2]);
            return 1;
        }

        shell_printf("Switched to policy: %s\r\n", policy->name);
        return 0;
    }

    if (strcmp(argv[1], "trace") == 0) {
        /* Subcommands:
         *   sched trace            — dump recorded events
         *   sched trace start      — enable recording (clears buffer)
         *   sched trace stop       — disable recording
         *   sched trace clear      — discard recorded events
         *   sched trace per-cpu    — ASCII per-CPU timeline summary
         */
        const char *sub = (argc >= 3) ? argv[2] : "show";
        if (strcmp(sub, "start") == 0) {
            sched_trace_start();
            shell_printf("Trace recording started (buffer: %u events)\r\n",
                        SCHED_TRACE_CAPACITY);
            return 0;
        }
        if (strcmp(sub, "stop") == 0) {
            sched_trace_stop();
            shell_puts("Trace recording stopped\r\n");
            return 0;
        }
        if (strcmp(sub, "clear") == 0) {
            sched_trace_clear();
            shell_puts("Trace buffer cleared\r\n");
            return 0;
        }

        /* Snapshot events. Size the stack copy at a cap so we don't
         * blow the 8 KB shell task stack. 512 events × 16 bytes = 8 KB,
         * which is the most the caller can see in one dump. */
        enum { DUMP_MAX = 512 };
        static struct sched_trace_record buf[DUMP_MAX];
        uint32_t n = sched_trace_snapshot(buf, DUMP_MAX);
        uint64_t total = sched_trace_total_events();

        shell_printf("Trace: %s   captured %u of %lu total events%s\r\n\r\n",
                    sched_trace_is_enabled() ? "ON" : "OFF",
                    n, (unsigned long)total,
                    total > SCHED_TRACE_CAPACITY ? " (older overwritten)" : "");

        if (strcmp(sub, "per-cpu") == 0) {
            /* Bucket the time range into TIMELINE_SLOTS columns. For
             * each (cpu, slot), count events. Tasks running in that
             * bucket render as '#', scheduling noise as '.', idle
             * as ' '. Column count chosen to fit a 78-col terminal. */
            enum { TIMELINE_SLOTS = 48 };
            if (n == 0) {
                shell_puts("(no events recorded — use `sched trace start`)\r\n");
                return 0;
            }
            uint64_t t_first = buf[0].timestamp_ns;
            uint64_t t_last  = buf[n - 1].timestamp_ns;
            uint64_t span_ns = t_last > t_first ? (t_last - t_first) : 1u;
            uint8_t cells[8][TIMELINE_SLOTS] = {0};
            uint32_t hits[8] = {0};
            uint32_t max_cpu = 0;

            for (uint32_t i = 0; i < n; i++) {
                uint32_t c = buf[i].cpu;
                if (c >= 8) continue;
                if (c > max_cpu) max_cpu = c;
                uint64_t rel = buf[i].timestamp_ns - t_first;
                uint64_t slot_u64 = (rel * TIMELINE_SLOTS) / span_ns;
                if (slot_u64 >= TIMELINE_SLOTS) slot_u64 = TIMELINE_SLOTS - 1;
                uint32_t slot = (uint32_t)slot_u64;
                /* Non-idle run => '#', migrate => '>', else '.'. */
                char mark = '.';
                if (buf[i].event == SCHED_TRACE_SCHED &&
                    buf[i].next_task_id != 0) {
                    mark = '#';
                } else if (buf[i].event == SCHED_TRACE_MIGRATE) {
                    mark = '>';
                }
                /* Overwrite priority: '#' > '>' > '.'. */
                char prev = (char)cells[c][slot];
                if (prev == 0 || mark == '#' ||
                    (mark == '>' && prev != '#')) {
                    cells[c][slot] = (uint8_t)mark;
                }
                hits[c]++;
            }

            uint64_t span_us = span_ns / 1000u;
            shell_printf("Window: %lu us    Legend: # = run/sched, "
                        "> = migrate, . = tick/noise\r\n\r\n",
                        (unsigned long)span_us);
            for (uint32_t c = 0; c <= max_cpu; c++) {
                shell_printf("CPU %u ", c);
                for (uint32_t s = 0; s < TIMELINE_SLOTS; s++) {
                    char m = (char)cells[c][s];
                    shell_putc(m ? m : ' ');
                }
                shell_printf(" (%u events)\r\n", hits[c]);
            }
            return 0;
        }

        /* Default: tabular dump of the snapshot. */
        shell_puts("Time(us)    CPU  Event     Task  From->To\r\n");
        shell_puts("----------  ---  --------  ----  --------\r\n");
        uint64_t t_first = (n > 0) ? buf[0].timestamp_ns : 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t rel = (buf[i].timestamp_ns - t_first) / 1000u;
            const char *evn = "?";
            switch (buf[i].event) {
            case SCHED_TRACE_SCHED:   evn = "SCHED";   break;
            case SCHED_TRACE_MIGRATE: evn = "MIGRATE"; break;
            case SCHED_TRACE_WAKE:    evn = "WAKE";    break;
            case SCHED_TRACE_PREEMPT: evn = "PREEMPT"; break;
            }
            if (buf[i].event == SCHED_TRACE_MIGRATE) {
                shell_printf("%10lu  %3u  %-8s  %4u  CPU%u->CPU%u\r\n",
                            (unsigned long)rel, buf[i].cpu, evn,
                            (unsigned)buf[i].next_task_id,
                            (unsigned)buf[i].prev_cpu,
                            (unsigned)buf[i].cpu);
            } else {
                shell_printf("%10lu  %3u  %-8s  %4u  %u->%u\r\n",
                            (unsigned long)rel, buf[i].cpu, evn,
                            (unsigned)buf[i].next_task_id,
                            (unsigned)buf[i].prev_task_id,
                            (unsigned)buf[i].next_task_id);
            }
        }
        return 0;
    }

    if (strcmp(argv[1], "stats") == 0) {
        struct sched_stats stats;
        scheduler_get_stats(&stats);

        shell_puts("Scheduler Statistics:\r\n");
        shell_printf("  Policy:           %s\r\n", sched_get_policy());
        shell_printf("  Tasks:            %u\r\n", stats.task_count);
        shell_printf("  Ready:            %u\r\n", stats.ready_count);
        shell_printf("  Context switches: %lu\r\n", (unsigned long)stats.context_switches);
        shell_printf("  Timer ticks:      %lu\r\n", (unsigned long)stats.timer_ticks);

#ifdef CONFIG_AI_SCHEDULER
        shell_puts("\r\nPer-CPU Utilization:\r\n");
        for (uint32_t c = 0; c < cpu_count; c++) {
            struct cpu_runqueue *rq = sched_cpu_rq(c);
            uint32_t pct = 0;
            if (rq->total_ticks > 0) {
                pct = (uint32_t)(rq->running_ticks * 100 / rq->total_ticks);
            }
            shell_printf("  CPU %u: %u%% (%lu / %lu ticks)\r\n",
                        c, pct,
                        (unsigned long)rq->running_ticks,
                        (unsigned long)rq->total_ticks);
        }

        /* AI policy stats and action histogram */
        {
            extern void sched_ai_get_stats(const char *, uint32_t *, uint32_t *,
                                           uint64_t *, const uint32_t **, int *);
            const char *policy = sched_get_policy();
            uint32_t ai_dec, ai_fb;
            uint64_t ai_lat;
            const uint32_t *hist;
            int n_act;
            sched_ai_get_stats(policy, &ai_dec, &ai_fb, &ai_lat, &hist, &n_act);
            if (ai_dec > 0) {
                shell_printf("\r\nAI Policy (%s):\r\n", policy);
                shell_printf("  Decisions:    %u\r\n", ai_dec);
                shell_printf("  Fallbacks:    %u\r\n", ai_fb);
                shell_printf("  Avg latency:  %lu ns\r\n", (unsigned long)ai_lat);
                if (hist && n_act > 0) {
                    shell_puts("  Action distribution:\r\n");
                    for (int a = 0; a < n_act; a++) {
                        if (hist[a] == 0) continue;
                        struct ai_sched_action act;
                        ai_decode_action(a, &act);
                        shell_printf("    [%d] core=%u pri=%u pre=%u: %u (%u%%)\r\n",
                                    a, act.core_assignment, act.priority_adj,
                                    act.preempt, hist[a],
                                    (uint32_t)(hist[a] * 100 / ai_dec));
                    }
                }
            }
        }
#endif

        return 0;
    }

    if (strcmp(argv[1], "compare") == 0) {
        /* #193: run the context-switch microbenchmark under every
         * registered policy and print a side-by-side table. The
         * workload is the same for each policy so differences in the
         * per-round-trip latency and context_switches count are
         * attributable to the scheduler, not the measurement harness.
         */
        int n_policies = sched_policy_count();
        if (n_policies <= 0) {
            shell_puts("sched compare: no policies registered\r\n");
            return 1;
        }

        /* Save the current policy so we can restore at the end. */
        const char *saved_name = sched_get_policy();
        const struct sched_policy_ops *saved_policy = NULL;
        for (int i = 0; i < n_policies; i++) {
            const struct sched_policy_ops *p = sched_policy_get(i);
            if (p && saved_name && strcmp(p->name, saved_name) == 0) {
                saved_policy = p;
                break;
            }
        }

        shell_puts("Scheduler policy comparison\r\n");
        shell_puts("---------------------------\r\n");
        shell_puts("Workload: bench context (" "100"
                  " round-trips, TASK_PRIORITY_HIGH)\r\n\r\n");
        shell_puts("Policy           Runtime(ms)   Ctx switches   Avg(us)\r\n");
        shell_puts("---------------  ------------  -------------  -------\r\n");

        uint64_t best_avg_ns = UINT64_MAX;
        const char *best_policy = NULL;

        for (int i = 0; i < n_policies; i++) {
            const struct sched_policy_ops *p = sched_policy_get(i);
            if (!p) continue;

            /* Switch to this policy; stay on failure to avoid a
             * scramble — caller will see the label and the row. */
            int rc = sched_set_policy(p);
            if (rc < 0) {
                shell_printf("%-15s  %-12s  %-13s  %s\r\n",
                            p->name, "—", "—", "switch failed");
                continue;
            }

            /* Snapshot counters before the run. Silence the bench's
             * own output so the compare table stays readable. */
            struct sched_stats s0, s1;
            scheduler_get_stats(&s0);
            uint64_t t0 = slm_get_time_ns();

            bench_ctx_quiet = true;
            bench_context_switch();
            bench_ctx_quiet = false;

            uint64_t t1 = slm_get_time_ns();
            scheduler_get_stats(&s1);

            uint64_t elapsed_ns = t1 > t0 ? (t1 - t0) : 0;
            uint64_t elapsed_ms = elapsed_ns / 1000000ULL;
            uint64_t ctx = s1.context_switches - s0.context_switches;
            uint64_t avg_ns = 0;
            if (bench_ctx_count > 1) {
                avg_ns = bench_ctx_total / (uint64_t)(bench_ctx_count - 1);
            }
            uint64_t avg_us = avg_ns / 1000;

            shell_printf("%-15s  %12lu  %13lu  %7lu\r\n",
                        p->name,
                        (unsigned long)elapsed_ms,
                        (unsigned long)ctx,
                        (unsigned long)avg_us);

            if (avg_ns > 0 && avg_ns < best_avg_ns) {
                best_avg_ns = avg_ns;
                best_policy = p->name;
            }
        }

        if (best_policy) {
            shell_printf("\r\nBest average context-switch latency: %s "
                        "(%lu us)\r\n",
                        best_policy, (unsigned long)(best_avg_ns / 1000));
        }

        /* Restore the caller's original policy. */
        if (saved_policy) {
            sched_set_policy(saved_policy);
            shell_printf("Restored policy: %s\r\n", saved_policy->name);
        }
        return 0;
    }

    shell_puts("Usage: sched [policy [<name>] | stats | compare | "
              "trace [start|stop|clear|per-cpu]]\r\n");
    return 1;
}

/* ============================================================================
 * eviction — AI eviction policy inspection and control (Phase AI-Eviction M7)
 * ============================================================================
 *
 * Subcommands:
 *   eviction                — summary: current policy, pool utilisation,
 *                             eviction counts
 *   eviction policy         — list available policies (active one tagged)
 *   eviction policy <name>  — switch to the named policy at runtime
 *   eviction stats          — detailed snapshot including CACHEUS expert
 *                             weights when the installed policy is an
 *                             ensemble selector
 */

static void eviction_print_summary(const RustEvictionStats *s, const char *name)
{
    if (!s->feature_enabled) {
        shell_puts("AI eviction: disabled (build with AI_EVICTION=ON)\r\n");
        return;
    }
    shell_puts("AI eviction:\r\n");
    shell_printf("  Policy:              %s\r\n", name);
    shell_printf("  Models:              %s\r\n",
                s->models_available ? "trained (xgb + mlp)" : "stubs");
    shell_printf("  Weight pool:         %zu / %zu blocks allocated\r\n",
                s->weight_allocated, s->weight_total);
    shell_printf("  Workspace pool:      %zu / %zu blocks allocated\r\n",
                s->workspace_allocated, s->workspace_total);
    shell_printf("  Evictions (weight):  %lu\r\n",
                (unsigned long)s->weight_evictions);
    shell_printf("  Evictions (ws):      %lu\r\n",
                (unsigned long)s->workspace_evictions);
    shell_printf("  Evictable candidates (snapshot): %d\r\n",
                s->snapshot_candidates);
    /* #115: per-policy decision/fallback/latency counters. Reset on
     * every policy swap, so these reflect the currently-installed
     * policy's lifetime only. */
    shell_printf("  Decisions:           %lu\r\n",
                (unsigned long)s->policy_decisions);
    shell_printf("  Fallbacks:           %lu\r\n",
                (unsigned long)s->policy_fallbacks);
    shell_printf("  Avg select latency:  %lu ns\r\n",
                (unsigned long)s->policy_avg_latency_ns);
}

static void eviction_print_policies(const char *active)
{
    const uint8_t *list = rust_eviction_policy_list();
    shell_puts("Available eviction policies:\r\n");
    /* `list` is a single space-separated string terminated by NUL. */
    const char *cursor = (const char *)list;
    while (*cursor) {
        /* Skip leading spaces. */
        while (*cursor == ' ') cursor++;
        if (!*cursor) break;
        /* Find end of this token. */
        const char *end = cursor;
        while (*end && *end != ' ') end++;
        size_t len = (size_t)(end - cursor);
        bool is_active = false;
        if (active) {
            size_t alen = 0;
            while (active[alen] != 0) alen++;
            if (alen == len && strncmp(cursor, active, len) == 0) {
                is_active = true;
            }
        }
        shell_puts("  ");
        for (size_t i = 0; i < len; i++) {
            shell_putc(cursor[i]);
        }
        if (is_active) shell_puts(" (active)");
        shell_puts("\r\n");
        cursor = end;
    }
}

static void eviction_print_stats(const RustEvictionStats *s, const char *name)
{
    eviction_print_summary(s, name);
    if (s->cacheus_expert_count > 0) {
        shell_printf("\r\nCACHEUS expert weights (%u experts):\r\n",
                    s->cacheus_expert_count);
        const char *expert_labels[5] = {"expert0","expert1","expert2","expert3","expert4"};
        /* Weights are supplied in basis points (0..10000) so this
         * formatter stays entirely in integer arithmetic. The trait
         * doesn't expose per-expert names through the stats blob;
         * ml_only is (XGBoost, MLP) in order; all_5 is (LRU, LFU,
         * SLM, XGBoost, MLP). */
        for (uint32_t i = 0; i < s->cacheus_expert_count && i < 5; i++) {
            uint32_t bp = s->expert_weights_bp[i];
            /* Render as D.DD% from basis points. */
            shell_printf("  %s: %3u.%02u%%\r\n",
                        expert_labels[i], bp / 100, bp % 100);
        }
    }
}

int cmd_eviction(int argc, char *argv[])
{
    char name_buf[32];
    name_buf[0] = 0;
    rust_eviction_policy_name((uint8_t *)name_buf, sizeof(name_buf));

    RustEvictionStats stats = {0};
    rust_eviction_get_stats(&stats);

    if (argc < 2) {
        eviction_print_summary(&stats, name_buf);
        return 0;
    }

    if (strcmp(argv[1], "policy") == 0) {
        if (argc < 3) {
            /* Show per-pool policy names (#120). */
            char wp[32] = {0}, sp[32] = {0};
            rust_eviction_policy_name_pool(0, (uint8_t *)wp, sizeof(wp));
            rust_eviction_policy_name_pool(1, (uint8_t *)sp, sizeof(sp));
            shell_printf("Weight pool policy:    %s\r\n", wp[0] ? wp : "(none)");
            shell_printf("Workspace pool policy: %s\r\n", sp[0] ? sp : "(none)");
            shell_puts("\r\n");
            eviction_print_policies(name_buf);
            shell_puts("\r\nUsage:\r\n");
            shell_puts("  eviction policy <name>           — set both pools\r\n");
            shell_puts("  eviction policy weight <name>    — set weight pool only\r\n");
            shell_puts("  eviction policy workspace <name> — set workspace pool only\r\n");
            return 0;
        }

        /* Per-pool variant: `eviction policy weight <name>` or
         * `eviction policy workspace <name>` (#120). */
        if (argc >= 4 &&
            (strcmp(argv[2], "weight") == 0 || strcmp(argv[2], "workspace") == 0)) {
            uint8_t pool_id = (strcmp(argv[2], "weight") == 0) ? 0 : 1;
            int rc = rust_eviction_policy_set_pool(pool_id,
                                                    (const uint8_t *)argv[3]);
            if (rc == -2) {
                shell_puts("AI eviction disabled — rebuild with AI_EVICTION=ON\r\n");
                return 1;
            }
            if (rc != 0) {
                shell_printf("Unknown policy: '%s'\r\n", argv[3]);
                return 1;
            }
            char buf[32] = {0};
            rust_eviction_policy_name_pool(pool_id, (uint8_t *)buf, sizeof(buf));
            shell_printf("Set %s pool policy: %s\r\n", argv[2], buf);
            return 0;
        }

        /* Global variant: `eviction policy <name>` (sets both pools). */
        int rc = rust_eviction_policy_set((const uint8_t *)argv[2]);
        if (rc == -2) {
            shell_puts("AI eviction disabled — rebuild with AI_EVICTION=ON\r\n");
            return 1;
        }
        if (rc != 0) {
            shell_printf("Unknown policy: '%s'\r\n", argv[2]);
            shell_puts("Use 'eviction policy' to list available policies.\r\n");
            return 1;
        }
        rust_eviction_policy_name((uint8_t *)name_buf, sizeof(name_buf));
        shell_printf("Switched weight pool to: %s (workspace reset to LRU)\r\n", name_buf);
        return 0;
    }

    if (strcmp(argv[1], "stats") == 0) {
        eviction_print_stats(&stats, name_buf);
        return 0;
    }

    if (strcmp(argv[1], "trajectory") == 0) {
        /* Pull the last N entries (default 16) of the CACHEUS weight
         * trajectory. Empty if CACHEUS is not the active policy or the
         * ring is empty. */
        uint32_t requested = 16;
        if (argc >= 3) {
            uint32_t v;
            if (shell_parse_uint(argv[2], &v) < 0 || v == 0) {
                shell_puts("eviction trajectory: count must be a positive integer\r\n");
                return 1;
            }
            if (v > 128) v = 128;
            requested = v;
        }
        static RustTrajectoryEntry traj[128];
        int32_t n = rust_eviction_get_trajectory(traj, requested);
        if (n < 0) {
            shell_puts("eviction trajectory: invalid request\r\n");
            return 1;
        }
        shell_printf("CACHEUS trajectory: %d entries (policy: %s)\r\n\r\n",
                    n, name_buf);
        if (n == 0) {
            shell_puts("(no trajectory available — CACHEUS must be installed "
                      "and feedback received)\r\n");
            return 0;
        }
        shell_puts("Time(ms)    Experts  Weights (basis points, 1 bp = 0.01%)\r\n");
        shell_puts("----------  -------  ------------------------------------\r\n");
        uint64_t t0 = traj[0].timestamp_ns;
        for (int32_t i = 0; i < n; i++) {
            uint64_t rel_ms = (traj[i].timestamp_ns - t0) / 1000000ULL;
            shell_printf("%10lu  %7u ", (unsigned long)rel_ms, traj[i].n_experts);
            for (uint32_t k = 0; k < traj[i].n_experts && k < 5; k++) {
                uint32_t bp = traj[i].weights_bp[k];
                /* Render as D.DD%% with no float arithmetic. */
                shell_printf(" %3u.%02u%%", bp / 100, bp % 100);
            }
            shell_puts("\r\n");
        }
        return 0;
    }

    if (strcmp(argv[1], "demo") == 0 || strcmp(argv[1], "pressure") == 0) {
        /* #194: deliberately push the weight pool to capacity and
         * beyond so the user can watch the active eviction policy
         * pick victims live. The demo allocates 2 MB blocks using
         * the same FFI the model loader uses, so the decisions
         * flowing through the registry are real eviction decisions
         * — not a mock. Allocations are freed at the end to leave
         * the system in a clean state.
         */
        extern void *rust_model_alloc_weights_raw(size_t size);
        /* Re-declare the ModelHandle-returning API locally: matches
         * kernel/tests/test_model_mem.c. We don't have a public
         * header for ModelHandle, so repeat the struct here to keep
         * the demo self-contained. */
        typedef struct {
            uint16_t block_index;
            uint8_t  pool_id;
            uint8_t  generation;
            uint32_t _reserved;
        } DemoHandle;
        extern DemoHandle rust_model_alloc_weights(size_t size);
        extern int rust_model_free(DemoHandle handle);

        const size_t MODEL_BLOCK_SIZE = 2u * 1024u * 1024u;
        /* Cap at 192 attempts so we always reach eviction even on
         * the 256 MB / 128-block weight pool, with headroom for a
         * few eviction rounds. Backs a static handle table so the
         * demo doesn't heap-allocate. */
        enum { MAX_DEMO_ALLOCS = 192 };
        static DemoHandle handles[MAX_DEMO_ALLOCS];
        int held = 0;
        /* Stop after this many observed evictions to keep output short. */
        const int TARGET_EVICTIONS = 3;

        RustEvictionStats before = {0};
        rust_eviction_get_stats(&before);

        shell_printf("Eviction pressure demo (policy: %s)\r\n",
                    name_buf[0] ? name_buf : "(none)");
        shell_printf("Weight pool: %lu / %lu blocks used, %lu evictions so far\r\n",
                    (unsigned long)before.weight_allocated,
                    (unsigned long)before.weight_total,
                    (unsigned long)before.weight_evictions);
        shell_printf("Plan: allocate 2 MB blocks until %d evictions observed.\r\n\r\n",
                    TARGET_EVICTIONS);

        shell_puts("Event       Step    Pool (used/total)   New evictions\r\n");
        shell_puts("----------  ----    -----------------   -------------\r\n");

        uint64_t prev_evictions = before.weight_evictions;
        int admitted_quiet = 0;
        bool aborted = false;

        for (int i = 0; i < MAX_DEMO_ALLOCS; i++) {
            DemoHandle h = rust_model_alloc_weights(MODEL_BLOCK_SIZE);
            bool is_null = (h.block_index == 0xFFFF && h.pool_id == 0xFF);
            RustEvictionStats after = {0};
            rust_eviction_get_stats(&after);
            uint64_t new_evictions = after.weight_evictions - before.weight_evictions;
            bool evict_step = after.weight_evictions > prev_evictions;
            prev_evictions = after.weight_evictions;

            if (!is_null && held < MAX_DEMO_ALLOCS) {
                handles[held++] = h;
            }

            if (is_null) {
                shell_printf("FAIL        %4d    %5lu / %5lu     %13lu\r\n",
                            i + 1,
                            (unsigned long)after.weight_allocated,
                            (unsigned long)after.weight_total,
                            (unsigned long)new_evictions);
                shell_puts("  (allocator refused further allocation)\r\n");
                aborted = true;
                break;
            }

            if (evict_step) {
                /* Flush pending quiet admits then print the eviction row. */
                if (admitted_quiet > 0) {
                    shell_printf("admit x %-3d %4d    %5lu / %5lu     %13lu\r\n",
                                admitted_quiet, i,
                                (unsigned long)after.weight_allocated,
                                (unsigned long)after.weight_total,
                                (unsigned long)new_evictions);
                    admitted_quiet = 0;
                }
                shell_printf("EVICT       %4d    %5lu / %5lu     %13lu\r\n",
                            i + 1,
                            (unsigned long)after.weight_allocated,
                            (unsigned long)after.weight_total,
                            (unsigned long)new_evictions);
                if ((int)new_evictions >= TARGET_EVICTIONS) {
                    break;
                }
            } else {
                admitted_quiet++;
            }
        }

        if (admitted_quiet > 0 && !aborted) {
            RustEvictionStats mid = {0};
            rust_eviction_get_stats(&mid);
            shell_printf("admit x %-3d         %5lu / %5lu\r\n",
                        admitted_quiet,
                        (unsigned long)mid.weight_allocated,
                        (unsigned long)mid.weight_total);
        }

        /* Snapshot final counters. */
        RustEvictionStats after = {0};
        rust_eviction_get_stats(&after);
        shell_printf("\r\nFinal: %lu / %lu blocks used; %lu total evictions; "
                    "policy decisions: %lu (fallbacks %lu, avg %lu ns)\r\n",
                    (unsigned long)after.weight_allocated,
                    (unsigned long)after.weight_total,
                    (unsigned long)after.weight_evictions,
                    (unsigned long)after.policy_decisions,
                    (unsigned long)after.policy_fallbacks,
                    (unsigned long)after.policy_avg_latency_ns);

        /* Clean up so the demo is idempotent. Handles for evicted
         * blocks are now invalid and rust_model_free returns -1 on
         * them — we ignore that and count only successful frees. */
        int freed = 0;
        for (int i = 0; i < held; i++) {
            if (rust_model_free(handles[i]) == 0) {
                freed++;
            }
        }
        shell_printf("Released %d of %d demo allocations (the rest were evicted).\r\n",
                    freed, held);
        return 0;
    }

    if (strcmp(argv[1], "features") == 0) {
        uint32_t count = rust_eviction_feature_count();
        if (count == 0) {
            shell_puts("AI eviction disabled — no features available.\r\n");
            return 0;
        }
        shell_printf("Eviction feature vector (%u features):\r\n\r\n", count);
        shell_puts("  Index  Name\r\n");
        shell_puts("  -----  ----------------------------\r\n");
        for (uint32_t i = 0; i < count; i++) {
            char fbuf[48] = {0};
            rust_eviction_feature_name(i, (uint8_t *)fbuf, sizeof(fbuf));
            const char *group = (i < 15) ? "per-block" : "global";
            shell_printf("  %5u  %-28s  (%s)\r\n", i, fbuf, group);
        }
        return 0;
    }

    shell_puts("Usage: eviction [policy [<name>] | stats | features | "
              "trajectory [N] | demo | pressure]\r\n");
    return 1;
}

/* ============================================================================
 * timdiag — Timer/interrupt delivery diagnostic
 *
 * Probes GIC group state, timer registers, and tests interrupt delivery
 * paths. Primary purpose: investigate hardware timer preemption on
 * platforms where COOP_PREEMPT is the current workaround.
 * ============================================================================ */

#if !defined(PLATFORM_X86_64)

/* Volatile counter incremented by the FIQ test handler */
volatile uint32_t timdiag_fiq_count;
volatile uint32_t timdiag_fiq_irqnum;

static void timdiag_dump_timer_state(void)
{
    uint64_t cntfrq, cntpct, cntp_ctl, cntp_cval, cntp_tval;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cntpct));
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(cntp_ctl));
    __asm__ volatile("mrs %0, cntp_cval_el0" : "=r"(cntp_cval));
    __asm__ volatile("mrs %0, cntp_tval_el0" : "=r"(cntp_tval));

    shell_printf("  CNTFRQ_EL0:    %lu Hz\r\n", cntfrq);
    shell_printf("  CNTPCT_EL0:    0x%lx\r\n", cntpct);
    shell_printf("  CNTP_CTL_EL0:  0x%lx (EN=%lu IMASK=%lu ISTATUS=%lu)\r\n",
                cntp_ctl,
                cntp_ctl & 1, (cntp_ctl >> 1) & 1, (cntp_ctl >> 2) & 1);
    shell_printf("  CNTP_CVAL_EL0: 0x%lx\r\n", cntp_cval);
    shell_printf("  CNTP_TVAL_EL0: %ld\r\n", (int64_t)cntp_tval);

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* On Jetson at EL2+VHE, CNTHP registers are directly accessible */
    uint64_t cnthp_ctl;
    __asm__ volatile("mrs %0, cnthp_ctl_el2" : "=r"(cnthp_ctl));
    shell_printf("  CNTHP_CTL_EL2: 0x%lx (EN=%lu IMASK=%lu ISTATUS=%lu)\r\n",
                cnthp_ctl,
                cnthp_ctl & 1, (cnthp_ctl >> 1) & 1, (cnthp_ctl >> 2) & 1);
#endif
}

/* GIC_VERSION is defined in platform.h for Jetson (3) and Pi 5 (2).
 * For QEMU virt, platform.h does not define it. Default to 2. */
#ifndef GIC_VERSION
#define GIC_VERSION 2
#endif

#if GIC_VERSION == 3
static void timdiag_dump_gicv3(void)
{
    uint32_t cpu = cpu_id();
    /* GICR SGI base = GICR_BASE + cpu_offset + 0x10000 */
    /* We read the registers via the same macros gic.c uses, but need
     * access to the gicr_sgi_base. Since that's static in gic.c,
     * re-derive from the platform constants. For Jetson, the offsets
     * are hardcoded in gic.c; we can read the registers directly. */

    /* Read ICC system registers.
     * NOTE: ICC_IGRPEN0_EL1 is trapped by TF-A on Jetson (even reads).
     * TF-A configures EL3 to trap all Group 0 ICC register accesses from
     * NS. Reading ICC_IGRPEN0 causes EC=0x18 trap → "Unhandled Exception
     * from EL2" crash. Only read Group 1 NS registers. */
    uint64_t icc_sre, icc_pmr, icc_bpr1, icc_ctlr, icc_igrpen1;
    __asm__ volatile("mrs %0, ICC_SRE_EL1" : "=r"(icc_sre));
    __asm__ volatile("mrs %0, ICC_PMR_EL1" : "=r"(icc_pmr));
    __asm__ volatile("mrs %0, ICC_BPR1_EL1" : "=r"(icc_bpr1));
    __asm__ volatile("mrs %0, ICC_CTLR_EL1" : "=r"(icc_ctlr));
    /* ICC_IGRPEN0_EL1: SKIP — trapped by EL3 on two-security-state GICv3 */
    __asm__ volatile("mrs %0, ICC_IGRPEN1_EL1" : "=r"(icc_igrpen1));

    shell_printf("\r\n  GICv3 CPU Interface (CPU %u):\r\n", cpu);
    shell_printf("    ICC_SRE_EL1:    0x%lx (SRE=%lu)\r\n",
                icc_sre, icc_sre & 1);
    shell_printf("    ICC_PMR_EL1:    0x%lx\r\n", icc_pmr);
    shell_printf("    ICC_BPR1_EL1:   0x%lx\r\n", icc_bpr1);
    shell_printf("    ICC_CTLR_EL1:   0x%lx (EOImode=%lu)\r\n",
                icc_ctlr, (icc_ctlr >> 1) & 1);
    shell_printf("    ICC_IGRPEN0_EL1: (trapped by EL3 — not readable from NS)\r\n");
    shell_printf("    ICC_IGRPEN1_EL1: %lu (Group 1 %s)\r\n",
                icc_igrpen1, icc_igrpen1 ? "ENABLED" : "disabled");

    /* Read GICR registers for this CPU.
     * We need the redistributor base. For Jetson, use the hardcoded offsets.
     * For other GICv3 platforms, use stride. */
#if defined(PLATFORM_JETSON_ORIN_NANO)
    static const uint32_t jetson_redist_offset[] = {
        0x000000, 0x020000, 0x040000, 0x060000,
        0x0C0000, 0x0E0000
    };
    uintptr_t gicr_rd = GIC_REDIST_BASE + (cpu < 6 ? jetson_redist_offset[cpu] : 0);
#else
    uintptr_t gicr_rd = GIC_REDIST_BASE + (cpu * 0x20000);
#endif
    uintptr_t gicr_sgi = gicr_rd + 0x10000;

    uint32_t igroupr0  = *(volatile uint32_t *)(gicr_sgi + 0x080);
    uint32_t igrpmodr0 = *(volatile uint32_t *)(gicr_sgi + 0xD00);
    uint32_t isenabler0 = *(volatile uint32_t *)(gicr_sgi + 0x100);
    uint32_t ispendr0  = *(volatile uint32_t *)(gicr_sgi + 0x200);
    uint32_t waker     = *(volatile uint32_t *)(gicr_rd + 0x014);

    shell_printf("\r\n  GICR (Redistributor, CPU %u at 0x%lx):\r\n",
                cpu, (unsigned long)gicr_rd);
    shell_printf("    GICR_WAKER:     0x%x (Sleep=%u ChildrenAsleep=%u)\r\n",
                waker, (waker >> 1) & 1, (waker >> 2) & 1);
    shell_printf("    GICR_IGROUPR0:  0x%08x", igroupr0);
    if (igroupr0 == 0)
        shell_puts(" (ALL Group 0 — NS writes blocked by EL3)\r\n");
    else if (igroupr0 == 0xFFFFFFFF)
        shell_puts(" (ALL Group 1 NS)\r\n");
    else
        shell_printf(" (mixed: PPI30=%u PPI26=%u PPI27=%u)\r\n",
                    (igroupr0 >> 30) & 1, (igroupr0 >> 26) & 1,
                    (igroupr0 >> 27) & 1);
    shell_printf("    GICR_IGRPMODR0: 0x%08x", igrpmodr0);
    if (igrpmodr0 == 0)
        shell_puts(" (RAZ from NS — expected)\r\n");
    else
        shell_printf(" (unexpected non-zero!)\r\n");
    shell_printf("    GICR_ISENABLER0: 0x%08x (PPI30=%u PPI26=%u PPI27=%u)\r\n",
                isenabler0,
                (isenabler0 >> 30) & 1, (isenabler0 >> 26) & 1,
                (isenabler0 >> 27) & 1);
    shell_printf("    GICR_ISPENDR0:  0x%08x (PPI30=%u PPI26=%u PPI27=%u)\r\n",
                ispendr0,
                (ispendr0 >> 30) & 1, (ispendr0 >> 26) & 1,
                (ispendr0 >> 27) & 1);

    /* GICD state */
    uint32_t gicd_ctlr = *(volatile uint32_t *)(GIC_DIST_BASE + 0x000);
    shell_printf("\r\n  GICD_CTLR: 0x%x (ARE_NS=%u EN_G1=%u EN_G0=%u)\r\n",
                gicd_ctlr,
                (gicd_ctlr >> 4) & 1, (gicd_ctlr >> 1) & 1, gicd_ctlr & 1);

    /* GICD_IGROUPR for first few SPI banks (check if SPIs are Group 1 NS) */
    shell_puts("\r\n  GICD_IGROUPR (SPI groups, NS view):\r\n");
    for (uint32_t i = 1; i <= 4; i++) {
        uint32_t igroupr = *(volatile uint32_t *)(GIC_DIST_BASE + 0x080 + 4 * i);
        shell_printf("    GICD_IGROUPR[%u]: 0x%08x (IRQs %u-%u)%s\r\n",
                    i, igroupr, i * 32, i * 32 + 31,
                    igroupr == 0xFFFFFFFF ? " ALL G1NS" :
                    igroupr == 0 ? " ALL G0/G1S" : "");
    }

    /* DAIF state */
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    shell_printf("\r\n  DAIF: 0x%lx (D=%lu A=%lu I=%lu F=%lu)\r\n",
                daif,
                (daif >> 9) & 1, (daif >> 8) & 1,
                (daif >> 7) & 1, (daif >> 6) & 1);
}

/*
 * Test 1: Enable Group 0 delivery + unmask FIQ, see if timer fires as FIQ.
 *
 * Chain of reasoning:
 *   - Timer PPI 30 is in Group 0 (confirmed by GICR_IGROUPR0=0x0)
 *   - Group 0 interrupts generate FIQ
 *   - If SCR_EL3.FIQ=0, FIQ is taken at current EL (EL2 with VHE)
 *   - If SCR_EL3.FIQ=1, FIQ is taken at EL3 (we never see it)
 *
 * This test enables ICC_IGRPEN0 and unmasks DAIF.F to see which case
 * we're in. The el1_fiq handler will read ICC_IAR0 and increment
 * timdiag_fiq_count.
 */
static void timdiag_test_fiq(void)
{
    shell_puts("\r\n--- Test: FIQ delivery (Group 0 + DAIF.F unmask) ---\r\n");

    /* Save current state */
    uint64_t saved_igrpen0;
    __asm__ volatile("mrs %0, ICC_IGRPEN0_EL1" : "=r"(saved_igrpen0));
    uint64_t saved_daif;
    __asm__ volatile("mrs %0, daif" : "=r"(saved_daif));

    /* Reset FIQ test counter */
    timdiag_fiq_count = 0;
    timdiag_fiq_irqnum = 0xFFFFFFFF;
    __asm__ volatile("dmb ish" ::: "memory");

    /* Ensure timer is running and will fire soon */
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t short_interval = freq / 1000; /* 1ms */
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(short_interval));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1)); /* Enable, unmask */
    __asm__ volatile("isb" ::: "memory");

    /* Enable Group 0 delivery */
    __asm__ volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"((uint64_t)1));
    __asm__ volatile("isb" ::: "memory");

    /* Unmask FIQ (DAIF.F clear) — keep IRQ masked to isolate the test */
    __asm__ volatile("msr daifclr, #1" ::: "memory"); /* #1 = FIQ */
    __asm__ volatile("isb" ::: "memory");

    /* Spin for ~5ms checking if FIQ fires */
    uint64_t start;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    uint64_t deadline = start + (freq / 200); /* 5ms */
    uint64_t now = start;
    while (now < deadline && timdiag_fiq_count == 0) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    }

    /* Re-mask FIQ */
    __asm__ volatile("msr daifset, #1" ::: "memory");

    /* Restore Group 0 enable state */
    __asm__ volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"(saved_igrpen0));
    __asm__ volatile("isb" ::: "memory");

    /* Restore timer to normal operation */
    uint64_t normal_interval = freq / TIMER_HZ;
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(normal_interval));
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)1));
    __asm__ volatile("isb" ::: "memory");

    /* Check timer ISTATUS */
    uint64_t cntp_ctl;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(cntp_ctl));

    uint32_t elapsed_us = (uint32_t)((now - start) * 1000000 / freq);

    if (timdiag_fiq_count > 0) {
        shell_printf("  RESULT: FIQ DELIVERED! count=%u irq=%u (elapsed=%u us)\r\n",
                    timdiag_fiq_count, timdiag_fiq_irqnum, elapsed_us);
        shell_puts("  >>> SCR_EL3.FIQ=0 — FIQ-based timer preemption IS viable!\r\n");
    } else {
        shell_printf("  RESULT: No FIQ after %u us. ISTATUS=%lu\r\n",
                    elapsed_us, (cntp_ctl >> 2) & 1);
        if ((cntp_ctl >> 2) & 1)
            shell_puts("  Timer condition IS asserted but FIQ not delivered.\r\n"
                      "  >>> SCR_EL3.FIQ=1 — FIQ trapped to EL3.\r\n");
        else
            shell_puts("  Timer did not fire (unexpected).\r\n");
    }
}

/*
 * Test 2: Hypervisor physical timer (CNTHP, PPI 26) — Jetson EL2 only.
 *
 * Same as Test 1 but using the EL2 hypervisor timer. PPI 26 may be in
 * a different group than PPI 30, or TF-A may treat it differently.
 */
#if defined(PLATFORM_JETSON_ORIN_NANO)
static void timdiag_test_cnthp(void)
{
    shell_puts("\r\n--- Test: CNTHP (hypervisor timer PPI 26) via FIQ ---\r\n");

    uint32_t cpu = cpu_id();

    /* Enable PPI 26 in the redistributor */
    static const uint32_t jetson_redist_offset[] = {
        0x000000, 0x020000, 0x040000, 0x060000,
        0x0C0000, 0x0E0000
    };
    uintptr_t gicr_sgi = GIC_REDIST_BASE +
        (cpu < 6 ? jetson_redist_offset[cpu] : 0) + 0x10000;
    *(volatile uint32_t *)(gicr_sgi + 0x100) = (1u << 26); /* ISENABLER0: enable PPI 26 */
    /* Set priority for PPI 26 */
    volatile uint32_t *priptr = (volatile uint32_t *)(gicr_sgi + 0x400 + (26/4)*4);
    uint32_t prival = *priptr;
    prival &= ~(0xFF << ((26 % 4) * 8));
    prival |= (0x80 << ((26 % 4) * 8));
    *priptr = prival;
    __asm__ volatile("dsb sy" ::: "memory");

    /* Save state */
    uint64_t saved_igrpen0;
    __asm__ volatile("mrs %0, ICC_IGRPEN0_EL1" : "=r"(saved_igrpen0));

    timdiag_fiq_count = 0;
    timdiag_fiq_irqnum = 0xFFFFFFFF;
    __asm__ volatile("dmb ish" ::: "memory");

    /* Program CNTHP for 1ms */
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t short_interval = freq / 1000;
    __asm__ volatile("msr cnthp_tval_el2, %0" :: "r"(short_interval));
    __asm__ volatile("msr cnthp_ctl_el2, %0" :: "r"((uint64_t)1)); /* Enable */
    __asm__ volatile("isb" ::: "memory");

    /* Enable Group 0 delivery + unmask FIQ */
    __asm__ volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"((uint64_t)1));
    __asm__ volatile("isb" ::: "memory");
    __asm__ volatile("msr daifclr, #1" ::: "memory");
    __asm__ volatile("isb" ::: "memory");

    /* Spin for ~5ms */
    uint64_t start;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    uint64_t deadline = start + (freq / 200);
    uint64_t now = start;
    while (now < deadline && timdiag_fiq_count == 0) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    }

    /* Re-mask FIQ, restore state */
    __asm__ volatile("msr daifset, #1" ::: "memory");
    __asm__ volatile("msr cnthp_ctl_el2, %0" :: "r"((uint64_t)0)); /* Disable CNTHP */
    __asm__ volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"(saved_igrpen0));
    __asm__ volatile("isb" ::: "memory");

    /* Disable PPI 26 */
    *(volatile uint32_t *)(gicr_sgi + 0x180) = (1u << 26); /* ICENABLER0 */
    __asm__ volatile("dsb sy" ::: "memory");

    uint64_t cnthp_ctl;
    __asm__ volatile("mrs %0, cnthp_ctl_el2" : "=r"(cnthp_ctl));

    uint32_t elapsed_us = (uint32_t)((now - start) * 1000000 / freq);

    /* Check pending bit for PPI 26 */
    uint32_t ispendr0 = *(volatile uint32_t *)(gicr_sgi + 0x200);

    if (timdiag_fiq_count > 0) {
        shell_printf("  RESULT: FIQ DELIVERED via CNTHP! count=%u irq=%u (%u us)\r\n",
                    timdiag_fiq_count, timdiag_fiq_irqnum, elapsed_us);
        shell_puts("  >>> CNTHP (PPI 26) FIQ delivery works!\r\n");
    } else {
        shell_printf("  RESULT: No FIQ after %u us. CNTHP ISTATUS=%lu PPI26_PEND=%u\r\n",
                    elapsed_us, (cnthp_ctl >> 2) & 1, (ispendr0 >> 26) & 1);
    }
}
#endif /* PLATFORM_JETSON_ORIN_NANO */

/*
 * Test 3: WFI wake-up test.
 *
 * Even if FIQ is trapped to EL3, WFI may still wake on the pending
 * interrupt condition. This tests whether the idle task's WFI loop
 * can be driven by the timer without GIC-delivered interrupts.
 */
#endif /* GIC_VERSION == 3 */

static void timdiag_test_wfi_wake(void)
{
    shell_puts("\r\n--- Test: WFI wake-up from timer ---\r\n");
#if GIC_VERSION == 3
    shell_puts("  SKIP: WFI wake-up test (would hang on this platform)\r\n");
    shell_puts("  Reason: All PPIs/SPIs are Group 0. ICC_IGRPEN0 trapped by EL3.\r\n");
    shell_puts("  SCR_EL3.FIQ=1 routes Group 0 FIQ to EL3. No wake source for WFI.\r\n");
    shell_puts("  >>> WFI timer wake: NOT VIABLE.\r\n");
#else
    shell_puts("  SKIP: WFI wake-up test\r\n");
    shell_puts("  >>> WFI timer wake: NOT VIABLE (see pi5-preemption-resolution.md).\r\n");
#endif
}

int cmd_timdiag(int argc, char *argv[])
{
    (void)argc; (void)argv;
    shell_puts("\r\n=== Timer/Interrupt Delivery Diagnostic ===\r\n");
    shell_printf("Platform: %s\r\n", PLATFORM_NAME);
    shell_printf("timer_handler_count: %u\r\n", timer_handler_count);
    shell_printf("pit_ticks: %lu\r\n", pit_ticks);
#if defined(COOP_PREEMPT)
    shell_puts("COOP_PREEMPT: ON\r\n");
#else
    shell_puts("COOP_PREEMPT: OFF\r\n");
#endif

    shell_puts("\r\nTimer State:\r\n");
    timdiag_dump_timer_state();

#if GIC_VERSION == 3
    timdiag_dump_gicv3();

    const char *what = (argc >= 2) ? argv[1] : "safe";
    if (strcmp(what, "fiq") == 0) {
        shell_puts("\r\nWARNING: FIQ test writes ICC_IGRPEN0 — may crash if TF-A traps it!\r\n");
        timdiag_test_fiq();
#if defined(PLATFORM_JETSON_ORIN_NANO)
        timdiag_test_cnthp();
#endif
    } else {
        shell_puts("\r\n(FIQ test skipped — run 'timdiag fiq' to test, may crash on Jetson)\r\n");
    }

    timdiag_test_wfi_wake();
#elif GIC_VERSION == 2
    shell_puts("\r\nGICv2 platform — FIQ test skipped (see pi5-preemption-resolution.md)\r\n");
    timdiag_test_wfi_wake();
#endif

    shell_puts("\r\n=== End Diagnostic ===\r\n");
    return 0;
}

#endif /* !PLATFORM_X86_64 */

#if defined(PLATFORM_RASPI5) && defined(ENABLE_NETWORKING)

#include "macb.h"

/*
 * MACB IRQ delivery diagnostic — answers "does RP1 MSIX_CFG fire on
 * Cadence MACB peripheral assertion, or does it hit the same blocker
 * that UART RX ran into?" Prints irq_count (bumped by macb_irq_handler),
 * the last MACB_ISR, GIC registration state, and live MIP0 MSIX_CFG
 * for vector 6 (ETH).
 */
int cmd_macbdiag(int argc, char *argv[])
{
    (void)argc; (void)argv;

    shell_puts("\r\n=== MACB IRQ Diagnostic ===\r\n");

    shell_printf("  GIC handler registered: %s\r\n",
                macb_irq_is_registered() ? "YES" : "NO");
    shell_printf("  IRQ count:              %u\r\n",
                macb_get_irq_count());
    shell_printf("  Last MACB_ISR observed: 0x%08x\r\n",
                macb_get_last_isr());

    /* Live MIP0 state for RP1 vector 6 (ETH) */
    volatile uint32_t *msix_cfg =
        (volatile uint32_t *)(RP1_INTC_BASE + RP1_MSIX_CFG(RP1_INT_ETH));
    uint32_t cfg = *msix_cfg;
    shell_printf("  MIP0 MSIX_CFG[vec %u]:   0x%08x",
                (unsigned)RP1_INT_ETH, cfg);
    if (cfg & MSIX_CFG_ENABLE)   shell_puts(" ENABLE");
    if (cfg & MSIX_CFG_IACK_EN)  shell_puts(" IACK_EN");
    if (cfg & MSIX_CFG_IACK)     shell_puts(" IACK");
    shell_puts("\r\n");

    volatile uint32_t *intstatl =
        (volatile uint32_t *)(RP1_INTC_BASE + RP1_INTC_INTSTATL);
    uint32_t stat = *intstatl;
    shell_printf("  MIP0 INTSTATL (0-31):   0x%08x\r\n", stat);
    shell_printf("    ETH vec %u asserted:    %s\r\n",
                (unsigned)RP1_INT_ETH,
                (stat & (1u << RP1_INT_ETH)) ? "YES" : "no");

    shell_puts("\r\n=== End Diagnostic ===\r\n");
    return 0;
}

#endif /* PLATFORM_RASPI5 && ENABLE_NETWORKING */

#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)
#include "eth_rtl8169.h"

/*
 * rtldiag — Tegra PCIe C8 / RTL8168 probe diagnostic. Reports in
 * increasing depth:
 *   1. APPL controller wrapper state (CTRL, DEBUG/LTSSM) — tells us
 *      whether the RC block is clocked + reset-deasserted.
 *   2. DBI bus-0 config-space readout — the RC bridge ID (NVIDIA
 *      0x10DE:0x229c if the RC survived kexec).
 *   3. RTL8168 endpoint state (bus 1 — requires iATU programming,
 *      not yet implemented so marked pending).
 */
int cmd_rtldiag(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uart_puts("\r\n=== Tegra PCIe C8 / RTL8168 Diagnostic ===\r\n");
    uart_puts("  [Reading live APPL + DBI registers — may abort if RC cold]\r\n");

    rtl8169_refresh_rc_state();

    /* Layer 1: APPL wrapper — if clocks are gated or resets asserted,
     * these reads will external-abort before we get here. */
    uart_printf("  APPL_CTRL:   0x%08x  (LTSSM_EN=%s)\r\n",
                (unsigned)rtl8169_get_appl_ctrl(),
                (rtl8169_get_appl_ctrl() & (1u << 7)) ? "1" : "0");
    uart_printf("  APPL_DEBUG:  0x%08x  (LTSSM state [8:3] = 0x%02x)\r\n",
                (unsigned)rtl8169_get_appl_debug(),
                (unsigned)((rtl8169_get_appl_debug() >> 3) & 0x3F));

    /* Layer 2: bus-0 RC bridge via DBI. */
    uart_printf("  DBI bus0:    vendor=0x%04x  device=0x%04x  "
                "(expect 0x10DE:0x229c)\r\n",
                (unsigned)rtl8169_get_rc_bridge_vendor(),
                (unsigned)rtl8169_get_rc_bridge_device());
    uart_printf("  RC alive:    %s\r\n",
                rtl8169_get_rc_alive() ? "YES" : "NO (cold — see kexec note)");

    if (!rtl8169_is_probed()) {
        uart_puts("  Endpoint:    NOT PROBED — bus-1 iATU setup pending\r\n");
        uart_puts("=== End Diagnostic ===\r\n");
        return 0;
    }

    /* Probed path — Stage 2+ fills these fields in. Today the driver
     * returns false from probe() unconditionally, so this block is
     * unreachable at runtime but stays as the shape the shell output
     * takes once the Stage-2 MAC/chip-version read lands. */
    uart_printf("  PCI vendor:   0x%04x  (expected 0x10EC)\r\n",
                (unsigned)rtl8169_get_pci_vendor());
    uart_printf("  PCI device:   0x%04x  (expected 0x8168)\r\n",
                (unsigned)rtl8169_get_pci_device());
    uart_printf("  PCI revision: 0x%02x\r\n",
                (unsigned)rtl8169_get_pci_revision());
    uart_printf("  BAR2 phys:    0x%lx\r\n",
                (unsigned long)rtl8169_get_bar2());

    const uint8_t *mac = rtl8169_get_mac_address();
    if (mac) {
        uart_printf("  MAC addr:     %02x:%02x:%02x:%02x:%02x:%02x\r\n",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        uart_puts("  MAC addr:     (not yet read — Stage 2 pending)\r\n");
    }

    uart_printf("  MAC_VER raw:  0x%x  (0 = not yet read)\r\n",
                (unsigned)rtl8169_get_mac_ver_raw());
    uart_printf("  Link up:      %s\r\n",
                rtl8169_get_link_up() ? "YES" : "no");

    uart_puts("=== End Diagnostic ===\r\n");
    return 0;
}

/*
 * xhcidiag — reads Tegra XHCI host controller registers from SLM-OS
 * at EL2. Diagnostic probe for the USB CDC-ECM fallback path — tests
 * whether the CBB firewall behaves the same way here as it did for
 * PCIe (see docs/jetson-pcie-investigation.md). If the reads return
 * meaningful values, USB-based networking is a viable pivot. If they
 * return 0xFFFFFFFF like the PCIe RC, CBB blocks DMA-capable
 * peripherals at EL2 across the board and we need a different
 * strategy entirely.
 *
 * Expected-good values (verified from Linux /dev/mem):
 *   HCD[0x00] CAPLENGTH|HCIVERSION = 0x01200020
 *   HCD[0x04] HCSPARAMS1           = 0x08000524
 *   FPCI[0x00] device/vendor       = 0x229810de  (NVIDIA Tegra xHCI)
 */
int cmd_xhcidiag(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uart_puts("\r\n=== Tegra XHCI CBB-at-EL2 Probe ===\r\n");
    uart_puts("  [Reading live XHCI registers — may abort if CBB-blocked]\r\n");

    volatile uint32_t *hcd  = (volatile uint32_t *)TEGRA_XHCI_HCD_BASE;
    volatile uint32_t *fpci = (volatile uint32_t *)TEGRA_XHCI_FPCI_BASE;

    uint32_t caplen_hciver = hcd[0];
    uint32_t hcsparams1    = hcd[1];
    uint32_t hcsparams2    = hcd[2];
    uint32_t hcsparams3    = hcd[3];
    uint32_t hccparams1    = hcd[4];
    uint32_t fpci_devven   = fpci[0];

    uart_printf("  HCD CAPLENGTH|HCIVER: 0x%08x  (expect 0x01200020)\r\n",
                (unsigned)caplen_hciver);
    uart_printf("  HCD HCSPARAMS1:       0x%08x  (expect 0x08000524)\r\n",
                (unsigned)hcsparams1);
    uart_printf("  HCD HCSPARAMS2:       0x%08x\r\n", (unsigned)hcsparams2);
    uart_printf("  HCD HCSPARAMS3:       0x%08x\r\n", (unsigned)hcsparams3);
    uart_printf("  HCD HCCPARAMS1:       0x%08x  (expect 0x0180ff05)\r\n",
                (unsigned)hccparams1);
    uart_printf("  FPCI dev/vendor:      0x%08x  (expect 0x229810de)\r\n",
                (unsigned)fpci_devven);

    bool cbb_blocked = (caplen_hciver == 0xFFFFFFFF &&
                        fpci_devven   == 0xFFFFFFFF);
    uart_printf("  Verdict:              %s\r\n",
                cbb_blocked ? "CBB FIREWALL BLOCKS XHCI AT EL2"
                            : "XHCI ACCESSIBLE AT EL2");

    uart_puts("=== End XHCI Probe ===\r\n");
    return 0;
}
#endif /* PLATFORM_JETSON_ORIN_NANO && ENABLE_NETWORKING */

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "bpmp.h"
#include "hsp.h"

/*
 * hspdiag — probe the HSP controller directly (no BPMP IPC required).
 *
 * Verifies three things before the BPMP IPC stack can work:
 *   1. HSP_DIMENSIONING reads cleanly (not 0xffffffff). If it does,
 *      the MMIO mapping is broken or the HSP block is powered down.
 *   2. The computed BPMP doorbell address matches expectations for
 *      Tegra234 (should be near HSP+0x140000, far from the hardcoded
 *      0x10300 the old driver used).
 *   3. The BPMP doorbell's ENABLE register has the CCPLEX bit set,
 *      meaning BPMP has authorised us to ring it.
 *
 * Safe to run even if bpmp_init() failed or was never called.
 */
int cmd_hspdiag(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uart_puts("\r\n=== HSP / BPMP Doorbell Diagnostic ===\r\n");

    /* Idempotent init. If bpmp_init already ran, this returns 0 quickly
     * because hsp_init caches its own state. If not, we ran the
     * dimensioning read ourselves. */
    int rc = hsp_init(HSP_TOP_BASE);
    uart_printf("  hsp_init(0x%lx): rc=%d\r\n",
                (unsigned long)HSP_TOP_BASE, rc);
    if (rc != 0) {
        uart_puts("=== End Diagnostic ===\r\n");
        return 0;
    }

    uint32_t dim = hsp_dimensioning_raw();
    uint32_t sm = dim & 0xF;
    uint32_t ss = (dim >> 4) & 0xF;
    uint32_t as = (dim >> 8) & 0xF;
    uart_printf("  DIMENSIONING (0x%08x):  SharedMailboxes=%u "
                "SharedSemaphores=%u ArbitratedSemaphores=%u\r\n",
                (unsigned)dim, (unsigned)sm, (unsigned)ss, (unsigned)as);

    uintptr_t ccplex_db = hsp_ccplex_doorbell_addr();
    uintptr_t bpmp_db   = hsp_bpmp_doorbell_addr();
    uart_printf("  Doorbell CCPLEX:        0x%lx\r\n", (unsigned long)ccplex_db);
    uart_printf("  Doorbell BPMP:          0x%lx\r\n", (unsigned long)bpmp_db);

    uint32_t bpmp_enable  = *(volatile uint32_t *)(bpmp_db + HSP_DB_REG_ENABLE);
    uint32_t bpmp_pending = *(volatile uint32_t *)(bpmp_db + HSP_DB_REG_PENDING);
    uart_printf("  BPMP ENABLE:            0x%08x  (CCPLEX bit 17 = %s)\r\n",
                (unsigned)bpmp_enable,
                (bpmp_enable & (1u << HSP_DB_MASTER_CCPLEX)) ? "SET"
                                                             : "clear (BPMP not listening)");
    uart_printf("  BPMP PENDING:           0x%08x\r\n", (unsigned)bpmp_pending);

    uint32_t ccplex_enable  = *(volatile uint32_t *)(ccplex_db + HSP_DB_REG_ENABLE);
    uint32_t ccplex_pending = *(volatile uint32_t *)(ccplex_db + HSP_DB_REG_PENDING);
    uart_printf("  CCPLEX ENABLE:          0x%08x  (BPMP bit 19 = %s)\r\n",
                (unsigned)ccplex_enable,
                (ccplex_enable & (1u << HSP_DB_MASTER_BPMP)) ? "SET"
                                                              : "clear");
    uart_printf("  CCPLEX PENDING:         0x%08x  (BPMP notification = %s)\r\n",
                (unsigned)ccplex_pending,
                (ccplex_pending & (1u << HSP_DB_MASTER_BPMP)) ? "YES"
                                                               : "no");

    uart_puts("=== End Diagnostic ===\r\n");
    return 0;
}

/*
 * bpmp — exercise the BPMP IPC stack end-to-end.
 *
 *   bpmp         full test suite (ping, clock queries, pcie enable,
 *                rtldiag readback).
 *   bpmp ping    MRQ_PING round-trip only.
 *   bpmp clk     CMD_CLK_IS_ENABLED queries on UART-A + PEX2_C8_CORE.
 *   bpmp pcie    Enable PEX2_C8_CORE clock + deassert its resets,
 *                then re-read APPL_CTRL via the rtldiag pipeline.
 *                THIS IS THE #25 STEP 3 KICKOFF.
 */
int cmd_bpmp(int argc, char *argv[])
{
    const char *mode = (argc >= 2) ? argv[1] : "all";

    uart_puts("\r\n=== BPMP IPC Smoke Test ===\r\n");

    int rc = bpmp_init();
    uart_printf("  bpmp_init:            rc=%d\r\n", rc);
    if (rc != 0) {
        uart_puts("=== End Smoke Test ===\r\n");
        return 0;
    }

    /* Step 1: MRQ_PING. */
    if (argc < 2 || argv[1][0] == 'a' || argv[1][0] == 'p') {
        bool ok = bpmp_is_available();
        uart_printf("  MRQ_PING round-trip:  %s\r\n",
                    ok ? "OK" : "FAIL");
        if (mode[0] == 'p' && mode[1] == 'i') { /* "ping" */
            uart_puts("=== End Smoke Test ===\r\n");
            return 0;
        }
    }

    /* Step 2: Clock query smoke tests. UART-A should always be enabled
     * (Linux just used it); PEX2_C8_CORE state is what #25 cares about. */
    if (mode[0] == 'a' || mode[0] == 'c') {
        int uart_state = -1;
        int uart_rc = bpmp_clk_is_enabled(TEGRA234_CLK_UARTA, &uart_state);
        uart_printf("  UART_A IS_ENABLED:    rc=%d state=%d (expect 1)\r\n",
                    uart_rc, uart_state);

        int pcie_state = -1;
        int pcie_rc = bpmp_clk_is_enabled(TEGRA234_CLK_PEX2_C8_CORE,
                                          &pcie_state);
        uart_printf("  PEX2_C8_CORE EN:      rc=%d state=%d\r\n",
                    pcie_rc, pcie_state);
    }

    /* Step 3: Actually enable PEX2_C8_CORE + deassert the PCIe resets.
     * The real demonstration of BPMP IPC — and the door-opener for #25
     * Step 3 (PCIe RC re-init). Only runs when explicitly requested
     * (not under `bpmp all`) so a stray exec doesn't disturb the
     * already-live PCIe state when slmos-kexec pre-held the clocks. */
    if (mode[0] == 'p' && mode[1] == 'c') { /* "pcie" */
        uart_puts("\r\n--- PCIe C8 clock/reset sequence ---\r\n");
        int en_rc = bpmp_clk_enable(TEGRA234_CLK_PEX2_C8_CORE);
        uart_printf("  CLK_ENABLE(PEX2_C8_CORE):          rc=%d\r\n", en_rc);

        int rs_rc = bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8);
        uart_printf("  RESET_DEASSERT(PEX2_CORE_8):       rc=%d\r\n", rs_rc);

        int ra_rc = bpmp_reset_deassert(TEGRA234_RESET_PEX2_CORE_8_APB);
        uart_printf("  RESET_DEASSERT(PEX2_CORE_8_APB):   rc=%d\r\n", ra_rc);

        int post_state = -1;
        (void)bpmp_clk_is_enabled(TEGRA234_CLK_PEX2_C8_CORE, &post_state);
        uart_printf("  PEX2_C8_CORE post-enable state:    %d\r\n",
                    post_state);

        uart_puts("  (Run `rtldiag` next to check APPL liveness.)\r\n");
    }

    uart_puts("=== End Smoke Test ===\r\n");
    return 0;
}
#endif /* PLATFORM_JETSON_ORIN_NANO */
