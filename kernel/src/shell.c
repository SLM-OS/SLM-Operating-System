/*
 * shell.c - Debug Shell for SLM-OS
 *
 * Minimal command-line interface for system inspection and debugging.
 * Implements line editing, command parsing, and built-in commands.
 */

#include "shell.h"
#include "uart.h"
#include "task.h"
#include "sched.h"
#include "pmm.h"
#include "vmm.h"
#include "smp.h"
#include "ipc.h"
#include "slm_ffi.h"
#include "platform.h"
#include "dtb.h"
#include "elf.h"
#include "vfs.h"
#include "component.h"
#include <stddef.h>

/* ============================================================================
 * Forward declarations for built-in commands
 * ============================================================================ */

static int cmd_help(int argc, char *argv[]);
static int cmd_mem(int argc, char *argv[]);
static int cmd_tasks(int argc, char *argv[]);
static int cmd_cpu(int argc, char *argv[]);
static int cmd_uptime(int argc, char *argv[]);
static int cmd_clear(int argc, char *argv[]);
static int cmd_reboot(int argc, char *argv[]);
static int cmd_vmm(int argc, char *argv[]);
static int cmd_ipc(int argc, char *argv[]);
static int cmd_model(int argc, char *argv[]);
static int cmd_dtb(int argc, char *argv[]);
static int cmd_elftest(int argc, char *argv[]);
static int cmd_run(int argc, char *argv[]);
static int cmd_kill(int argc, char *argv[]);
static int cmd_ls(int argc, char *argv[]);
static int cmd_cat(int argc, char *argv[]);
static int cmd_component(int argc, char *argv[]);

/* ============================================================================
 * Command table
 * ============================================================================ */

static const shell_cmd_t builtin_commands[] = {
    {"help",   cmd_help,   "List available commands"},
    {"mem",    cmd_mem,    "Show memory statistics"},
    {"tasks",  cmd_tasks,  "List all tasks"},
    {"cpu",    cmd_cpu,    "Show CPU status"},
    {"uptime", cmd_uptime, "Show system uptime"},
    {"vmm",    cmd_vmm,    "Show virtual memory info"},
    {"ipc",    cmd_ipc,    "Show IPC statistics"},
    {"model",  cmd_model,  "Show model memory pools"},
    {"dtb",    cmd_dtb,    "Show device tree info"},
    {"elftest", cmd_elftest, "Test ELF loader"},
    {"run",    cmd_run,    "Run a program (run <name>)"},
    {"kill",   cmd_kill,   "Terminate a task by ID"},
    {"ls",     cmd_ls,     "List directory (ls <path>)"},
    {"cat",    cmd_cat,    "Show file contents (cat <path>)"},
    {"component", cmd_component, "Component system (list/register/status)"},
    {"clear",  cmd_clear,  "Clear screen"},
    {"reboot", cmd_reboot, "Restart the system"},
};

#define NUM_BUILTIN_COMMANDS (sizeof(builtin_commands) / sizeof(builtin_commands[0]))

/* External command slots for runtime registration */
#define MAX_EXTERNAL_COMMANDS 16
static shell_cmd_t external_commands[MAX_EXTERNAL_COMMANDS];
static int num_external_commands = 0;

/* ============================================================================
 * Line editing state
 * ============================================================================ */

static char line_buffer[SHELL_MAX_LINE];
static int line_pos = 0;

/* ============================================================================
 * Helper functions
 * ============================================================================ */

/*
 * Simple string comparison.
 */
