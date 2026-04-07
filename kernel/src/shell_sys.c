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
    uart_puts("Available commands:\r\n");
    uart_puts("\r\n");

    /* Built-in commands */
    for (int i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
        uart_printf("  %-10s %s\r\n",
                    builtin_commands[i].name,
                    builtin_commands[i].help);
    }

    /* External commands */
    for (int i = 0; i < num_external_commands; i++) {
        uart_printf("  %-10s %s\r\n",
                    external_commands[i].name,
                    external_commands[i].help);
    }

    uart_puts("\r\n");
    uart_puts("Use 'help <cmd>' for detailed help on a command.\r\n");
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

    uart_puts("Memory Statistics:\r\n");
    uart_puts("\r\n");
    uart_printf("  Total:     %lu KB (%lu pages)\r\n", total_kb, total_pages);
    uart_printf("  Used:      %lu KB (%lu pages)\r\n", used_kb, used_pages);
    uart_printf("  Free:      %lu KB (%lu pages)\r\n", free_kb, free_pages);
    uart_puts("\r\n");

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

    uart_puts("Task List:\r\n");
    uart_puts("\r\n");
    uart_puts("  ID  Name             State       Pri  CPU  Switches\r\n");
    uart_puts("  --  ---------------  ----------  ---  ---  --------\r\n");

    /* Iterate through possible task IDs */
    for (uint32_t id = 0; id < MAX_TASKS; id++) {
        struct task *t = task_get(id);
        if (t != NULL) {
            uart_printf("  %2lu  %-15s  %-10s  %3u  %3lu  %lu\r\n",
                        t->id,
                        t->name,
                        state_name(t->state),
                        t->effective_priority,
                        t->assigned_cpu,
                        t->switches);
        }
    }

    uart_puts("\r\n");
    return 0;
}

/*
 * cpu - Show CPU status
 */
