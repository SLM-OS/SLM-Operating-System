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

#if defined(PLATFORM_RASPI5)
    {
        extern int uart_is_irq_mode(void);
        uart_printf("\r\n  UART RX:     %s\r\n",
                    uart_is_irq_mode() ? "interrupt-driven" : "polling");

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
        uart_printf("  GICC: CTLR=0x%x PMR=0x%x BPR=0x%x RPR=0x%x HPPIR=%u\r\n",
                    *gicc_ctlr, *gicc_pmr, *gicc_bpr, *gicc_rpr, *gicc_hppir);

        /* Try software-triggering the interrupt to test handler */
        if (!uart_is_irq_mode()) {
            volatile uint32_t *ispendr_w = (volatile uint32_t *)((uint64_t)GIC_DIST_BASE + 0x200 + 4 * pend_reg);
            *ispendr_w = (1U << pend_bit);  /* Set pending */
            __asm__ volatile("dsb sy; isb" ::: "memory");
            uart_printf("  [Triggered IRQ %d — check UART RX mode on next `cpu`]\r\n", UART_IRQ);
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

int cmd_reboot(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uart_puts("Rebooting...\r\n");

    /* Use PSCI SYSTEM_RESET */
    register uint64_t x0 __asm__("x0") = 0x84000009;  /* SYSTEM_RESET */
    __asm__ volatile(
        "hvc #0"
        : "+r"(x0)
        :
        : "x1", "x2", "x3", "memory"
    );

    /* If PSCI fails, spin */
    uart_puts("Reboot failed!\r\n");
    while (1) {
        __asm__ volatile("wfi");
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