static int shell_strcmp(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

/*
 * Copy string.
 */
static void shell_strcpy(char *dst, const char *src)
{
    while ((*dst++ = *src++));
}

/*
 * Parse unsigned integer from string.
 * Returns 0 on success, -1 on error.
 */
static int parse_uint(const char *str, uint32_t *out)
{
    if (!str || !*str) return -1;

    uint32_t val = 0;
    while (*str) {
        if (*str < '0' || *str > '9') return -1;
        uint32_t digit = *str - '0';
        /* Check for overflow */
        if (val > (UINT32_MAX - digit) / 10) return -1;
        val = val * 10 + digit;
        str++;
    }
    *out = val;
    return 0;
}

/*
 * Find command by name.
 */
static const shell_cmd_t *find_command(const char *name)
{
    /* Check built-in commands */
    for (size_t i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
        if (shell_strcmp(name, builtin_commands[i].name) == 0) {
            return &builtin_commands[i];
        }
    }

    /* Check external commands */
    for (int i = 0; i < num_external_commands; i++) {
        if (shell_strcmp(name, external_commands[i].name) == 0) {
            return &external_commands[i];
        }
    }

    return NULL;
}

/*
 * Parse line into argc/argv.
 * Modifies the line buffer in place.
 * Returns argc.
 */
static int parse_line(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        /* Skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0') break;

        /* Start of argument */
        argv[argc++] = p;

        /* Find end of argument */
        while (*p && *p != ' ' && *p != '\t') p++;

        /* Null-terminate argument */
        if (*p) {
            *p++ = '\0';
        }
    }

    return argc;
}

/*
 * Read a line from UART with basic line editing.
 * Supports: backspace, enter
 * Returns line length (excluding null terminator).
 */
static int read_line(char *buf, int max_len)
{
    int pos = 0;
    char c;

    while (pos < max_len - 1) {
        c = uart_getc();

        if (c == '\r' || c == '\n') {
            /* Enter pressed */
            uart_puts("\r\n");
            break;
        } else if (c == '\b' || c == 0x7F) {
            /* Backspace or DEL */
            if (pos > 0) {
                pos--;
                uart_puts("\b \b");  /* Erase character on terminal */
            }
        } else if (c == 0x03) {
            /* Ctrl+C - cancel line */
            uart_puts("^C\r\n");
            pos = 0;
            break;
        } else if (c >= 0x20 && c < 0x7F) {
            /* Printable character */
            buf[pos++] = c;
            uart_putc(c);  /* Echo */
        }
        /* Ignore other control characters */
    }

    buf[pos] = '\0';
    return pos;
}

/* ============================================================================
 * Built-in commands
 * ============================================================================ */

/*
 * help - List available commands
 */
static int cmd_help(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uart_puts("Available commands:\r\n");
    uart_puts("\r\n");

    /* Built-in commands */
    for (size_t i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
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
    return 0;
}

/*
 * mem - Show memory statistics
 */
static int cmd_mem(int argc, char *argv[])
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
static int cmd_tasks(int argc, char *argv[])
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
static int cmd_cpu(int argc, char *argv[])
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

    uart_puts("\r\n");
    return 0;
}

/*
 * uptime - Show system uptime
 */
static int cmd_uptime(int argc, char *argv[])
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
static int cmd_clear(int argc, char *argv[])
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
static int cmd_reboot(int argc, char *argv[])
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
static int cmd_vmm(int argc, char *argv[])
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
static int cmd_ipc(int argc, char *argv[])
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
static int cmd_model(int argc, char *argv[])
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
static int cmd_dtb(int argc, char *argv[])
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

/*
 * elftest - Test ELF loader with a minimal handcrafted ELF
 */
