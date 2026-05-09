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
#include "gpu_consumer.h"
#include "gpu_tier.h"
#include "admin_telemetry.h"
#if defined(ENABLE_NETWORKING)
#include "tcp_telemetry_server.h"
#include "net.h"
#endif
#include "model_engine.h"
#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)
#include "cdc_ecm.h"
#endif
#ifdef CONFIG_AI_SCHEDULER
#include "ai_types.h"
#include "runtime_model.h"
#endif
#include "blob_autoload.h"
#include "pmm.h"
#include "runtime_blob_file.h"
#include "vmm.h"
#include "smp.h"
#include "cpu_supervisor.h"
#include "ipc.h"
#include "timer.h"
#include "timdiag_pi5_probes.h"
#include "slm_ffi.h"
#include "platform.h"
#include "ncmem.h"
#include "dtb.h"
#include "help.h"
#include "sdhci.h"
#include "string.h"
#include "../gpu/gpu.h"
#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../gpu/nvidia/ga10b_qmd.h"
#include "ga10b_qmd_selftest_reference.h"
#endif
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
 *   help          - List all commands grouped by category, alphabetized
 *   help <cmd>    - Show detailed help for a specific command
 *
 * The grouped list draws from both `builtin_commands[]` (in shell.c) and
 * `external_commands[]` (registered at runtime by lua/net/hailo/kernel/...).
 * For each category in the order defined by `shell_cmd_category_t`, gather
 * the matching entries from both tables, sort them alphabetically by name
 * with a simple insertion sort (~70 entries — trivial), then print a
 * category header and the entries beneath it.
 *
 * The source-side convention enforced by the comment block over
 * builtin_commands[] keeps the array layout matching this output, so
 * "where is command X registered" and "where does X show up in help"
 * give the same answer.
 */

/* Display labels for each category. Index by shell_cmd_category_t. */
static const char *const shell_cat_labels[SHELL_CAT_COUNT] = {
    [SHELL_CAT_SHELL]      = "Shell",
    [SHELL_CAT_FILESYSTEM] = "Filesystem",
    [SHELL_CAT_SYSINFO]    = "System info",
    [SHELL_CAT_PROCESS]    = "Processes & scheduling",
    [SHELL_CAT_COMPONENTS] = "Components & messaging",
    [SHELL_CAT_SCRIPTING]  = "Scripting & programs",
    [SHELL_CAT_NETWORK]    = "Network",
    [SHELL_CAT_HARDWARE]   = "Hardware control & diagnostics",
};

/* Cap on the number of commands gathered into the per-category sort
 * buffer. Sized for builtin_commands[] (~50) + MAX_EXTERNAL_COMMANDS (16)
 * with headroom. */
#define HELP_GATHER_MAX 96