int cmd_cpu(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uart_puts("CPU Status:\r\n");
    uart_puts("\r\n");
    uart_printf("  Platform:    %s\r\n", PLATFORM_NAME);
    uart_printf("  CPUs:        %lu online / %lu total\r\n", cpus_online, cpu_count);
    uart_puts("\r\n");

    uart_puts("  CPU  Status   Isolated  Current Task\r\n");
    uart_puts("  ---  ------   --------  ------------\r\n");

    for (uint32_t i = 0; i < cpu_count; i++) {
        bool online = (i < cpus_online);
        bool isolated = sched_is_core_isolated(i);
        struct task *current = NULL;

        /* Get current task for this CPU (if we can) */
        /* Note: This is a snapshot and may be racy */
        if (online && i == cpu_id()) {
            current = task_current();
        }

        uart_printf("  %3lu  %-6s   %-8s  %s\r\n",
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
        uart_printf("\r\n  Per-CPU scheduler diagnostics:\r\n");
        uart_printf("  CPU  Ticks     Schedule  Picked    IdleLoops\r\n");
        uart_printf("  ---  --------  --------  ------    ---------\r\n");
#if defined(PLATFORM_HAS_NC_MEMORY)
        extern volatile uint32_t *sched_diag_idle_loops;
#else
        extern volatile uint32_t sched_diag_idle_loops[];
#endif
        uart_printf("  diag_tick ptr=0x%lx val[0]=0x%x\r\n",
                    (unsigned long)(uintptr_t)sched_diag_tick,
                    *(volatile uint32_t *)sched_diag_tick);
        for (uint32_t i = 0; i < cpu_count; i++) {
            uint32_t t = sched_diag_tick[i];
            uint32_t s = sched_diag_schedule[i];
            uint32_t p = sched_diag_picked[i];
            uint32_t il = sched_diag_idle_loops[i];
            uart_printf("  %3lu  %8u  %8u  %6u    %9u\r\n", i, t, s, p, il);
        }
        uart_printf("  timer_handler_count: %u\r\n", timer_handler_count);
    }

#if defined(PLATFORM_RASPI5)
    {
        extern int uart_is_irq_mode(void);
        extern volatile uint32_t uart_irq_count;
        uart_printf("\r\n  UART RX:     %s (irq_count=%u)\r\n",
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

        uart_printf("  GIC pend:    %d  MIP status: 0x%x  RP1 INTSTAT: 0x%x\r\n",
                    pending, mip_st, rp1_st);
        uart_printf("  PL011 RIS:   0x%x  MIS: 0x%x\r\n", *ris, *mis);

        /* MSIX_CFG register for UART0 vector */
        volatile uint32_t *msix_cfg = (volatile uint32_t *)(RP1_INTC_BASE + RP1_MSIX_CFG(RP1_INT_UART0));
        uart_printf("  MSIX_CFG[25]:0x%x\r\n", *msix_cfg);

        /* Read MSI-X capability from config space (at 0x8000+0xB0) */
        volatile uint32_t *msix_cap = (volatile uint32_t *)(PCIE_RC_BASE + 0x8000 + 0xB0);
        uart_printf("  MSI-X cap:   0x%x (Enable=%d FuncMask=%d)\r\n",
                    *msix_cap, (*msix_cap >> 31) & 1, (*msix_cap >> 30) & 1);

        /* Read MSI-X table entry 25 */
        volatile uint32_t *e25 = (volatile uint32_t *)(RP1_MSIX_TABLE_BASE + 25 * 16);
        uart_printf("  MSIX tbl[25]:addr=0x%x_%08x data=0x%x ctrl=0x%x\r\n",
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

        uart_printf("  GIC IRQ %d: target=0x%x pri=0x%x active=%d enabled=%d\r\n",
                    UART_IRQ, target, priority, active, enabled);

        /* GICC state from EL1 */
        volatile uint32_t *gicc_ctlr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE);
        volatile uint32_t *gicc_pmr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x4);
        volatile uint32_t *gicc_bpr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x8);
        volatile uint32_t *gicc_rpr = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x14);
        volatile uint32_t *gicc_hppir = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x18);
        volatile uint32_t *gicc_ahppir = (volatile uint32_t *)((uint64_t)GIC_CPU_BASE + 0x28);
        uart_printf("  GICC: CTLR=0x%x PMR=0x%x BPR=0x%x RPR=0x%x HPPIR=%u AHPPIR=%u\r\n",
                    *gicc_ctlr, *gicc_pmr, *gicc_bpr, *gicc_rpr, *gicc_hppir, *gicc_ahppir);

        /* IGROUPR for our SPI */
        uint32_t grp_reg = UART_IRQ / 32;
        uint32_t grp_bit = UART_IRQ % 32;
        volatile uint32_t *igroupr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x080 + 4 * grp_reg);
        int grp1 = (*igroupr >> grp_bit) & 1;
        /* Also check GICD_CTLR */
        volatile uint32_t *gicd_ctlr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE);
        uart_printf("  GICD: CTLR=0x%x IGROUPR[%d] bit %d=%d\r\n",
                    *gicd_ctlr, grp_reg, grp_bit, grp1);
        /* Dump all IGROUPR registers to see which are Group 0 vs Group 1 */
        for (uint32_t g = 0; g < 10; g++) {
            volatile uint32_t *igr = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x080 + 4 * g);
            uart_printf("  IGROUPR[%u]=0x%08x\r\n", g, *igr);
        }

        /* Check SPI priority at various IRQ numbers to find the boundary */
        {
            uint32_t test_irqs[] = {64, 128, 185, 192, 224, 256, 261, 30};
            for (int t = 0; t < 8; t++) {
                uint32_t irq = test_irqs[t];
                volatile uint32_t *preg = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x400 + (irq & ~3));
                uint8_t pri = (*preg >> ((irq & 3) * 8)) & 0xFF;
                uart_printf("  IRQ%u pri=0x%x%s", irq, pri, (t < 7) ? "  " : "\r\n");
            }
        }

        /* BAR3 → MIP0 verification (BAR1 is firmware's RP1 MMIO mapping) */
        {
            volatile uint32_t *b3lo = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_LO);
            volatile uint32_t *b3hi = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_BAR3_CONFIG_HI);
            volatile uint32_t *rlo  = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP);
            volatile uint32_t *rhi  = (volatile uint32_t *)((uint64_t)PCIE_RC_BASE + PCIE_RC_UBUS_BAR3_REMAP_HI);
            uart_printf("  BAR3: %x_%08x remap=%x_%08x\r\n", *b3hi, *b3lo, *rhi, *rlo);
        }
    }