static int cmd_elftest(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    uart_puts("ELF Loader Test:\r\n\r\n");

    /* Test 1: Invalid data (not an ELF) */
    uart_puts("  Test 1: Invalid data... ");
    uint8_t not_elf[] = "This is not an ELF file";
    int ret = elf_validate(not_elf, sizeof(not_elf));
    if (ret == ELF_ERR_INVALID) {
        uart_puts("PASS (correctly rejected)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_ERR_INVALID, got %d)\r\n", ret);
    }

    /* Test 2: Truncated file */
    uart_puts("  Test 2: Truncated file... ");
    uint8_t truncated[] = {0x7f, 'E', 'L', 'F'};
    ret = elf_validate(truncated, sizeof(truncated));
    if (ret == ELF_ERR_TRUNCATED) {
        uart_puts("PASS (correctly rejected)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_ERR_TRUNCATED, got %d)\r\n", ret);
    }

    /* Test 3: Minimal valid ELF64 header (wrong arch) */
    uart_puts("  Test 3: Wrong architecture... ");
    uint8_t wrong_arch[64] = {0};
    wrong_arch[0] = 0x7f; wrong_arch[1] = 'E'; wrong_arch[2] = 'L'; wrong_arch[3] = 'F';
    wrong_arch[4] = 2;    /* ELFCLASS64 */
    wrong_arch[5] = 1;    /* ELFDATA2LSB */
    wrong_arch[6] = 1;    /* EV_CURRENT */
    /* e_type at offset 16 */
    wrong_arch[16] = 2;   /* ET_EXEC */
    /* e_machine at offset 18 - x86_64 */
    wrong_arch[18] = 0x3E;
    ret = elf_validate(wrong_arch, sizeof(wrong_arch));
    if (ret == ELF_ERR_ARCH) {
        uart_puts("PASS (correctly rejected)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_ERR_ARCH, got %d)\r\n", ret);
    }

    /* Test 4: Minimal valid ARM64 ELF header */
    uart_puts("  Test 4: Valid ARM64 header... ");
    uint8_t valid_header[64] = {0};
    valid_header[0] = 0x7f; valid_header[1] = 'E'; valid_header[2] = 'L'; valid_header[3] = 'F';
    valid_header[4] = 2;    /* ELFCLASS64 */
    valid_header[5] = 1;    /* ELFDATA2LSB */
    valid_header[6] = 1;    /* EV_CURRENT */
    /* e_type at offset 16 */
    valid_header[16] = 2;   /* ET_EXEC */
    /* e_machine at offset 18 - AARCH64 */
    valid_header[18] = 183; /* EM_AARCH64 */
    /* e_phentsize at offset 54 */
    valid_header[54] = 56;  /* sizeof(Elf64_Phdr) */
    /* e_phnum at offset 56 = 0, so no segments to validate */
    ret = elf_validate(valid_header, sizeof(valid_header));
    if (ret == ELF_OK) {
        uart_puts("PASS (accepted)\r\n");
    } else {
        uart_printf("FAIL (expected ELF_OK, got %d: %s)\r\n", ret, elf_strerror(ret));
    }

    uart_puts("\r\nELF loader validation tests complete.\r\n");
    uart_puts("Note: Full load tests require an actual ELF binary.\r\n");

    return 0;
}

/* ============================================================================
 * Embedded ELF Registry
 *
 * Each entry contains a name and pointer to an embedded ELF binary.
 * Use `run` to list available programs, `run <name>` to execute.
 * ============================================================================ */

typedef struct {
    const char *name;           /* Program name */
    const char *description;    /* Short description */
    const uint8_t *data;        /* Pointer to ELF binary */
    size_t size;                /* Size of ELF binary */
} elf_program_t;

/*
 * Minimal ARM64 ELF binary that just returns.
 *
 * Layout:
 *   0x00-0x3F: ELF64 header (64 bytes)
 *   0x40-0x77: Program header (56 bytes)
 *   0x78-0x7B: Code: ret instruction (4 bytes)
 *
 * Total: 124 bytes
 */
static const uint8_t test_elf_binary[] = {
    /* ELF Header (64 bytes) */
    0x7f, 'E', 'L', 'F',     /* e_ident[0-3]: ELF magic */
    2,                       /* e_ident[4]: ELFCLASS64 */
    1,                       /* e_ident[5]: ELFDATA2LSB (little-endian) */
    1,                       /* e_ident[6]: EV_CURRENT */
    0,                       /* e_ident[7]: ELFOSABI_NONE */
    0, 0, 0, 0, 0, 0, 0, 0,  /* e_ident[8-15]: padding */
    2, 0,                    /* e_type: ET_EXEC */
    0xB7, 0,                 /* e_machine: EM_AARCH64 (183) */
    1, 0, 0, 0,              /* e_version: 1 */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* e_entry: 0x78 (code offset) */
    0x40, 0, 0, 0, 0, 0, 0, 0,  /* e_phoff: 0x40 (program header offset) */
    0, 0, 0, 0, 0, 0, 0, 0,  /* e_shoff: 0 (no section headers) */
    0, 0, 0, 0,              /* e_flags: 0 */
    0x40, 0,                 /* e_ehsize: 64 */
    0x38, 0,                 /* e_phentsize: 56 */
    1, 0,                    /* e_phnum: 1 */
    0, 0,                    /* e_shentsize: 0 */
    0, 0,                    /* e_shnum: 0 */
    0, 0,                    /* e_shstrndx: 0 */

    /* Program Header (56 bytes at offset 0x40) */
    1, 0, 0, 0,              /* p_type: PT_LOAD */
    5, 0, 0, 0,              /* p_flags: PF_R | PF_X */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* p_offset: 0x78 */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* p_vaddr: 0x78 */
    0x78, 0, 0, 0, 0, 0, 0, 0,  /* p_paddr: 0x78 */
    4, 0, 0, 0, 0, 0, 0, 0,  /* p_filesz: 4 */
    4, 0, 0, 0, 0, 0, 0, 0,  /* p_memsz: 4 */
    4, 0, 0, 0, 0, 0, 0, 0,  /* p_align: 4 */

    /* Code (4 bytes at offset 0x78) */
    0xC0, 0x03, 0x5F, 0xD6   /* ret (ARM64: 0xD65F03C0) */
};