int cmd_help(int argc, char *argv[])
{
    /* If a command name is given, show detailed help from file */
    if (argc >= 2) {
        return help_show(argv[1]);
    }

    shell_puts("Available commands:\r\n");

    for (int cat = 0; cat < SHELL_CAT_COUNT; cat++) {
        /* Gather every entry whose category matches `cat`, then
         * insertion-sort by name. Two-table walk (builtin + external)
         * means the same category can pull from both. */
        const shell_cmd_t *gathered[HELP_GATHER_MAX];
        int n = 0;
        bool truncated = false;

        /* Cast to (int) on the enum side: under -Werror=sign-compare,
         * GCC treats unscoped enums as unsigned and rejects an enum-vs-
         * `int cat` comparison without an explicit conversion. */
        for (int i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
            if ((int)builtin_commands[i].category == cat) {
                if (n < HELP_GATHER_MAX) {
                    gathered[n++] = &builtin_commands[i];
                } else {
                    truncated = true;
                }
            }
        }
        for (int i = 0; i < num_external_commands; i++) {
            if ((int)external_commands[i].category == cat) {
                if (n < HELP_GATHER_MAX) {
                    gathered[n++] = &external_commands[i];
                } else {
                    truncated = true;
                }
            }
        }

        if (n == 0) {
            continue;  /* category empty for this build (e.g. NETWORK off) */
        }

        /* Insertion sort by name. n <= ~70 — O(n²) is fine. */
        for (int i = 1; i < n; i++) {
            const shell_cmd_t *key = gathered[i];
            int j = i - 1;
            while (j >= 0 && strcmp(gathered[j]->name, key->name) > 0) {
                gathered[j + 1] = gathered[j];
                j--;
            }
            gathered[j + 1] = key;
        }

        shell_puts("\r\n");
        shell_printf("%s:\r\n", shell_cat_labels[cat]);
        for (int i = 0; i < n; i++) {
            shell_printf("  %-12s %s\r\n", gathered[i]->name, gathered[i]->help);
        }
        if (truncated) {
            /* Help output should never silently swallow registered commands.
             * Surface this loudly so a future maintainer increasing the
             * builtin/external command count sees the cap. */
            shell_printf("  [warning: this category exceeded HELP_GATHER_MAX=%d "
                         "— increase the cap in shell_sys.c]\r\n",
                         HELP_GATHER_MAX);
        }
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
    /* `mem buddy [order]` — dump per-order free counts and (when an
     * order is given) walk the free list at that order printing each
     * block's address + next/prev fields. Diagnostic for free-list
     * corruption (e.g., #608's order-19 fault during slm load). */
    if (argc >= 2 && argv[1] && strcmp(argv[1], "buddy") == 0) {
        pmm_dump_stats();
        if (argc >= 3 && argv[2]) {
            unsigned int order = 0;
            for (const char *p = argv[2]; *p >= '0' && *p <= '9'; p++) {
                order = order * 10 + (unsigned int)(*p - '0');
            }
            extern void pmm_dump_free_list(unsigned int, size_t);
            pmm_dump_free_list(order, /* max_blocks = */ 32);
        }
        return 0;
    }

    size_t total_pages = pmm_get_total_pages();
    size_t free_pages = pmm_get_free_pages();
    size_t used_pages = total_pages - free_pages;

    size_t page_size = 4096;
    size_t total_kb = (total_pages * page_size) / 1024;
    size_t free_kb = (free_pages * page_size) / 1024;
    size_t used_kb = (used_pages * page_size) / 1024;

    shell_puts("Memory Statistics:\r\n");
    shell_puts("\r\n");
    shell_puts("PMM (physical pages):\r\n");
    shell_printf("  Total:     %lu KB (%lu pages)\r\n", total_kb, total_pages);
    shell_printf("  Used:      %lu KB (%lu pages)\r\n", used_kb, used_pages);
    shell_printf("  Free:      %lu KB (%lu pages)\r\n", free_kb, free_pages);
    shell_puts("\r\n");

    /* Rust heap (linked_list_allocator) — every Rust-side Vec/Box
     * lives here: SLM forward scratch, KV cache, tokenizer, msg
     * router publish payloads, component runtime, telemetry.
     * Sized by RUST_HEAP_MB in <config.h>. Zeros mean
     * `rust_heap_init` hasn't run yet (very-early-boot path). */
    size_t rh_total = rust_heap_size_bytes();
    if (rh_total > 0u) {
        size_t rh_used = rust_heap_used_bytes();
        size_t rh_free = rust_heap_free_bytes();
        size_t rh_total_kb = rh_total / 1024u;
        size_t rh_used_kb = rh_used / 1024u;
        size_t rh_free_kb = rh_free / 1024u;
        /* Percent used, ceiling-divided so a heap that's anything-
         * but-empty never displays as 0%. The outer `if (rh_total >
         * 0u)` already guards the divide. */
        unsigned int rh_pct = (unsigned int)((rh_used * 100u + rh_total - 1u) / rh_total);
        shell_puts("Rust heap (linked_list_allocator):\r\n");
        shell_printf("  Total:     %lu KB\r\n", rh_total_kb);
        shell_printf("  Used:      %lu KB (%u%%)\r\n", rh_used_kb, rh_pct);
        shell_printf("  Free:      %lu KB\r\n", rh_free_kb);
        shell_puts("\r\n");
    }

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
 * cpu - Show CPU status (default) or run a CPU subcommand.
 *
 * Subcommands (#216 Tier 2):
 *   cpu resurrect <N>  — manually resurrect a dormant secondary CPU
 *                        via psci_cpu_on. Equivalent to the
 *                        supervisor's auto-recovery path; useful for
 *                        integration tests that want to drive recovery
 *                        without waiting for the dormancy threshold.
 */
int cmd_cpu(int argc, char *argv[])
{
    if (argc >= 2 && argv[1] && strcmp(argv[1], "resurrect") == 0) {
        if (argc < 3 || !argv[2]) {
            shell_puts("usage: cpu resurrect <N>\r\n");
            return -1;
        }
        int signed_target = atoi(argv[2]);
        if (signed_target < 0) {
            shell_puts("usage: cpu resurrect <N>  (N must be non-negative)\r\n");
            return -1;
        }
        uint32_t target = (uint32_t)signed_target;
        int rc = cpu_supervisor_resurrect(target);
        shell_printf("cpu resurrect %u: rc=%d\r\n", target, rc);
        return rc;
    }

    shell_puts("CPU Status:\r\n");
    shell_puts("\r\n");
    shell_printf("  Platform:    %s\r\n", PLATFORM_NAME);
    shell_printf("  CPUs:        %lu online / %lu total\r\n", cpus_online, cpu_count);
    shell_puts("\r\n");

    shell_puts("  CPU  Status   EL       Isolated  Current Task\r\n");
    shell_puts("  ---  ------   ------   --------  ------------\r\n");

    for (uint32_t i = 0; i < cpu_count; i++) {
        bool online = (i < cpus_online);
        bool isolated = sched_is_core_isolated(i);
        struct task *current = NULL;

        /* Get current task for this CPU (if we can) */
        /* Note: This is a snapshot and may be racy */
        if (online && i == cpu_id()) {
            current = task_current();
        }

        /* Decode CurrentEL value captured at boot. CurrentEL bits
         * [3:2] hold the EL number; values 0/4/8/C correspond to
         * EL0/EL1/EL2/EL3. EL2 + HCR_EL2.E2H=1 still reports 0xC.
         *
         * VHE is a kernel-wide design choice on platforms that hit
         * EL2 (Pi 5, Jetson per #683); the boot banner annotates
         * "(VHE)" once for CPU 0 — this column reports EL only and
         * doesn't repeat the annotation per row. */
        uint64_t cur_el_raw = cpu_get_current_el(i);
        char el_text[8];
        if (cur_el_raw == CPU_EL_NOT_RECORDED) {
            el_text[0] = '-'; el_text[1] = '\0';
        } else {
            unsigned el = (unsigned)((cur_el_raw >> 2) & 0x3);
            el_text[0] = 'E'; el_text[1] = 'L';
            el_text[2] = (char)('0' + el);
            el_text[3] = 'h'; el_text[4] = '\0';
        }

        shell_printf("  %3lu  %-6s   %-6s   %-8s  %s\r\n",
                    i,
                    online ? "online" : "offline",
                    el_text,
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
int cmd_canary(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    int broken = task_canary_check_all();
    if (broken == 0) {
        shell_printf("All task stack canaries intact.\r\n");
    } else {
        shell_printf("%d broken canary region(s) detected (see "
                     "uart_printf log above for offsets/values).\r\n",
                     broken);
    }
    return 0;
}

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

#if !defined(PLATFORM_X86_64)
/* ============================================================================
 * usertest - Smoke test for #697 EL0 user-mode execution.
 *
 * Creates a user task whose entry is `user_smoke_main` (in .text.user),
 * adds it to the scheduler, and waits up to 1 s for it to terminate.
 * The user task is expected to print "[USERTEST] hello\n" via SYS_LOG
 * and exit via SYS_EXIT — proving the EL1 → EL0 → EL1 round-trip on
 * the per-task TTBR0_EL1 from PR-3 actually works end-to-end.
 * ============================================================================ */
extern void user_smoke_main(void *arg);
extern void user_mmap_smoke_main(void *arg);

/* Embedded EL0 hello ELF (kernel/src/user_hello_embed.S). */
extern const uint8_t user_hello_elf_start[];
extern const uint8_t user_hello_elf_end[];

/* Pin a freshly-created user task to this CPU, dispatch it, and
 * yield-poll until the slot is reaped or TERMINATED. Shared between
 * cmd_usertest, cmd_mmaptest, and cmd_userelf; the only difference
 * between those commands is how the task is constructed (linker
 * smoke vs ELF blob). Returns 0 on success or -1 on poll timeout. */
static int run_user_task_until_exit(struct task *t, const char *cmd_name)
{
    task_set_affinity(t, cpu_id());
    uint32_t task_id = t->id;
    scheduler_add_task(t);

    /* 100k yield cap absorbs cooperative-preempt delays on Pi 5 /
     * Jetson without a wall-clock check; on QEMU resolves in a few
     * yields. */
    for (int i = 0; i < 100000; i++) {
        struct task *cur = task_get(task_id);
        if (!cur || cur->id != task_id) {
            return 0;
        }
        if (cur->state == TASK_TERMINATED) {
            return 0;
        }
        yield();
    }

    shell_printf("%s: timeout — user task did not exit\r\n", cmd_name);
    return -1;
}

int cmd_usertest(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    struct task *t = task_create_user("usertest", user_smoke_main, NULL,
                                      TASK_PRIORITY_DEFAULT);
    if (!t) {
        shell_puts("usertest: task_create_user failed\r\n");
        return -1;
    }
    if (run_user_task_until_exit(t, "usertest") != 0) {
        return -1;
    }
    shell_puts("usertest: ok\r\n");
    return 0;
}

int cmd_mmaptest(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    struct task *t = task_create_user("mmaptest", user_mmap_smoke_main, NULL,
                                      TASK_PRIORITY_DEFAULT);
    if (!t) {
        shell_puts("mmaptest: task_create_user failed\r\n");
        return -1;
    }
    if (run_user_task_until_exit(t, "mmaptest") != 0) {
        return -1;
    }
    shell_puts("mmaptest: ok\r\n");
    return 0;
}

int cmd_userelf(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    size_t blob_len = (size_t)(user_hello_elf_end - user_hello_elf_start);
    struct task *t = task_create_user_elf("userelf",
                                          user_hello_elf_start, blob_len,
                                          TASK_PRIORITY_DEFAULT);
    if (!t) {
        shell_puts("userelf: task_create_user_elf failed\r\n");
        return -1;
    }
    if (run_user_task_until_exit(t, "userelf") != 0) {
        return -1;
    }
    shell_puts("userelf: ok\r\n");
    return 0;
}
#endif /* !PLATFORM_X86_64 */

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
        if (avg_ns < 10000) {
            shell_puts("    Rating:  Excellent (< 10 us)\r\n");
        } else if (avg_ns < 50000) {
            shell_puts("    Rating:  Good (< 50 us)\r\n");
        } else if (avg_ns < 100000) {
            shell_puts("    Rating:  Acceptable (< 100 us)\r\n");
        } else {
            shell_puts("    Rating:  Needs optimization (> 100 us)\r\n");
        }
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
        for (int j = 0; j < 4096; j += 64) {
            p[j] = (uint8_t)i;
        }
    }
    uint64_t write_ns = slm_get_time_ns() - start;

    /* Read 4KB, 1000 iterations */
    start = slm_get_time_ns();
    volatile uint8_t sink = 0;
    for (int i = 0; i < 1000; i++) {
        volatile uint8_t *p = (volatile uint8_t *)ptr;
        for (int j = 0; j < 4096; j += 64) {
            sink = p[j];
        }
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
        shell_puts("Usage: bench <context|irq|ipc|eviction|deadline|isolate|shared|smp|stealing|matmul|conv|quant|q4kdot|gpu|stats|all>\r\n");
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
    } else if (strcmp(argv[1], "q4kdot") == 0) {
        /* Q4_K · Q8_K vec_dot microbenchmark. Dominant per-token decode
         * cost on Qwen2.5; useful for measuring NEON kernel speedups
         * in isolation from the rest of the forward pass.
         *
         * Usage: bench q4kdot [scalar|neon] [elements [iterations]]
         *
         *   scalar  — force the scalar-only path
         *             (`vec_dot_q4_k_q8_k_scalar`); use to capture the
         *             baseline number for an A/B vs the NEON SDOT
         *             kernel from the same kernel binary.
         *   neon    — explicit "use the production dispatcher". Same
         *             as omitting the kernel selector.
         *   default — production dispatcher (NEON on aarch64).
         *
         * Defaults match Qwen2.5-1.5B's hidden_size (1536, the smallest
         * inner dim that appears in the decoder's projection matmuls).
         */
        uint32_t elements = 1536;
        uint32_t iters = 200;
        uint32_t force_scalar = 0;
        int arg_off = 2;  /* index of first non-verb arg */
        if (argc > arg_off) {
            if (strcmp(argv[arg_off], "scalar") == 0) {
                force_scalar = 1;
                arg_off++;
            } else if (strcmp(argv[arg_off], "neon") == 0) {
                arg_off++;
            }
        }
        if (argc > arg_off) {
            uint32_t n;
            if (shell_parse_uint(argv[arg_off], &n) == 0 &&
                n >= 256 && n <= (1u << 20)) {
                elements = n;
            }
            arg_off++;
        }
        if (argc > arg_off) {
            uint32_t n;
            if (shell_parse_uint(argv[arg_off], &n) == 0 &&
                n > 0 && n <= 100000) {
                iters = n;
            }
        }
        shell_puts("Q4_K vec_dot Benchmark\r\n");
        shell_puts("======================\r\n");
        rust_bench_q4k_q8k_dot(elements, iters, force_scalar);
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
                          ? "(NS RAZ when GICD_CTLR.DS=0 — see Sentinel C for actual config)"
                          : "(partial)");

    /*
     * Sentinel C: TF-A's own readback of GICD_IGROUPR0 from EL3,
     * captured by patched TF-A in plat_rpi_bl31_custom_setup right
     * after the 0xFFFFFFFF write. Lets us distinguish "write was
     * never persistent at EL3" from "write was persistent at EL3
     * but cleared before NS-EL2 reads."
     */
    {
        uint32_t c_magic = *(volatile uint32_t *)DIAG_GIC_SENTINEL_C_MAGIC;
        uint32_t c_pre   = *(volatile uint32_t *)DIAG_GIC_SENTINEL_C_IGROUPR0_PRE;
        uint32_t c_post  = *(volatile uint32_t *)DIAG_GIC_SENTINEL_C_IGROUPR0_POST;
        uint32_t c_ctlr  = *(volatile uint32_t *)DIAG_GIC_SENTINEL_C_GICD_CTLR;
        shell_printf("  Sentinel C (TF-A IGROUPR readback): magic=0x%08lx %s\r\n",
                    (unsigned long)c_magic,
                    c_magic == DIAG_GIC_SENTINEL_C_EXPECTED_MAGIC
                        ? "OK" : "MISSING");
        if (c_magic == DIAG_GIC_SENTINEL_C_EXPECTED_MAGIC) {
            /* Sentinel C captures TF-A's secure readback of IGROUPR[0]
             * after writing 0xFFFFFFFF. Both candidate timer PPIs
             * (CNTV=27 banked, CNTP=30) must end up in Group 1 NS — bits
             * 16-23 / 24 are RES0 on GIC-400 (unimplemented PPIs) so
             * they read back zero even when the write was accepted.
             * This invariant is independent of which PPI the runtime
             * timer driver picks via TIMER_IRQ. */
            uint32_t timer_ppis = (1u << 27) | (1u << 30);
            shell_printf("    TF-A view  IGROUPR[0]: pre=0x%08lx post=0x%08lx  %s\r\n",
                        (unsigned long)c_pre, (unsigned long)c_post,
                        (c_post == 0xFFFFFFFFul)
                            ? "(write fully held — all PPIs Group 1 NS)"
                            : ((c_post & timer_ppis) == timer_ppis)
                                  ? "(timer PPIs 27+30 in Group 1 NS; bits 16-23/24 RES0 on GIC-400)"
                                  : "(partial — at least one timer PPI NOT in Group 1 NS)");
            shell_printf("    TF-A view  GICD_CTLR : 0x%08lx\r\n",
                        (unsigned long)c_ctlr);
        }
    }

    /*
     * Sentinel D was reserved for ICC_SRE_EL3/EL2/EL1 captured from
     * Secure-EL3 by patched TF-A. Disabled — even ICC_SRE_EL3 read
     * from EL3 generates UNDEFINED on this Cortex-A76 implementation.
     * Reason is visible in ID_AA64PFR0_EL1 above:
     *   bits 27:24 (GIC field) == 0  →  GIC system register interface
     *                                   is NOT implemented.
     * SRE is moot — the CPU has no sysreg interface to listen on, so
     * IRQs must be delivered via the legacy memory-mapped GICC_*
     * interface unconditionally. Decode the GIC field here so the
     * fact is plain on every diag dump.
     */
    {
        uint64_t pfr0 = s->id_aa64pfr0_el1;
        uint32_t gic_field = (uint32_t)((pfr0 >> 24) & 0xF);
        const char *gic_decode =
            (gic_field == 0u) ? "0 (NOT implemented — CPU has no sysreg ICC_*)"
            : (gic_field == 1u) ? "1 (GICv3.0/4.0 sysreg interface)"
            : (gic_field == 3u) ? "3 (GICv4.1 sysreg interface)"
            : "?";
        shell_printf("  ID_AA64PFR0_EL1.GIC = %s\r\n", gic_decode);
        if (gic_field == 0u) {
            shell_puts("    >>> ICC_SRE_EL* are UNDEFINED on this CPU. SRE branch\r\n"
                       "        of #134 is moot — IRQ delivery has to flow through\r\n"
                       "        legacy GICC_* MMIO unconditionally.\r\n");
        }
    }
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
    volatile uint32_t *gicd_ctlr_runtime =
        (volatile uint32_t *)(GIC_DIST_BASE + 0x000UL);
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
    uint32_t gicd_ctlr_now = *gicd_ctlr_runtime;
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
    /*
     * GICD_CTLR from NS view. On GIC-400 with security extensions,
     * NS reads/writes only see one bit: EnableGrp1NS (NS-view bit 0
     * aliases the actual register's bit 1). bit 0 == 1 means the
     * distributor is forwarding NS Group 1 interrupts to the NS CPU
     * interface — which is what we need for PPI 30 (timer) to deliver.
     * This is the kernel side of the gate; if 0, kernel's
     * `GICD_CTLR = ctlr | 1` write in gic_init didn't take effect.
     */
    shell_printf("  GICD_CTLR = 0x%x  (NS view; bit0=EnableGrp1NS=%u)%s\r\n",
                gicd_ctlr_now, gicd_ctlr_now & 1u,
                (gicd_ctlr_now & 1u)
                    ? "  ← distributor forwards NS Group 1 ✓"
                    : "  ← distributor NOT forwarding NS Group 1 ✗");
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

    /*
     * GICC_ACTIVEPRIO[0..3] (offset 0xD0..0xDC). Each bit corresponds
     * to a "running priority" entry. If anything earlier (firmware,
     * boot ROM, TF-A) left an active priority bit set, the CPU
     * interface refuses to deliver new IRQs at or below that priority
     * — they queue at the distributor (visible as ISPENDR) but never
     * reach the CPU. Linux's gic_cpu_if_up clears these unconditionally
     * via writel(0, GICC + 0xD0 + i*4). Our kernel never touched them
     * before round 5 of the #134 probe.
     */
    {
        volatile uint32_t *gicc_aprn =
            (volatile uint32_t *)(GIC_CPU_BASE + 0xD0UL);
        shell_puts("  GICC_ACTIVEPRIO[0..3]: ");
        for (uint32_t i = 0; i < 4; i++) {
            shell_printf("0x%08x ", gicc_aprn[i]);
        }
        uint32_t any_set = 0;
        for (uint32_t i = 0; i < 4; i++) {
            any_set |= gicc_aprn[i];
        }
        shell_printf(" %s\r\n",
                    any_set ? "← stale active priority — CPU interface "
                              "would block lower IRQs"
                            : "(clean)");
    }

    /*
     * GICD_IPRIORITYR for PPI 30 (timer). Read from NS to see what
     * priority the CPU interface compares against PMR. With GIC-400
     * security extensions, the NS-view value of a Secure-written
     * priority may differ from what Secure software wrote.
     */
    {
        volatile uint32_t *gicd_ipri =
            (volatile uint32_t *)(GIC_DIST_BASE + 0x400UL);
        uint32_t row = 30u / 4u;          /* IPRIORITYR row containing IRQ 30 */
        uint32_t shift = (30u % 4u) * 8u; /* byte position within the row */
        uint8_t pri = (uint8_t)(gicd_ipri[row] >> shift);
        shell_printf("  IPRIORITYR[PPI 30] (NS view) = 0x%02x   PMR = 0x%x\r\n",
                    pri, *gicc_pmr);
        shell_printf("    delivers under PMR if pri < PMR  →  %s\r\n",
                    (pri < (uint8_t)(*gicc_pmr & 0xff))
                        ? "yes (priority allows delivery)"
                        : "NO — IPRIORITYR ≥ PMR, IRQ masked at CPU iface");
    }

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

    /* #253 (2026-04-23): give Hailo fw a chance to clean up before
     * we tear down. Linux writes FW_ACCESS_DRIVER_SHUTDOWN_MASK
     * (val=4) to the doorbell on device release; mirroring that
     * lets fw clear "active driver" state so the next boot starts
     * fresh.
     *
     * Weak extern so the call site compiles on every platform — on
     * builds without the Hailo backend linked (QEMU ARM64, Jetson,
     * x86-64), the symbol resolves to NULL at link time and the
     * address check skips the call. No build-time conditional
     * needed at the call site. */
    extern int hailo_control_signal_driver_shutdown(void) __attribute__((weak));
    if (&hailo_control_signal_driver_shutdown) {
        (void)hailo_control_signal_driver_shutdown();
    }

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

static int model_swap(int argc, char *argv[])
{
    if (argc < 4) {
        shell_puts("Usage: model swap <name|idx> <path>\r\n");
        shell_puts("  Atomically replace the weights of an existing model with\r\n");
        shell_puts("  a new ONNX file. In-flight inferences continue against the\r\n");
        shell_puts("  OLD weights; subsequent inferences see the NEW weights.\r\n");
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
        shell_printf("model swap: '%s' not found\r\n", argv[2]);
        return -1;
    }

    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[3], resolved, sizeof(resolved)) < 0) {
        shell_puts("model swap: path too long\r\n");
        return -1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0) {
        shell_printf("model swap: %s: file not found\r\n", resolved);
        return -1;
    }
    if (info.type != 0) {
        shell_printf("model swap: %s: not a file\r\n", resolved);
        return -1;
    }
    if (info.size == 0) {
        shell_puts("model swap: file is empty\r\n");
        return -1;
    }

    size_t pages_needed = (info.size + 4095) / 4096;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages_needed);
    if (!buf) {
        shell_puts("model swap: out of memory for read buffer\r\n");
        return -1;
    }

    int bytes_read = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (bytes_read <= 0) {
        shell_printf("model swap: failed to read %s\r\n", resolved);
        pmm_free_pages(buf, pages_needed);
        return -1;
    }

    /* Derive the new model's name from the file (matches `model load`). */
    const char *base = resolved;
    for (const char *p = resolved; *p; p++) {
        if (*p == '/')
            base = p + 1;
    }
    char model_name[32];
    size_t name_len = 0;
    for (const char *p = base; *p && *p != '.' && name_len < 31; p++) {
        model_name[name_len++] = *p;
    }
    model_name[name_len] = '\0';

    int rc = rust_model_swap((uint32_t)idx, model_name,
                             buf, (size_t)bytes_read);
    pmm_free_pages(buf, pages_needed);

    switch (rc) {
    case 0:
        break;
    case -1:
        /* Unreachable from this caller: we already validated path,
         * buf, bytes_read, and capped model_name to 31 chars before
         * the FFI call. -1 here means the kernel passed bad inputs
         * to rust_model_swap — a kernel bug, not user error. */
        shell_puts("model swap: internal: rust_model_swap rejected our inputs (kernel bug)\r\n");
        return -1;
    case -2:
        shell_printf("model swap: slot %d is empty\r\n", idx);
        return -1;
    case -3:
        shell_printf("model swap: slot %d backend does not support swap "
                     "(Hailo CCW re-upload tracked in #532)\r\n", idx);
        return -1;
    case -4:
        shell_printf("model swap: failed to load %s (parse or alloc error)\r\n",
                     model_name);
        return -1;
    default:
        shell_printf("model swap: unexpected error %d\r\n", rc);
        return -1;
    }

    /* Echo the new metadata so the operator can verify the swap. */
    RustModelInfo minfo;
    if (rust_model_get_info((uint32_t)idx, &minfo) == 0) {
        shell_printf("Swapped model at slot %d -> '%s'\r\n",
                     idx, model_name);
        shell_printf("  Format:     ONNX\r\n");
        shell_printf("  Parameters: %lu\r\n", (unsigned long)minfo.param_count);
        shell_printf("  Weights:    %lu bytes\r\n", (unsigned long)minfo.weight_size);
        shell_printf("  Nodes:      %lu\r\n", (unsigned long)minfo.node_count);
    } else {
        shell_printf("Swapped model at slot %d -> '%s'\r\n", idx, model_name);
    }
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
 * Hard upper bound on `model infer-file` payload size. The largest
 * input tensor expected today is the AI scheduler's feature vector
 * (a few KB at most); MNIST sits at 3,136 bytes (1×1×28×28 fp32).
 * 32 KB leaves comfortable headroom while keeping the temporary
 * `pmm_alloc_pages` allocation small enough to never trigger
 * eviction pressure on a freshly-booted system. The per-model
 * shape check below is the real gate; this constant just rejects
 * obviously bogus files before the alloc.
 */
#define MODEL_INFER_FILE_MAX_BYTES (32u * 1024u)

static int model_infer_file(int argc, char *argv[])
{
    if (argc < 4) {
        shell_puts("Usage: model infer-file <name|idx> <path>\r\n");
        shell_puts("  Path must point to raw little-endian fp32 matching the\r\n");
        shell_puts("  model's input shape (e.g. 3,136 bytes for MNIST 1x1x28x28).\r\n");
        return -1;
    }

    /* Resolve model index. */
    int idx = -1;
    uint32_t parsed_idx;
    if (shell_parse_uint(argv[2], &parsed_idx) == 0) {
        idx = (int)parsed_idx;
    } else {
        idx = rust_model_find(argv[2]);
    }
    if (idx < 0) {
        shell_printf("model infer-file: '%s' not found\r\n", argv[2]);
        return -1;
    }

    /* Look up the model's expected input element count up-front so a
     * shape mismatch becomes "expected 784 floats (3136 bytes), got
     * N" instead of the engine's opaque InvalidInput error. */
    int expected_floats = rust_model_expected_input_floats((uint32_t)idx);
    if (expected_floats <= 0) {
        shell_printf("model infer-file: '%s' has no usable input shape\r\n",
                     argv[2]);
        return -1;
    }
    uint32_t expected_bytes = (uint32_t)expected_floats * 4u;

    /* Resolve path against cwd, like the other shell file commands. */
    char resolved[VFS_MAX_PATH];
    if (shell_resolve_path(argv[3], resolved, sizeof(resolved)) < 0) {
        shell_printf("model infer-file: path too long: %s\r\n", argv[3]);
        return -1;
    }

    struct vfs_entry_info info;
    if (vfs_stat_path(resolved, &info) != 0 || info.type != 0 || info.size == 0) {
        shell_printf("model infer-file: cannot stat '%s'\r\n", resolved);
        return -1;
    }
    if (info.size > MODEL_INFER_FILE_MAX_BYTES) {
        shell_printf("model infer-file: '%s' is %u bytes — exceeds the %u-byte cap\r\n",
                     resolved, (unsigned)info.size,
                     (unsigned)MODEL_INFER_FILE_MAX_BYTES);
        return -1;
    }
    if (info.size != expected_bytes) {
        shell_printf("model infer-file: '%s' is %u bytes; '%s' expects %d "
                     "fp32 elements (%u bytes)\r\n",
                     resolved, (unsigned)info.size, argv[2],
                     expected_floats, (unsigned)expected_bytes);
        return -1;
    }

    /* Page-aligned PMM buffer guarantees fp32 alignment for the NEON
     * loads inside the inference engine — same pattern Lua's
     * `slm.model_infer_file` uses. */
    size_t pages = (info.size + 4095u) / 4096u;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(pages);
    if (!buf) {
        shell_printf("model infer-file: out of memory\r\n");
        return -1;
    }

    int rd = vfs_read_path(resolved, (char *)buf, info.size, 0);
    if (rd != (int)info.size) {
        pmm_free_pages(buf, pages);
        shell_printf("model infer-file: short read (%d/%u)\r\n",
                     rd, (unsigned)info.size);
        return -1;
    }

    /* M3 telemetry: same hook the zero-input `model infer` command and
     * the Lua model_infer* bindings call. Keeps the rate / latency-hist
     * / tel.inf event counter consistent across entry points. */
    uint64_t t0 = slm_get_time_ns();
    int result = rust_infer_buf_and_print((uint32_t)idx,
                                          (const float *)buf,
                                          info.size / 4u);
    uint64_t t1 = slm_get_time_ns();
    admin_telemetry_record_inference(t1 > t0 ? t1 - t0 : 0u, result >= 0);

    pmm_free_pages(buf, pages);

    if (result < 0) {
        /* Map the contract codes from rust_infer_buf_and_print
         * (slm_ffi.h) to operator-readable lines. The detailed
         * UART trace from the Rust side has the exact failure;
         * this just makes the shell output meaningful on its own. */
        const char *why;
        switch (result) {
        case -1: why = "model not loaded";          break;
        case -2: why = "empty / null input buffer"; break;
        case -3: why = "engine error (see UART log)"; break;
        default: why = "unknown error";             break;
        }
        shell_printf("model infer-file: %s (rc=%d)\r\n", why, result);
        return -1;
    }
    /* Echo the prediction to the active shell session — the detailed
     * logits-per-bucket dump went to UART (kernel-Rust uses
     * `uart_printf`); telnet operators wouldn't otherwise see the
     * result. The Rust function returns argmax on success. */
    uint64_t elapsed_us = (t1 > t0 ? t1 - t0 : 0u) / 1000u;
    shell_printf("predicted: %d  (%lu us)\r\n",
                 result, (unsigned long)elapsed_us);
    return 0;
}

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

    /* M3 telemetry: same instrumentation the three Lua model_infer*
     * bindings apply (kernel/src/lua_slm.c). Without it, shell-driven
     * `model infer` calls would not increment the inference rate /
     * latency-hist / `tel.inf` event counter — surfaced during M7
     * hardware verification on jetson-nano-2. */
    uint64_t t0 = slm_get_time_ns();
    int result = rust_infer_and_print((uint32_t)idx);
    uint64_t t1 = slm_get_time_ns();
    admin_telemetry_record_inference(t1 > t0 ? t1 - t0 : 0u, result >= 0);

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
    if (strcmp(subcmd, "swap") == 0) {
        return model_swap(argc, argv);
    }
    if (strcmp(subcmd, "pools") == 0) {
        model_show_pools();
        return 0;
    }
    if (strcmp(subcmd, "infer") == 0) {
        return model_infer(argc, argv);
    }
    if (strcmp(subcmd, "infer-file") == 0) {
        return model_infer_file(argc, argv);
    }
    if (strcmp(subcmd, "use-gpu") == 0) {
        if (argc < 4) {
            shell_puts("Usage: model use-gpu <name|idx> <on|off>\r\n");
            shell_puts("  Per-model GPU-dispatch toggle. Layered on top of the master\r\n");
            shell_puts("  `gpu use inference` flag — both must be ON for the engine to\r\n");
            shell_puts("  dispatch on the GPU. Default at load is ON.\r\n");
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
            shell_printf("model use-gpu: '%s' not found\r\n", argv[2]);
            return -1;
        }

        bool enabled;
        if (strcmp(argv[3], "on") == 0) {
            enabled = true;
        } else if (strcmp(argv[3], "off") == 0) {
            enabled = false;
        } else {
            shell_printf("model use-gpu: unknown action '%s' (want on|off)\r\n",
                         argv[3]);
            return -1;
        }

        if (rust_model_set_gpu_dispatch((uint32_t)idx, enabled ? 1 : 0) != 0) {
            shell_printf("model use-gpu: failed to set flag for slot %d\r\n", idx);
            return -1;
        }
        shell_printf("model use-gpu: '%s' (slot %d): %s\r\n",
                     argv[2], idx, enabled ? "ON" : "off");
        if (enabled && !gpu_consumer_enabled(GPU_CONSUMER_INFERENCE)) {
            shell_puts("           note: master `gpu use inference` is OFF — "
                       "engine will still use CPU\r\n");
        }
        return 0;
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

    if (strcmp(subcmd, "engines") == 0) {
        const struct model_engine_info *engines[MODEL_KIND_COUNT];
        size_t n = model_engine_info_list(engines);
        shell_puts("Model engines:\r\n");
        for (size_t i = 0; i < n; i++) {
            const char *state =
                engines[i]->state == MODEL_ENGINE_READY    ? "READY"
              : engines[i]->state == MODEL_ENGINE_NOSYS    ? "NOSYS"
              : engines[i]->state == MODEL_ENGINE_DISABLED ? "DISBL"
              :                                              "?????";
            shell_printf("  %-8s %-6s %s\r\n",
                         engines[i]->name, state, engines[i]->summary);
        }
        return 0;
    }

    if (strcmp(subcmd, "meta") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model meta <name>\r\n");
            return -1;
        }
        struct model_meta meta;
        int rc = model_meta_read(argv[2], &meta);
        if (rc != MODEL_LAUNCH_OK) {
            const char *why =
                rc == MODEL_LAUNCH_ERR_NOMETA   ? "no /mnt/files/models/<name>.meta sidecar"
              : rc == MODEL_LAUNCH_ERR_BADMETA  ? "malformed sidecar"
              : rc == MODEL_LAUNCH_ERR_BADKIND  ? "unknown kind"
              :                                   "unknown error";
            shell_printf("model meta: %s: %s (rc=%d)\r\n", argv[2], why, rc);
            return rc;
        }
        shell_printf("Meta for '%s':\r\n", argv[2]);
        shell_printf("  name           %s\r\n",
                     meta.name[0] ? meta.name : argv[2]);
        shell_printf("  kind           %s\r\n", model_kind_name(meta.kind));
        shell_printf("  size           %lu bytes\r\n", (unsigned long)meta.size);
        if (meta.sha256[0]) {
            shell_printf("  sha256         %s\r\n", meta.sha256);
        }
        if (meta.uploaded_ts_ms) {
            shell_printf("  uploaded_ts_ms %lu\r\n",
                         (unsigned long)meta.uploaded_ts_ms);
        }
        return 0;
    }

    if (strcmp(subcmd, "launch") == 0) {
        if (argc < 3) {
            shell_puts("Usage: model launch <name>\r\n");
            return -1;
        }
        int task_id = -1;
        int rc = model_engine_launch(argv[2], &task_id);
        if (rc != MODEL_LAUNCH_OK) {
            const char *why =
                rc == MODEL_LAUNCH_ERR_NOMETA   ? "no /mnt/files/models/<name>.meta sidecar"
              : rc == MODEL_LAUNCH_ERR_BADMETA  ? "malformed sidecar"
              : rc == MODEL_LAUNCH_ERR_BADKIND  ? "unknown kind"
              : rc == MODEL_LAUNCH_ERR_NOSYS    ? "engine for kind is a stub on this build"
              : rc == MODEL_LAUNCH_ERR_FAILED   ? "engine returned failure"
              :                                   "unknown error";
            shell_printf("model launch: %s: %s (rc=%d)\r\n", argv[2], why, rc);
            return rc;
        }
        shell_printf("model launch %s: ok (task_id=%d)\r\n", argv[2], task_id);
        return 0;
    }

    shell_puts("Usage: model [load|list|info|unload|swap|pin|unpin|preload|preload-status|"
              "infer|infer-file|use-gpu|bench|stats|pools|gpu|engines|meta|launch]\r\n");
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
        if (*s >= '0' && *s <= '9') {
            d = *s - '0';
        } else if (*s >= 'a' && *s <= 'f') {
            d = 10 + (*s - 'a');
        } else if (*s >= 'A' && *s <= 'F') {
            d = 10 + (*s - 'A');
        } else {
            shell_puts("bad hex address\r\n");
            return -1;
        }
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
 * dtb-dump - Hex dump of the firmware-passed DTB for offline triage.
 *
 * Prints the runtime DTB (post firmware fix-ups) as a hex stream
 * between DTB-START and DTB-END markers. Capture the serial output,
 * pipe through `xxd -r -p` to recover the binary, and `dtc -I dtb -O dts`
 * for a readable diff against the on-disk .dtb file.
 *
 * Originally added during the #414 EMMC2 investigation; kept in tree
 * because DTB triage on Pi/Jetson recurs.
 */

/* Sanity bound: refuse to dump a DTB that claims to be larger than this.
 * Pi 5's runtime DTB is ~80 KB; Linux DTBs typically fit in 100 KB.
 * 1 MB is generous but bounded — a corrupt totalsize won't make the
 * shell stream gigabytes over UART. */
#define DTB_DUMP_MAX_BYTES   (1U << 20)

/* Hex encoding is done in chunks to avoid one shell_puts per byte (each
 * call would acquire the UART lock). At 64 bytes per chunk, an 80 KB DTB
 * needs ~1280 puts calls instead of ~80,000. */
#define DTB_DUMP_CHUNK_BYTES 64

int cmd_dtb_dump(int argc, char *argv[])
{
    (void)argc; (void)argv;
    extern const void *dtb_get_blob(void);
    const uint8_t *p = (const uint8_t *)dtb_get_blob();
    if (!p) { shell_puts("no DTB available\r\n"); return -1; }

    /* FDT magic (big-endian 0xd00dfeed) at offset 0, totalsize at offset 4. */
    uint32_t magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                     ((uint32_t)p[2] <<  8) | ((uint32_t)p[3] <<  0);
    if (magic != 0xd00dfeed) {
        shell_printf("not a DTB: magic=0x%08lx at %p\r\n",
                     (unsigned long)magic, (const void *)p);
        return -1;
    }
    uint32_t total = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
                     ((uint32_t)p[6] <<  8) | ((uint32_t)p[7] <<  0);
    if (total == 0 || total > DTB_DUMP_MAX_BYTES) {
        shell_printf("DTB totalsize implausible: %lu\r\n", (unsigned long)total);
        return -1;
    }

    shell_printf("DTB-START addr=%p size=%lu magic=0x%08lx\r\n",
                 (const void *)p, (unsigned long)total, (unsigned long)magic);
    static const char hex[] = "0123456789abcdef";
    char chunk[DTB_DUMP_CHUNK_BYTES * 2 + 3];   /* hex + "\r\n" + NUL */
    uint32_t i = 0;
    while (i < total) {
        uint32_t take = total - i < DTB_DUMP_CHUNK_BYTES
                        ? total - i : DTB_DUMP_CHUNK_BYTES;
        uint32_t pos = 0;
        for (uint32_t k = 0; k < take; k++) {
            uint8_t b = p[i + k];
            chunk[pos++] = hex[(b >> 4) & 0xF];
            chunk[pos++] = hex[b & 0xF];
        }
        chunk[pos++] = '\r';
        chunk[pos++] = '\n';
        chunk[pos] = '\0';
        shell_puts(chunk);
        i += take;
    }
    shell_puts("DTB-END\r\n");
    return 0;
}

#if defined(PLATFORM_RASPI5)
/* #414 diagnostic: trigger the Pi 5 SDHCI bring-up sequence on
 * demand from the shell instead of from the boot path. Lets us
 * discriminate "bring-up fails because hardware state is wrong"
 * from "bring-up fails because boot-time firmware state is wrong." */
int cmd_emmc_bringup(int argc, char *argv[])
{
    (void)argc; (void)argv;
    shell_puts("invoking sdhci_pi5_bringup_now()\r\n");
    struct blkdev *dev = sdhci_pi5_bringup_now();
    if (dev) {
        shell_printf("emmc-bringup: succeeded (dev=%p)\r\n", (void *)dev);
    } else {
        shell_puts("emmc-bringup: returned NULL\r\n");
    }
    return 0;
}
#else
int cmd_emmc_bringup(int argc, char *argv[])
{
    (void)argc; (void)argv;
    shell_puts("emmc-bringup: not supported on this platform\r\n");
    return -1;
}
#endif

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
            if (*s >= '0' && *s <= '9') {
                d = *s - '0';
            } else if (*s >= 'a' && *s <= 'f') {
                d = 10 + (*s - 'a');
            } else if (*s >= 'A' && *s <= 'F') {
                d = 10 + (*s - 'A');
            } else {
                uart_puts("bad hex\r\n");
                return -1;
            }
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
    extern bool xhci_dump_info(void);
    extern int  xhci_cmd_noop_probe(void);

    if (argc >= 2 && strcmp(argv[1], "noop") == 0) {
        int rc = xhci_cmd_noop_probe();
        shell_printf("xhci noop: %s (rc=%d)\r\n",
                     rc == 0 ? "ok" : "failed", rc);
        return rc;
    }

    if (argc >= 2) {
        shell_puts("usage: xhci [noop]\r\n");
        return -1;
    }

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
/* gpu use <consumer> <on|off|status>
 *
 * Per-consumer GPU enable toggle (admin & telemetry suite, M2). M3+
 * unlocks each consumer in turn — until then `on` returns the
 * configured rejection reason. `off` always succeeds. `status` (and
 * the bare `gpu use`) prints a tabular summary of all three flags. */
static int cmd_gpu_use(int argc, char *argv[])
{
    if (argc < 3 || strcmp(argv[2], "status") == 0) {
        struct gpu_consumer_status st;
        gpu_consumer_status_get(&st);
        shell_puts("GPU consumer toggles:\r\n");
        shell_printf("  gpu_ready    %s\r\n", st.gpu_ready ? "yes" : "no");
        shell_printf("  sched        %s   (last change %lu ms)\r\n",
                     st.sched ? "ON " : "off",
                     (unsigned long)st.sched_change_ms);
        shell_printf("  eviction     %s   (last change %lu ms)\r\n",
                     st.eviction ? "ON " : "off",
                     (unsigned long)st.eviction_change_ms);
        shell_printf("  inference    %s   (last change %lu ms)\r\n",
                     st.inference ? "ON " : "off",
                     (unsigned long)st.inference_change_ms);
        return 0;
    }

    if (argc < 4) {
        shell_puts("usage: gpu use <sched|eviction|inference> <on|off>\r\n");
        shell_puts("       gpu use status\r\n");
        return -1;
    }

    enum gpu_consumer c = gpu_consumer_from_name(argv[2]);
    if (c == GPU_CONSUMER_COUNT) {
        shell_printf("gpu use: unknown consumer '%s' (want sched|eviction|inference)\r\n",
                     argv[2]);
        return -1;
    }

    bool enabled;
    if (strcmp(argv[3], "on") == 0) {
        enabled = true;
    } else if (strcmp(argv[3], "off") == 0) {
        enabled = false;
    } else {
        shell_printf("gpu use: unknown action '%s' (want on|off)\r\n", argv[3]);
        return -1;
    }

    const char *reason = NULL;
    int rc = gpu_consumer_set(c, enabled, &reason);
    if (rc != 0) {
        shell_printf("gpu use %s %s: rejected (%s)\r\n",
                     argv[2], argv[3],
                     reason ? reason : "unknown reason");
        return rc;
    }

    shell_printf("gpu use %s: %s\r\n", argv[2], enabled ? "ON" : "off");
    if (enabled && reason) {
        /* Accepted but the consumer's GPU dispatch path isn't
         * actually wired yet (see gpu_consumer.c: sched/eviction
         * are scaffold-only). Surface the caveat so the operator's
         * expectation matches reality. */
        shell_printf("           note: %s\r\n", reason);
    }
    return 0;
}

/* gpu tier <auto|hmma|simt|fp32|cpu>
 *
 * Tier preference toggle (#664, tensor-core stage 6). Sibling of
 * `gpu use` — selects which kernel variant the dispatcher prefers
 * when multiple are present in the operator library. Setting a tier
 * while `gpu use inference` is OFF is allowed; the preference is
 * recorded and surfaces a note in the response.
 *
 *   gpu tier              — print status
 *   gpu tier status       — print status
 *   gpu tier <name>       — set preference
 */
static int cmd_gpu_tier(int argc, char *argv[])
{
    if (argc < 3 || strcmp(argv[2], "status") == 0) {
        struct gpu_tier_status st;
        gpu_tier_status_get(&st);
        const char *name = gpu_tier_name(st.tier);
        shell_puts("GPU tier preference:\r\n");
        shell_printf("  tier         %s   (last change %lu ms)\r\n",
                     name ? name : "?",
                     (unsigned long)st.change_ms);
        shell_printf("  gpu_ready    %s\r\n", st.gpu_ready ? "yes" : "no");
        shell_printf("  inference    %s\r\n",
                     st.inference_enabled ? "ON " : "off");
        if (st.tier != SLM_GPU_TIER_CPU
            && st.tier != SLM_GPU_TIER_AUTO
            && !st.inference_enabled) {
            shell_puts("           note: tier preference is set but "
                       "inactive — `gpu use inference on` to enable\r\n");
        }
        return 0;
    }

    uint32_t tier = gpu_tier_from_name(argv[2]);
    if (tier == SLM_GPU_TIER_INVALID) {
        shell_printf("gpu tier: unknown tier '%s' "
                     "(want auto|hmma|simt|fp32|cpu)\r\n",
                     argv[2]);
        return -1;
    }

    const char *reason = NULL;
    int rc = gpu_tier_set(tier, &reason);
    if (rc != 0) {
        shell_printf("gpu tier %s: rejected (%s)\r\n",
                     argv[2], reason ? reason : "unknown reason");
        return rc;
    }

    shell_printf("gpu tier: %s\r\n", gpu_tier_name(tier));
    if (reason) {
        shell_printf("           note: %s\r\n", reason);
    }
    return 0;
}

int cmd_gpu(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "use") == 0) {
        return cmd_gpu_use(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "tier") == 0) {
        return cmd_gpu_tier(argc, argv);
    }

    if (argc >= 2 && strcmp(argv[1], "debug") == 0) {
#if defined(PLATFORM_JETSON_ORIN_NANO)
        /* Forward decls — the full ga10b_bringup.h include lives in
         * a later JETSON-only block of this file (around the nvgpu
         * shell command). Pulling that whole header up here would
         * drag in the full bringup API for a two-function shell
         * command; cheaper to declare the two we need inline. */
        extern void ga10b_dispatch_verbose_set(bool on);
        extern bool ga10b_dispatch_verbose_get(void);

        if (argc < 3 || strcmp(argv[2], "status") == 0) {
            shell_printf("gpu dispatch tracing: %s\r\n",
                         ga10b_dispatch_verbose_get() ? "ON" : "OFF");
            return 0;
        }
        if (strcmp(argv[2], "on") == 0) {
            ga10b_dispatch_verbose_set(true);
            shell_puts("gpu dispatch tracing: ON\r\n");
            return 0;
        }
        if (strcmp(argv[2], "off") == 0) {
            ga10b_dispatch_verbose_set(false);
            shell_puts("gpu dispatch tracing: OFF\r\n");
            return 0;
        }
        shell_puts("usage: gpu debug [on|off|status]\r\n");
        return -1;
#else
        shell_puts("gpu debug: only supported on JETSON_ORIN_NANO\r\n");
        return -1;
#endif
    }

    if (argc >= 2 && strcmp(argv[1], "qmd-selftest") == 0) {
#if defined(PLATFORM_JETSON_ORIN_NANO)
        /* Reference blob + fixed inputs come from the shared header
         * `ga10b_qmd_selftest_reference.h`. The same header drives a
         * host-side regression check in test_ga10b_bringup.c, so any
         * intentional change to `ga10b_qmd_populate` defaults that
         * forgets to regenerate the reference fails the host test
         * before it can ship. The on-device run additionally catches
         * AArch64-only corner cases (alignment, atomics, etc.) and
         * memory corruption between encoder run and readback. */
        uint32_t actual[GA10B_QMD_DWORDS];
        ga10b_qmd_populate(actual,
                           GA10B_QMD_SELFTEST_SHADER_GPU_VA,
                           GA10B_QMD_SELFTEST_CBUF_GPU_VA,
                           GA10B_QMD_SELFTEST_REGISTER_COUNT,
                           GA10B_QMD_SELFTEST_GRID_X,
                           GA10B_QMD_SELFTEST_GRID_Y,
                           GA10B_QMD_SELFTEST_GRID_Z,
                           GA10B_QMD_SELFTEST_BLOCK_X,
                           GA10B_QMD_SELFTEST_BLOCK_Y,
                           GA10B_QMD_SELFTEST_BLOCK_Z);

        unsigned mismatches = 0;
        for (unsigned i = 0; i < GA10B_QMD_DWORDS; i++) {
            if (actual[i] != ga10b_qmd_selftest_expected[i]) mismatches++;
        }

        if (mismatches == 0u) {
            shell_puts("qmd-selftest: PASS — encoder produced 256 bytes "
                       "byte-identical to host reference\r\n");
            return 0;
        }

        shell_printf("qmd-selftest: FAIL — %u dword(s) differ\r\n",
                     mismatches);
        /* Print up to the first 4 mismatching dwords so the diff is
         * actionable from a single shell session. */
        unsigned printed = 0;
        for (unsigned i = 0; i < GA10B_QMD_DWORDS && printed < 4u; i++) {
            if (actual[i] != ga10b_qmd_selftest_expected[i]) {
                shell_printf("  qmd[%2u]: expected 0x%08lx, "
                             "got 0x%08lx (xor 0x%08lx)\r\n",
                             i,
                             (unsigned long)ga10b_qmd_selftest_expected[i],
                             (unsigned long)actual[i],
                             (unsigned long)(actual[i] ^ ga10b_qmd_selftest_expected[i]));
                printed++;
            }
        }
        if (mismatches > printed) {
            shell_printf("  ... and %u more dword(s) differ\r\n",
                         mismatches - printed);
        }
        return -1;
#else
        (void)argv;  /* unused on non-Jetson */
        shell_puts("gpu qmd-selftest: only supported on "
                   "JETSON_ORIN_NANO\r\n");
        return -1;
#endif
    }

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

/*
 * telemetry - Telemetry feed introspection (admin & telemetry suite, M4).
 *
 *   telemetry             Same as `telemetry stats`.
 *   telemetry stats       Print cumulative event counts per emitter
 *                         topic plus the topic names operators can
 *                         subscribe to.
 *   telemetry list-topics List the known telemetry topics.
 *
 * `telemetry subscribe <pattern>` is intentionally deferred — Lua
 * scripts subscribe via `slm.telemetry_subscribe(pattern, fn)` (an
 * alias of `slm.msg_subscribe`); a shell-side blocking subscribe
 * is M4-followup once a per-shell mailbox slot allocator lands.
 */
#if defined(ENABLE_NETWORKING)
/* File-scope visitor used by `telemetry server sessions` to print one
 * row per connected client. The `user` pointer carries a uint32_t row
 * counter so the caller can detect "no sessions" without a second walk. */
static bool telemetry_session_print_visitor(
    const struct tcp_telemetry_session_info *info, void *user)
{
    uint32_t *count = (uint32_t *)user;
    shell_printf("  %3u  0x%08x:%u  %-15s  %7lu  %5lu\r\n",
                 (unsigned)info->session_id,
                 (unsigned)info->peer_ip,
                 (unsigned)info->peer_port,
                 info->filter,
                 (unsigned long)info->samples_sent,
                 (unsigned long)info->drops);
    if (count) (*count)++;
    return true;
}
#endif

int cmd_telemetry(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "stats";

    if (strcmp(sub, "list-topics") == 0) {
        struct admin_telemetry_feed_stats st;
        admin_telemetry_get_feed_stats(&st);
        shell_puts("Telemetry topics:\r\n");
        shell_printf("  %-12s eviction decisions + fallbacks\r\n",
                     st.eviction_topic);
        shell_printf("  %-12s inference call results (ok=0|1)\r\n",
                     st.inference_topic);
        shell_puts("Wildcard: tel.* matches all telemetry topics.\r\n");
        return 0;
    }

    if (strcmp(sub, "stats") == 0) {
        struct admin_telemetry_feed_stats st;
        admin_telemetry_get_feed_stats(&st);
        shell_puts("Telemetry feed:\r\n");
        shell_printf("  %-12s published %lu\r\n",
                     st.eviction_topic, (unsigned long)st.eviction_published);
        shell_printf("  %-12s published %lu\r\n",
                     st.inference_topic, (unsigned long)st.inference_published);
        shell_printf("  total                  %lu\r\n",
                     (unsigned long)(st.eviction_published +
                                     st.inference_published));
        shell_puts("Subscribe via `lua -e 'slm.telemetry_subscribe(\"tel.*\", "
                   "function(t,d) print(t,d) end)'`\r\n");
        return 0;
    }

#if defined(ENABLE_NETWORKING)
    if (strcmp(sub, "server") == 0) {
        const char *act = (argc >= 3) ? argv[2] : "status";

        if (strcmp(act, "start") == 0) {
            if (!net_is_up()) {
                shell_puts("telemetry server: network not initialized "
                           "(run `net init` first)\r\n");
                return -1;
            }
            uint16_t port = TCP_TELEMETRY_DEFAULT_PORT;
            if (argc >= 4) {
                uint32_t p = 0;
                if (shell_parse_uint(argv[3], &p) != 0 || p == 0 || p > 0xFFFFu) {
                    shell_puts("telemetry server: invalid port\r\n");
                    return -1;
                }
                port = (uint16_t)p;
            }
            int rc = tcp_telemetry_server_start(port);
            if (rc == 0) {
                shell_printf("telemetry server: listening on port %u\r\n",
                             (unsigned)port);
                return 0;
            }
            shell_printf("telemetry server: start failed (%d)\r\n", rc);
            return rc;
        }
        if (strcmp(act, "stop") == 0) {
            if (!tcp_telemetry_server_running()) {
                shell_puts("telemetry server: not running\r\n");
                return 0;
            }
            tcp_telemetry_server_stop();
            shell_puts("telemetry server: stopped\r\n");
            return 0;
        }
        if (strcmp(act, "status") == 0) {
            struct tcp_telemetry_server_stats st;
            tcp_telemetry_server_get_stats(&st);
            if (st.listening) {
                shell_printf("telemetry server: running on port %u — "
                             "active=%u opened=%u closed=%u rejects=%lu\r\n",
                             (unsigned)st.port,
                             (unsigned)st.sessions_active,
                             (unsigned)st.sessions_opened,
                             (unsigned)st.sessions_closed,
                             (unsigned long)st.accept_rejects);
                shell_printf("  samples dequeued=%lu delivered=%lu dropped=%lu\r\n",
                             (unsigned long)st.samples_dequeued,
                             (unsigned long)st.samples_delivered,
                             (unsigned long)st.samples_dropped);
            } else {
                shell_puts("telemetry server: stopped\r\n");
            }
            return 0;
        }
        if (strcmp(act, "sessions") == 0) {
            shell_printf("   ID  Peer (network-byte ip:port)  Filter           Sent     Drops\r\n");
            shell_printf("  ---  --------------------------  ---------------  -------  -----\r\n");
            uint32_t count = 0;
            tcp_telemetry_server_foreach(telemetry_session_print_visitor,
                                         &count);
            if (count == 0) {
                shell_puts("  (no active sessions)\r\n");
            }
            return 0;
        }
        if (strcmp(act, "kick") == 0) {
            if (argc < 4) {
                shell_puts("usage: telemetry server kick <session-id>\r\n");
                return -1;
            }
            uint32_t id = 0;
            if (shell_parse_uint(argv[3], &id) != 0) {
                shell_puts("telemetry server: invalid session id\r\n");
                return -1;
            }
            if (tcp_telemetry_server_kick(id)) {
                shell_printf("telemetry server: kicked session %u\r\n",
                             (unsigned)id);
                return 0;
            }
            shell_printf("telemetry server: no active session with id %u\r\n",
                         (unsigned)id);
            return -1;
        }
        shell_printf("telemetry server: unknown action '%s'\r\n", act);
        shell_puts("usage: telemetry server [start [port] | stop | status | "
                   "sessions | kick <id>]\r\n");
        return -1;
    }
#endif

    shell_printf("telemetry: unknown subcommand '%s'\r\n", sub);
#if defined(ENABLE_NETWORKING)
    shell_puts("usage: telemetry [stats|list-topics|server ...]\r\n");
#else
    shell_puts("usage: telemetry [stats|list-topics]\r\n");
#endif
    return -1;
}

#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../gpu/nvidia/ga10b_bringup.h"
#include "../gpu/nvidia/ga10b_channel_handoff.h"
#include "../gpu/nvidia/ga10b_gmmu.h"
#include "oplib_pool.h"
#include "oplib_dispatch.h"
/* cache_clean_range / pmm_alloc_page are already pulled in via the
 * earlier `gpu/gpu.h` and top-level `pmm.h` includes. Don't add
 * `cache.h` here — gpu.h declares cache_clean_range as
 * `void cache_clean_range(void *, size_t)` (the linked C ABI in
 * `kernel/gpu/cache.c`) while `kernel/include/cache.h` declares it
 * `static inline ... (const volatile void *, size_t)`; including
 * both in the same TU triggers conflicting-types build errors.
 * Pre-existing API inconsistency tracked separately; for this PR
 * we use whichever declaration is already in scope. */

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
/* GA10B BAR0 base + GR/PBDMA register byte offsets used by the
 * `nvgpu engine-status` and `engine-clear` shell verbs. The BAR0
 * base mirrors `GA10B_BAR0_BASE` in kernel/gpu/nvidia/ga10b_gmmu.c
 * (file-static there); these are spelled out here rather than
 * exported via a header because they're used only at this single
 * call site. PBDMA[0] uses the i=0 stride of pbdma_*_r(i); GR
 * registers are non-indexed. All offsets sourced from
 * ~/slmos-ref/nvidia/nvgpu-include-nvgpu-hw-ga10b/
 * hw_pbdma_ga10b.h and hw_gr_ga10b.h. */
#define ENG_BAR0_BASE              0x17000000ull
#define ENG_PBDMA0_INTR_0          0x00040108u   /* pbdma_intr_0_r(0) */
#define ENG_PBDMA0_STATUS_SCHED    0x0004015cu   /* pbdma_status_sched_r(0) */
#define ENG_PBDMA0_SUBDEVICE       0x00040094u   /* pbdma_subdevice_r(0) */
#define ENG_PBDMA0_PB_HEADER       0x00040084u   /* pbdma_pb_header_r(0) */
#define ENG_PBDMA0_ACQUIRE         0x00040010u   /* pbdma_acquire_r(0) */
#define ENG_GR_INTR                0x00400100u   /* gr_intr_r() */
#define ENG_GR_EXCEPTION           0x00400108u   /* gr_exception_r() */
#define ENG_GR_STATUS              0x00400700u   /* gr_status_r() */
#define ENG_GR_STATUS_1            0x00400604u   /* gr_status_1_r() */
#define ENG_GR_ENGINE_STATUS       0x0040060cu   /* gr_engine_status_r() */

static volatile uint32_t *eng_bar0(void)
{
    return (volatile uint32_t *)(uintptr_t)ENG_BAR0_BASE;
}

static int cmd_nvgpu_engine_clear(void)
{
    /* Attempt write-1-to-clear on `gr_intr_r` and `gr_exception_r`
     * — nvgpu's gr_intr_handle pattern (`writel(gr_intr_r,
     * U32_MAX)`) does this from the kernel driver.
     *
     * EMPIRICAL: Verified twice on jetson-nano-1 (PR #747) that
     * the writes appear to be accepted (no fault) but the
     * registers do NOT change value. Either the GA10B PRIv
     * firewall blocks writes from NS-EL2 even where reads work,
     * or `gr_exception.gpc` is a status rollup whose underlying
     * per-GPC exception state must be cleared at
     * `gr_pri_gpc{N}_exception_r` (0x00501xxx range) before the
     * top-level rollup will reset. The verb is still useful as a
     * confirmation tool — running it and seeing pre==post is
     * the diagnostic. Real clearing of GPC-rooted exceptions
     * needs per-GPC writes, which requires a way to clear the
     * underlying SM hww_warp_esr / hww_global_esr first; those
     * are `peek 0x17504730 / 0x17504734`-accessible and
     * `poke ... 0` empirically does clear them (see PR #747
     * commit message for the BSSY barrier-slot diagnosis chain
     * that used this). */
    volatile uint32_t *bar = eng_bar0();
    uint32_t prev_intr = bar[ENG_GR_INTR / 4u];
    uint32_t prev_exc  = bar[ENG_GR_EXCEPTION / 4u];
    bar[ENG_GR_INTR      / 4u] = 0xFFFFFFFFu;
    bar[ENG_GR_EXCEPTION / 4u] = 0xFFFFFFFFu;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t new_intr = bar[ENG_GR_INTR / 4u];
    uint32_t new_exc  = bar[ENG_GR_EXCEPTION / 4u];
    shell_printf("engine-clear: gr_intr 0x%08x -> 0x%08x, "
                 "gr_exception 0x%08x -> 0x%08x%s\r\n",
                 (unsigned)prev_intr, (unsigned)new_intr,
                 (unsigned)prev_exc, (unsigned)new_exc,
                 (prev_intr == new_intr && prev_exc == new_exc)
                     ? " (writes ignored — see verb comment)"
                     : "");
    return 0;
}

/* PBDMA[0] status block of `engine-status`, split out for
 * readability. Decodes `pbdma_intr_0` pending bits, the
 * `status_sched` channel-status enum, and the subdevice
 * active/DMA-enable bits. */
static void engine_status_dump_pbdma(volatile uint32_t *bar)
{
    uint32_t pbdma_intr_0       = bar[ENG_PBDMA0_INTR_0       / 4u];
    uint32_t pbdma_status_sched = bar[ENG_PBDMA0_STATUS_SCHED / 4u];
    uint32_t pbdma_subdevice    = bar[ENG_PBDMA0_SUBDEVICE    / 4u];
    uint32_t pbdma_pb_header    = bar[ENG_PBDMA0_PB_HEADER    / 4u];
    uint32_t pbdma_acquire      = bar[ENG_PBDMA0_ACQUIRE      / 4u];

    shell_printf("\r\n=== PBDMA[0] ===\r\n");
    shell_printf("  intr_0       = 0x%08x  ",
                 (unsigned)pbdma_intr_0);
    if (pbdma_intr_0 != 0) {
        shell_printf("(");
        if (pbdma_intr_0 & 0x2000u)   shell_printf("gpfifo ");
        if (pbdma_intr_0 & 0x4000u)   shell_printf("gpptr ");
        if (pbdma_intr_0 & 0x8000u)   shell_printf("gpentry ");
        if (pbdma_intr_0 & 0x10000u)  shell_printf("gpcrc ");
        if (pbdma_intr_0 & 0x20000u)  shell_printf("pbptr ");
        if (pbdma_intr_0 & 0x40000u)  shell_printf("pbentry ");
        if (pbdma_intr_0 & 0x80000u)  shell_printf("pbcrc ");
        if (pbdma_intr_0 & 0x200000u) shell_printf("method ");
        if (pbdma_intr_0 & 0x800000u) shell_printf("device ");
        shell_printf(")\r\n");
    } else {
        shell_printf("(no pending interrupts)\r\n");
    }

    unsigned tsgid = (unsigned)(pbdma_status_sched & 0xfffu);
    unsigned chan_status = (unsigned)((pbdma_status_sched >> 13) & 0x7u);
    unsigned next_tsgid = (unsigned)((pbdma_status_sched >> 16) & 0xfffu);
    const char *cs_name;
    switch (chan_status) {
        case 0: cs_name = "invalid"; break;
        case 1: cs_name = "valid"; break;
        case 5: cs_name = "chsw_save"; break;
        case 6: cs_name = "chsw_load"; break;
        case 7: cs_name = "chsw_switch"; break;
        default: cs_name = "?"; break;
    }
    shell_printf("  status_sched = 0x%08x  (tsgid=%u next_tsgid=%u "
                 "chan_status=%u/%s)\r\n",
                 (unsigned)pbdma_status_sched,
                 tsgid, next_tsgid, chan_status, cs_name);

    unsigned subdev_id = (unsigned)(pbdma_subdevice & 0xfffu);
    bool subdev_active = (pbdma_subdevice & 0x10000000u) != 0;
    bool subdev_dma = (pbdma_subdevice & 0x20000000u) != 0;
    shell_printf("  subdevice    = 0x%08x  (id=%u active=%d dma_en=%d)"
                 "\r\n",
                 (unsigned)pbdma_subdevice,
                 subdev_id, subdev_active, subdev_dma);

    shell_printf("  pb_header    = 0x%08x\r\n",
                 (unsigned)pbdma_pb_header);
    shell_printf("  acquire      = 0x%08x\r\n",
                 (unsigned)pbdma_acquire);
}

/* GR engine status block of `engine-status`, split out for
 * readability. Decodes `gr_exception` (top-level rollup of
 * fe/memfmt/pd/scc/ds/ssync/mme/sked/mme_fe1/gpc), and the
 * `gr_status` busy + fe-method-upper/lower flags. Note that
 * GR registers read 0xbadf1002 (PRI poison) when FECS is idle
 * — they only become readable after at least one channel
 * dispatch has woken the GR engine. */
static void engine_status_dump_gr(volatile uint32_t *bar)
{
    uint32_t gr_intr           = bar[ENG_GR_INTR          / 4u];
    uint32_t gr_exception      = bar[ENG_GR_EXCEPTION     / 4u];
    uint32_t gr_status         = bar[ENG_GR_STATUS        / 4u];
    uint32_t gr_status_1       = bar[ENG_GR_STATUS_1      / 4u];
    uint32_t gr_engine_status  = bar[ENG_GR_ENGINE_STATUS / 4u];

    shell_printf("\r\n=== GR ===\r\n");
    shell_printf("  intr         = 0x%08x  %s\r\n",
                 (unsigned)gr_intr,
                 gr_intr == 0 ? "(no pending)" : "(pending!)");
    shell_printf("  exception    = 0x%08x  ",
                 (unsigned)gr_exception);
    if (gr_exception != 0) {
        shell_printf("(");
        if (gr_exception & 0x1u)        shell_printf("fe ");
        if (gr_exception & 0x2u)        shell_printf("memfmt ");
        if (gr_exception & 0x4u)        shell_printf("pd ");
        if (gr_exception & 0x8u)        shell_printf("scc ");
        if (gr_exception & 0x10u)       shell_printf("ds ");
        if (gr_exception & 0x20u)       shell_printf("ssync ");
        if (gr_exception & 0x80u)       shell_printf("mme ");
        if (gr_exception & 0x100u)      shell_printf("sked ");
        if (gr_exception & 0x200u)      shell_printf("mme_fe1 ");
        if (gr_exception & 0x1000000u)  shell_printf("gpc ");
        shell_printf(")\r\n");
    } else {
        shell_printf("(no exception)\r\n");
    }
    bool gr_busy = (gr_status & 0x1u) != 0;
    bool gr_fe_method_upper = (gr_status & 0x2u) != 0;
    bool gr_fe_method_lower = (gr_status & 0x4u) != 0;
    shell_printf("  status       = 0x%08x  (busy=%d fe_method_upper=%d "
                 "fe_method_lower=%d)\r\n",
                 (unsigned)gr_status,
                 gr_busy, gr_fe_method_upper, gr_fe_method_lower);
    shell_printf("  status_1     = 0x%08x\r\n",
                 (unsigned)gr_status_1);
    shell_printf("  engine_status= 0x%08x\r\n",
                 (unsigned)gr_engine_status);
    shell_printf("\r\n");
}

static int cmd_nvgpu_engine_status(void)
{
    /* Read GR + PBDMA engine status registers from BAR0 and
     * decode key fields. Diagnosis tool for the "first dispatch
     * passes, second hangs" failure mode (PR #744 follow-up,
     * PR #747 root-causing). Run before and after a stalled
     * dispatch to localize the wedged engine. PBDMA[0] is the
     * only instance our channel uses (channel 0, PBDMA stride
     * 2048 bytes). */
    volatile uint32_t *bar = eng_bar0();
    engine_status_dump_pbdma(bar);
    engine_status_dump_gr(bar);
    return 0;
}

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
    if (strcmp(argv[1], "engine-clear") == 0) {
        return cmd_nvgpu_engine_clear();
    }
    if (strcmp(argv[1], "engine-status") == 0) {
        return cmd_nvgpu_engine_status();
    }
    if (strcmp(argv[1], "submit") == 0) {
        /* Phase 7: pushbuffer smoke test. */
        int rc = ga10b_bringup_smoke_test(&b);
        shell_printf("submit: rc=%d, state=%d\r\n", rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "submit-compute") == 0) {
        /* Phase 7 (compute): COMPUTE_B SEMAPHORE_RELEASE smoke test.
         * Requires `nvgpu inherit` + `nvgpu channel` first (same as
         * the host-family `submit`). Unblocked by PR #295. */
        int rc = ga10b_bringup_smoke_test_compute(&b);
        shell_printf("submit-compute: rc=%d, state=%d\r\n",
                     rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "launch-kernel") == 0) {
        /* Phase 8: launch a pre-uploaded compute kernel from a v3
         * handoff (shader + QMD + cbuf + output pre-populated by
         * scripts/gpu-kernel-launch.c --preserve-for-kexec). */
        int rc = ga10b_bringup_launch_kernel(&b);
        shell_printf("launch-kernel: rc=%d, state=%d\r\n",
                     rc, (int)b.state);
        return rc;
    }
    if (strcmp(argv[1], "run-mnist") == 0) {
        /* M7: dispatch a v5 multi-op pipeline (the MNIST chain
         * pre-uploaded by scripts/gpu-kernel-mnist.c) and read
         * back the final op's 10 fp32 logits. Argmax over those
         * logits is the predicted class. Requires `nvgpu inherit`
         * + `nvgpu channel` to have run already.
         *
         * Output formatting prints the logits as raw fp32 bit
         * patterns — the kernel target compiles with
         * -mgeneral-regs-only and can't format fp32 as decimal.
         * Use `slm.gpu_run_mnist()` from Lua for decimal output. */
        int rc = ga10b_bringup_launch_kernel(&b);
        if (rc < 0) {
            shell_printf("run-mnist: launch failed rc=%d\r\n", rc);
            return rc;
        }
        uint8_t logits_bytes[40];
        int n = ga10b_bringup_read_pipeline_output(&b, logits_bytes,
                                                    sizeof(logits_bytes));
        if (n < 0) {
            shell_printf("run-mnist: failed to read logits "
                         "(no v5 handoff?)\r\n");
            return -1;
        }
        int argmax = slm_fp32_argmax(logits_bytes, 10u);
        shell_puts("run-mnist: logits (fp32 bit patterns):\r\n");
        for (int i = 0; i < 10; i++) {
            uint32_t bits;
            __builtin_memcpy(&bits, logits_bytes + i * 4, 4);
            shell_printf("  [%d] 0x%08x%s\r\n",
                         i, bits, i == argmax ? "  <-- argmax" : "");
        }
        shell_printf("run-mnist: predicted class = %d\r\n", argmax);
        return 0;
    }

    if (strcmp(argv[1], "oplib") == 0) {
        if (argc < 3 || strcmp(argv[2], "status") == 0) {
            oplib_pool_status_print();
            return 0;
        }
        if (strcmp(argv[2], "stage") == 0) {
            /* Stage the embedded SASS region into GPU VA so the
             * dispatcher (#714) can fetch instructions through the
             * inherited channel's GMMU. Uses inst_block_phys from
             * the loaded handoff if present, else FECS_CURRENT_CTX.
             *
             *   nvgpu oplib stage              — auto-discover inst block
             *   nvgpu oplib stage <inst_hex>   — explicit inst block
             */
            uint64_t inst_phys = 0;
            if (argc >= 4) {
                const char *s = argv[3];
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    s += 2;
                }
                while (*s) {
                    uint64_t d;
                    if (*s >= '0' && *s <= '9') {
                        d = *s - '0';
                    } else if (*s >= 'a' && *s <= 'f') {
                        d = 10 + (*s - 'a');
                    } else if (*s >= 'A' && *s <= 'F') {
                        d = 10 + (*s - 'A');
                    } else {
                        shell_puts("bad hex inst_phys\r\n");
                        return -1;
                    }
                    inst_phys = (inst_phys << 4) | d;
                    s++;
                }
            } else {
                const struct ga10b_channel_handoff *h =
                    ga10b_bringup_handoff();
                /* Diagnostic dump of relevant handoff fields, gated
                 * behind `gpu debug on` so steady-state callers
                 * aren't spammed. Useful when the fast path doesn't
                 * trigger and you need to tell "helper didn't pre-
                 * stage" from "fast path picked but failed". */
                if (ga10b_dispatch_verbose_get()) {
                    shell_printf("oplib stage: handoff h=%p shader_phys=0x%lx "
                                 "shader_gpu_va=0x%lx shader_size=%u "
                                 "cbuf_phys=0x%lx cbuf_gpu_va=0x%lx\r\n",
                                 (const void *)h,
                                 (h ? (unsigned long)h->shader_phys : 0ul),
                                 (h ? (unsigned long)h->shader_gpu_va : 0ul),
                                 (h ? (unsigned)h->shader_size : 0u),
                                 (h ? (unsigned long)h->cbuf_phys : 0ul),
                                 (h ? (unsigned long)h->cbuf_gpu_va : 0ul));
                }
                /* Fast path: when the helper pre-staged the SASS
                 * region in the channel's GMMU (v7 mode), skip
                 * inst-block discovery entirely — `oplib_pool_-
                 * stage_to_gpu` doesn't need it. The function
                 * detects the pre-staged region via h->shader_*
                 * and writes the SASS bytes directly into the
                 * helper's mapped buffer. */
                if (h != NULL && h->shader_gpu_va != 0 &&
                    h->shader_phys != 0) {
                    shell_puts("oplib stage: using helper-staged SASS "
                               "region (no inst block discovery needed)\r\n");
                    inst_phys = 0;
                    goto oplib_stage_call;
                }
                if (h != NULL && h->inst_block_phys != 0) {
                    inst_phys = h->inst_block_phys;
                } else {
                    /* FECS_CURRENT_CTX may hold a stale pointer when
                     * Linux nvgpu freed and reused the inst-block-
                     * pointing memory between the helper's last
                     * channel activity and the kexec. Verify that the
                     * discovered inst block actually maps the
                     * inherited channel's pushbuffer; if not, fall
                     * back to a DRAM walk that cross-checks every
                     * candidate against the handoff's
                     * (pushbuf_gpu_va, pushbuf_phys) pair. */
                    inst_phys = ga10b_gmmu_discover_inst_block_phys();
                    bool fecs_ok = false;
                    if (inst_phys != 0 && h != NULL &&
                        h->pushbuf_gpu_va != 0 && h->pushbuf_phys != 0) {
                        struct ga10b_gmmu_walk_result wr;
                        ga10b_gmmu_walk(inst_phys,
                                        h->pushbuf_gpu_va, &wr);
                        shell_printf("oplib stage: FECS inst=0x%lx walk "
                                     "status=%d levels=%d pdb=0x%lx "
                                     "leaf=0x%lx (want 0x%lx)\r\n",
                                     (unsigned long)inst_phys,
                                     (int)wr.status,
                                     wr.levels_walked,
                                     (unsigned long)wr.pdb_phys,
                                     (unsigned long)wr.leaf_phys,
                                     (unsigned long)h->pushbuf_phys);
                        if (wr.status == GA10B_GMMU_WALK_OK &&
                            wr.leaf_phys == h->pushbuf_phys) {
                            fecs_ok = true;
                        }
                    }
                    if (!fecs_ok) {
                        if (h == NULL || h->pushbuf_gpu_va == 0 ||
                            h->pushbuf_phys == 0) {
                            shell_puts("oplib stage: no handoff and "
                                       "FECS_CURRENT_CTX read failed\r\n");
                            return -1;
                        }
                        /* Scan all of mapped DRAM — both low (Linux
                         * dma_alloc_coherent often places inst
                         * blocks here) and high (where the per-
                         * channel nvmap dmabufs live). High region
                         * scanned first since inst blocks usually
                         * cluster near the dmabufs that follow them
                         * in allocation order. The phys_in_dram
                         * helper already excludes the OP-TEE
                         * carveout (0xBE..0xC2) so the walker won't
                         * fault inside it. */
                        struct { uint64_t lo, hi; } ranges[] = {
                            { 0x100000000ull, 0x180000000ull },
                            { 0x80000000ull,  0x100000000ull },
                        };
                        shell_printf("oplib stage: FECS inst=0x%lx didn't "
                                     "map handoff PB; walking DRAM "
                                     "(2 ranges)\r\n",
                                     (unsigned long)inst_phys);
                        inst_phys = 0;
                        for (size_t r = 0; r < sizeof(ranges)/sizeof(ranges[0]);
                             r++) {
                            inst_phys = ga10b_gmmu_discover_inst_block_via_walk(
                                h->pushbuf_gpu_va, h->pushbuf_phys,
                                ranges[r].lo, ranges[r].hi);
                            if (inst_phys != 0) break;
                        }
                        if (inst_phys == 0) {
                            shell_puts("oplib stage: walk-based discovery "
                                       "found no inst block matching the "
                                       "handoff's PB\r\n");
                            return -1;
                        }
                    }
                    shell_printf("oplib stage: discovered inst_block_phys "
                                 "= 0x%lx (%s)\r\n",
                                 (unsigned long)inst_phys,
                                 fecs_ok ? "FECS" : "DRAM walk");
                }
            }
oplib_stage_call:
            int rc = oplib_pool_stage_to_gpu(inst_phys);
            if (rc < 0) {
                shell_printf("oplib stage: rc=%d\r\n", rc);
                return rc;
            }
            shell_printf("oplib stage: ok, gpu_va_base=0x%lx\r\n",
                         (unsigned long)oplib_pool_gpu_va_base());
            return 0;
        }
        if (strcmp(argv[2], "probe") == 0) {
            /* Look up an SASS kernel by (op_kind, tier, dtype) and
             * print its GPU VA + size. Validates the staged pool
             * resolves a known triple correctly.
             *
             *   nvgpu oplib probe <op_kind> <tier> <dtype>
             *
             * Discriminants are decimal ints — see kernel/include/
             * gpu_handoff.h for the enum values.
             */
            if (argc < 6) {
                shell_puts("usage: nvgpu oplib probe "
                           "<op_kind> <tier> <dtype>\r\n");
                return -1;
            }
            uint32_t op_kind = (uint32_t)atoi(argv[3]);
            uint32_t tier    = (uint32_t)atoi(argv[4]);
            uint32_t dtype   = (uint32_t)atoi(argv[5]);
            uint64_t gpu_va = 0;
            size_t   size = 0;
            int rc = oplib_pool_get_sass_gpu_va(op_kind, tier, dtype,
                                                 &gpu_va, &size);
            if (rc != 0) {
                shell_printf("oplib probe: (%u, %u, %u) rc=%d\r\n",
                             (unsigned)op_kind, (unsigned)tier,
                             (unsigned)dtype, rc);
                return rc;
            }
            shell_printf("oplib probe: (%u, %u, %u) gpu_va=0x%lx size=%zu\r\n",
                         (unsigned)op_kind, (unsigned)tier, (unsigned)dtype,
                         (unsigned long)gpu_va, size);
            return 0;
        }
        if (strcmp(argv[2], "prep-rmsnorm") == 0) {
            /* #714 A.2 follow-on: prepare a v7-op for RMSNORM
             * dispatch. Allocates input/gamma/output buffers via
             * GMMU, builds + populates a cbuf, computes launch
             * shape — everything except the actual pushbuffer
             * submit. Prints the prepared dispatch parameters for
             * inspection.
             *
             * Usage: nvgpu oplib prep-rmsnorm <n_rows> <n>
             *
             * Hardware verification: stage the operator library
             * first via `nvgpu oplib stage`, then run this verb.
             * Output should show non-zero shader_va, cbuf_va, and
             * the expected grid/block dims.
             */
            if (argc < 5) {
                shell_puts("usage: nvgpu oplib prep-rmsnorm "
                           "<n_rows> <n>\r\n");
                return -1;
            }
            uint32_t n_rows = (uint32_t)atoi(argv[3]);
            uint32_t n      = (uint32_t)atoi(argv[4]);
            if (n_rows == 0 || n == 0) {
                shell_puts("oplib prep-rmsnorm: n_rows and n must be > 0\r\n");
                return -1;
            }
            /* Cap inputs at safety margins large enough for any
             * realistic SLM workload (Qwen2.5-1.5B prefill: 16
             * rows × 8960 elements; LM head: 1 row × 151936
             * vocab). 1<<16 each leaves >2 orders of magnitude of
             * headroom on both axes.
             *
             * Without this cap, atoi-supplied multi-million values
             * would overflow uint32_t in `n_rows * n * 2`, allocate
             * a tiny cbuf, and pass garbage grid_x past GA10B's
             * 65535-CTA limit to the launch-shape function. */
            if (n_rows > 65536u || n > 65536u) {
                shell_printf("oplib prep-rmsnorm: n_rows=%u n=%u "
                             "exceeds safety cap (65536 each)\r\n",
                             (unsigned)n_rows, (unsigned)n);
                return -1;
            }

            /* Resolve inst_block_phys: handoff first, FECS fallback. */
            uint64_t inst_phys = 0;
            const struct ga10b_channel_handoff *h =
                ga10b_bringup_handoff();
            if (h != NULL && h->inst_block_phys != 0) {
                inst_phys = h->inst_block_phys;
            } else {
                inst_phys = ga10b_gmmu_discover_inst_block_phys();
                if (inst_phys == 0) {
                    shell_puts("oplib prep-rmsnorm: no handoff and "
                               "FECS_CURRENT_CTX read failed\r\n");
                    return -1;
                }
            }

            /* Allocate input/gamma/output buffers in the channel's
             * GMMU. RmsNorm consumes input + gamma (both n_rows*n /
             * n FP16 halves), produces output (n_rows*n halves).
             * Use uint64_t for the byte arithmetic — the input cap
             * above (n_rows*n ≤ 2^32) means n_rows*n*2 fits in u64
             * with room. Round each up to a 4 KB page boundary. */
            uint64_t in_bytes  = (uint64_t)n_rows * n * 2u;
            uint64_t gam_bytes = (uint64_t)n * 2u;
            uint64_t out_bytes = (uint64_t)n_rows * n * 2u;
            uint32_t in_pages  = (uint32_t)((in_bytes  + 4095u) / 4096u);
            uint32_t gam_pages = (uint32_t)((gam_bytes + 4095u) / 4096u);
            uint32_t out_pages = (uint32_t)((out_bytes + 4095u) / 4096u);
            if (in_pages == 0)  in_pages = 1;
            if (gam_pages == 0) gam_pages = 1;
            if (out_pages == 0) out_pages = 1;

            uint64_t in_va = 0, gam_va = 0, out_va = 0;
            uint64_t in_phys = 0, gam_phys = 0, out_phys = 0;
            void *in_cpu = NULL, *gam_cpu = NULL, *out_cpu = NULL;
            int rc = ga10b_gmmu_alloc(inst_phys, in_pages, 0,
                                       &in_va, &in_cpu, &in_phys);
            if (rc < 0) {
                shell_printf("oplib prep-rmsnorm: input alloc rc=%d\r\n", rc);
                return rc;
            }
            rc = ga10b_gmmu_alloc(inst_phys, gam_pages, 0,
                                   &gam_va, &gam_cpu, &gam_phys);
            if (rc < 0) {
                shell_printf("oplib prep-rmsnorm: gamma alloc rc=%d\r\n", rc);
                return rc;
            }
            rc = ga10b_gmmu_alloc(inst_phys, out_pages, 0,
                                   &out_va, &out_cpu, &out_phys);
            if (rc < 0) {
                shell_printf("oplib prep-rmsnorm: output alloc rc=%d\r\n", rc);
                return rc;
            }
            shell_printf("oplib prep-rmsnorm: in=0x%lx gamma=0x%lx out=0x%lx\r\n",
                         (unsigned long)in_va, (unsigned long)gam_va,
                         (unsigned long)out_va);

            /* Build args. eps = 1e-6f (Qwen default) — 0x358637BD
             * is the IEEE 754 bit pattern. */
            struct operator_dispatch_args args = {
                .op_kind = SLM_GPU_OP_RMSNORM,
                .u.rmsnorm = {
                    .x_gpu_va     = in_va,
                    .gamma_gpu_va = gam_va,
                    .out_gpu_va   = out_va,
                    .n_rows       = n_rows,
                    .n            = n,
                    .eps_bits     = 0x358637BDu,
                },
            };

            struct slm_oplib_dispatch_prep prep;
            rc = slm_oplib_prepare_dispatch(inst_phys,
                                             SLM_GPU_OP_RMSNORM,
                                             SLM_GPU_TIER_SIMT,
                                             SLM_GPU_DTYPE_FP16,
                                             &args, &prep);
            if (rc < 0) {
                shell_printf("oplib prep-rmsnorm: prepare rc=%d "
                             "(did you `nvgpu oplib stage` first?)\r\n", rc);
                return rc;
            }
            shell_printf("oplib prep-rmsnorm: PREPARED — submit not yet "
                         "wired (#714 follow-on)\r\n");
            return 0;
        }
        if (strcmp(argv[2], "dispatch-rmsnorm") == 0) {
            /* #732: end-to-end RMSNORM-on-GPU smoke test. Allocates
             * input/gamma/output buffers via GMMU, fills input with
             * a known FP16 pattern (sentinel: alternating 0x3C00
             * and 0xBC00 = +1.0 / -1.0 so RMSNorm has work to do
             * but a deterministic answer), fills gamma with all
             * 0x3C00 (+1.0), then dispatches RMSNORM through the
             * operator library and prints the first few output
             * halves for inspection.
             *
             * Usage: nvgpu oplib dispatch-rmsnorm <n_rows> <n>
             *
             * Pre: `nvgpu oplib stage` must have run + the channel
             * must be inherited. */
            if (argc < 5) {
                shell_puts("usage: nvgpu oplib dispatch-rmsnorm "
                           "<n_rows> <n>\r\n");
                return -1;
            }
            uint32_t n_rows = (uint32_t)atoi(argv[3]);
            uint32_t n      = (uint32_t)atoi(argv[4]);
            if (n_rows == 0 || n == 0) {
                shell_puts("dispatch-rmsnorm: n_rows and n must be > 0\r\n");
                return -1;
            }
            if (n_rows > 65536u || n > 65536u) {
                shell_printf("dispatch-rmsnorm: cap exceeded "
                             "(n_rows=%u n=%u, max 65536 each)\r\n",
                             (unsigned)n_rows, (unsigned)n);
                return -1;
            }

            uint64_t inst_phys = 0;
            const struct ga10b_channel_handoff *h =
                ga10b_bringup_handoff();
            uint64_t in_bytes  = (uint64_t)n_rows * n * 2u;
            uint64_t gam_bytes = (uint64_t)n * 2u;
            uint64_t out_bytes = (uint64_t)n_rows * n * 2u;
            uint32_t in_pages  = (uint32_t)((in_bytes  + 4095u) / 4096u);
            uint32_t gam_pages = (uint32_t)((gam_bytes + 4095u) / 4096u);
            uint32_t out_pages = (uint32_t)((out_bytes + 4095u) / 4096u);
            if (in_pages == 0)  in_pages = 1;
            if (gam_pages == 0) gam_pages = 1;
            if (out_pages == 0) out_pages = 1;

            uint64_t in_va = 0, gam_va = 0, out_va = 0;
            uint64_t in_phys = 0, gam_phys = 0, out_phys = 0;
            void *in_cpu = NULL, *gam_cpu = NULL, *out_cpu = NULL;
            int rc;

            /* Fast path: when the helper pre-staged the SASS pool
             * (1 MB), carve scratch input/gamma/output out of the
             * tail. The SASS region itself sits at offset 0 with
             * length 7680 B for the rmsnorm-only blob; we leave a
             * safety margin and start the scratch carve-outs at
             * 64 KB so any future SASS region growth doesn't
             * collide. Each region is sized to in_bytes/gam_bytes/
             * out_bytes — the cap upstream limits each to
             * 65536² × 2 B but the smoke test uses tiny shapes
             * (4×16 → 128 B). The 1 MB pool is plenty for any
             * shape this verb is realistically invoked with.
             *
             * Avoiding the post-kexec ga10b_gmmu_alloc path means
             * we don't need a discovered inst_block_phys — the
             * helper already mapped the pool in the channel's
             * address space. */
            if (h != NULL && h->shader_gpu_va != 0 &&
                h->shader_phys != 0 && h->shader_size >= 0x40000u) {
                uint64_t base_phys = h->shader_phys;
                uint64_t base_gva  = h->shader_gpu_va;
                /* SASS occupies [0..0x10000); leave that alone. */
                in_phys  = base_phys + 0x10000ull;
                in_va    = base_gva  + 0x10000ull;
                gam_phys = base_phys + 0x20000ull;
                gam_va   = base_gva  + 0x20000ull;
                out_phys = base_phys + 0x30000ull;
                out_va   = base_gva  + 0x30000ull;
                if (in_bytes  > 0x10000u || gam_bytes > 0x10000u ||
                    out_bytes > 0x10000u) {
                    shell_printf("dispatch-rmsnorm: scratch slot too small "
                                 "(in=%llu gam=%llu out=%llu vs 64 KB)\r\n",
                                 (unsigned long long)in_bytes,
                                 (unsigned long long)gam_bytes,
                                 (unsigned long long)out_bytes);
                    return -1;
                }
                in_cpu  = (void *)(uintptr_t)in_phys;
                gam_cpu = (void *)(uintptr_t)gam_phys;
                out_cpu = (void *)(uintptr_t)out_phys;
                shell_puts("dispatch-rmsnorm: scratch buffers carved from "
                           "pre-staged SASS pool\r\n");
            } else {
                if (h != NULL && h->inst_block_phys != 0) {
                    inst_phys = h->inst_block_phys;
                } else {
                    inst_phys = ga10b_gmmu_discover_inst_block_phys();
                    if (inst_phys == 0) {
                        shell_puts("dispatch-rmsnorm: no handoff and "
                                   "FECS_CURRENT_CTX read failed\r\n");
                        return -1;
                    }
                }
                rc = ga10b_gmmu_alloc(inst_phys, in_pages, 0,
                                       &in_va, &in_cpu, &in_phys);
                if (rc < 0) {
                    shell_printf("dispatch-rmsnorm: input alloc rc=%d\r\n", rc);
                    return rc;
                }
                rc = ga10b_gmmu_alloc(inst_phys, gam_pages, 0,
                                       &gam_va, &gam_cpu, &gam_phys);
                if (rc < 0) {
                    shell_printf("dispatch-rmsnorm: gamma alloc rc=%d\r\n", rc);
                    return rc;
                }
                rc = ga10b_gmmu_alloc(inst_phys, out_pages, 0,
                                       &out_va, &out_cpu, &out_phys);
                if (rc < 0) {
                    shell_printf("dispatch-rmsnorm: output alloc rc=%d\r\n", rc);
                    return rc;
                }
            }

            /* Fill input with alternating +1.0/-1.0 FP16 (0x3C00 /
             * 0xBC00). After RMSNorm with gamma=1.0, the output
             * should also be ±1.0 for each element since
             * sqrt(mean(1^2)) == 1.0 and rms_inv = 1.0. */
            volatile uint16_t *in_p = (volatile uint16_t *)in_cpu;
            uint64_t in_count = (uint64_t)n_rows * n;
            for (uint64_t i = 0; i < in_count; i++) {
                in_p[i] = (i & 1) ? 0xBC00u : 0x3C00u;
            }
            cache_clean_range(in_cpu, in_pages * 4096u);

            volatile uint16_t *gam_p = (volatile uint16_t *)gam_cpu;
            for (uint32_t i = 0; i < n; i++) {
                gam_p[i] = 0x3C00u;     /* FP16 +1.0 */
            }
            cache_clean_range(gam_cpu, gam_pages * 4096u);

            /* Pre-fill output with a sentinel so we can tell if
             * the GPU wrote anything at all. */
            volatile uint16_t *out_p = (volatile uint16_t *)out_cpu;
            for (uint64_t i = 0; i < in_count; i++) {
                out_p[i] = 0xCAFEu;
            }
            cache_clean_range(out_cpu, out_pages * 4096u);

            shell_printf("dispatch-rmsnorm: in=0x%lx gamma=0x%lx out=0x%lx "
                         "(n_rows=%u, n=%u)\r\n",
                         (unsigned long)in_va, (unsigned long)gam_va,
                         (unsigned long)out_va,
                         (unsigned)n_rows, (unsigned)n);

            struct operator_dispatch_args args = {
                .op_kind = SLM_GPU_OP_RMSNORM,
                .u.rmsnorm = {
                    .x_gpu_va     = in_va,
                    .gamma_gpu_va = gam_va,
                    .out_gpu_va   = out_va,
                    .n_rows       = n_rows,
                    .n            = n,
                    .eps_bits     = 0x358637BDu,    /* 1e-6f */
                },
            };

            rc = slm_oplib_dispatch(&b, inst_phys,
                                     SLM_GPU_OP_RMSNORM,
                                     SLM_GPU_TIER_SIMT,
                                     SLM_GPU_DTYPE_FP16,
                                     &args);
            if (rc < 0) {
                shell_printf("dispatch-rmsnorm: slm_oplib_dispatch rc=%d\r\n", rc);
                return rc;
            }

            /* Read back output. Cache-invalidate first so the CPU
             * sees what the GPU wrote (not stale L1). */
            cache_invalidate_range(out_cpu, out_pages * 4096u);

            /* Print the first 8 output halves + a sample from the
             * tail. With sentinel-fill above, any 0xCAFE means the
             * GPU didn't write that slot. */
            shell_puts("dispatch-rmsnorm: output[0..7] = ");
            for (int i = 0; i < 8 && (uint64_t)i < in_count; i++) {
                shell_printf("0x%04x ", (unsigned)out_p[i]);
            }
            shell_puts("\r\n");
            if (in_count > 8) {
                uint64_t tail = in_count - 1;
                shell_printf("dispatch-rmsnorm: output[%llu] = 0x%04x\r\n",
                             (unsigned long long)tail,
                             (unsigned)out_p[tail]);
            }

            /* Sentinel check: at least output[0] must not be 0xCAFE. */
            if (out_p[0] == 0xCAFEu) {
                shell_puts("dispatch-rmsnorm: FAIL — output[0] is "
                           "still 0xCAFE sentinel (GPU didn't write)\r\n");
                return -1;
            }
            shell_puts("dispatch-rmsnorm: PASS — GPU wrote output\r\n");
            return 0;
        }
        shell_puts("usage: nvgpu oplib [status | stage [<inst_hex>] | "
                   "probe <op_kind> <tier> <dtype> | "
                   "prep-rmsnorm <n_rows> <n> | "
                   "dispatch-rmsnorm <n_rows> <n>]\r\n");
        return -1;
    }

    if (strcmp(argv[1], "gmmu") == 0) {
        /* Read-only GMMU page-table walker (#666 Milestone A).
         *
         * Subcommands:
         *   nvgpu gmmu pushbuf   — walk g_handoff.pushbuf_gpu_va,
         *                          assert leaf phys == pushbuf_phys
         *   nvgpu gmmu walk <hex_va>
         *                        — walk an arbitrary GPU VA against
         *                          the loaded handoff's inst block
         *   nvgpu gmmu walk-raw <inst_block_phys_hex> <gpu_va_hex>
         *                        — walk against a caller-supplied
         *                          inst-block phys. Bypasses the
         *                          handoff requirement; useful for
         *                          structural validation when the
         *                          host-side helper hasn't run.
         *
         * `pushbuf` and `walk` need a handoff (run `nvgpu inherit`
         * + `nvgpu channel` first). `walk-raw` doesn't.
         */
        if (argc < 3) {
            shell_puts("usage: nvgpu gmmu <pushbuf | walk <hex_va> | "
                       "walk-raw <inst_phys_hex> <hex_va> | "
                       "alloc-page | alloc-page-synth | "
                       "alloc-multi-synth <n_pages>>\r\n");
            return -1;
        }
        if (strcmp(argv[2], "reuse-synth") == 0) {
            /* #666 Milestone D: alloc-free-alloc round-trip on a
             * synthetic inst block. Validates:
             *   - free's TLB-invalidate doesn't crash the GPU
             *   - the freed extent goes into the tracker
             *   - the next alloc with matching n_pages reuses the
             *     same VA from the tracker (not the bump cursor)
             *   - the new PTE is written correctly at the reused
             *     VA (walker reads back the new alloc's phys, not
             *     stale bytes from the freed alloc)
             */
            void *inst = pmm_alloc_page();
            void *pdb_page = pmm_alloc_page();
            if (inst == NULL || pdb_page == NULL) {
                shell_puts("reuse-synth: PMM exhaustion\r\n");
                return -1;
            }
            for (int i = 0; i < 512; i++) {
                ((volatile uint64_t *)inst)[i] = 0;
                ((volatile uint64_t *)pdb_page)[i] = 0;
            }
            uint64_t inst_phys = (uint64_t)(uintptr_t)inst;
            uint64_t pdb_phys = (uint64_t)(uintptr_t)pdb_page;
            volatile uint32_t *iw = (volatile uint32_t *)inst;
            iw[128] = ((uint32_t)pdb_phys & 0xfffff000u) |
                      (2u << 1) | (1u << 3);
            iw[129] = (uint32_t)(pdb_phys >> 32);
            cache_clean_range(inst, 4096);
            cache_clean_range(pdb_page, 4096);

            /* First alloc. */
            uint64_t va_a = 0, phys_a = 0;
            void *cpu_a = NULL;
            int rc = ga10b_gmmu_alloc_page(inst_phys, 0, &va_a, &cpu_a, &phys_a);
            if (rc < 0) { shell_printf("alloc#1 rc=%d\r\n", rc); return rc; }
            shell_printf("alloc#1 va=0x%lx phys=0x%lx\r\n",
                         (unsigned long)va_a, (unsigned long)phys_a);

            /* Free it — fires TLB invalidate, returns extent to tracker. */
            int frc = ga10b_gmmu_free(inst_phys, va_a, 1);
            if (frc < 0) { shell_printf("free rc=%d\r\n", frc); return frc; }
            shell_printf("free: ok (tracker_count=%u)\r\n",
                         (unsigned)ga10b_gmmu_free_tracker_count());

            /* Second alloc — should pull va_a back out of the tracker. */
            uint64_t va_b = 0, phys_b = 0;
            void *cpu_b = NULL;
            rc = ga10b_gmmu_alloc_page(inst_phys, 0, &va_b, &cpu_b, &phys_b);
            if (rc < 0) { shell_printf("alloc#2 rc=%d\r\n", rc); return rc; }
            shell_printf("alloc#2 va=0x%lx phys=0x%lx (tracker_count=%u)\r\n",
                         (unsigned long)va_b, (unsigned long)phys_b,
                         (unsigned)ga10b_gmmu_free_tracker_count());

            /* Walk the reused VA — leaf must point at alloc#2's phys
             * (not alloc#1's stale phys). */
            struct ga10b_gmmu_walk_result wr;
            ga10b_gmmu_walk(inst_phys, va_b, &wr);
            if (wr.status != GA10B_GMMU_WALK_OK) {
                shell_printf("walk after alloc#2: status=%d — FAIL\r\n",
                             (int)wr.status);
                return -1;
            }
            int va_match = (va_b == va_a) ? 1 : 0;
            int phys_match = (wr.leaf_phys == phys_b) ? 1 : 0;
            int phys_distinct = (phys_b != phys_a) ? 1 : 0;
            shell_printf("VERIFY: va_reused=%s leaf_match=%s phys_distinct=%s\r\n",
                         va_match ? "YES" : "NO",
                         phys_match ? "YES" : "NO",
                         phys_distinct ? "YES" : "NO");
            return (va_match && phys_match && phys_distinct) ? 0 : -1;
        }
        if (strcmp(argv[2], "alloc-multi-synth") == 0) {
            /* #666 Milestone C: alloc N contiguous-VA pages on a
             * synthetic inst block, walk a few sample VAs to
             * confirm leaf PTEs are valid + correct, then free
             * and walk again to confirm PTEs are cleared.
             *
             * Synthetic inst block (same setup as alloc-page-synth)
             * keeps this self-contained — no real GPU channel
             * needed. */
            if (argc < 4) {
                shell_puts("usage: nvgpu gmmu alloc-multi-synth <n_pages>\r\n");
                return -1;
            }
            uint32_t n_pages = 0;
            if (shell_parse_uint(argv[3], &n_pages) < 0 ||
                n_pages == 0 || n_pages > 512) {
                shell_puts("n_pages must be 1..512\r\n");
                return -1;
            }
            void *inst = pmm_alloc_page();
            void *pdb_page = pmm_alloc_page();
            if (inst == NULL || pdb_page == NULL) {
                shell_puts("alloc-multi-synth: PMM exhaustion\r\n");
                return -1;
            }
            for (int i = 0; i < 512; i++) {
                ((volatile uint64_t *)inst)[i] = 0;
                ((volatile uint64_t *)pdb_page)[i] = 0;
            }
            uint64_t inst_phys = (uint64_t)(uintptr_t)inst;
            uint64_t pdb_phys = (uint64_t)(uintptr_t)pdb_page;
            volatile uint32_t *iw = (volatile uint32_t *)inst;
            iw[128] = ((uint32_t)pdb_phys & 0xfffff000u) |
                      (2u << 1) | (1u << 3);
            iw[129] = (uint32_t)(pdb_phys >> 32);
            cache_clean_range(inst, 4096);
            cache_clean_range(pdb_page, 4096);

            shell_printf("alloc-multi-synth: inst=0x%lx pdb=0x%lx n=%u\r\n",
                         (unsigned long)inst_phys,
                         (unsigned long)pdb_phys,
                         (unsigned)n_pages);

            uint64_t va_base = 0, first_phys = 0;
            void *first_cpu_va = NULL;
            int rc = ga10b_gmmu_alloc(inst_phys, n_pages, 0,
                                      &va_base, &first_cpu_va, &first_phys);
            if (rc < 0) {
                shell_printf("ga10b_gmmu_alloc(n=%u) failed: rc=%d\r\n",
                             (unsigned)n_pages, rc);
                return rc;
            }
            shell_printf("alloc-multi-synth: va_base=0x%lx first_cpu=%p first_phys=0x%lx\r\n",
                         (unsigned long)va_base, first_cpu_va,
                         (unsigned long)first_phys);

            /* Walk first / middle / last to spot-check. */
            uint32_t probes[3] = {0, n_pages / 2, n_pages - 1};
            int n_probes = (n_pages == 1) ? 1 : (n_pages == 2 ? 2 : 3);
            int verify_pass = 1;
            for (int p = 0; p < n_probes; p++) {
                uint32_t idx = probes[p];
                uint64_t va_i = va_base + (uint64_t)idx * 4096ull;
                struct ga10b_gmmu_walk_result wr;
                ga10b_gmmu_walk(inst_phys, va_i, &wr);
                if (wr.status != GA10B_GMMU_WALK_OK) {
                    shell_printf("PROBE[%u] va=0x%lx: walk status=%d — FAIL\r\n",
                                 (unsigned)idx, (unsigned long)va_i,
                                 (int)wr.status);
                    verify_pass = 0;
                    continue;
                }
                shell_printf("PROBE[%u] va=0x%lx leaf_phys=0x%lx\r\n",
                             (unsigned)idx, (unsigned long)va_i,
                             (unsigned long)wr.leaf_phys);
            }

            /* Free and confirm PTEs are cleared. */
            int frc = ga10b_gmmu_free(inst_phys, va_base, n_pages);
            if (frc < 0) {
                shell_printf("ga10b_gmmu_free failed: rc=%d\r\n", frc);
                return frc;
            }
            shell_printf("free: ok (tracker_count=%u)\r\n",
                         (unsigned)ga10b_gmmu_free_tracker_count());

            int free_verify_pass = 1;
            for (int p = 0; p < n_probes; p++) {
                uint32_t idx = probes[p];
                uint64_t va_i = va_base + (uint64_t)idx * 4096ull;
                struct ga10b_gmmu_walk_result wr;
                ga10b_gmmu_walk(inst_phys, va_i, &wr);
                if (wr.status == GA10B_GMMU_WALK_PTE_INVALID) {
                    shell_printf("POST-FREE[%u] va=0x%lx: PTE_INVALID — OK\r\n",
                                 (unsigned)idx, (unsigned long)va_i);
                } else {
                    shell_printf("POST-FREE[%u] va=0x%lx: status=%d phys=0x%lx — FAIL\r\n",
                                 (unsigned)idx, (unsigned long)va_i,
                                 (int)wr.status, (unsigned long)wr.leaf_phys);
                    free_verify_pass = 0;
                }
            }

            shell_printf("VERIFY: alloc=%s free=%s\r\n",
                         verify_pass ? "PASS" : "FAIL",
                         free_verify_pass ? "PASS" : "FAIL");
            return (verify_pass && free_verify_pass) ? 0 : -1;
        }
        if (strcmp(argv[2], "alloc-page-synth") == 0) {
            /* Synthetic-handoff variant of alloc-page: build a fresh
             * inst block + PDB page in PMM, then run the writer
             * against that. Proves the writer composes correct
             * PDE/PTE entries that the walker can read back —
             * without depending on a real GPU channel handoff
             * (jetson-nano-1 has no gpu-mnist host helper, so no
             * handoff gets published pre-kexec).
             *
             * Real-channel validation needs `nvgpu inherit + channel`
             * on a Jetson with helpers staged. Tracked in #678 /
             * Milestone B follow-up. */
            void *inst = pmm_alloc_page();
            void *pdb_page = pmm_alloc_page();
            if (inst == NULL || pdb_page == NULL) {
                shell_puts("alloc-page-synth: PMM exhaustion\r\n");
                return -1;
            }
            /* Zero both pages so all PT entries are invalid. */
            for (int i = 0; i < 512; i++) {
                ((volatile uint64_t *)inst)[i] = 0;
                ((volatile uint64_t *)pdb_page)[i] = 0;
            }
            /* Write the PDB pointer into the inst block at byte
             * offset 512 (= word 128 = ram_in_page_dir_base_lo_w()).
             * Encoding: bits[2:1]=target sys_mem_coh(2), bit[3]=vol,
             * bits[31:12]=pdb_phys[31:12], hi word = pdb_phys[63:32]. */
            uint64_t inst_phys = (uint64_t)(uintptr_t)inst;
            uint64_t pdb_phys = (uint64_t)(uintptr_t)pdb_page;
            volatile uint32_t *iw = (volatile uint32_t *)inst;
            iw[128] = ((uint32_t)pdb_phys & 0xfffff000u) |
                      (2u << 1) /* target=sys_mem_coh */ |
                      (1u << 3) /* volatile */;
            iw[129] = (uint32_t)(pdb_phys >> 32);
            cache_clean_range(inst, 4096);
            cache_clean_range(pdb_page, 4096);

            shell_printf("alloc-page-synth: inst=0x%lx pdb=0x%lx\r\n",
                         (unsigned long)inst_phys, (unsigned long)pdb_phys);

            uint64_t gpu_va = 0, phys = 0;
            void *cpu_va = NULL;
            int rc = ga10b_gmmu_alloc_page(inst_phys, 0,
                                           &gpu_va, &cpu_va, &phys);
            if (rc < 0) {
                shell_printf("ga10b_gmmu_alloc_page failed: rc=%d\r\n", rc);
                return rc;
            }
            shell_printf("alloc-page-synth: gpu_va=0x%lx cpu_va=%p phys=0x%lx\r\n",
                         (unsigned long)gpu_va, cpu_va, (unsigned long)phys);

            volatile uint32_t *sentinel = (volatile uint32_t *)cpu_va;
            sentinel[0] = 0xDEADBEEFu;
            sentinel[1] = 0xCAFEBABEu;
            shell_printf("alloc-page-synth: wrote sentinel via cpu_va: "
                         "[0]=0x%08x [1]=0x%08x\r\n",
                         (unsigned)sentinel[0], (unsigned)sentinel[1]);

            struct ga10b_gmmu_walk_result wr;
            int wrc = ga10b_gmmu_walk(inst_phys, gpu_va, &wr);
            if (wrc != 0) {
                shell_printf("alloc-page-synth: walker rc=%d\r\n", wrc);
                return wrc;
            }
            ga10b_gmmu_walk_print(&wr);
            if (wr.status == GA10B_GMMU_WALK_OK && wr.leaf_phys == phys) {
                shell_printf("VERIFY: walker leaf_phys 0x%lx == "
                             "alloc'd phys 0x%lx — PASS\r\n",
                             (unsigned long)wr.leaf_phys,
                             (unsigned long)phys);
                return 0;
            }
            shell_printf("VERIFY: walker leaf_phys 0x%lx != "
                         "alloc'd phys 0x%lx — FAIL\r\n",
                         (unsigned long)wr.leaf_phys,
                         (unsigned long)phys);
            return -1;
        }
        if (strcmp(argv[2], "alloc-page") == 0) {
            /* #666 Milestone B: allocate a single 4 KB page in the
             * inherited channel's GMMU address space, write a
             * sentinel pattern via the kernel-VA alias, then re-walk
             * the GPU VA via Milestone A's walker and confirm the
             * leaf PTE points at our new page. CPU-side smoke test
             * — proves the writer composed correct PDE/PTE entries
             * that the walker can read back. Real GPU verification
             * (a kernel that reads from gpu_va) is deferred. */
            const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
            if (h == NULL) {
                shell_puts("alloc-page: no handoff loaded — run `nvgpu inherit` "
                           "+ `nvgpu channel` first\r\n");
                return -1;
            }
            uint64_t inst_phys2 = h->inst_block_phys;
            if (inst_phys2 == 0) {
                inst_phys2 = ga10b_gmmu_discover_inst_block_phys();
                if (inst_phys2 == 0) {
                    shell_puts("alloc-page: handoff inst_block_phys=0 "
                               "and FECS_CURRENT_CTX read failed\r\n");
                    return -1;
                }
                shell_printf("alloc-page: using FECS_CURRENT_CTX inst=0x%lx\r\n",
                             (unsigned long)inst_phys2);
            }
            uint64_t gpu_va = 0, phys = 0;
            void *cpu_va = NULL;
            int rc = ga10b_gmmu_alloc_page(inst_phys2, 0,
                                           &gpu_va, &cpu_va, &phys);
            if (rc < 0) {
                shell_printf("ga10b_gmmu_alloc_page failed: rc=%d\r\n", rc);
                return rc;
            }
            shell_printf("alloc-page: gpu_va=0x%lx cpu_va=%p phys=0x%lx\r\n",
                         (unsigned long)gpu_va, cpu_va, (unsigned long)phys);
            /* Write sentinel via cpu_va to prove the page is writable. */
            volatile uint32_t *sentinel = (volatile uint32_t *)cpu_va;
            sentinel[0] = 0xDEADBEEFu;
            sentinel[1] = 0xCAFEBABEu;
            shell_printf("alloc-page: wrote sentinel via cpu_va: "
                         "[0]=0x%08x [1]=0x%08x\r\n",
                         (unsigned)sentinel[0], (unsigned)sentinel[1]);
            /* Re-walk the GPU VA — leaf phys must equal phys we got. */
            struct ga10b_gmmu_walk_result wr;
            int wrc = ga10b_gmmu_walk(inst_phys2, gpu_va, &wr);
            if (wrc != 0) {
                shell_printf("alloc-page: walker rc=%d\r\n", wrc);
                return wrc;
            }
            ga10b_gmmu_walk_print(&wr);
            if (wr.status == GA10B_GMMU_WALK_OK && wr.leaf_phys == phys) {
                shell_printf("VERIFY: walker leaf_phys 0x%lx == "
                             "alloc'd phys 0x%lx — PASS\r\n",
                             (unsigned long)wr.leaf_phys,
                             (unsigned long)phys);
                return 0;
            }
            shell_printf("VERIFY: walker leaf_phys 0x%lx != "
                         "alloc'd phys 0x%lx — FAIL\r\n",
                         (unsigned long)wr.leaf_phys,
                         (unsigned long)phys);
            return -1;
        }
        if (strcmp(argv[2], "walk-raw") == 0) {
            if (argc < 5) {
                shell_puts("usage: nvgpu gmmu walk-raw <inst_phys_hex> <hex_va>\r\n");
                return -1;
            }
            uint64_t inst = 0, va = 0;
            for (int a = 0; a < 2; a++) {
                const char *s = argv[3 + a];
                if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    s += 2;
                }
                uint64_t v = 0;
                while (*s) {
                    uint64_t d;
                    if (*s >= '0' && *s <= '9') {
                        d = *s - '0';
                    } else if (*s >= 'a' && *s <= 'f') {
                        d = 10 + (*s - 'a');
                    } else if (*s >= 'A' && *s <= 'F') {
                        d = 10 + (*s - 'A');
                    } else {
                        shell_puts("bad hex\r\n");
                        return -1;
                    }
                    v = (v << 4) | d;
                    s++;
                }
                if (a == 0) {
                    inst = v;
                } else {
                    va = v;
                }
            }
            struct ga10b_gmmu_walk_result wr;
            int rc = ga10b_gmmu_walk(inst, va, &wr);
            if (rc != 0) {
                shell_printf("ga10b_gmmu_walk failed: rc=%d\r\n", rc);
                return rc;
            }
            ga10b_gmmu_walk_print(&wr);
            return 0;
        }
        const struct ga10b_channel_handoff *h = ga10b_bringup_handoff();
        if (h == NULL) {
            shell_puts("gmmu: no handoff loaded — run `nvgpu inherit` "
                       "+ `nvgpu channel` first (or use `walk-raw`)\r\n");
            return -1;
        }
        if (strcmp(argv[2], "pushbuf") == 0) {
            uint64_t inst_phys = h->inst_block_phys;
            if (inst_phys == 0) {
                inst_phys = ga10b_gmmu_discover_inst_block_phys();
                if (inst_phys == 0) {
                    shell_puts("gmmu pushbuf: handoff inst_block_phys=0 "
                               "and FECS_CURRENT_CTX read failed\r\n");
                    return -1;
                }
                shell_printf("gmmu pushbuf: handoff inst_block_phys=0; "
                             "discovered via FECS_CURRENT_CTX = 0x%lx\r\n",
                             (unsigned long)inst_phys);
            }
            struct ga10b_gmmu_walk_result wr;
            int rc = ga10b_gmmu_walk(inst_phys, h->pushbuf_gpu_va, &wr);
            if (rc != 0) {
                shell_printf("ga10b_gmmu_walk failed: rc=%d\r\n", rc);
                return rc;
            }
            ga10b_gmmu_walk_print(&wr);
            if (wr.status == GA10B_GMMU_WALK_OK) {
                if (wr.leaf_phys == h->pushbuf_phys) {
                    shell_printf("VERIFY: leaf phys 0x%lx == "
                                 "g_handoff.pushbuf_phys 0x%lx — PASS\r\n",
                                 (unsigned long)wr.leaf_phys,
                                 (unsigned long)h->pushbuf_phys);
                    return 0;
                }
                shell_printf("VERIFY: leaf phys 0x%lx != "
                             "g_handoff.pushbuf_phys 0x%lx — FAIL\r\n",
                             (unsigned long)wr.leaf_phys,
                             (unsigned long)h->pushbuf_phys);
                return -1;
            }
            return -1;
        }
        if (strcmp(argv[2], "walk") == 0) {
            if (argc < 4) {
                shell_puts("usage: nvgpu gmmu walk <hex_va>\r\n");
                return -1;
            }
            const char *s = argv[3];
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                s += 2;
            }
            uint64_t va = 0;
            while (*s) {
                uint64_t d;
                if (*s >= '0' && *s <= '9') {
                    d = *s - '0';
                } else if (*s >= 'a' && *s <= 'f') {
                    d = 10 + (*s - 'a');
                } else if (*s >= 'A' && *s <= 'F') {
                    d = 10 + (*s - 'A');
                } else {
                    shell_puts("bad hex VA\r\n");
                    return -1;
                }
                va = (va << 4) | d;
                s++;
            }
            uint64_t inst_phys3 = h->inst_block_phys;
            if (inst_phys3 == 0) {
                inst_phys3 = ga10b_gmmu_discover_inst_block_phys();
                if (inst_phys3 == 0) {
                    shell_puts("walk: handoff inst_block_phys=0 "
                               "and FECS_CURRENT_CTX read failed\r\n");
                    return -1;
                }
            }
            struct ga10b_gmmu_walk_result wr;
            int rc = ga10b_gmmu_walk(inst_phys3, va, &wr);
            if (rc != 0) {
                shell_printf("ga10b_gmmu_walk failed: rc=%d\r\n", rc);
                return rc;
            }
            ga10b_gmmu_walk_print(&wr);
            return (wr.status == GA10B_GMMU_WALK_OK) ? 0 : -1;
        }
        shell_puts("usage: nvgpu gmmu <pushbuf | walk <hex_va>>\r\n");
        return -1;
    }

    shell_puts("usage: nvgpu [info | prepare | inherit | acr | test | "
              "channel | engine-status | engine-clear | "
              "submit | submit-compute | launch-kernel | "
              "run-mnist | fecs | gpccs | pmu | run | "
              "gmmu <pushbuf | walk | walk-raw | "
              "alloc-page | alloc-page-synth | "
              "alloc-multi-synth | reuse-synth> | "
              "oplib]\r\n");
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
#ifdef CONFIG_AI_SCHEDULER
static const char *sched_model_state_name(uint16_t state)
{
    switch (state) {
        case SCHED_MODEL_EMPTY: return "empty";
        case SCHED_MODEL_STAGED: return "staged";
        case SCHED_MODEL_ACTIVE: return "active";
        case SCHED_MODEL_ROLLED_BACK: return "rolled_back";
        default: return "unknown";
    }
}