#endif

    uart_puts("\r\n");
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

    uart_printf("Uptime: %lu:%02lu:%02lu\r\n", hours, minutes, seconds);

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
    uart_puts("\033[2J\033[H");

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
        uart_puts("Usage: sleep <ms>\r\n");
        return 1;
    }

    uint32_t ms = (uint32_t)atoi(argv[1]);
    if (ms == 0) {
        uart_puts("sleep: duration must be > 0\r\n");
        return 1;
    }

    uint64_t before = slm_get_time_ns();
    sleep_ms(ms);
    uint64_t after = slm_get_time_ns();
    uint64_t elapsed_ms = (after - before) / 1000000;

    uart_printf("Slept %lu ms (requested %u ms)\r\n", elapsed_ms, ms);
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

/* --- Context switch benchmark --- */

static volatile int bench_ctx_done;
static volatile uint64_t bench_ctx_start;
static volatile uint64_t bench_ctx_total;
static volatile int bench_ctx_count;

static void bench_ctx_task(void *arg)
{
    (void)arg;
    while (bench_ctx_count < BENCH_ITERATIONS) {
        uint64_t now = slm_get_time_ns();
        if (bench_ctx_start > 0) {
            bench_ctx_total += now - bench_ctx_start;
        }
        bench_ctx_count++;
        bench_ctx_start = slm_get_time_ns();
        yield();
    }
    bench_ctx_done = 1;
}

static void bench_context_switch(void)
{
    bench_ctx_done = 0;
    bench_ctx_start = 0;
    bench_ctx_total = 0;
    bench_ctx_count = 0;

    struct task *t = task_create_with_priority("bench_ctx",
        bench_ctx_task, NULL, TASK_PRIORITY_HIGH);
    if (!t) {
        uart_puts("  Failed to create benchmark task\r\n");
        return;
    }
    scheduler_add_task_to_cpu(t, 0);

    /* Wait for completion */
    int timeout = 500;
    while (!bench_ctx_done && timeout > 0) {
        yield();
        timeout--;
    }

    if (bench_ctx_count > 1) {
        uint64_t avg_ns = bench_ctx_total / (uint64_t)(bench_ctx_count - 1);
        uart_printf("  Context switch: %d round-trips\r\n", bench_ctx_count - 1);
        uart_printf("    Average: %lu ns (%lu us)\r\n",
                    (unsigned long)avg_ns, (unsigned long)(avg_ns / 1000));
        if (avg_ns < 10000)
            uart_puts("    Rating:  Excellent (< 10 us)\r\n");
        else if (avg_ns < 50000)
            uart_puts("    Rating:  Good (< 50 us)\r\n");
        else if (avg_ns < 100000)
            uart_puts("    Rating:  Acceptable (< 100 us)\r\n");
        else
            uart_puts("    Rating:  Needs optimization (> 100 us)\r\n");
    } else {
        uart_puts("  Context switch: insufficient data\r\n");
    }
}

/* --- Interrupt latency benchmark --- */