/* Registry of embedded ELF programs */
static const elf_program_t elf_programs[] = {
    {"test",    "Minimal ELF that returns immediately", test_elf_binary, sizeof(test_elf_binary)},
    /* Add more embedded ELF programs here */
};

#define NUM_ELF_PROGRAMS (sizeof(elf_programs) / sizeof(elf_programs[0]))

/*
 * Find an ELF program by name.
 */
static const elf_program_t *find_elf_program(const char *name)
{
    for (size_t i = 0; i < NUM_ELF_PROGRAMS; i++) {
        if (shell_strcmp(name, elf_programs[i].name) == 0) {
            return &elf_programs[i];
        }
    }
    return NULL;
}

static int cmd_run(int argc, char *argv[])
{
    /* No arguments: list available programs */
    if (argc < 2) {
        uart_puts("Available programs:\r\n\r\n");
        for (size_t i = 0; i < NUM_ELF_PROGRAMS; i++) {
            uart_printf("  %-12s %s\r\n",
                        elf_programs[i].name,
                        elf_programs[i].description);
        }
        uart_puts("\r\nUsage: run <name>\r\n");
        return 0;
    }

    /* Find the program by name */
    const elf_program_t *prog = find_elf_program(argv[1]);
    if (!prog) {
        uart_printf("Unknown program: %s\r\n", argv[1]);
        uart_puts("Use 'run' to list available programs.\r\n");
        return -1;
    }

    uart_printf("Loading '%s'...\r\n", prog->name);

    /* Load the ELF */
    struct elf_info info;
    int ret = elf_load(prog->data, prog->size, &info);
    if (ret != ELF_OK) {
        uart_printf("  Failed to load ELF: %s\r\n", elf_strerror(ret));
        return -1;
    }

    uart_printf("  Loaded %zu segment(s), entry=0x%lx\r\n",
                info.num_segments, info.entry);

    /*
     * Build argv for the ELF program.
     * argv[0] = program name
     * argv[1..n] = additional arguments from command line
     */
    int elf_argc = argc - 1;  /* Skip "run" */
    char **elf_argv = &argv[1];  /* Points to program name */

    /* Create task from ELF with arguments */
    struct task *task = elf_create_task_with_args(&info, prog->name,
                                                   elf_argc, elf_argv);
    if (!task) {
        uart_puts("  Failed to create task\r\n");
        elf_unload(&info);
        return -1;
    }

    uart_printf("  Created task '%s' (id=%u) with %d arg(s)\r\n",
                prog->name, task->id, elf_argc);

    /* Add to scheduler */
    scheduler_add_task(task);
    uart_puts("  Task added to scheduler\r\n");

    /*
     * Note: ELF segment memory is now automatically freed when the task
     * terminates. The cleanup callback set by elf_create_task_with_args
     * calls elf_unload() to free the segment memory.
     */

    return 0;
}

/*
 * kill <pid> - Terminate a task by ID
 */