static uint16_t sched_model_kind_id(const char *name)
{
    if (strcmp(name, "mlp") == 0) return SCHED_MODEL_KIND_MLP;
    if (strcmp(name, "ppo") == 0) return SCHED_MODEL_KIND_PPO;
    if (strcmp(name, "config") == 0) return SCHED_MODEL_KIND_CONFIG;
    if (strcmp(name, "thresholds") == 0) return SCHED_MODEL_KIND_THRESHOLDS;
    if (strcmp(name, "rebalance") == 0) return SCHED_MODEL_KIND_REBALANCE;
    return 0;
}

static const char *sched_model_kind_name(uint16_t kind_id)
{
    switch (kind_id) {
        case SCHED_MODEL_KIND_MLP: return "mlp";
        case SCHED_MODEL_KIND_PPO: return "ppo";
        case SCHED_MODEL_KIND_CONFIG: return "config";
        case SCHED_MODEL_KIND_THRESHOLDS: return "thresholds";
        case SCHED_MODEL_KIND_REBALANCE: return "rebalance";
        default: return "unknown";
    }
}

static void sched_print_model_meta(const char *label,
                                   uint32_t present,
                                   const struct sched_model_meta *meta)
{
    if (!present || !meta) {
        shell_printf("    %-8s %s\r\n", label, "(none)");
        return;
    }

    shell_printf("    %-8s version=%u schema=%u features=%u actions=%u count=%u payload=%lu checksum=0x%08lx\r\n",
                 label,
                 (unsigned)meta->version,
                 (unsigned)meta->schema_version,
                 (unsigned)meta->feature_version,
                 (unsigned)meta->action_version,
                 (unsigned)meta->action_count,
                 (unsigned long)meta->payload_len,
                 (unsigned long)meta->checksum);
}