static void bench_irq_latency(void)
{
    uint64_t freq = timer_get_frequency();
    uint64_t expected_interval_ns = 10000000ULL; /* 100 Hz = 10ms */
    uint64_t samples[20];
    int count = 0;

    uart_printf("  Timer frequency: %lu Hz (expected 10 ms ticks)\r\n",
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

    uart_printf("  Timer tick jitter (%d samples):\r\n", count);
    uart_printf("    Min: %lu ns (%lu us)\r\n",
                (unsigned long)min_ns, (unsigned long)(min_ns / 1000));
    uart_printf("    Avg: %lu ns (%lu us)\r\n",
                (unsigned long)avg_ns, (unsigned long)(avg_ns / 1000));
    uart_printf("    Max: %lu ns (%lu us)\r\n",
                (unsigned long)max_ns, (unsigned long)(max_ns / 1000));

    /* Jitter = max deviation from expected interval */
    int64_t jitter = (int64_t)max_ns - (int64_t)min_ns;
    uart_printf("    Jitter (max-min): %lu us\r\n",
                (unsigned long)(jitter > 0 ? (uint64_t)jitter / 1000 : 0));
    (void)expected_interval_ns;
}

/* --- IPC benchmark --- */

static void bench_ipc_latency(void)
{
    struct msg_queue *q = msg_queue_create(16, 8);
    if (!q) {
        uart_puts("  Failed to create message queue\r\n");
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
    uart_printf("  IPC send+recv round-trip (%d iterations):\r\n", ipc_iters);
    uart_printf("    Total: %lu ns\r\n", (unsigned long)total_ns);
    uart_printf("    Average: %lu ns (%lu us)\r\n",
                (unsigned long)avg_ns, (unsigned long)(avg_ns / 1000));

    msg_queue_destroy(q);
}

/* --- Scheduler stats snapshot --- */

static void bench_sched_stats(void)
{
    struct sched_stats stats;
    scheduler_get_stats(&stats);

    uint64_t uptime_ns = slm_get_time_ns();
    uint64_t uptime_s = uptime_ns / 1000000000ULL;

    uart_printf("  Scheduler stats (uptime %lu s):\r\n", (unsigned long)uptime_s);
    uart_printf("    Tasks:            %lu\r\n", (unsigned long)stats.task_count);
    uart_printf("    Context switches: %lu\r\n", (unsigned long)stats.context_switches);
    uart_printf("    Timer ticks:      %lu\r\n", (unsigned long)stats.timer_ticks);
    if (uptime_s > 0) {
        uart_printf("    Switches/sec:     %lu\r\n",
                    (unsigned long)(stats.context_switches / uptime_s));
    }
}

int cmd_bench(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: bench <context|irq|ipc|stats|all>\r\n");
        return 1;
    }

    uart_puts("\r\n");

    if (strcmp(argv[1], "context") == 0) {
        uart_puts("Context Switch Benchmark\r\n");
        uart_puts("========================\r\n");
        bench_context_switch();
    } else if (strcmp(argv[1], "irq") == 0) {
        uart_puts("Interrupt Latency Benchmark\r\n");
        uart_puts("===========================\r\n");
        bench_irq_latency();
    } else if (strcmp(argv[1], "ipc") == 0) {
        uart_puts("IPC Latency Benchmark\r\n");
        uart_puts("=====================\r\n");
        bench_ipc_latency();
    } else if (strcmp(argv[1], "stats") == 0) {
        uart_puts("Scheduler Statistics\r\n");
        uart_puts("====================\r\n");
        bench_sched_stats();
    } else if (strcmp(argv[1], "all") == 0) {
        uart_puts("SLM-OS Performance Benchmarks\r\n");
        uart_puts("=============================\r\n\r\n");
        bench_context_switch();
        uart_puts("\r\n");
        bench_irq_latency();
        uart_puts("\r\n");
        bench_ipc_latency();
        uart_puts("\r\n");
        bench_sched_stats();
    } else {
        uart_printf("Unknown benchmark: %s\r\n", argv[1]);
        uart_puts("Available: context, irq, ipc, stats, all\r\n");
        return 1;
    }

    uart_puts("\r\n");
    return 0;
}

int cmd_reboot(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uart_puts("Rebooting...\r\n");

    psci_system_reset();

    /* If reset fails, spin */
    uart_puts("Reboot failed!\r\n");
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

    uart_puts("Virtual Memory Statistics:\r\n");
    uart_puts("\r\n");
    uart_printf("  L1 tables:       %lu\r\n", (unsigned long)stats.l1_tables);
    uart_printf("  L2 tables:       %lu\r\n", (unsigned long)stats.l2_tables);
    uart_printf("  Blocks mapped:   %lu (2MB each)\r\n", (unsigned long)stats.blocks_mapped);
    uart_printf("  Bytes mapped:    %lu MB\r\n", (unsigned long)(stats.bytes_mapped / (1024 * 1024)));
    uart_puts("\r\n");

    uart_puts("Memory Regions:\r\n");
    uart_puts("  Region           Start            End              Flags\r\n");
    uart_puts("  ---------------  ---------------  ---------------  -----\r\n");

    /* Kernel code/data region */
    uart_printf("  Kernel           0x%08lx       0x%08lx       RWX\r\n",
                (unsigned long)0x40000000, (unsigned long)0x40200000);

    /* Device MMIO region */
    uart_printf("  MMIO (Devices)   0x%08lx       0x%08lx       RW-\r\n",
                (unsigned long)0x08000000, (unsigned long)0x10000000);

    uart_puts("\r\n");
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

    uart_puts("IPC Statistics:\r\n");
    uart_puts("\r\n");

    uart_puts("  Message Queues:\r\n");
    uart_printf("    Active queues:     %lu\r\n", (unsigned long)stats.queue_count);
    uart_printf("    Total msgs sent:   %lu\r\n", (unsigned long)stats.total_msgs_sent);
    uart_printf("    Total msgs recv:   %lu\r\n", (unsigned long)stats.total_msgs_recv);
    uart_puts("\r\n");

    uart_puts("  Shared Buffers:\r\n");
    uart_printf("    Active buffers:    %lu\r\n", (unsigned long)stats.buffer_count);
    uart_puts("\r\n");

    return 0;
}

/*
 * model - Show model memory pool statistics
 */
int cmd_model(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    const size_t block_size_kb = 2048;
    RustPoolStats weight_stats = rust_weight_pool_stats();
    RustPoolStats workspace_stats = rust_workspace_pool_stats();


    uart_puts("Model Memory Pools:\r\n");
    uart_puts("\r\n");

    uart_puts("  Weight Pool (read-only model parameters):\r\n");
    uart_printf("    Block size:      %lu KB\r\n", (unsigned long)block_size_kb);
    uart_printf("    Total blocks:    %lu\r\n", (unsigned long)weight_stats.total_blocks);
    uart_printf("    Free blocks:     %lu\r\n", (unsigned long)weight_stats.free_blocks);
    uart_printf("    Allocated:       %lu\r\n", (unsigned long)weight_stats.allocated_blocks);
    uart_printf("    Shared:          %lu\r\n", (unsigned long)weight_stats.shared_blocks);
    uart_printf("    Peak usage:      %lu\r\n", (unsigned long)weight_stats.peak_usage);
    uart_puts("\r\n");

    uart_puts("  Workspace Pool (inference scratch space):\r\n");
    uart_printf("    Block size:      %lu KB\r\n", (unsigned long)block_size_kb);
    uart_printf("    Total blocks:    %lu\r\n", (unsigned long)workspace_stats.total_blocks);
    uart_printf("    Free blocks:     %lu\r\n", (unsigned long)workspace_stats.free_blocks);
    uart_printf("    Allocated:       %lu\r\n", (unsigned long)workspace_stats.allocated_blocks);
    uart_printf("    Shared:          %lu\r\n", (unsigned long)workspace_stats.shared_blocks);
    uart_printf("    Peak usage:      %lu\r\n", (unsigned long)workspace_stats.peak_usage);
    uart_puts("\r\n");

    /* Calculate totals */
    size_t total_blocks = weight_stats.total_blocks + workspace_stats.total_blocks;
    size_t total_mb = total_blocks * block_size_kb / 1024;
    size_t free_blocks = weight_stats.free_blocks + workspace_stats.free_blocks;
    size_t free_mb = free_blocks * block_size_kb / 1024;

    uart_printf("  Total: %lu blocks (%lu MB), %lu free (%lu MB)\r\n",
                (unsigned long)total_blocks, (unsigned long)total_mb,
                (unsigned long)free_blocks, (unsigned long)free_mb);
    uart_puts("\r\n");

    return 0;
}

/*
 * dtb - Show device tree information
 */
int cmd_dtb(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    const fdt_info_t *info = dtb_get_info();

    uart_puts("Device Tree Information:\r\n");
    uart_puts("\r\n");
    uart_printf("  Status:       %s\r\n", info->valid ? "parsed from DTB" : "using defaults");
    uart_puts("\r\n");

    uart_puts("  Memory:\r\n");
    uart_printf("    Base:       0x%lx\r\n", (unsigned long)info->ram_base);
    uart_printf("    Size:       %lu MB\r\n", (unsigned long)(info->ram_size / (1024 * 1024)));
    uart_puts("\r\n");

    uart_puts("  UART:\r\n");
    uart_printf("    Base:       0x%lx\r\n", (unsigned long)info->uart_base);
    uart_printf("    IRQ:        %lu\r\n", (unsigned long)info->uart_irq);
    uart_puts("\r\n");

    uart_puts("  GIC:\r\n");
    uart_printf("    Dist base:  0x%lx\r\n", (unsigned long)info->gic_dist_base);
    uart_printf("    CPU base:   0x%lx\r\n", (unsigned long)info->gic_cpu_base);
    uart_puts("\r\n");

    uart_puts("  CPUs:\r\n");
    uart_printf("    Count:      %lu\r\n", (unsigned long)info->cpu_count);
    uart_puts("\r\n");

    uart_puts("  Timer:\r\n");
    uart_printf("    IRQ:        %lu\r\n", (unsigned long)info->timer_irq);
    uart_puts("\r\n");

    return 0;
}