static int cmd_kill(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: kill <pid>\r\n");
        uart_puts("  Terminates a task by its process ID.\r\n");
        uart_puts("  Use 'tasks' to see running task IDs.\r\n");
        return -1;
    }

    uint32_t pid;
    if (parse_uint(argv[1], &pid) != 0) {
        uart_printf("Invalid PID: %s\r\n", argv[1]);
        return -1;
    }

    /* Find the task */
    struct task *target = task_get(pid);
    if (!target) {
        uart_printf("No task with PID %lu\r\n", (unsigned long)pid);
        return -1;
    }

    /* Don't allow killing the current task (shell) */
    struct task *current = task_current();
    if (target == current) {
        uart_puts("Cannot kill the current task (shell)\r\n");
        return -1;
    }

    /* Don't allow killing idle tasks (they have special names like "idle" or "idle_0") */
    if (shell_strcmp(target->name, "idle") == 0 ||
        (target->name[0] == 'i' && target->name[1] == 'd' &&
         target->name[2] == 'l' && target->name[3] == 'e' &&
         target->name[4] == '_')) {
        uart_puts("Cannot kill idle tasks\r\n");
        return -1;
    }

    /* Check if already terminated */
    if (target->state == TASK_TERMINATED) {
        uart_printf("Task %lu is already terminated\r\n", (unsigned long)pid);
        return 0;
    }

    uart_printf("Killing task '%s' (pid=%lu)...\r\n", target->name, (unsigned long)pid);

    /* Mark as terminated and remove from scheduler */
    target->state = TASK_TERMINATED;
    scheduler_remove_task(target);

    /* Destroy the task (frees stack) */
    task_destroy(target);

    uart_puts("Task terminated.\r\n");
    return 0;
}

/* ============================================================================
 * VFS Commands (ls, cat)
 * ============================================================================ */

/*
 * Callback for listing directory entries.
 */
static void ls_print_entry(struct vfs_node *node, void *ctx)
{
    (void)ctx;
    if (node->type == VFS_NODE_DIR) {
        uart_printf("  %s/\r\n", node->name);
    } else {
        uart_printf("  %s\r\n", node->name);
    }
}

/*
 * ls <path> - List directory contents
 */
static int cmd_ls(int argc, char *argv[])
{
    const char *path = "/";  /* Default to root */

    if (argc >= 2) {
        path = argv[1];
    }

    struct vfs_node *node = vfs_lookup(path);
    if (!node) {
        uart_printf("ls: %s: No such file or directory\r\n", path);
        return -1;
    }

    if (node->type != VFS_NODE_DIR) {
        /* It's a file, just show its name */
        uart_printf("%s\r\n", node->name);
        return 0;
    }

    /* List directory contents */
    uart_printf("%s:\r\n", path);
    vfs_list(node, ls_print_entry, NULL);

    return 0;
}

/*
 * cat <path> - Show file contents
 */
static int cmd_cat(int argc, char *argv[])
{
    if (argc < 2) {
        uart_puts("Usage: cat <path>\r\n");
        uart_puts("  Show contents of a virtual file.\r\n");
        uart_puts("  Example: cat /sys/memory\r\n");
        return -1;
    }

    const char *path = argv[1];

    struct vfs_node *node = vfs_lookup(path);
    if (!node) {
        uart_printf("cat: %s: No such file or directory\r\n", path);
        return -1;
    }

    if (node->type == VFS_NODE_DIR) {
        uart_printf("cat: %s: Is a directory\r\n", path);
        return -1;
    }

    /* Read file contents */
    char buf[1024];
    int len = vfs_read(node, buf, sizeof(buf) - 1);
    if (len < 0) {
        uart_printf("cat: %s: Read error\r\n", path);
        return -1;
    }

    buf[len] = '\0';

    /* Print contents, converting \n to \r\n */
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(buf[i]);
    }

    /* Ensure newline at end */
    if (len > 0 && buf[len - 1] != '\n') {
        uart_puts("\r\n");
    }

    return 0;
}

/*
 * cmd_component - Component system management.
 */