static void sched_print_balance_config(const char *label,
                                       const struct sched_runtime_balance_config *cfg)
{
    if (!cfg) return;

    shell_printf("    %-8s enabled=%lu min_target_ready=%lu min_active_cpus=%lu imbalance=%lu/%lu\r\n",
                 label,
                 (unsigned long)cfg->enabled,
                 (unsigned long)cfg->min_target_ready,
                 (unsigned long)cfg->min_active_cpus,
                 (unsigned long)cfg->imbalance_num,
                 (unsigned long)cfg->imbalance_den);
}

static void sched_print_deadline_thresholds(
    const char *label,
    const struct sched_runtime_deadline_thresholds *cfg)
{
    if (!cfg) return;

    shell_printf("    %-8s critical_ns=%llu high_ns=%llu boost_ns=%llu\r\n",
                 label,
                 (unsigned long long)cfg->critical_ns,
                 (unsigned long long)cfg->high_ns,
                 (unsigned long long)cfg->boost_ns);
}

static void sched_print_rebalance_config(
    const char *label,
    const struct sched_runtime_rebalance_config *cfg)
{
    if (!cfg) return;

    shell_printf("    %-8s enabled=%lu interval_ticks=%lu imbalance_min=%lu\r\n",
                 label,
                 (unsigned long)cfg->enabled,
                 (unsigned long)cfg->interval_ticks,
                 (unsigned long)cfg->imbalance_min);
}

