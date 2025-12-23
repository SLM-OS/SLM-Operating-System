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
#include "smp.h"
#include "ipc.h"
#include "platform.h"
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

/* ============================================================================
 * Command table
 * ============================================================================ */

static const shell_cmd_t builtin_commands[] = {
    {"help",   cmd_help,   "List available commands"},
    {"mem",    cmd_mem,    "Show memory statistics"},
    {"tasks",  cmd_tasks,  "List all tasks"},
    {"cpu",    cmd_cpu,    "Show CPU status"},
    {"uptime", cmd_uptime, "Show system uptime"},
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