static int cmd_component(int argc, char *argv[])
{
    if (argc < 2) {
        /* Show help */
        uart_puts("Component System Commands:\r\n");
        uart_puts("  component list      - List all registered components\r\n");
        uart_puts("  component register <name> <version> <type> [priority]\r\n");
        uart_puts("                      - Register a component\r\n");
        uart_puts("                        type: service|driver|application\r\n");
        uart_puts("                        priority: idle|low|normal|high|critical\r\n");
        uart_puts("  component unregister <idx> - Unregister component by index\r\n");
        uart_puts("  component status <name|idx> - Show component details\r\n");
        return 0;
    }

    const char *subcmd = argv[1];

    /* component list */
    if (shell_strcmp(subcmd, "list") == 0) {
        uint32_t count = component_count();
        uart_printf("Registered Components: %u\r\n", count);

        if (count == 0) {
            uart_puts("  (none)\r\n");
            return 0;
        }

        uart_puts("  Idx  Name                 Version   Type        State       Pri\r\n");
        uart_puts("  ---  ----                 -------   ----        -----       ---\r\n");

        for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
            component_info_t info;
            if (component_get_info(i, &info) == 0) {
                uart_printf("  %3u  %-20s %-9s %-11s %-11s %s\r\n",
                    i,
                    (const char *)info.name,
                    (const char *)info.version,
                    component_type_name(info.component_type),
                    component_state_name(info.state),
                    info.priority == COMPONENT_PRIORITY_CRITICAL ? "crit" :
                    info.priority == COMPONENT_PRIORITY_HIGH ? "high" :
                    info.priority == COMPONENT_PRIORITY_LOW ? "low" :
                    info.priority == COMPONENT_PRIORITY_IDLE ? "idle" : "norm");
            }
        }
        return 0;
    }

    /* component register <name> <version> <type> [priority] */
    if (shell_strcmp(subcmd, "register") == 0) {
        if (argc < 5) {
            uart_puts("Usage: component register <name> <version> <type> [priority]\r\n");
            return -1;
        }

        const char *name = argv[2];
        const char *version = argv[3];
        const char *type_str = argv[4];
        const char *prio_str = (argc > 5) ? argv[5] : "normal";

        /* Parse type */
        uint8_t type;
        if (shell_strcmp(type_str, "service") == 0) {
            type = COMPONENT_TYPE_SERVICE;
        } else if (shell_strcmp(type_str, "driver") == 0) {
            type = COMPONENT_TYPE_DRIVER;
        } else if (shell_strcmp(type_str, "application") == 0) {
            type = COMPONENT_TYPE_APPLICATION;
        } else {
            uart_printf("Unknown type: %s\r\n", type_str);
            return -1;
        }

        /* Parse priority */
        uint8_t priority;
        if (shell_strcmp(prio_str, "idle") == 0) {
            priority = COMPONENT_PRIORITY_IDLE;
        } else if (shell_strcmp(prio_str, "low") == 0) {
            priority = COMPONENT_PRIORITY_LOW;
        } else if (shell_strcmp(prio_str, "high") == 0) {
            priority = COMPONENT_PRIORITY_HIGH;
        } else if (shell_strcmp(prio_str, "critical") == 0) {
            priority = COMPONENT_PRIORITY_CRITICAL;
        } else {
            priority = COMPONENT_PRIORITY_NORMAL;
        }

        int idx = component_register(name, version, type, priority);
        if (idx < 0) {
            uart_puts("Failed to register component\r\n");
            return -1;
        }

        uart_printf("Registered component '%s' at index %d\r\n", name, idx);
        return 0;
    }

    /* component unregister <idx> */
    if (shell_strcmp(subcmd, "unregister") == 0) {
        if (argc < 3) {
            uart_puts("Usage: component unregister <idx>\r\n");
            return -1;
        }

        uint32_t idx;
        if (parse_uint(argv[2], &idx) != 0) {
            uart_puts("Invalid index\r\n");
            return -1;
        }
        if (component_unregister(idx) != 0) {
            uart_printf("Failed to unregister component %u\r\n", idx);
            return -1;
        }

        uart_printf("Unregistered component %u\r\n", idx);
        return 0;
    }

    /* component status <name|idx> */
    if (shell_strcmp(subcmd, "status") == 0) {
        if (argc < 3) {
            uart_puts("Usage: component status <name|idx>\r\n");
            return -1;
        }

        const char *arg = argv[2];
        int idx;

        /* Check if numeric */
        if (arg[0] >= '0' && arg[0] <= '9') {
            uint32_t parsed;
            if (parse_uint(arg, &parsed) != 0) {
                uart_puts("Invalid index\r\n");
                return -1;
            }
            idx = (int)parsed;
        } else {
            idx = component_find(arg);
            if (idx < 0) {
                uart_printf("Component '%s' not found\r\n", arg);
                return -1;
            }
        }

        component_info_t info;
        if (component_get_info((uint32_t)idx, &info) != 0) {
            uart_printf("Failed to get info for component %d\r\n", idx);
            return -1;
        }

        uart_printf("Component %d:\r\n", idx);
        uart_printf("  Name:        %s\r\n", (const char *)info.name);
        uart_printf("  Version:     %s\r\n", (const char *)info.version);
        uart_printf("  Type:        %s\r\n", component_type_name(info.component_type));
        uart_printf("  State:       %s\r\n", component_state_name(info.state));
        uart_printf("  Priority:    %u\r\n", info.priority);
        uart_printf("  Task ID:     %u\r\n", info.task_id);
        uart_printf("  Memory:      %u KB\r\n", info.memory_kb);
        uart_printf("  Switches:    %llu\r\n", (unsigned long long)info.switches);
        return 0;
    }

    uart_printf("Unknown subcommand: %s\r\n", subcmd);
    uart_puts("Use 'component' for help.\r\n");
    return -1;
}