static int sched_model_status_one(uint16_t kind_id)
{
    struct sched_model_status status = {0};
    struct sched_runtime_balance_config cfg = {0};
    struct sched_runtime_deadline_thresholds thresholds = {0};
    struct sched_runtime_rebalance_config rebalance = {0};
    int have_cfg = 0;
    int have_thresholds = 0;
    int have_rebalance = 0;

    if (sched_model_status(kind_id, &status) != 0) {
        shell_printf("sched model status: invalid kind %u\r\n", (unsigned)kind_id);
        return 1;
    }

    if (kind_id == SCHED_MODEL_KIND_CONFIG &&
        sched_runtime_balance_config_snapshot(&cfg) == 0) {
        have_cfg = 1;
    }
    if (kind_id == SCHED_MODEL_KIND_THRESHOLDS &&
        sched_runtime_deadline_thresholds_snapshot(&thresholds) == 0) {
        have_thresholds = 1;
    }
    if (kind_id == SCHED_MODEL_KIND_REBALANCE &&
        sched_runtime_rebalance_config_snapshot(&rebalance) == 0) {
        have_rebalance = 1;
    }

    shell_printf("  %s: %s\r\n",
                 sched_model_kind_name(kind_id),
                 sched_model_state_name(status.state));
    sched_print_model_meta("staged", status.has_staged, &status.staged);
    sched_print_model_meta("active", status.has_active, &status.active);
    sched_print_model_meta("rollback", status.has_rollback, &status.rollback);
    if (have_cfg) {
        sched_print_balance_config("config", &cfg);
    }
    if (have_thresholds) {
        sched_print_deadline_thresholds("thresholds", &thresholds);
    }
    if (have_rebalance) {
        sched_print_rebalance_config("rebalance", &rebalance);
    }
    return 0;
}

static int sched_model_load_file(uint16_t kind_id, const char *path)
{
    char resolved[VFS_MAX_PATH];
    int rc = sched_blob_stage_file(kind_id, path, resolved, sizeof(resolved));

    if (rc == RUNTIME_BLOB_FILE_PATH_TOO_LONG) {
        shell_puts("sched model load: path too long\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_FOUND) {
        shell_printf("sched model load: %s: file not found\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_A_FILE) {
        shell_printf("sched model load: %s: not a file\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_EMPTY) {
        shell_puts("sched model load: file is empty\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOMEM) {
        shell_puts("sched model load: out of memory for read buffer\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_READ_FAILED) {
        shell_printf("sched model load: failed to read %s\r\n", path);
        return 1;
    }
    if (rc != RUNTIME_BLOB_FILE_OK) {
        shell_printf("sched model load: failed to stage %s\r\n", path);
        return 1;
    }
    shell_printf("Staged %s scheduler runtime blob from %s\r\n",
                 sched_model_kind_name(kind_id), resolved);
    return 0;
}

static int sched_model_autoload_set_file(const char *kind, const char *path)
{
    char resolved[VFS_MAX_PATH];
    int rc = blob_autoload_set("sched", kind, path);

    if (rc == RUNTIME_BLOB_FILE_PATH_TOO_LONG) {
        shell_puts("sched model autoload: path too long\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_FOUND) {
        shell_printf("sched model autoload: %s: file not found\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_A_FILE) {
        shell_printf("sched model autoload: %s: not a file\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_EMPTY) {
        shell_puts("sched model autoload: file is empty\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOMEM) {
        shell_puts("sched model autoload: out of memory for read buffer\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_READ_FAILED) {
        shell_printf("sched model autoload: failed to read %s\r\n", path);
        return 1;
    }
    if (rc != 0 || blob_autoload_get("sched", kind, resolved, sizeof(resolved)) != 0) {
        shell_printf("sched model autoload: invalid blob for %s\r\n", kind);
        return 1;
    }

    shell_printf("Set scheduler autoload %s -> %s\r\n", kind, resolved);
    return 0;
}

static int sched_model_autoload_cmd(int argc, char *argv[])
{
    static const uint16_t kinds[] = {
        SCHED_MODEL_KIND_MLP, SCHED_MODEL_KIND_PPO,
        SCHED_MODEL_KIND_CONFIG, SCHED_MODEL_KIND_THRESHOLDS,
        SCHED_MODEL_KIND_REBALANCE
    };

    if (argc < 4 || strcmp(argv[3], "status") == 0) {
        shell_puts("Scheduler blob autoload:\r\n");
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            struct blob_autoload_info info;
            const char *kind = sched_model_kind_name(kinds[i]);
            if (blob_autoload_info_get("sched", kind, &info) == 0) {
                shell_printf("  %s -> %s size=%u checksum=0x%08x\r\n",
                             kind, info.path, info.size_bytes, info.checksum);
            } else {
                shell_printf("  %s -> (none)\r\n", kind);
            }
        }
        if (argc < 4) {
            shell_puts("\r\nUsage:\r\n");
            shell_puts("  sched model autoload status\r\n");
            shell_puts("  sched model autoload set <kind> <path>\r\n");
            shell_puts("  sched model autoload clear <kind>\r\n");
        }
        return 0;
    }

    if (strcmp(argv[3], "set") == 0) {
        if (argc < 6) {
            shell_puts("Usage: sched model autoload set <kind> <path>\r\n");
            return 1;
        }
        if (sched_model_kind_id(argv[4]) == 0) {
            shell_printf("Unknown scheduler model kind: '%s'\r\n", argv[4]);
            return 1;
        }
        return sched_model_autoload_set_file(argv[4], argv[5]);
    }

    if (strcmp(argv[3], "clear") == 0) {
        if (argc < 5) {
            shell_puts("Usage: sched model autoload clear <kind>\r\n");
            return 1;
        }
        if (sched_model_kind_id(argv[4]) == 0) {
            shell_printf("Unknown scheduler model kind: '%s'\r\n", argv[4]);
            return 1;
        }
        if (blob_autoload_clear("sched", argv[4]) != 0) {
            shell_printf("sched model autoload: failed to clear %s\r\n", argv[4]);
            return 1;
        }
        shell_printf("Cleared scheduler autoload for %s\r\n", argv[4]);
        return 0;
    }

    shell_puts("Usage: sched model autoload <status|set|clear> ...\r\n");
    return 1;
}
#endif

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

#ifdef CONFIG_AI_SCHEDULER
    if (strcmp(argv[1], "model") == 0) {
        if (argc < 3 || strcmp(argv[2], "status") == 0) {
            shell_puts("Scheduler runtime blobs:\r\n");
            if (sched_model_status_one(SCHED_MODEL_KIND_MLP) != 0) return 1;
            if (sched_model_status_one(SCHED_MODEL_KIND_PPO) != 0) return 1;
            if (sched_model_status_one(SCHED_MODEL_KIND_CONFIG) != 0) return 1;
            if (sched_model_status_one(SCHED_MODEL_KIND_THRESHOLDS) != 0) return 1;
            if (sched_model_status_one(SCHED_MODEL_KIND_REBALANCE) != 0) return 1;
            if (argc < 3) {
                shell_puts("\r\nUsage:\r\n");
                shell_puts("  sched model status\r\n");
                shell_puts("  sched model load <kind> <path>\r\n");
                shell_puts("  sched model activate <kind>\r\n");
                shell_puts("  sched model rollback <kind>\r\n");
                shell_puts("  sched model clear <kind>\r\n");
            }
            return 0;
        }

        if (strcmp(argv[2], "autoload") == 0) {
            return sched_model_autoload_cmd(argc, argv);
        }

        if (argc < 4) {
            shell_puts("Usage: sched model <load|activate|rollback|clear|autoload> <kind> [path]\r\n");
            return 1;
        }

        uint16_t kind_id = sched_model_kind_id(argv[3]);
        if (kind_id == 0) {
            shell_printf("Unknown scheduler model kind: '%s'\r\n", argv[3]);
            return 1;
        }

        if (strcmp(argv[2], "load") == 0) {
            if (argc < 5) {
                shell_puts("Usage: sched model load <kind> <path>\r\n");
                return 1;
            }
            return sched_model_load_file(kind_id, argv[4]);
        }

        if (strcmp(argv[2], "activate") == 0) {
            if (sched_model_activate(kind_id) != 0) {
                shell_printf("sched model activate: no staged blob for %s\r\n", argv[3]);
                return 1;
            }
            shell_printf("Activated runtime blob for scheduler %s\r\n", argv[3]);
            return 0;
        }

        if (strcmp(argv[2], "rollback") == 0) {
            if (sched_model_rollback(kind_id) != 0) {
                shell_printf("sched model rollback: no rollback blob for %s\r\n", argv[3]);
                return 1;
            }
            shell_printf("Rolled back runtime blob for scheduler %s\r\n", argv[3]);
            return 0;
        }

        if (strcmp(argv[2], "clear") == 0) {
            if (sched_model_clear(kind_id) != 0) {
                shell_printf("sched model clear: failed for %s\r\n", argv[3]);
                return 1;
            }
            shell_printf("Cleared runtime blob state for scheduler %s\r\n", argv[3]);
            return 0;
        }

        shell_puts("Usage: sched model <status|load|activate|rollback|clear> ...\r\n");
        return 1;
    }
#endif

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
 * eviction — eviction policy inspection and control (Phase AI-Eviction M7)
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
        shell_puts("Eviction: disabled (rebuild without DISABLE_EVICTION=ON)\r\n");
        return;
    }
    shell_puts("Eviction:\r\n");
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

static int eviction_blob_kind_id(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, "xgboost") == 0) return 1;
    if (strcmp(name, "mlp") == 0) return 2;
    if (strcmp(name, "cacheus_config") == 0) return 3;
    return 0;
}

static const char *eviction_blob_kind_name(uint16_t kind_id)
{
    switch (kind_id) {
        case 1: return "xgboost";
        case 2: return "mlp";
        case 3: return "cacheus_config";
        default: return "unknown";
    }
}

static const char *eviction_blob_state_name(uint16_t state)
{
    switch (state) {
        case 0: return "empty";
        case 1: return "staged";
        case 2: return "active";
        case 3: return "rolled_back";
        default: return "unknown";
    }
}

static void eviction_print_blob_meta(const char *label,
                                     uint32_t present,
                                     const RustEvictionBlobMeta *meta)
{
    if (!present || !meta) {
        shell_printf("    %-8s %s\r\n", label, "(none)");
        return;
    }
    shell_printf("    %-8s version=%u schema=%u payload=%lu checksum=0x%08lx\r\n",
                label,
                (unsigned)meta->version,
                (unsigned)meta->feature_schema_version,
                (unsigned long)meta->payload_len,
                (unsigned long)meta->checksum);
}

static int eviction_model_status_one(uint16_t kind_id)
{
    RustEvictionBlobStatus status = {0};
    int rc = rust_eviction_blob_status(kind_id, &status);
    if (rc == -2) {
        shell_puts("Eviction disabled — rebuild without DISABLE_EVICTION=ON\r\n");
        return 1;
    }
    if (rc != 0) {
        shell_printf("eviction model status: invalid kind %u\r\n", (unsigned)kind_id);
        return 1;
    }

    shell_printf("  %s: %s\r\n",
                eviction_blob_kind_name(kind_id),
                eviction_blob_state_name(status.state));
    eviction_print_blob_meta("staged", status.has_staged, &status.staged);
    eviction_print_blob_meta("active", status.has_active, &status.active);
    eviction_print_blob_meta("rollback", status.has_rollback, &status.rollback);
    return 0;
}

static int eviction_model_load_file(uint16_t kind_id, const char *path)
{
    char resolved[VFS_MAX_PATH];
    int rc = eviction_blob_stage_file(kind_id, path, resolved, sizeof(resolved));

    if (rc == RUNTIME_BLOB_FILE_PATH_TOO_LONG) {
        shell_puts("eviction model load: path too long\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_FOUND) {
        shell_printf("eviction model load: %s: file not found\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_A_FILE) {
        shell_printf("eviction model load: %s: not a file\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_EMPTY) {
        shell_puts("eviction model load: file is empty\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOMEM) {
        shell_puts("eviction model load: out of memory for read buffer\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_READ_FAILED) {
        shell_printf("eviction model load: failed to read %s\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_KIND_MISMATCH) {
        shell_printf("eviction model load: blob kind mismatch for %s\r\n",
                    eviction_blob_kind_name(kind_id));
        return 1;
    }
    if (rc != RUNTIME_BLOB_FILE_OK) {
        shell_printf("eviction model load: failed to stage %s\r\n", path);
        return 1;
    }

    shell_printf("Staged %s runtime blob from %s\r\n",
                eviction_blob_kind_name(kind_id), resolved);
    return 0;
}

static int eviction_model_autoload_set_file(const char *kind, const char *path)
{
    char resolved[VFS_MAX_PATH];
    int rc = blob_autoload_set("eviction", kind, path);

    if (rc == RUNTIME_BLOB_FILE_PATH_TOO_LONG) {
        shell_puts("eviction model autoload: path too long\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_FOUND) {
        shell_printf("eviction model autoload: %s: file not found\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOT_A_FILE) {
        shell_printf("eviction model autoload: %s: not a file\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_EMPTY) {
        shell_puts("eviction model autoload: file is empty\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_NOMEM) {
        shell_puts("eviction model autoload: out of memory for read buffer\r\n");
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_READ_FAILED) {
        shell_printf("eviction model autoload: failed to read %s\r\n", path);
        return 1;
    }
    if (rc == RUNTIME_BLOB_FILE_KIND_MISMATCH) {
        shell_printf("eviction model autoload: blob kind mismatch for %s\r\n", kind);
        return 1;
    }
    if (rc != 0 || blob_autoload_get("eviction", kind, resolved, sizeof(resolved)) != 0) {
        shell_printf("eviction model autoload: invalid blob for %s\r\n", kind);
        return 1;
    }

    shell_printf("Set eviction autoload %s -> %s\r\n", kind, resolved);
    return 0;
}

static int eviction_model_autoload_cmd(int argc, char *argv[])
{
    static const char *kinds[] = {"xgboost", "mlp", "cacheus_config"};

    if (argc < 4 || strcmp(argv[3], "status") == 0) {
        shell_puts("Eviction blob autoload:\r\n");
        for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
            struct blob_autoload_info info;
            if (blob_autoload_info_get("eviction", kinds[i], &info) == 0) {
                shell_printf("  %s -> %s size=%u checksum=0x%08x\r\n",
                             kinds[i], info.path, info.size_bytes, info.checksum);
            } else {
                shell_printf("  %s -> (none)\r\n", kinds[i]);
            }
        }
        if (argc < 4) {
            shell_puts("\r\nUsage:\r\n");
            shell_puts("  eviction model autoload status\r\n");
            shell_puts("  eviction model autoload set <kind> <path>\r\n");
            shell_puts("  eviction model autoload clear <kind>\r\n");
        }
        return 0;
    }

    if (strcmp(argv[3], "set") == 0) {
        if (argc < 6) {
            shell_puts("Usage: eviction model autoload set <kind> <path>\r\n");
            return 1;
        }
        if (eviction_blob_kind_id(argv[4]) == 0) {
            shell_printf("Unknown eviction model kind: '%s'\r\n", argv[4]);
            return 1;
        }
        return eviction_model_autoload_set_file(argv[4], argv[5]);
    }

    if (strcmp(argv[3], "clear") == 0) {
        if (argc < 5) {
            shell_puts("Usage: eviction model autoload clear <kind>\r\n");
            return 1;
        }
        if (eviction_blob_kind_id(argv[4]) == 0) {
            shell_printf("Unknown eviction model kind: '%s'\r\n", argv[4]);
            return 1;
        }
        if (blob_autoload_clear("eviction", argv[4]) != 0) {
            shell_printf("eviction model autoload: failed to clear %s\r\n", argv[4]);
            return 1;
        }
        shell_printf("Cleared eviction autoload for %s\r\n", argv[4]);
        return 0;
    }

    shell_puts("Usage: eviction model autoload <status|set|clear> ...\r\n");
    return 1;
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
                shell_puts("Eviction disabled — rebuild without DISABLE_EVICTION=ON\r\n");
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
            shell_puts("Eviction disabled — rebuild without DISABLE_EVICTION=ON\r\n");
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

    if (strcmp(argv[1], "model") == 0) {
        if (argc < 3 || strcmp(argv[2], "status") == 0) {
            shell_puts("Eviction runtime blobs:\r\n");
            if (eviction_model_status_one(1) != 0) return 1;
            if (eviction_model_status_one(2) != 0) return 1;
            if (eviction_model_status_one(3) != 0) return 1;
            if (argc < 3) {
                shell_puts("\r\nUsage:\r\n");
                shell_puts("  eviction model status\r\n");
                shell_puts("  eviction model load <kind> <path>\r\n");
                shell_puts("  eviction model activate <kind>\r\n");
                shell_puts("  eviction model rollback <kind>\r\n");
                shell_puts("  eviction model clear <kind>\r\n");
            }
            return 0;
        }

        if (strcmp(argv[2], "autoload") == 0) {
            return eviction_model_autoload_cmd(argc, argv);
        }

        if (argc < 4) {
            shell_puts("Usage: eviction model <load|activate|rollback|clear|autoload> <kind> [path]\r\n");
            return 1;
        }

        int kind_id = eviction_blob_kind_id(argv[3]);
        if (kind_id == 0) {
            shell_printf("Unknown eviction model kind: '%s'\r\n", argv[3]);
            return 1;
        }

        if (strcmp(argv[2], "load") == 0) {
            if (argc < 5) {
                shell_puts("Usage: eviction model load <kind> <path>\r\n");
                return 1;
            }
            return eviction_model_load_file((uint16_t)kind_id, argv[4]);
        }

        if (strcmp(argv[2], "activate") == 0) {
            int rc = rust_eviction_blob_activate((uint16_t)kind_id);
            if (rc == -2) {
                shell_puts("Eviction disabled — rebuild without DISABLE_EVICTION=ON\r\n");
                return 1;
            }
            if (rc == -3) {
                shell_printf("eviction model activate: no staged blob for %s\r\n", argv[3]);
                return 1;
            }
            if (rc != 0) {
                shell_printf("eviction model activate: failed for %s\r\n", argv[3]);
                return 1;
            }
            shell_printf("Activated runtime blob for %s\r\n", argv[3]);
            return 0;
        }

        if (strcmp(argv[2], "rollback") == 0) {
            int rc = rust_eviction_blob_rollback((uint16_t)kind_id);
            if (rc == -2) {
                shell_puts("Eviction disabled — rebuild without DISABLE_EVICTION=ON\r\n");
                return 1;
            }
            if (rc == -3) {
                shell_printf("eviction model rollback: no rollback blob for %s\r\n", argv[3]);
                return 1;
            }
            if (rc != 0) {
                shell_printf("eviction model rollback: failed for %s\r\n", argv[3]);
                return 1;
            }
            shell_printf("Rolled back runtime blob for %s\r\n", argv[3]);
            return 0;
        }

        if (strcmp(argv[2], "clear") == 0) {
            int rc = rust_eviction_blob_clear((uint16_t)kind_id);
            if (rc == -2) {
                shell_puts("Eviction disabled — rebuild without DISABLE_EVICTION=ON\r\n");
                return 1;
            }
            if (rc != 0) {
                shell_printf("eviction model clear: failed for %s\r\n", argv[3]);
                return 1;
            }
            shell_printf("Cleared runtime blob state for %s\r\n", argv[3]);
            return 0;
        }

        shell_puts("Usage: eviction model <status|load|activate|rollback|clear> ...\r\n");
        return 1;
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

#if defined(PLATFORM_RASPI5) && defined(PI5_IRQ_DIAG)
    /* Track B probes (#134 / #635). Independent diagnostics that
     * probe the GICv2 → A76 IRQ path on Pi 5. Each probe is opt-in
     * via a subcommand argument because they perturb live state
     * (DAIF, GICC_CTLR, timer) and shouldn't fire on every timdiag
     * invocation. Prototypes in timdiag_pi5_probes.h. */
    if (argc >= 2 && argv[1]) {
        if (strcmp(argv[1], "sgi") == 0) {
            timdiag_pi5_probe_sgi();
        } else if (strcmp(argv[1], "bypass") == 0) {
            timdiag_pi5_probe_bypass();
        } else if (strcmp(argv[1], "smc") == 0) {
            timdiag_pi5_probe_smc();
        }
    } else {
        shell_puts("\r\nPi 5 GICv2 probes (Track B of #134):\r\n"
                   "  timdiag sgi     — Test GIC→CPU IRQ path via self-SGI\r\n"
                   "  timdiag bypass  — Toggle GICC_CTLR bypass-disable bits, observe timer\r\n"
                   "  timdiag smc     — Time PSCI SMC round-trip (SCR_EL3.IRQ inference)\r\n");
    }
#endif
#endif

    shell_puts("\r\n=== End Diagnostic ===\r\n");
    return 0;
}

#if defined(PLATFORM_RASPI5) && defined(PI5_IRQ_DIAG)
#include "diag_pi5.h"
/*
 * irqtest — Track C Stage 2 / 2.5 diagnostic.
 *
 * Briefly unmasks DAIF.I and observes whether the IRQ vector counter
 * (`diag_vec_counts.irq` in NC memory) advances on the current CPU.
 *
 * Three outcomes:
 *
 *   irq counter advances:
 *     SCR_EL3 / GIC routing path WORKS. The kernel is blocking IRQs by
 *     holding DAIF.I=1 in tasks. Stage 2.5 (unmask DAIF.I in
 *     task_entry_trampoline + activate SECONDARY_PREEMPT) will
 *     activate hardware preemption.
 *
 *   fiq counter advances:
 *     PPI is still in Group 0. Group register write from EL3 didn't
 *     take effect — TF-A patch needs revisiting.
 *
 *   neither advances:
 *     SCR_EL3 / GIC routing patches not effective. Either our TF-A
 *     isn't loaded or another gate is in play.
 *
 * Brief = 100 µs. Short enough that even if the IRQ fires, the timer
 * handler doesn't have time to call schedule() (which would wedge on
 * Pi 5 without SECONDARY_PREEMPT). The vector entry's first instruction
 * (DIAG_BUMP_VEC in vectors.S) increments the counter, which is all
 * we need to observe.
 *
 * WARNING: without SECONDARY_PREEMPT this command WILL HANG Pi 5
 * hardware if any IRQ is currently pending — that's the diagnostic
 * (the hang IS proof that SCR_EL3 routing works) but operators who
 * run it without context will need to power-cycle.
 */
/* Direct UART write — bypass kernel UART driver. Used by irqtest's
 * unmask probe to localize hangs without depending on shell_printf
 * (which uses uart_lock + may itself wedge). Pi 5 RP1 PL011 DR is
 * at IRQTEST_UART_DR (0x1f00030000). The same address is reconstructed
 * via movz/movk in vectors.S:STAGE25_TRACE_CHAR — keep the two in sync
 * when the RP1 mapping changes. */
#define IRQTEST_UART_DR  0x1f00030000ULL
#define IRQTEST_UART_FR  0x1f00030018ULL  /* PL011 Flag Register */
#define IRQTEST_FR_TXFF  (1u << 5)        /* Transmit FIFO full */
#define IRQTEST_FR_TXFE  (1u << 7)        /* Transmit FIFO empty */
#define IRQTEST_FR_BUSY  (1u << 3)        /* UART busy transmitting */

/* Spin until FIFO is fully empty AND UART finished shifting the last
 * char on the wire. Bounded ~10 ms timeout. Used to make the next raw
 * write GUARANTEED to fit in the FIFO so we can pin down whether code
 * after daifclr executes or not. */
static inline void irqtest_fifo_drain(void)
{
    int t = 10000000;  /* ~6 ms at 1.5 GHz including MMIO latency */
    while (t-- > 0) {
        uint32_t fr = *(volatile uint32_t *)IRQTEST_UART_FR;
        if ((fr & IRQTEST_FR_TXFE) && !(fr & IRQTEST_FR_BUSY)) {
            break;
        }
        __asm__ volatile("" ::: "memory");
    }
}
static inline void irqtest_putc(char c)
{
    /* Wait for room in TX FIFO with a short timeout (so we cannot
     * deadlock if the UART is genuinely wedged). 100k cycles ≈ 67 µs
     * at 1.5 GHz — more than enough for a single FIFO slot to drain
     * at 115200 baud (~87 µs/char), but bounded so traces written
     * during the unmask probe can never block the test forever. */
    int timeout = 100000;
    while ((*(volatile uint32_t *)IRQTEST_UART_FR & IRQTEST_FR_TXFF) &&
           --timeout > 0) {
        __asm__ volatile("" ::: "memory");
    }
    *(volatile uint32_t *)IRQTEST_UART_DR = (uint32_t)(unsigned char)c;
}

/* Raw UART write — no FIFO check, no lock, no driver. Identical to
 * vectors.S:STAGE25_TRACE_CHAR. Drops the char silently if the FIFO
 * is full, but never blocks execution. Used for tight probe points
 * around `daifclr` where we need to know "did execution reach this
 * line" rather than "did the char definitely make it on the wire". */
#define IRQTEST_RAW_PUTC(ch) do {                                     \
    __asm__ volatile(                                                 \
        "movz x16, #0x0000\n\t"                                       \
        "movk x16, #0x0003, lsl #16\n\t"                              \
        "movk x16, #0x001f, lsl #32\n\t"                              \
        "mov  w17, %w0\n\t"                                           \
        "str  w17, [x16]\n\t"                                         \
        :: "r"((uint32_t)(unsigned char)(ch)) : "x16", "x17", "memory"); \
} while (0)
static inline void irqtest_puts(const char *s)
{
    while (*s) {
        irqtest_putc(*s++);
    }
}

/* Dump VBAR_EL1, DAIF, SCTLR_EL1, and the NS-visible GIC state for
 * the active timer PPI (TIMER_IRQ). Run before any DAIF manipulation
 * so the captured state reflects what the kernel sees right before
 * `daifclr` — useful for telling "vector base corrupt" or "PMR/RPR
 * blocks delivery" apart from "IRQ pin really should fire". */
static void irqtest_dump_pre_daifclr_state(void)
{
    uint64_t vbar_now, daif_pre, sctlr_now;
    __asm__ volatile("mrs %0, vbar_el1"  : "=r"(vbar_now));
    __asm__ volatile("mrs %0, daif"      : "=r"(daif_pre));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr_now));
    shell_printf("\r\n  pre-daifclr: VBAR_EL1=0x%lx DAIF=0x%lx SCTLR_EL1=0x%lx\r\n",
                 (unsigned long)vbar_now,
                 (unsigned long)daif_pre,
                 (unsigned long)sctlr_now);
    extern char exception_vectors[];
    shell_printf("  exception_vectors symbol = %p (must equal VBAR_EL1)\r\n",
                 (void *)exception_vectors);
    /* GIC delivery state — what NS-EL1 actually sees about the active
     * timer PPI (TIMER_IRQ).
     *   GICC_HPPIR (off 0x18): highest priority pending Group 1 NS
     *     IRQ visible to NS — returns 1023 (none), 1022 (top is
     *     Group 0/1-Secure, NS can't see), or the IRQ id.
     *   GICC_AHPPIR (off 0x28): aliased — secure HPPIR (NS reads 0
     *     unless GICC_CTLR.AckCtl steers it).
     *   GICD_ISACTIVER0 (0x300): which IRQs are currently ACTIVE.
     *   GICD_ITARGETSR7 (0x81C): IRQs 28..31 → CPU mask. PPI is
     *     banked, so this returns 0x01010101 if banked-as-CPU0.
     *   GICD_ICFGR1   (0xC04): trigger config for IRQs 16..31, 2
     *     bits per IRQ. For IRQ N (16..31), bit (N%16)*2+1 = 1 →
     *     edge-triggered, 0 → level-triggered. */
    uint32_t hppir   = *(volatile uint32_t *)(GIC_CPU_BASE  + 0x18);
    uint32_t ahppir  = *(volatile uint32_t *)(GIC_CPU_BASE  + 0x28);
    uint32_t rpr     = *(volatile uint32_t *)(GIC_CPU_BASE  + 0x14);
    uint32_t pmr     = *(volatile uint32_t *)(GIC_CPU_BASE  + 0x04);
    uint32_t bpr     = *(volatile uint32_t *)(GIC_CPU_BASE  + 0x08);
    uint32_t isacti0 = *(volatile uint32_t *)(GIC_DIST_BASE + 0x300);
    uint32_t itarg7  = *(volatile uint32_t *)(GIC_DIST_BASE + 0x81C);
    uint32_t icfgr1  = *(volatile uint32_t *)(GIC_DIST_BASE + 0xC04);
    /* IPRIORITYR for IRQ N = GICD_BASE + 0x400 + N (per-byte offset) */
    uint8_t  ipriby  = *(volatile uint8_t  *)(GIC_DIST_BASE + 0x400 + TIMER_IRQ);
    unsigned trig_bit = ((TIMER_IRQ % 16u) * 2u) + 1u;
    shell_printf("  GIC NS:  HPPIR=0x%x  AHPPIR=0x%x  RPR=0x%x  PMR=0x%x  BPR=0x%x\r\n",
                 hppir, ahppir, rpr, pmr, bpr);
    shell_printf("           ISACTIVER0=0x%x  ITARGETSR7=0x%x  ICFGR1=0x%x  IPRI[%u]=0x%02x\r\n",
                 isacti0, itarg7, icfgr1, (unsigned)TIMER_IRQ, ipriby);
    shell_printf("  HPPIR decode: %s\r\n",
                 (hppir & 0x3FF) == 0x3FF ? "1023 (no Grp1NS pending)" :
                 (hppir & 0x3FF) == 0x3FE ? "1022 (top pending is Grp0/Grp1S — NS cannot see)" :
                 "actual IRQ id (Grp1NS pending — should deliver)");
    shell_printf("  Delivery gate: pri(0x%02x) < (RPR(0x%x) & PMR(0x%x))? %s\r\n",
                 ipriby, rpr, pmr,
                 ipriby < (rpr < pmr ? rpr : pmr) ? "yes — should deliver"
                                                   : "NO — RPR/PMR blocks delivery");
    shell_printf("  ICFGR1 decode: PPI %u trigger = %s\r\n",
                 (unsigned)TIMER_IRQ,
                 (icfgr1 & (1u << trig_bit)) ? "edge" : "level");
}

/* Undo per-mode side effects so the probes don't poison kernel state.
 * mode_iar deliberately ack'd without EOI (active-priority bit stays
 * raised, future IRQs at <= that priority are silently masked).
 * mode_linit changed GICC_CTLR (set EOImodeNS, which changes
 * kernel-wide EOI semantics from single-write-EOIR to DIR-after-EOI).
 * Both must be undone before returning to the shell. */
static void irqtest_cleanup_state(uint32_t iar_saved_id, bool iar_pending_eoi,
                                  uint32_t linit_saved_ctlr, bool linit_changed_ctlr)
{
    if (iar_pending_eoi) {
        *(volatile uint32_t *)(GIC_CPU_BASE + 0x10) = iar_saved_id;
        __asm__ volatile("dsb sy" ::: "memory");
        irqtest_puts(" IAR-EOI");
    }
    if (linit_changed_ctlr) {
        *(volatile uint32_t *)(GIC_CPU_BASE + 0x00) = linit_saved_ctlr;
        __asm__ volatile("dsb sy" ::: "memory");
        irqtest_puts(" CTLR-restored");
    }
}

int cmd_irqtest(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /*
     * SCR_EL3 sentinel readout (#134 Stage 2.5+). Always runs first,
     * before any DAIF manipulation, so it's safe under every path
     * (`irqtest`, `irqtest noirq`, `irqtest fiq`). Tells the operator:
     *   - Did our TF-A `setup_ns_context` PLAT_RPI5 patch run?
     *   - What scr_el3 value did it write to the NS-context buffer?
     *   - Did `cm_prepare_el3_exit_ns` see the same value just before
     *     the assembly `el3_exit` loaded it?
     *
     * If sentinel magic words are missing, the patched TF-A isn't
     * loaded (deploy regression — armstub8-2712.bin is stock or
     * absent). If sentinel A's value has bits 1/2 set, our clear
     * isn't taking effect. If B differs from A, an override path
     * runs between them.
     */
    {
        uint32_t a_magic = *(volatile uint32_t *)DIAG_SCR_SENTINEL_A_MAGIC;
        uint64_t a_val   = *(volatile uint64_t *)DIAG_SCR_SENTINEL_A_VAL;
        uint32_t b_magic = *(volatile uint32_t *)DIAG_SCR_SENTINEL_B_MAGIC;
        uint64_t b_val   = *(volatile uint64_t *)DIAG_SCR_SENTINEL_B_VAL;

        shell_puts("\r\n--- SCR_EL3 sentinels (TF-A debug) ---\r\n");
        shell_printf("  Sentinel A (setup_ns_context):     magic=0x%08x %s\r\n",
                    a_magic,
                    a_magic == DIAG_SCR_SENTINEL_A_EXPECTED_MAGIC
                        ? "OK" : "MISSING — patched TF-A not loaded?");
        if (a_magic == DIAG_SCR_SENTINEL_A_EXPECTED_MAGIC) {
            shell_printf("                                     scr_el3=0x%lx (IRQ=%lu FIQ=%lu)\r\n",
                        (unsigned long)a_val,
                        (unsigned long)((a_val >> 1) & 1),
                        (unsigned long)((a_val >> 2) & 1));
        }
        shell_printf("  Sentinel B (cm_prepare_el3_exit_ns): magic=0x%08x %s\r\n",
                    b_magic,
                    b_magic == DIAG_SCR_SENTINEL_B_EXPECTED_MAGIC
                        ? "OK" : "MISSING — patched TF-A not loaded?");
        if (b_magic == DIAG_SCR_SENTINEL_B_EXPECTED_MAGIC) {
            shell_printf("                                     scr_el3=0x%lx (IRQ=%lu FIQ=%lu)\r\n",
                        (unsigned long)b_val,
                        (unsigned long)((b_val >> 1) & 1),
                        (unsigned long)((b_val >> 2) & 1));
        }
        if (a_magic == DIAG_SCR_SENTINEL_A_EXPECTED_MAGIC &&
            b_magic == DIAG_SCR_SENTINEL_B_EXPECTED_MAGIC) {
            if (a_val == b_val) {
                shell_puts("  Verdict: A == B → no override between setup_ns_context and el3_exit.\r\n");
            } else {
                shell_printf("  Verdict: A != B → SOMETHING OVERRODE SCR_EL3 (delta=0x%lx).\r\n",
                            (unsigned long)(a_val ^ b_val));
            }
        }
        shell_puts("\r\n");
    }

    /* Header via direct UART writes only. */
    irqtest_puts("\r\nirqtest probe:");

    uint32_t cpu = cpu_id();
    if (cpu >= MAX_CPUS) {
        irqtest_puts(" bogus cpu_id\r\n");
        return -1;
    }

#if !defined(SECONDARY_PREEMPT)
    /* The probe unmasks DAIF.I. If a timer IRQ fires (which is
     * exactly what we're testing for), the IRQ vector calls
     * schedule() from IRQ context — which corrupts the abandoned
     * exception frame on real ARM64 hardware (PR #98). Empirical
     * result: system hangs and operator must power-cycle.
     *
     * The hang IS the diagnostic for Stage 2 — it's how we confirm
     * the SCR_EL3 routing patch works before Stage 2.5 (which lands
     * SECONDARY_PREEMPT) goes in. So we don't skip the probe. We
     * pause to give the operator a chance to abort if they typed
     * the command without reading docs/pi5-armstub-track-c.md. */
    shell_puts("  WARNING: this probe will hang the system if a timer IRQ\r\n"
               "  fires (the IRQ vector calls schedule() from IRQ context\r\n"
               "  without SECONDARY_PREEMPT — see #98). The hang itself IS\r\n"
               "  the diagnostic: it proves SCR_EL3 routing works. Power-\r\n"
               "  cycle to recover. (5 second pause — Ctrl-C to abort...)\r\n");
    {
        uint64_t freq_pause;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq_pause));
        uint64_t now_pause;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now_pause));
        uint64_t pause_target = now_pause + freq_pause * 5;  /* 5 s */
        do {
            __asm__ volatile("yield");
            __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now_pause));
        } while (now_pause < pause_target);
    }
#endif

    struct diag_vec_counts *vec = diag_vec_counts_cpu(cpu);
    uint64_t irq_before = vec->irq;
    uint64_t fiq_before = vec->fiq;

    uint64_t daif_save;
    __asm__ volatile("mrs %0, daif" : "=r"(daif_save));
    irqtest_puts(" 1");  /* checkpoint 1: read DAIF */

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t now;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    uint64_t target = now + (freq / 10000);  /* 100 µs */
    irqtest_puts(" 2");  /* checkpoint 2: read CNTFRQ + CNTPCT */

    /* Unmask DAIF.I (#2 = I bit). FIQ stays masked unless caller
     * passes "fiq" arg.
     *
     * For the bisect test we ALSO support "noirq" to skip the daifclr
     * entirely — confirms whether the daifclr itself or something
     * else is the cause of the hang. "single" mode neutralizes the
     * SECONDARY_PREEMPT trampoline (sets preempt_disabled[cpu]=1)
     * so the IRQ handler returns to the original PC instead of
     * detouring via resched_trampoline. Lets us prove IRQ delivery
     * works without involving schedule(). */
    bool skip_daifclr = (argc >= 2 && argv[1] && strcmp(argv[1], "noirq") == 0);
    bool single_mode  = (argc >= 2 && argv[1] && strcmp(argv[1], "single") == 0);
#if defined(SECONDARY_PREEMPT)
    extern volatile int preempt_disabled[MAX_CPUS];
    if (single_mode) {
        preempt_disabled[cpu] = 1;
        __asm__ volatile("dsb sy" ::: "memory");
        irqtest_puts(" PD=1");
    }
#else
    (void)single_mode;
#endif
    /* #672 probe: dump VBAR_EL1, DAIF, SCTLR_EL1, and the NS-visible
     * GIC state for TIMER_IRQ before any DAIF manipulation. Goal: rule
     * out a corrupted vector base, stuck I-bit, or PMR/RPR-blocked
     * delivery before we daifclr. */
    irqtest_dump_pre_daifclr_state();

    irqtest_puts(skip_daifclr ? " 3-skipped" : " 3");  /* checkpoint 3: about to daifclr */

    /* Drain the FIFO before the raw markers so we are guaranteed to
     * see them on the wire. PL011 silently drops writes to a full
     * FIFO, so without the drain, the markers can vanish and we'd
     * misread "char missing" as "code did not execute". */
    /* #672 daifclr-wedge isolation:
     *   "noirq"        → no daifclr at all (control: should print A B)
     *   "single"/""    → daifclr #2 (clear I — original test)
     *   "fiq"          → daifclr #3 (clear I+F)
     *   "set"          → daifset #2 (set I — already set, true no-op)
     *   "isb"          → just isb (no DAIF write at all)
     *   "dbg"          → daifclr #1 (clear D bit only — should never raise IRQ)
     *
     * Differential reading:
     *   - all wedge except "noirq" → it's *any* msr to DAIF that wedges
     *   - "set" / "dbg" / "isb" pass, only #2 / #3 wedge → IRQ unmask
     *     specifically delivers an exception that hangs the CPU
     *   - "isb" wedges → not DAIF at all; some unrelated state
     */
    bool mode_set    = (argc >= 2 && argv[1] && strcmp(argv[1], "set") == 0);
    bool mode_isb    = (argc >= 2 && argv[1] && strcmp(argv[1], "isb") == 0);
    bool mode_dbg    = (argc >= 2 && argv[1] && strcmp(argv[1], "dbg") == 0);
    bool mode_fiq    = (argc >= 2 && argv[1] && strcmp(argv[1], "fiq") == 0);
    bool mode_fmask  = (argc >= 2 && argv[1] && strcmp(argv[1], "fmask") == 0);
    bool mode_clr    = (argc >= 2 && argv[1] && strcmp(argv[1], "clr") == 0);
    bool mode_wfi    = (argc >= 2 && argv[1] && strcmp(argv[1], "wfi") == 0);
    bool mode_idle   = (argc >= 2 && argv[1] && strcmp(argv[1], "idle") == 0);
    bool mode_tdis   = (argc >= 2 && argv[1] && strcmp(argv[1], "tdis") == 0);
    bool mode_cmem   = (argc >= 2 && argv[1] && strcmp(argv[1], "cmem") == 0);
    bool mode_dsb    = (argc >= 2 && argv[1] && strcmp(argv[1], "dsb") == 0);
    bool mode_wait   = (argc >= 2 && argv[1] && strcmp(argv[1], "wait") == 0);
    bool mode_iar    = (argc >= 2 && argv[1] && strcmp(argv[1], "iar") == 0);
    bool mode_match  = (argc >= 2 && argv[1] && strcmp(argv[1], "match") == 0);
    bool mode_putsx  = (argc >= 2 && argv[1] && strcmp(argv[1], "putsx") == 0);
    bool mode_linit  = (argc >= 2 && argv[1] && strcmp(argv[1], "linit") == 0);
    bool mode_drain  = (argc >= 2 && argv[1] && strcmp(argv[1], "iardrain") == 0);
    bool mode_dis    = (argc >= 2 && argv[1] && strcmp(argv[1], "dis") == 0);

    /* "clr" mode: clear the pending timer PPI in GIC before daifclr.
     * Tests whether the wedge is caused by the GIC asserting the IRQ
     * pin AT the moment we unmask DAIF.I. GIC-400 ICPENDR clears
     * pending state without needing ack/EOI. The timer PPI is banked
     * per-CPU so this only affects the running CPU. Operates on the
     * currently-active TIMER_IRQ (PPI 27 on Pi 5 since #672). */
    /* "tdis" mode: disable the generic timer at the CNTP_CTL_EL0 level
     * before daifclr. If this fixes the wedge, the wedge is caused by
     * the timer's IRQ assertion at the GIC level. */
    if (mode_tdis) {
        __asm__ volatile("msr cntp_ctl_el0, %0\n\tisb"
                         :: "r"((uint64_t)0) : "memory");
        irqtest_puts(" CNTP-DIS");
    }

    /* "idle" mode: emulate idle's exact prologue — cacheable BSS load,
     * mrs mpidr, write to cacheable memory — to test whether the
     * MMIO-heavy path right before daifclr (UART drain) is the
     * differentiator. */
    if (mode_idle) {
        static volatile uint32_t idle_emu_counter;
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        idle_emu_counter += (uint32_t)(mpidr & 0xff);
        irqtest_puts(" IDLE-EMU");
    }

    if (mode_clr) {
        /* GICD_ICPENDR0 = GICD_BASE + 0x280; write 1<<TIMER_IRQ to
         * clear the timer PPI (banked per-CPU for IDs 0..31). */
        uint32_t timer_bit = 1u << TIMER_IRQ;
        uint32_t pre = *(volatile uint32_t *)(GIC_DIST_BASE + 0x200);
        *(volatile uint32_t *)(GIC_DIST_BASE + 0x280) = timer_bit;
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t post = *(volatile uint32_t *)(GIC_DIST_BASE + 0x200);
        /* ALSO disable the timer PPI entirely via ICENABLER0. */
        *(volatile uint32_t *)(GIC_DIST_BASE + 0x180) = timer_bit;
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t after_dis = *(volatile uint32_t *)(GIC_DIST_BASE + 0x100);
        shell_printf("\r\n  GIC ISPENDR0 pre=0x%08x post-clear=0x%08x ISENABLER0 post-disable=0x%08x (PPI %u)\r\n",
                     pre, post, after_dis, (unsigned)TIMER_IRQ);
        irqtest_puts(" CLR-PEND");
    }

    /* "putsx" mode: extra UART puts BEFORE the drain+A+drain sequence.
     * Mimics what `idle` mode adds via puts(" IDLE-EMU") but with no
     * cacheable bump, no mpidr read. */
    if (mode_putsx) {
        irqtest_puts(" PUTSX");
    }

    /* "linit" mode: replay Linux's gic_cpu_if_up sequence before daifclr.
     * Linux on the same hardware delivers timer IRQs successfully —
     * if our `daifclr` wedge is from missing GIC config, this should
     * unblock. Sequence (irq-gic.c:gic_cpu_if_up):
     *   - Clear GICC_ACTIVEPRIO[0..3]
     *   - Read GICC_CTLR, preserve bypass bits
     *   - Write GICC_CTLR = bypass | EOImodeNS | EnableGrp1NS
     */
    /* "iardrain" mode: ack/EOI in a tight loop until IAR returns
     * spurious. Tests whether multiple IAR-ack-EOI cycles drain a
     * stuck pending state vs a single ack. */
    if (mode_drain) {
        int n = 0;
        for (int i = 0; i < 32 && n < 10; i++) {
            uint32_t iar = *(volatile uint32_t *)(GIC_CPU_BASE + 0x0C);
            __asm__ volatile("dsb sy" ::: "memory");
            if ((iar & 0x3FF) == 0x3FF) {
                break;
            }
            *(volatile uint32_t *)(GIC_CPU_BASE + 0x10) = iar;
            __asm__ volatile("dsb sy" ::: "memory");
            n++;
        }
        shell_printf("\r\n  IARDRAIN: ack'd+eoi'd %d IRQs\r\n", n);
    }

    /* "dis" mode: disable distributor, then re-enable. Linux always
     * does disable→config→enable. We don't (we incrementally configure
     * to preserve secure-side state). Tests whether a NS-side
     * distributor-disable cycle re-evaluates the IRQ pending state
     * in some way the kernel hasn't triggered. */
    if (mode_dis) {
        uint32_t pre_ctlr = *(volatile uint32_t *)(GIC_DIST_BASE + 0x000);
        *(volatile uint32_t *)(GIC_DIST_BASE + 0x000) = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        *(volatile uint32_t *)(GIC_DIST_BASE + 0x000) = pre_ctlr;
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t post_ctlr = *(volatile uint32_t *)(GIC_DIST_BASE + 0x000);
        shell_printf("\r\n  DIS-CYCLE: GICD_CTLR pre=0x%x post=0x%x\r\n",
                     pre_ctlr, post_ctlr);
    }

    /* Saved across mode_linit so the post-irqtest cleanup block can
     * restore the original GICC_CTLR. EOImodeNS changes kernel-wide
     * EOI semantics from "single-write to EOIR" to "DIR-after-EOI",
     * so leaving it set after the probe poisons every subsequent
     * IRQ acknowledge in the running kernel. */
    uint32_t linit_saved_ctlr = 0;
    bool linit_changed_ctlr = false;
    if (mode_linit) {
        for (int i = 0; i < 4; i++) {
            *(volatile uint32_t *)(GIC_CPU_BASE + 0xD0 + i * 4) = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t ctlr_pre = *(volatile uint32_t *)(GIC_CPU_BASE + 0x00);
        uint32_t bypass = ctlr_pre & 0x1E0; /* bypass bits 5..8 */
        linit_saved_ctlr = ctlr_pre;
        linit_changed_ctlr = true;
        *(volatile uint32_t *)(GIC_CPU_BASE + 0x00) =
            bypass | (1u << 9) /* EOImodeNS */ | 1u /* EnableGrp1NS */;
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t ctlr_post = *(volatile uint32_t *)(GIC_CPU_BASE + 0x00);
        shell_printf("\r\n  LINIT: GICC_CTLR pre=0x%x post=0x%x (set EOImodeNS — restored on exit)\r\n",
                     ctlr_pre, ctlr_post);
    }

    irqtest_fifo_drain();
    IRQTEST_RAW_PUTC('A');
    irqtest_fifo_drain();

    /* "match" mode: run idle's *exact* pre-daifclr instruction stream
     * — bl-style function call to cpu_logical_id + cacheable r/m/w of
     * a counter — with NO MMIO between this prologue and the daifclr.
     * If "match" passes while "single" wedges, the unblocker is the
     * function-call + cacheable-bump pattern specifically, not the
     * total amount of activity. */
    if (mode_match) {
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        int log_id = cpu_logical_id(mpidr);
        static volatile uint32_t match_counter[8];
        if (log_id >= 0 && log_id < 8) match_counter[log_id]++;
        /* No further activity — let daifclr run in same pipeline state
         * as idle's. */
    }


    /* "cmem" mode: a single cacheable load/store right after the
     * drain and immediately before daifclr — tests whether *any*
     * cacheable memory access between MMIO and daifclr unblocks the
     * wedge. Counter-intuitive but `idle` mode points this way.
     *
     * "dsb" mode: full DSB SY barrier between drain and daifclr. */
    if (mode_cmem) {
        static volatile uint32_t cmem_probe;
        cmem_probe++;
    }
    if (mode_dsb) {
        __asm__ volatile("dsb sy" ::: "memory");
    }
    /* "wait" mode: ~10 ms CNTPCT busy-wait between drain and daifclr —
     * tests pure timing as the unblocker (no MMIO, no cacheable
     * activity beyond CNTPCT reads). */
    /* "iar" mode: manually ack the pending IRQ via NS GICC_IAR before
     * daifclr. If the GIC accepts the ack, IAR returns 30, the IRQ
     * transitions to ACTIVE, the IRQ pin de-asserts, and daifclr should
     * complete normally. EOI'd via GICC_EOIR after.
     *
     * If IAR returns 1023 (spurious), GIC refused to ack — proves
     * Group 1 NS routing is broken at the CPU interface despite the
     * HPPIR readback saying otherwise.
     *
     * If daifclr STILL wedges after a successful ack, the wedge is not
     * the IRQ pin assertion at all — something else is going wrong
     * in the daifclr path. */
    /* Saved across mode_iar so the post-irqtest cleanup block can EOI
     * the IRQ that mode_iar deliberately leaves in ACTIVE state.
     * Without that EOI, the active priority bit stays raised and
     * future IRQs at <= that priority are silently masked at the CPU
     * interface — i.e. the next "irqtest" invocation, normal kernel
     * timer ticks, etc. all stop working. */
    uint32_t iar_saved_id = 0x3FF;
    bool iar_pending_eoi = false;
    if (mode_iar) {
        uint32_t iar = *(volatile uint32_t *)(GIC_CPU_BASE + 0x0C);
        __asm__ volatile("dsb sy" ::: "memory");
        uint32_t isacti_after = *(volatile uint32_t *)(GIC_DIST_BASE + 0x300);
        uint32_t hppir_after  = *(volatile uint32_t *)(GIC_CPU_BASE  + 0x18);
        shell_printf("\r\n  IAR-ack: GICC_IAR=0x%x (id=%u)  ISACTIVER0=0x%x  HPPIR=0x%x\r\n",
                     iar, iar & 0x3FF, isacti_after, hppir_after);
        /* DELIBERATELY NO IMMEDIATE EOI. State stays ACTIVE so the pin
         * is de-asserted (until EOI). If daifclr now succeeds, the
         * wedge was about pin assertion. If it still wedges, the
         * wedge is unrelated to GIC pin state. The saved id is EOI'd
         * in the cleanup block at the end of cmd_irqtest so the active
         * priority is released before the shell returns. */
        iar_saved_id = iar;
        iar_pending_eoi = ((iar & 0x3FF) != 0x3FF);
    }
    if (mode_wait) {
        uint64_t freq_w, now_w, target_w;
        __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq_w));
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now_w));
        target_w = now_w + freq_w / 100;
        do {
            __asm__ volatile("yield");
            __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now_w));
        } while (now_w < target_w);
    }

    /* "fmask" mode: mask FIQ first, then clear I. If FIQ delivery is
     * the wedge cause (Group 0 timer signaled as FIQ → vector entry
     * faults silently), this should pass while the default daifclr
     * #2 fails. Pre-existing DAIF=0x80 (I=1, F=0); fmask transitions
     * I=1,F=1 → I=0,F=1. */
    if (mode_fmask) {
        __asm__ volatile("msr daifset, #1\n\tisb" ::: "memory"); /* mask F */
        IRQTEST_RAW_PUTC('a');  /* lowercase a after F mask */
        irqtest_fifo_drain();
    }
    if (!skip_daifclr) {
        if (mode_set) {
            __asm__ volatile("msr daifset, #2\n\tisb" ::: "memory");
        } else if (mode_isb) {
            __asm__ volatile("isb" ::: "memory");
        } else if (mode_dbg) {
            __asm__ volatile("msr daifclr, #1\n\tisb" ::: "memory");
        } else if (mode_fiq) {
            __asm__ volatile("msr daifclr, #3\n\tisb" ::: "memory");
        } else if (mode_wfi) {
            /* Mirror idle's exact sequence: daifclr #2 + isb + wfi */
            __asm__ volatile("msr daifclr, #2\n\tisb\n\twfi" ::: "memory");
        } else {
            __asm__ volatile("msr daifclr, #2\n\tisb" ::: "memory");
        }
    }
    /* Burst raw writes WITHOUT drain — if any of these makes it on the
     * wire, the CPU is alive after daifclr. If none makes it, CPU is
     * truly halted. */
    IRQTEST_RAW_PUTC('B');
    IRQTEST_RAW_PUTC('B');
    IRQTEST_RAW_PUTC('B');
    IRQTEST_RAW_PUTC('B');
    irqtest_fifo_drain();
    IRQTEST_RAW_PUTC('C');
    irqtest_fifo_drain();
    /* #672 probe: read DAIF immediately and emit one hex byte to UART
     * via irqtest_putc. The DAIF bits we care about are 6 (F), 7 (I);
     * print the I+F nibble (high 2 bits of byte 0). If `1` is set
     * after a daifclr #2 (I), the unmask did not take effect. If both
     * bits are 0, IRQ unmask succeeded but IRQ never delivered. */
    {
        uint64_t daif_now;
        __asm__ volatile("mrs %0, daif" : "=r"(daif_now));
        IRQTEST_RAW_PUTC('C');
        uint8_t hi = (uint8_t)((daif_now >> 6) & 0x3);
        IRQTEST_RAW_PUTC('D');
        irqtest_puts(" daif=");
        irqtest_putc('0' + ((hi >> 1) & 1));  /* I bit */
        irqtest_putc('0' + (hi & 1));         /* F bit */
    }
    irqtest_puts(" 4");  /* checkpoint 4: daifclr returned (or skipped) */
    __asm__ volatile("isb" ::: "memory");
    irqtest_puts(" 5");  /* checkpoint 5: isb returned */

    /* Spin briefly with periodic trace dots so we can see how far
     * the spin gets before hang vs. completion. Both `now` and
     * `target` are u64; use the subtract-and-compare pattern to
     * avoid wraparound surprises. */
    int dot_counter = 0;
    do {
        __asm__ volatile("yield");
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
        /* Emit '.' every ~256 iterations of the spin so we know if
         * the spin is making forward progress. */
        if ((++dot_counter & 0xFF) == 0) {
            irqtest_putc('.');
        }
    } while (now < target);

    irqtest_puts(" 6");  /* checkpoint 6: spin completed */

    /* Re-mask. */
    __asm__ volatile("msr daif, %0" :: "r"(daif_save));
    __asm__ volatile("isb" ::: "memory");

    irqtest_puts(" 7");  /* checkpoint 7: re-mask done */

#if defined(SECONDARY_PREEMPT)
    /* Restore preempt_disabled so the kernel resumes normal scheduling
     * after the probe. */
    if (single_mode) {
        preempt_disabled[cpu] = 0;
        __asm__ volatile("dsb sy" ::: "memory");
        irqtest_puts(" PD=0");
    }
#endif

    irqtest_cleanup_state(iar_saved_id, iar_pending_eoi,
                          linit_saved_ctlr, linit_changed_ctlr);

    uint64_t irq_after = vec->irq;
    uint64_t fiq_after = vec->fiq;

    shell_printf("  CPU %u — diag_vec_counts after:  irq=%lu fiq=%lu\r\n",
                cpu, (unsigned long)irq_after, (unsigned long)fiq_after);

    if (irq_after > irq_before) {
        shell_printf("  RESULT: IRQ DELIVERED (delta=%lu).\r\n",
                    (unsigned long)(irq_after - irq_before));
        shell_puts("  >>> SCR_EL3 / GIC routing path WORKS. The kernel is\r\n"
                   "      blocking IRQs by holding DAIF.I=1 in tasks. Stage 2.5\r\n"
                   "      (unmask DAIF.I in task_entry_trampoline + activate\r\n"
                   "      SECONDARY_PREEMPT) will activate hardware preemption.\r\n");
    } else if (fiq_after > fiq_before) {
        shell_printf("  RESULT: FIQ DELIVERED (delta=%lu).\r\n",
                    (unsigned long)(fiq_after - fiq_before));
        shell_puts("  >>> PPI is still in Group 0 — group register write\r\n"
                   "      from EL3 didn't take effect.\r\n");
    } else {
        shell_puts("  RESULT: No IRQ or FIQ delivered.\r\n");
        shell_puts("  >>> SCR_EL3 / GIC routing patches not effective. Either\r\n"
                   "      TF-A isn't loaded or another gate is in play.\r\n");
    }

    return 0;
}
#endif /* PLATFORM_RASPI5 && PI5_IRQ_DIAG */

#endif /* !PLATFORM_X86_64 */

#if defined(PLATFORM_RASPI5)

#include "bcm_mailbox.h"
#include "string.h"

/*
 * Diagnostic shell command for the Pi 5 firmware mailbox clock
 * interface. Lets us iterate clock IDs interactively without
 * rebuilding the kernel for each candidate.
 *
 * Usage:
 *   mboxclk            - dump state for ids 1..16 (state + cfg rate
 *                        + measured rate)
 *   mboxclk <id>       - dump one id
 *   mboxclk <id> on    - SET_CLOCK_STATE(id, 1) + show before/after
 *   mboxclk <id> off   - SET_CLOCK_STATE(id, 0) + show before/after
 *
 * Tied to #414: empirical evidence pinned Pi 5 EMMC to firmware id 1
 * (cfg_rate=200000000 matches the dtsi `clk_emmc2: clock-frequency
 * = <200000000>` fixed-clock). Pi 4-era id 12 doesn't drive EMMC on
 * Pi 5. Use this command to discover the right id for any other
 * peripheral whose firmware mapping isn't yet known.
 */

/* Print one column with formatted hex on success, fixed-width "(err)"
 * placeholder on failure — keeps the dump aligned regardless of
 * which queries the firmware accepted. */
static void mboxclk_print_field(int rc, uint32_t value)
{
    if (rc == 0) {
        shell_printf("0x%08x", value);
    } else {
        shell_puts("(err)     ");
    }
}

static void mboxclk_dump_one(uint32_t id)
{
    uint32_t state = 0xFFFFFFFFu, cfg = 0xFFFFFFFFu, meas = 0xFFFFFFFFu;
    int rc_state = bcm_mailbox_get_clock_state(id, &state);
    int rc_cfg   = bcm_mailbox_get_clock_rate(id, &cfg);
    int rc_meas  = bcm_mailbox_get_clock_rate_measured(id, &meas);

    shell_printf("  clk %2u state=", id);
    mboxclk_print_field(rc_state, state);
    shell_puts(" cfg=");
    mboxclk_print_field(rc_cfg, cfg);
    shell_puts(" meas=");
    mboxclk_print_field(rc_meas, meas);
    shell_puts("\r\n");
}

/*
 * Strict decimal parser — accepts digit-only input. Returns 0 on
 * success and writes the parsed value into *out, -1 on any non-digit
 * character (including empty string and overflow past 32-bit).
 */
static int mboxclk_parse_id(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') return -1;
    uint32_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        uint32_t d = (uint32_t)(*p - '0');
        /* Reject overflow before it produces a wrap-around value. */
        if (v > 0xFFFFFFFFu / 10u || (v * 10u) > 0xFFFFFFFFu - d) return -1;
        v = v * 10u + d;
    }
    *out = v;
    return 0;
}

static void mboxclk_print_usage(void)
{
    shell_puts("usage: mboxclk            - dump clocks 1..16\r\n");
    shell_puts("       mboxclk <id>       - dump one clock id\r\n");
    shell_puts("       mboxclk <id> on    - enable clock id\r\n");
    shell_puts("       mboxclk <id> off   - disable clock id\r\n");
}

int cmd_mboxclk(int argc, char *argv[])
{
    if (argc == 1) {
        shell_puts("Pi firmware clock-id state (state | cfg_rate | meas_rate)\r\n");
        for (uint32_t id = 1; id <= 16; id++) {
            mboxclk_dump_one(id);
        }
        return 0;
    }
    uint32_t id;
    if (mboxclk_parse_id(argv[1], &id) != 0) {
        shell_printf("mboxclk: invalid clock id '%s'\r\n", argv[1]);
        mboxclk_print_usage();
        return -1;
    }
    if (argc == 2) {
        mboxclk_dump_one(id);
        return 0;
    }
    /* SET path: argv[2] must be exactly "on" or "off". */
    bool on;
    if (strcmp(argv[2], "on") == 0) {
        on = true;
    } else if (strcmp(argv[2], "off") == 0) {
        on = false;
    } else {
        shell_printf("mboxclk: invalid action '%s' (expected 'on' or 'off')\r\n", argv[2]);
        mboxclk_print_usage();
        return -1;
    }
    shell_puts("Before:\r\n");
    mboxclk_dump_one(id);
    int rc = bcm_mailbox_set_clock_state(id, on);
    shell_puts(rc == 0 ? "SET_CLOCK_STATE: OK\r\n" : "SET_CLOCK_STATE: FAILED\r\n");
    shell_puts("After:\r\n");
    mboxclk_dump_one(id);
    return rc;
}

#endif /* PLATFORM_RASPI5 */

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

/*
 * cdcdiag — CDC-ECM TX path snapshot (#427 debug).
 *
 * Prints aggregate counters + per-slot state needed to localise
 * the "telnet hangs after `model`" stall:
 *
 *   - tx_completions == tx_submits → completions arriving on time, no stall
 *   - tx_submits >> tx_completions → completions stuck in flight
 *   - tx_busy_returns growing → all 4 slots in_use, lwIP backpressure
 *   - in_use_count saturated at 4 → confirmed slot exhaustion
 *
 * Run before/after the trigger ('cdcdiag', then 'model', then 'cdcdiag')
 * to see the delta.
 */
int cmd_cdcdiag(int argc, char *argv[])
{
    (void)argc; (void)argv;

    struct cdc_ecm_tx_diag d;
    cdc_ecm_get_tx_diag(&d);

    /* Width of the diag struct's per-slot arrays (cdc_ecm.h) — pinned
     * by a static_assert in cdc_ecm.c against CDC_ECM_TX_SLOTS. */
    const unsigned slots = sizeof(d.slot_in_use) / sizeof(d.slot_in_use[0]);

    shell_puts("CDC-ECM TX diag (#427):\r\n");
    shell_printf("  tx_completions    %lu\r\n", (unsigned long)d.tx_completions);
    shell_printf("  tx_submits        %lu\r\n", (unsigned long)d.tx_submits);
    shell_printf("  tx_busy_returns   %lu\r\n", (unsigned long)d.tx_busy_returns);
    shell_printf("  tx_submit_errors  %lu\r\n", (unsigned long)d.tx_submit_errors);
    shell_printf("  in_use_count      %u of %u\r\n",
                 (unsigned)d.in_use_count, slots);
    shell_printf("  completed_count   %u (waiting reap)\r\n",
                 (unsigned)d.completed_count);
    shell_puts("  per-slot:\r\n");
    for (unsigned i = 0; i < slots; i++) {
        shell_printf("    [%u] in_use=%u completed=%u\r\n",
                     i,
                     (unsigned)d.slot_in_use[i],
                     (unsigned)d.slot_completed[i]);
    }
    /* Inflight delta — non-zero means submits outpace completions. */
    long inflight = (long)d.tx_submits - (long)d.tx_completions;
    shell_printf("  inflight (submits - completions) = %ld\r\n", inflight);
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

    /* Step 3 kickoff: set APPL_CTRL.LTSSM_EN after the clock/reset
     * sequence, and sample APPL_DEBUG LTSSM state a few times to see
     * whether the link trains. This is a minimal stab at Step 3 —
     * full RC init (DBI programming, Gen3/4 eq, iATU) is not here. */
    if (mode[0] == 'l' && mode[1] == 't') { /* "ltssm" */
        uart_puts("\r\n--- LTSSM_EN probe (trivial, no DBI init) ---\r\n");
        volatile uint32_t *appl_ctrl  = (volatile uint32_t *)(uintptr_t)0x140a0004UL;
        volatile uint32_t *appl_debug = (volatile uint32_t *)(uintptr_t)0x140a00d0UL;

        uint32_t before_ctrl = *appl_ctrl;
        uint32_t before_dbg  = *appl_debug;
        uart_printf("  Before: APPL_CTRL=0x%08x APPL_DEBUG=0x%08x LTSSM=0x%02x\r\n",
                    (unsigned)before_ctrl, (unsigned)before_dbg,
                    (unsigned)((before_dbg >> 3) & 0x3F));

        *appl_ctrl = before_ctrl | (1u << 7);   /* APPL_CTRL_LTSSM_EN */
        __asm__ volatile("dsb sy" ::: "memory");

        for (int i = 0; i < 10; i++) {
            for (volatile uint32_t k = 0; k < 100000; k++) { }
            uint32_t ctrl = *appl_ctrl;
            uint32_t dbg  = *appl_debug;
            uart_printf("  tick %d: APPL_CTRL=0x%08x LTSSM_EN=%u LTSSM=0x%02x\r\n",
                        i, (unsigned)ctrl,
                        (ctrl >> 7) & 1u,
                        (unsigned)((dbg >> 3) & 0x3F));
        }
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

#include "pcie_tegra194.h"

/*
 * pcietrain — full Tegra PCIe C8 host init + link training + iATU
 * program + endpoint VID/DID read, using the kernel/drivers/pcie/
 * pcie_tegra194.c driver layered on top of the BPMP IPC stack.
 *
 *   pcietrain       Default: full init + LTSSM train + EP probe.
 *   pcietrain warm  Do clock/reset + P2U init but DO NOT toggle
 *                   PEX_RST or set LTSSM_EN. Checks whether Linux's
 *                   pre-kexec link survives — if so, the problem
 *                   is in our re-init sequence, not elsewhere.
 *
 * The success criterion for #25 Step 3:
 *   - LTSSM reaches L0 (0x11)
 *   - DBI bus 0 vendor/device reads 0x10DE:0x229c (NVIDIA RC bridge)
 *   - CFG bus 1 dev 0 fn 0 reads 0x10EC:0x8168 (Realtek RTL8168)
 */
int cmd_pcietrain(int argc, char *argv[])
{
    const char *mode = (argc >= 2) ? argv[1] : "full";

    uart_puts("\r\n=== Tegra PCIe C8 link-up sequence ===\r\n");

    int rc = bpmp_init();
    uart_printf("  bpmp_init:            rc=%d\r\n", rc);
    if (rc != 0) {
        uart_puts("  (cannot proceed without BPMP)\r\n");
        uart_puts("=== End ===\r\n");
        return 0;
    }

    rc = pcie_tegra_host_init();
    uart_printf("  pcie host init:       rc=%d\r\n", rc);
    if (rc != 0) {
        uart_puts("=== End ===\r\n");
        return 0;
    }

    uint32_t ltssm = 0;
    int link_rc = 0;
    if (mode[0] == 'w') {
        /* warm: read current LTSSM state without toggling anything */
        struct pcie_tegra_snapshot s0;
        pcie_tegra_read_snapshot(&s0);
        ltssm = s0.ltssm_state;
        uart_printf("  (warm mode) current LTSSM=0x%02x LTSSM_EN=%u\r\n",
                    (unsigned)ltssm, (unsigned)(s0.ltssm_en ? 1 : 0));
        link_rc = (ltssm == 0x11) ? 0 : -1;
    } else {
        link_rc = pcie_tegra_start_link(2000, &ltssm);
    }
    uart_printf("  pcie start link:      rc=%d  final LTSSM=0x%02x\r\n",
                link_rc, (unsigned)ltssm);

    struct pcie_tegra_snapshot s;
    pcie_tegra_read_snapshot(&s);
    uart_printf("  APPL_CTRL:            0x%08x  (LTSSM_EN=%u)\r\n",
                (unsigned)s.appl_ctrl, (unsigned)(s.ltssm_en ? 1 : 0));
    uart_printf("  APPL_DEBUG:           0x%08x  (LTSSM=0x%02x)\r\n",
                (unsigned)s.appl_debug, (unsigned)s.ltssm_state);
    uart_printf("  APPL_PINMUX:          0x%08x  (PEX_RST=%u)\r\n",
                (unsigned)s.appl_pinmux,
                (unsigned)(s.appl_pinmux & 1));
    uart_printf("  APPL_LINK_STATUS:     0x%08x  (RDLH_LINK_UP=%u)\r\n",
                (unsigned)s.appl_link_status,
                (unsigned)(s.appl_link_status & 1));
    uart_printf("  DBI bus0 VID:DID:     0x%08x  (expect 0x229c10de)\r\n",
                (unsigned)s.dbi_bus0_vid_did);
    uart_printf("  RC alive:             %s\r\n",
                s.rc_alive ? "YES" : "no (DBI returns all-ones/zeros)");

    if (link_rc == 0) {
        uint32_t ep = 0;
        int ep_rc = pcie_tegra_probe_endpoint(&ep);
        uart_printf("\r\n  EP probe:             rc=%d\r\n", ep_rc);
        uart_printf("  CFG bus1 VID:DID:     0x%08x  (expect 0x816810ec)\r\n",
                    (unsigned)ep);
        if ((ep & 0xFFFF) == 0x10EC && ((ep >> 16) & 0xFFFF) == 0x8168) {
            uart_puts("  *** RTL8168 endpoint reachable from SLM-OS! ***\r\n");
        }
    } else {
        uart_puts("  (EP probe skipped — link never reached L0)\r\n");
    }

    uart_puts("=== End ===\r\n");
    return 0;
}

#include "i2c_tegra.h"
#include "imx219.h"

/*
 * imx219 — power up the Sony IMX219 sensor and read CHIP_ID.
 *
 * #396 Hardware Task 2 verification gate. The full power-up sequence
 * (extperiph1 XCLK, regulator GPIO, mux selector, reset GPIO release,
 * datasheet settle) lives in `kernel/drivers/camera/imx219.c`. This
 * shell command is a thin wrapper that calls `imx219_power_on`,
 * prints the CHIP_ID it reads, and decodes the rc table for
 * diagnostic clarity.
 *
 * Prerequisites:
 *   1. IMX219 module physically attached to connector A (J17).
 *   2. The IMX219-A DT overlay loaded by Linux pre-kexec (configure
 *      via `config-by-hardware.py -n 2='Camera IMX219-A'`). This
 *      ensures Linux configures the cam_i2c controller, the i2c-mux
 *      driver knows about the cam_i2cmux GPIO, and the sensor's
 *      regulator topology is set up. SLM-OS inherits all of that.
 *
 * Expected output on a working setup:
 *
 *   === IMX219 power-up + CHIP_ID readback (cam_i2c, slave 0x10) ===
 *     imx219_power_on:      rc=0
 *     CHIP_ID:              0x0219 (expect 0x0219)
 *     *** IMX219 detected — Hardware Task 2 GREEN ***
 */
int cmd_imx219(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uart_puts("\r\n=== IMX219 power-up + CHIP_ID readback "
              "(cam_i2c, slave 0x10) ===\r\n");

    int rc = imx219_power_on();
    uart_printf("  imx219_power_on:      rc=%d\r\n", rc);

    if (rc == 0) {
        uint16_t chip_id = 0;
        int read_rc = imx219_read_chip_id(&chip_id);
        uart_printf("  CHIP_ID:              0x%04x (expect 0x0219)\r\n",
                    (unsigned)chip_id);
        if (read_rc == 0 && chip_id == 0x0219u) {
            uart_puts("  *** IMX219 detected — Hardware Task 2 GREEN ***\r\n");
        } else {
            uart_printf("  *** Re-read CHIP_ID failed: read_rc=%d, "
                        "chip_id=0x%04x (expected 0x0219). The sensor "
                        "responded to the power-up read but went away "
                        "between probes — check XCLK / mux state. ***\r\n",
                        read_rc, (unsigned)chip_id);
        }
    } else if (rc == -2) {
        uart_puts("  *** Power-up succeeded but CHIP_ID read failed —    ***\r\n");
        uart_puts("  *** sensor not responding on I²C. Check overlay,    ***\r\n");
        uart_puts("  *** I²C MUX state, and reset-GPIO timing.           ***\r\n");
    } else if (rc == -3) {
        uart_puts("  *** Power-up succeeded but CHIP_ID mismatch — wrong ***\r\n");
        uart_puts("  *** sensor (IMX477?) or stuck on a different mux ch.***\r\n");
    } else {
        uart_puts("  *** Power-up failed — see WARN log lines above.     ***\r\n");
    }

    uart_puts("=== End ===\r\n");
    return 0;
}

#include "nvcsi.h"

/*
 * nvcsi — bring up the Tegra234 NVCSI receiver for the IMX219-A
 * port and dump the post-init interrupt status registers.
 *
 * #396 Hardware Task 3 verification gate. After this returns 0,
 * NVCSI is armed (PP_EN_CTRL = 1) and waiting for D-PHY packets.
 * Actual frame capture requires (a) the IMX219 to be told to start
 * streaming over I²C — `imx219_stream_on` is a future task, the
 * `imx219` shell command leaves MODE_SELECT = 0 — and (b) VI to
 * accept the resulting frames. INTR_STATUS / ERR_INTR_STATUS are
 * the smoke-test signal: 0 means the receiver came up clean, any
 * non-zero value indicates a header/timing error.
 *
 * Prerequisites:
 *   1. IMX219 has been powered up via `imx219` (XCLK + reset).
 *      NVCSI itself doesn't need the sensor running, but without a
 *      sensor the INTR_STATUS will stay 0 forever (no packets ever
 *      arrive).
 *   2. NVCSI MMIO at TEGRA234_NVCSI_BASE is mapped (vmm.c).
 *   3. BPMP IPC is up (this command's chain of MRQ calls — VI
 *      power-domain enable, NVCSI clock enable, NVCSI reset
 *      deassert — asserts that on entry).
 *
 * Expected output on a working setup:
 *   === NVCSI bring-up + intr-status dump (port imx219_a) ===
 *     nvcsi_stream_init:    rc=0
 *     INTR_STATUS:          0x00000000
 *     ERR_INTR_STATUS:      0x00000000
 *     *** NVCSI armed — Hardware Task 3 GREEN (idle baseline) ***
 */
int cmd_nvcsi(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uart_puts("\r\n=== NVCSI bring-up + intr-status dump "
              "(port imx219_a) ===\r\n");

    int rc = nvcsi_stream_init(&nvcsi_imx219_a_port);
    uart_printf("  nvcsi_stream_init:    rc=%d\r\n", rc);
    if (rc != 0) {
        if (rc == -2) {
            uart_puts("  *** BPMP power-domain or clock enable   ***\r\n");
            uart_puts("  *** failed — check BPMP IVC handshake   ***\r\n");
            uart_puts("  *** healthy via 'bpmp' shell command.   ***\r\n");
        } else if (rc == -3) {
            uart_puts("  *** CIL_CONFIG readback mismatch — NVCSI ***\r\n");
            uart_puts("  *** MMIO is being filtered by CBB or the ***\r\n");
            uart_puts("  *** clock isn't actually running.        ***\r\n");
        } else {
            uart_puts("  *** Bad arguments — fix the port struct. ***\r\n");
        }
        uart_puts("=== End ===\r\n");
        return -1;
    }

    uint32_t intr = 0, err = 0;
    int sr = nvcsi_get_intr_status(&nvcsi_imx219_a_port, &intr, &err);
    uart_printf("  INTR_STATUS:          0x%08x\r\n", (unsigned)intr);
    uart_printf("  ERR_INTR_STATUS:      0x%08x\r\n", (unsigned)err);
    if (sr == 0 && intr == 0u && err == 0u) {
        uart_puts("  *** NVCSI armed — Hardware Task 3 GREEN "
                  "(idle baseline) ***\r\n");
    } else if (intr == 0xFFFFFFFFu || err == 0xFFFFFFFFu) {
        uart_puts("  *** All-ones readback — NVCSI MMIO blocked  ***\r\n");
        uart_puts("  *** by CBB firewall or address mismap.      ***\r\n");
    } else {
        uart_puts("  *** Non-zero INTR_STATUS — receiver saw a   ***\r\n");
        uart_puts("  *** packet or fault during init. Inspect    ***\r\n");
        uart_puts("  *** bits per ~/slmos-ref/tegra-l4t/l4t-csi4_registers.h ***\r\n");
    }

    uart_puts("=== End ===\r\n");
    return 0;
}

#include "camrtc.h"
#include "camrtc_capture.h"

/*
 * rcediag — Camera RTCPU (RCE) HSP-VM transport probe + handshake.
 *
 * #396 Hardware Task 3 Option B prerequisite. Dumps the hsp_rce
 * controller state (DIMENSIONING + R5 power state + per-mailbox
 * SHRD_MBOX values) so a user can confirm RCE is alive and reachable
 * post-kexec, then runs the HELLO + PROTOCOL + RESUME handshake to
 * establish a working session with the camera firmware. After this
 * succeeds, follow-up commands (CH_SETUP, CAPTURE_PHY_STREAM_OPEN,
 * CAPTURE_CSI_STREAM_SET_CONFIG) can use camrtc_send_msg() to
 * configure NVCSI / VI through RCE — the only path that works on
 * Tegra234 (NVCSI direct-MMIO is BLOCKED, see PR #426).
 *
 * Expected output on a working setup (kexec from Linux with no
 * camera driver active so RCE is idle in WFI):
 *
 *   === RCE HSP-VM diag + handshake ===
 *   [INFO] camrtc: hsp_rce DIMENSIONING=0x00080048 (SM=8 SS=4)
 *   [INFO] camrtc: rce-pm R5_CTRL_0=0x00000002 (FWLOADDONE=1)
 *   [INFO] camrtc: rce-pm PWR_STATUS_0=0x04600000 (WFIPIPESTOPPED=1)
 *   [INFO] camrtc: VM-TX SHRD_MBOX=0x00000000 (FULL=0)
 *   [INFO] camrtc: VM-RX SHRD_MBOX=0x00000000 (FULL=0)
 *   [INFO] camrtc: HELLO echo matched (cookie=0x...)
 *   [INFO] camrtc: RCE FW protocol version=6 (SM6 expected)
 *   [INFO] camrtc: RESUME ack (status=0x...)
 *     camrtc_init: rc=0
 *     *** RCE HSP-VM session established ***
 */
int cmd_rcediag(int argc, char *argv[])
{
    (void)argc; (void)argv;

    uart_puts("\r\n=== RCE HSP-VM diag + handshake ===\r\n");
    (void)camrtc_diag_dump();

    int rc = camrtc_init();
    uart_printf("  camrtc_init:          rc=%d\r\n", rc);
    if (rc == 0) {
        uart_puts("  *** RCE HSP-VM session established ***\r\n");

        /* Round-trip a CAMRTC_HSP_PING (opcode 0x45) to confirm
         * the established session can carry arbitrary HSP-VM
         * messages — not just the boot-sync HELLO/PROTOCOL/RESUME
         * sequence. PING is documented in
         * `~/slmos-ref/tegra-l4t/l4t-camrtc-commands.h:64-66` as the
         * "check aliveness of RCE FW and the HSP protocol" probe;
         * RCE echoes the 24-bit param verbatim. This is the
         * smallest pre-CH_SETUP gate proving `camrtc_send_msg` is
         * usable for the upcoming Hardware Task 3 messages
         * (CH_SETUP, CAPTURE_PHY_STREAM_OPEN_REQ,
         * CAPTURE_CSI_STREAM_SET_CONFIG_REQ). */
        uint32_t ping_param = 0xCAFE42u;  /* anything random-ish */
        uint32_t ping_resp  = 0;
        int ping_rc = camrtc_send_msg(CAMRTC_HSP_PING, ping_param,
                                       &ping_resp, 100000u);
        uart_printf("  PING:                 rc=%d echo=0x%06x "
                    "(sent=0x%06x)\r\n",
                    ping_rc, (unsigned)ping_resp, (unsigned)ping_param);
        if (ping_rc == 0 && ping_resp == ping_param) {
            uart_puts("  *** PING round-trip OK — HSP-VM session  ***\r\n");
            uart_puts("  *** ready for arbitrary message traffic. ***\r\n");
        } else {
            uart_printf("  *** PING failed (rc=%d) — see WARN log "
                        "lines for details. ***\r\n", ping_rc);
        }

        /* Hardware Task 3 sub-step: stand up the capture-control
         * IVC channel via CAMRTC_HSP_CH_SETUP. After this returns
         * 0, RCE is bound to the SLM-OS-allocated rx/tx ring
         * IOVAs and the next milestone (sending
         * CAPTURE_PHY_STREAM_OPEN_REQ over the ring) is unblocked. */
        int chs_rc = camrtc_ch_setup_capture_control();
        uintptr_t region = camrtc_ch_setup_region_phys();
        uart_printf("  CH_SETUP capture-ctrl: rc=%d region=0x%lx\r\n",
                    chs_rc, (unsigned long)region);
        if (chs_rc == 0) {
            uart_puts("  *** CH_SETUP OK — RCE bound the rx/tx     ***\r\n");
            uart_puts("  *** rings; ready for capture-control IVC. ***\r\n");
        } else {
            uart_puts("  *** CH_SETUP failed — see WARN log lines. ***\r\n");
        }
    } else if (rc == -1) {
        uart_puts("  *** hsp_rce MMIO unreachable — CBB firewall  ***\r\n");
        uart_puts("  *** or HSP block clock-gated.                ***\r\n");
    } else if (rc == -2) {
        uart_puts("  *** BPMP rejected a poweron MRQ, OR RCE       ***\r\n");
        uart_puts("  *** firmware not loaded — check the WARN log  ***\r\n");
        uart_puts("  *** line above for which step failed, then    ***\r\n");
        uart_puts("  *** verify BPMP IPC via the `bpmp` command.   ***\r\n");
    } else if (rc == -3) {
        uart_puts("  *** HELLO/PROTOCOL/RESUME timed out — RCE    ***\r\n");
        uart_puts("  *** is not responding on the HSP mailbox.    ***\r\n");
    } else if (rc == -4) {
        uart_puts("  *** PROTOCOL mismatch — RCE FW version is    ***\r\n");
        uart_puts("  *** not SM6. Update the driver-side version. ***\r\n");
    } else {
        uart_puts("  *** Handshake failed — see WARN log lines.   ***\r\n");
    }

    uart_puts("=== End ===\r\n");
    return 0;
}

/*
 * csidiag — first capture-control IVC round-trip:
 *   1. camrtc_capture_init (HSP-VM session + CH_SETUP + IVC ring init)
 *   2. CAPTURE_PHY_STREAM_OPEN_REQ (NVCSI port A, stream 0, D-PHY)
 *   3. Print the response result.
 *
 * Result codes are in `~/slmos-ref/tegra-l4t/l4t-camrtc-capture-messages.h`
 * (CAPTURE_OK = 0, CAPTURE_ERROR_* otherwise). Anything other than
 * 0 means the request reached RCE, came back, but RCE rejected it
 * — e.g. NVCSI not powered, port already open, bad PHY type. The
 * goal here is to *prove the IVC ring works end-to-end*; an
 * RCE-side error is still a successful round-trip from SLM-OS's
 * perspective.
 */
int cmd_csidiag(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uart_puts("\r\n=== NVCSI port A open via RCE IVC ===\r\n");

    int rc = camrtc_capture_init();
    uart_printf("  capture_init:   rc=%d\r\n", rc);
    if (rc != 0) {
        uart_puts("  *** capture_init failed — see WARN log; ***\r\n");
        uart_puts("  *** can't proceed without IVC channel.   ***\r\n");
        uart_puts("=== End ===\r\n");
        return 0;
    }

    uint32_t result = 0xDEADBEEFu;
    /* NVCSI_STREAM_0 = 0, NVCSI_PORT_A = 0, NVCSI_PHY_TYPE_DPHY = 0
     * (`~/slmos-ref/tegra-l4t/l4t-camrtc-capture.h:1372/1387/1443`). */
    rc = camrtc_capture_phy_stream_open(0u, 0u, 0u, &result);
    uart_printf("  PHY_STREAM_OPEN: rc=%d result=0x%x\r\n",
                rc, (unsigned)result);
    if (rc != 0 || result != 0u) {
        if (rc != 0) {
            uart_puts("  *** PHY_STREAM_OPEN failed at the IVC layer  ***\r\n");
            uart_puts("  *** (see WARN log); skipping SET_CONFIG.     ***\r\n");
        } else {
            uart_puts("  *** RCE rejected PHY_STREAM_OPEN; skipping   ***\r\n");
            uart_puts("  *** SET_CONFIG. Decode result via            ***\r\n");
            uart_puts("  *** l4t-camrtc-capture-messages.h.           ***\r\n");
        }
        uart_puts("=== End ===\r\n");
        return 0;
    }

    /* PHY_STREAM_OPEN succeeded. Configure the brick + CIL for
     * IMX219: 2 D-PHY lanes, 456 MHz MIPI clock (the IMX219
     * default link freq from `~/slmos-ref/linux/linux-imx219.c:139`).
     * SoC-default t_hs_settle / t_clk_settle (0). */
    uint32_t cfg_result = 0xDEADBEEFu;
    rc = camrtc_capture_csi_stream_set_config(0u, 0u, 2u, 456000u,
                                              &cfg_result);
    uart_printf("  CSI_SET_CONFIG:  rc=%d result=0x%x\r\n",
                rc, (unsigned)cfg_result);
    if (rc != 0 || cfg_result != 0u) {
        if (rc == 0) {
            uart_puts("  *** RCE rejected SET_CONFIG; decode result   ***\r\n");
            uart_puts("  *** via l4t-camrtc-capture-messages.h.       ***\r\n");
        } else {
            uart_puts("  *** SET_CONFIG failed at the IVC layer.      ***\r\n");
        }
        uart_puts("=== End ===\r\n");
        return 0;
    }

    /* CSI_SET_CONFIG succeeded — fire CHANNEL_SETUP_REQ with the
     * real VI request ring + memoryinfo ring carved out of the
     * NC region by camrtc.c. RCE should now accept the request
     * (queue_depth=1 + valid IOVAs) and assign a channel_id; the
     * actual CAPTURE_REQUEST_REQ to fill a frame lands in PR4. */
    uint32_t ch_result = 0xDEADBEEFu;
    uint32_t ch_id     = 0xDEADBEEFu;
    uint64_t vi_mask   = 0xDEADBEEFDEADBEEFull;
    rc = camrtc_capture_channel_setup(0u, 0u,
                                      camrtc_vi_req_ring_iova(),
                                      camrtc_vi_req_meminfo_iova(),
                                      camrtc_vi_req_queue_depth(),
                                      camrtc_vi_req_request_size(),
                                      camrtc_vi_req_meminfo_size(),
                                      &ch_result, &ch_id, &vi_mask);
    uart_printf("  CHANNEL_SETUP:   rc=%d result=0x%x channel_id=0x%x "
                "vi_mask=0x%lx\r\n",
                rc, (unsigned)ch_result, (unsigned)ch_id,
                (unsigned long)vi_mask);
    if (rc != 0 || ch_result != 0u) {
        if (rc == 0) {
            uart_puts("  *** Round-trip OK; RCE rejected the request   ***\r\n");
            uart_puts("  *** — see WARN log + decode result via        ***\r\n");
            uart_puts("  *** l4t-camrtc-capture-messages.h.            ***\r\n");
        } else {
            uart_puts("  *** CHANNEL_SETUP failed at the IVC layer.   ***\r\n");
        }
        uart_puts("=== End ===\r\n");
        return 0;
    }
    uart_printf("  *** CHANNEL_SETUP OK — RCE allocated VI      ***\r\n");
    uart_printf("  *** channel %u (vi_mask=0x%lx).              ***\r\n",
                (unsigned)ch_id, (unsigned long)vi_mask);

    /* CHANNEL_SETUP succeeded — populate slot 0 of the request
     * ring with a *full* capture_descriptor (header + populated
     * vi_channel_config) and fire CAPTURE_REQUEST_REQ over the
     * capture IVC channel.
     *
     * vi_channel_config carries IMX219 binning-mode RAW10 frame
     * geometry (1640×1232) and atomp surface[0] = the 4 MB
     * frame-buffer carveout at 0xA1000000 (PMM-reserved in
     * pmm.c). RCE programs the VI hardware, the sensor (newly
     * told to STREAMING via I²C below) emits a frame, the VI
     * pipeline writes ~4 MB of T_R16 pixels into the buffer, and
     * RCE sends CAPTURE_STATUS_IND with `capture_status.status`
     * = CAPTURE_STATUS_SUCCESS (1). */

    /* Apply IMX219 mode-init register bank — 1640×1232 RAW10
     * binning. ~45 register writes (PLL + lane mode + crop +
     * binning + format). Without this the sensor stays in
     * default state and never emits a SOF — verified hardware
     * blocker on jetson-nano-1, PR #513. */
    rc = imx219_set_mode_binning_1640x1232();
    uart_printf("  imx219 mode-init: rc=%d (1640x1232 RAW10 binning)\r\n", rc);
    if (rc != 0) {
        uart_puts("  *** IMX219 mode-init failed — see WARN log.   ***\r\n");
        uart_puts("  *** Run `imx219` first to power the sensor.   ***\r\n");
        uart_puts("=== End ===\r\n");
        return 0;
    }

    /* Tell IMX219 to start streaming. MODE_SELECT (0x0100) goes
     * 0 → 1; sensor begins emitting CSI-2 frames on the next
     * frame boundary (~33 ms at 30 fps). */
    rc = imx219_streaming_enable();
    uart_printf("  imx219 stream-on: rc=%d (MODE_SELECT=0x01)\r\n", rc);
    if (rc != 0) {
        uart_puts("  *** I²C write to IMX219 MODE_SELECT failed.  ***\r\n");
        uart_puts("=== End ===\r\n");
        return 0;
    }
    /* Wait ~50 ms for the sensor to settle. At 30 fps the first
     * SOF appears within 33 ms; one full frame interval gives the
     * sensor's PLL time to lock and the AGC to converge. */
    timer_busy_wait_us(50000u);

    /* Zero the entire descriptor slot first so any RCE-side read
     * of an unset field (pfsd_cfg, prefence, pad) lands as 0. */
    volatile uint32_t *desc_words =
        (volatile uint32_t *)camrtc_vi_req_ring_iova();
    uint32_t slot_words = camrtc_vi_req_request_size() / 4u;
    for (uint32_t i = 0; i < slot_words; i++) desc_words[i] = 0u;
    uintptr_t desc_base = camrtc_vi_req_ring_iova();

    /* Header overlay — sequence + capture_flags + timeouts. */
    volatile struct camrtc_capture_descriptor_header *desc =
        (volatile struct camrtc_capture_descriptor_header *)desc_base;
    desc->sequence                 = 1u;
    desc->capture_flags            = CAPTURE_FLAG_STATUS_REPORT_ENABLE
                                   | CAPTURE_FLAG_ERROR_REPORT_ENABLE;
    /* Cap RCE-side waits at 1500 ms each so STATUS_IND fires
     * within our 2 s IVC poll even on a "no frame" path. The
     * channel default is 5000 ms which is too long for first-
     * light debugging. */
    desc->frame_start_timeout      = 1500u;
    desc->frame_completion_timeout = 1500u;

    /* vi_channel_config overlay — IMX219 binning-mode RAW10
     * (1640×1232) on NVCSI stream 0 / virtual channel 0, written
     * to the frame-buffer carveout as 16-bit-per-pixel (T_R16).
     * Layout: ch_cfg sits at offset 64 of the descriptor (per
     * CAMRTC_DESC_CH_CFG_OFFSET in camrtc_capture.h, which is
     * computed from header(12) + prefence_count(4) + prefence[2]
     * (48) = 64). */
    volatile struct camrtc_vi_channel_config *vi =
        (volatile struct camrtc_vi_channel_config *)
        (desc_base + CAMRTC_DESC_CH_CFG_OFFSET);

    /* Channel selector: match RAW10 datatype (CSI-2 datatype
     * 0x2B = 43) on stream 0 / VC 0. Per L4T `vi5_fops.c:61-78`
     * (capture_template) + `vi5_fops.c:407-412` (per-frame
     * overrides), the masks are NOT zero — they enable matching
     * on the corresponding fields:
     *   stream_mask = 0x3f   (6 NVCSI streams)
     *   vc_mask     = 0xffff (16-bit VC field)
     *   datatype_mask = 0x3f (6-bit CSI-2 datatype field)
     * `match.stream` and `match.vc` use ONE-HOT bit encoding
     * (1 << id). frameid* / dol* stay zero — L4T's template
     * doesn't set them. */
    vi->match.datatype       = 43u;             /* NVCSI_DATATYPE_RAW10 */
    vi->match.datatype_mask  = 0x3fu;
    vi->match.stream         = (uint8_t)(1u << 0); /* one-hot: NVCSI_STREAM_0 */
    vi->match.stream_mask    = 0x3fu;
    vi->match.vc             = (uint16_t)(1u << 0);/* one-hot: NVCSI_VIRTUAL_CHANNEL_0 */
    vi->match.vc_mask        = 0xFFFFu;

    /* Frame geometry — IMX219 binning mode 1640×1232. embed_*
     * = 0 disables embedded-data lines (we don't need sensor
     * metadata). skip + crop = 0 means "emit the full frame". */
    vi->frame.frame_x = (uint16_t)camrtc_frame_buffer_width();
    vi->frame.frame_y = (uint16_t)camrtc_frame_buffer_height();

    /* Pixel formatter — T_R16 (= 196 in L4T `vi5_formats.h:74`)
     * is the standard memory format for any RAW8/10/12 sensor on
     * VI5: each sample stored as a u16 with the upper 10 bits
     * carrying the RAW10 value. pad0_en=0 means "leave the lower
     * 6 bits untouched" — fine for first-light; setting pad0_en=1
     * would zero them. */
    vi->pixfmt_enable          = 1u;
    vi->pixfmt.format          = 196u;       /* TEGRA_IMAGE_FORMAT_T_R16 */
    vi->pixfmt.pad0_en         = 0u;

    /* Atomic packer stride — bytes between rows (= width × 2 for
     * T_R16). The surface IOVA goes in the *memoryinfo* ring,
     * NOT here in vi_channel_config — see below. */
    uintptr_t fb_iova               = camrtc_frame_buffer_iova();
    vi->atomp.surface_stride[0]     = camrtc_frame_buffer_stride();

    /* memoryinfo ring slot 0 — RCE reads the per-surface IOVA +
     * size from here in lock-step with the request_ring slot.
     * Per L4T `vi5_fops.c:416-417`. The ring was zeroed inside
     * camrtc_ch_setup_capture_control. */
    volatile struct camrtc_capture_descriptor_memoryinfo *meminfo =
        (volatile struct camrtc_capture_descriptor_memoryinfo *)
        camrtc_vi_req_meminfo_iova();
    meminfo->surface[0].base_address = (uint64_t)fb_iova;
    meminfo->surface[0].size         = (uint64_t)camrtc_frame_buffer_stride()
                                     * camrtc_frame_buffer_height();

    __asm__ volatile("dsb sy" ::: "memory");

    uart_printf("  vi_channel_config populated: %ux%u T_R16 → "
                "surface[0]=0x%lx stride=%u\r\n",
                (unsigned)camrtc_frame_buffer_width(),
                (unsigned)camrtc_frame_buffer_height(),
                (unsigned long)fb_iova,
                (unsigned)camrtc_frame_buffer_stride());

    uart_printf("  CAPTURE_REQUEST: send buffer_index=0 "
                "(descriptor at 0x%lx)\r\n", (unsigned long)desc_base);
    uint32_t status_index = 0xDEADBEEFu;
    /* Allow up to 2 s for the request: ~33 ms first-frame
     * latency on a streaming sensor, plus headroom for sensor
     * warm-up + CSI training + the 1500 ms RCE-side timeouts to
     * fire if no frame ever arrives. */
    rc = camrtc_capture_request(0u, &status_index, 2000000u);
    uart_printf("  CAPTURE_REQUEST: rc=%d status_buffer_index=0x%x\r\n",
                rc, (unsigned)status_index);

    if (rc == 0) {
        /* STATUS_IND received — RCE has filled the descriptor's
         * `capture_status` substruct at offset 272 with the
         * per-frame outcome. Decode it. */
        volatile const struct camrtc_capture_status *cap_status =
            (volatile const struct camrtc_capture_status *)
            (desc_base + CAMRTC_DESC_STATUS_OFFSET);
        uint32_t code = cap_status->status;
        uart_printf("  capture_status: status=%u frame_id=%u "
                    "src_stream=%u vc=%u\r\n",
                    (unsigned)code,
                    (unsigned)cap_status->frame_id,
                    (unsigned)cap_status->src_stream,
                    (unsigned)cap_status->virtual_channel);
        if (code == CAPTURE_STATUS_SUCCESS) {
            uint64_t sof = cap_status->sof_timestamp;
            uint64_t eof = cap_status->eof_timestamp;
            uart_printf("  sof_ts=0x%lx eof_ts=0x%lx (ticks)\r\n",
                        (unsigned long)sof, (unsigned long)eof);
            uart_puts("  *** CAPTURE SUCCESS — first frame in       ***\r\n");
            uart_printf("  *** buffer at 0x%lx (~%u KB).             ***\r\n",
                        (unsigned long)fb_iova,
                        (unsigned)(camrtc_frame_buffer_size() / 1024u));
        } else {
            uint64_t notify = cap_status->notify_bits;
            uart_printf("  err_data=0x%x flags=0x%x notify_bits=0x%lx\r\n",
                        (unsigned)cap_status->err_data,
                        (unsigned)cap_status->flags,
                        (unsigned long)notify);
            /* Print symbolic name for the most common first-light
             * failure paths (full enum in l4t-camrtc-capture.h:833).
             * Indexed by status code 0..15; SUCCESS is unreachable
             * here (handled in the if-branch above). */
            static const char *const names[] = {
                [CAPTURE_STATUS_UNKNOWN]               = "UNKNOWN",
                [CAPTURE_STATUS_SUCCESS]               = "SUCCESS",
                [CAPTURE_STATUS_CSIMUX_FRAME]          = "CSIMUX_FRAME",
                [CAPTURE_STATUS_CSIMUX_STREAM]         = "CSIMUX_STREAM",
                [CAPTURE_STATUS_CHANSEL_FAULT]         = "CHANSEL_FAULT",
                [CAPTURE_STATUS_CHANSEL_FAULT_FE]      = "CHANSEL_FAULT_FE",
                [CAPTURE_STATUS_CHANSEL_COLLISION]     = "CHANSEL_COLLISION",
                [CAPTURE_STATUS_CHANSEL_SHORT_FRAME]   = "CHANSEL_SHORT_FRAME",
                [CAPTURE_STATUS_ATOMP_PACKER_OVERFLOW] = "ATOMP_PACKER_OVERFLOW",
                [CAPTURE_STATUS_ATOMP_FRAME_TRUNCATED] = "ATOMP_FRAME_TRUNCATED",
                [CAPTURE_STATUS_ATOMP_FRAME_TOSSED]    = "ATOMP_FRAME_TOSSED",
                [CAPTURE_STATUS_ISPBUF_FIFO_OVERFLOW]  = "ISPBUF_FIFO_OVERFLOW",
                [CAPTURE_STATUS_SYNC_FAILURE]          = "SYNC_FAILURE",
                [CAPTURE_STATUS_NOTIFIER_BACKEND_DOWN] = "NOTIFIER_BACKEND_DOWN",
                [CAPTURE_STATUS_FALCON_ERROR]          = "FALCON_ERROR",
                [CAPTURE_STATUS_CHANSEL_NOMATCH]       = "CHANSEL_NOMATCH",
            };
            const char *name = (code < (sizeof(names) / sizeof(names[0])))
                             ? names[code] : "?";
            if (name == NULL) name = "?";
            uart_printf("  *** Capture FAILED — status=%u (%s).       ***\r\n",
                        (unsigned)code, name);
            if (notify & CAPTURE_STATUS_NOTIFY_BIT_FRAME_START_TIMEOUT) {
                uart_puts("  *** notify: FRAME_START_TIMEOUT — sensor    ***\r\n");
                uart_puts("  *** never produced an SOF on CSI-2.         ***\r\n");
                uart_puts("  *** Likely fix: full IMX219 register-bank   ***\r\n");
                uart_puts("  *** init (binning mode + format + AE)       ***\r\n");
                uart_puts("  *** before MODE_SELECT=1.                   ***\r\n");
            } else if (notify & CAPTURE_STATUS_NOTIFY_BIT_FRAME_COMPLETION_TIMEOUT) {
                uart_puts("  *** notify: FRAME_COMPLETION_TIMEOUT —      ***\r\n");
                uart_puts("  *** SOF arrived but EOF didn't.             ***\r\n");
            } else if (notify & CAPTURE_STATUS_NOTIFY_BIT_CHANSEL_NO_MATCH) {
                uart_puts("  *** notify: CHANSEL_NO_MATCH — frame        ***\r\n");
                uart_puts("  *** received but no VI channel selector     ***\r\n");
                uart_puts("  *** matched. Check vi_channel_config.match. ***\r\n");
            }
        }
    } else {
        uart_puts("  *** CAPTURE_REQUEST failed at the IVC layer ***\r\n");
        uart_puts("  *** — see WARN log for details.             ***\r\n");
    }

    /* Stop streaming so the sensor doesn't keep firing CSI
     * frames into a now-stale VI configuration. */
    int stop_rc = imx219_streaming_disable();
    if (stop_rc != 0) {
        uart_printf("  imx219 stream-off: rc=%d (sensor still streaming)\r\n",
                    stop_rc);
    }

    uart_puts("=== End ===\r\n");
    return 0;
}
#endif /* PLATFORM_JETSON_ORIN_NANO */