/* ============================================================================
 * Shell public API
 * ============================================================================ */

/*
 * Initialize the shell subsystem.
 */
void shell_init(void)
{
    line_pos = 0;
    num_external_commands = 0;

    uart_puts("\r\n");
    uart_puts("SLM-OS Debug Shell\r\n");
    uart_puts("Type 'help' for available commands.\r\n");
    uart_puts("\r\n");
}

/*
 * Register an external command.
 */
int shell_register_command(const shell_cmd_t *cmd)
{
    if (num_external_commands >= MAX_EXTERNAL_COMMANDS) {
        return -1;
    }

    external_commands[num_external_commands++] = *cmd;
    return 0;
}

/*
 * Execute a command string directly.
 */
int shell_execute(const char *cmdline)
{
    char buf[SHELL_MAX_LINE];
    char *argv[SHELL_MAX_ARGS];
    int argc;

    /* Copy to modifiable buffer */
    shell_strcpy(buf, cmdline);

    /* Parse into argc/argv */
    argc = parse_line(buf, argv, SHELL_MAX_ARGS);

    if (argc == 0) {
        return 0;  /* Empty command */
    }

    /* Find and execute command */
    const shell_cmd_t *cmd = find_command(argv[0]);
    if (cmd == NULL) {
        uart_printf("Unknown command: %s\r\n", argv[0]);
        return -1;
    }

    return cmd->handler(argc, argv);
}

/*
 * Shell main loop.
 */
void shell_run(void)
{
    char *argv[SHELL_MAX_ARGS];
    int argc;

    while (1) {
        /* Print prompt */
        uart_puts(SHELL_PROMPT);

        /* Read line */
        int len = read_line(line_buffer, SHELL_MAX_LINE);

        if (len == 0) {
            continue;  /* Empty line */
        }

        /* Parse into argc/argv */
        argc = parse_line(line_buffer, argv, SHELL_MAX_ARGS);

        if (argc == 0) {
            continue;  /* Whitespace only */
        }

        /* Find command */
        const shell_cmd_t *cmd = find_command(argv[0]);
        if (cmd == NULL) {
            uart_printf("Unknown command: %s\r\n", argv[0]);
            uart_puts("Type 'help' for available commands.\r\n");
            continue;
        }

        /* Execute command */
        int ret = cmd->handler(argc, argv);
        if (ret != 0) {
            uart_printf("Command returned error: %d\r\n", ret);
        }
    }
}

/*
 * Shell task entry point.
 */
static void shell_task_entry(void *arg)
{
    (void)arg;

    shell_init();
    shell_run();

    /* Should never reach here */
    task_exit();
}

/*
 * Start the shell task.
 */
void shell_start(void)
{
    /* Use IDLE priority so shell doesn't interfere with tests or real work.
     * Shell still runs when the system is otherwise idle. */
    struct task *t = task_create_with_priority("shell", shell_task_entry, NULL,
                                                TASK_PRIORITY_IDLE);
    if (t == NULL) {
        uart_puts("[SHELL] Failed to create shell task!\r\n");
        return;
    }

    /* Pin to CPU 0 for consistent behavior */
    task_set_affinity(t, 0);

    /* Add to scheduler */
    scheduler_add_task(t);

    uart_puts("[SHELL] Shell task started\r\n");
}
