/*
 * shell.c - Shell core for SLM-OS
 *
 * Command dispatch, line editing, path resolution, and public API.
 * Command handlers are in shell_sys.c, shell_fs.c, shell_exec.c,
 * and shell_component.c.
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
#include "slm_ffi.h"
#include "platform.h"
#include "dtb.h"
#include "elf.h"
#include "vfs.h"
#include "component.h"
#include "littlefs_slm.h"
#include "help.h"
#if defined(PLATFORM_QEMU_VIRT)
#include "net.h"
#endif
#include "lua_slm.h"
#include "string.h"
#include <stddef.h>

/* ============================================================================
 * Command table
 * ============================================================================ */

const shell_cmd_t builtin_commands[] = {
    {"help",   cmd_help,   "List available commands"},
    {"mem",    cmd_mem,    "Show memory statistics"},
    {"tasks",  cmd_tasks,  "List all tasks"},
    {"cpu",    cmd_cpu,    "Show CPU status"},
    {"uptime", cmd_uptime, "Show system uptime"},
    {"vmm",    cmd_vmm,    "Show virtual memory info"},
    {"ipc",    cmd_ipc,    "Show IPC statistics"},
    {"model",  cmd_model,  "Model management (load/list/info/unload/pools)"},
    {"dtb",    cmd_dtb,    "Show device tree info"},
    {"elftest", cmd_elftest, "Test ELF loader"},
    {"run",    cmd_run,    "Run a program (run <name>)"},
    {"kill",   cmd_kill,   "Terminate a task by ID"},
    {"ls",     cmd_ls,     "List directory (ls [path])"},
    {"cd",     cmd_cd,     "Change directory (cd [path])"},
    {"pwd",    cmd_pwd,    "Print working directory"},
    {"cat",    cmd_cat,    "Show file contents (cat <path>)"},
    {"write",  cmd_write,  "Write to file (write <path> <content>)"},
    {"mkdir",  cmd_mkdir,  "Create directory (mkdir <path>)"},
    {"rm",     cmd_rm,     "Remove file/dir (rm <path>)"},
    {"mv",     cmd_mv,     "Move/rename (mv <src> <dst>)"},
    {"df",     cmd_df,     "Filesystem stats (df [path])"},
    {"truncate", cmd_truncate, "Truncate file (truncate <path> <size>)"},
    {"append", cmd_append, "Append to file (append <path> <content>)"},
    {"cp",     cmd_cp,     "Copy file (cp <src> <dst>)"},
    {"touch",  cmd_touch,  "Create empty file (touch <path>)"},
    {"stat",   cmd_stat,   "Show file info (stat <path>)"},
    {"tree",   cmd_tree,   "Recursive directory listing (tree [path])"},
    {"wc",     cmd_wc,     "Count lines/words/bytes (wc <path>)"},
    {"hexdump", cmd_hexdump, "Hex dump file (hexdump <path> [offset] [len])"},
    {"grep",   cmd_grep,   "Search in file (grep <pattern> <path>)"},
    {"find",   cmd_find,   "Find files (find <path> <pattern>)"},
    {"component", cmd_component, "Component system (list/register/status)"},
    {"msg",       cmd_msg,       "Message router (send/list/subscribe)"},
    {"sleep",  cmd_sleep,  "Sleep for N ms (sleep <ms>)"},
    {"bench",  cmd_bench,  "Performance benchmarks (bench <context|irq|ipc|stats|all>)"},
    {"sched",  cmd_sched,  "Scheduler (sched [policy [<name>] | stats])"},
    {"eviction", cmd_eviction, "AI eviction (eviction [policy [<name>] | stats])"},
    {"clear",  cmd_clear,  "Clear screen"},
    {"reboot", cmd_reboot, "Restart the system"},
#if defined(PI5_IRQ_DIAG)
    {"diag",   cmd_diag,   "Pi 5 IRQ-delivery diagnostics (diag <el2|vec|fiq|all>)"},
#endif
#if !defined(PLATFORM_X86_64)
    {"timdiag", cmd_timdiag, "Timer/interrupt delivery diagnostic"},
#endif
};

const int NUM_BUILTIN_COMMANDS = sizeof(builtin_commands) / sizeof(builtin_commands[0]);

/* External command slots for runtime registration */
#define MAX_EXTERNAL_COMMANDS 16
shell_cmd_t external_commands[MAX_EXTERNAL_COMMANDS];
int num_external_commands = 0;

/* ============================================================================
 * Line editing state
 * ============================================================================ */

static char line_buffer[SHELL_MAX_LINE];
static int line_pos = 0;

/* Current working directory */
char shell_cwd[VFS_MAX_PATH] = "/";

/* ============================================================================
 * Helper functions
 * ============================================================================ */

/*
 * Parse unsigned integer from string.
 * Returns 0 on success, -1 on error.
 */
int shell_parse_uint(const char *str, uint32_t *out)
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
 * Resolve a path (relative or absolute) to a canonical absolute path.
 * Handles: relative paths, ".", "..", trailing slashes, double slashes.
 * Returns 0 on success, -1 on error (path too long).
 */
int shell_resolve_path(const char *path, char *out, size_t max_len)
{
    char work[VFS_MAX_PATH];
    size_t work_len = 0;

    if (!path || !out || max_len < 2) {
        return -1;
    }

    /* Empty path means current directory */
    if (*path == '\0') {
        size_t cwd_len = strlen(shell_cwd);
        if (cwd_len >= max_len) return -1;
        strcpy(out, shell_cwd);
        return 0;
    }

    /* Start with cwd for relative paths, empty for absolute */
    if (path[0] != '/') {
        /* Relative path - start with cwd. Do NOT append a separator here;
         * the component-append loop below inserts one as needed. Previously
         * this branch appended '/' eagerly, which combined with the loop's
         * own separator produced "/foo//bar" for cwd=/foo, path=bar. */
        size_t cwd_len = strlen(shell_cwd);
        if (cwd_len >= sizeof(work)) return -1;
        strcpy(work, shell_cwd);
        work_len = cwd_len;
    } else {
        /* Absolute path */
        work[0] = '/';
        work[1] = '\0';
        work_len = 1;
        path++;  /* Skip leading / */
    }

    /* Process each component of the path */
    while (*path) {
        /* Skip leading slashes */
        while (*path == '/') path++;
        if (*path == '\0') break;

        /* Find end of component */
        const char *comp_start = path;
        size_t comp_len = 0;
        while (*path && *path != '/') {
            comp_len++;
            path++;
        }

        /* Handle special components */
        if (comp_len == 1 && comp_start[0] == '.') {
            /* "." - current directory, skip */
            continue;
        } else if (comp_len == 2 && comp_start[0] == '.' && comp_start[1] == '.') {
            /* ".." - parent directory */
            if (work_len > 1) {
                /* Remove trailing slash if present */
                if (work[work_len - 1] == '/') {
                    work_len--;
                }
                /* Find last slash */
                while (work_len > 1 && work[work_len - 1] != '/') {
                    work_len--;
                }
                /* Keep the slash for root, remove for others */
                if (work_len > 1) {
                    work_len--;
                }
                work[work_len] = '\0';
            }
            /* At root, ".." stays at root */
        } else {
            /* Regular component - append */
            /* Ensure there's a separator */
            if (work_len > 1 || (work_len == 1 && work[0] != '/')) {
                if (work_len + 1 >= sizeof(work)) return -1;
                work[work_len++] = '/';
            }
            /* Append component */
            if (work_len + comp_len >= sizeof(work)) return -1;
            for (size_t i = 0; i < comp_len; i++) {
                work[work_len++] = comp_start[i];
            }
            work[work_len] = '\0';
        }
    }

    /* Ensure we have at least "/" */
    if (work_len == 0) {
        work[0] = '/';
        work[1] = '\0';
        work_len = 1;
    }

    /* Copy to output */
    if (work_len >= max_len) return -1;
    strcpy(out, work);
    return 0;
}

/*
 * Find command by name.
 */
static const shell_cmd_t *find_command(const char *name)
{
    /* Check built-in commands */
    for (int i = 0; i < NUM_BUILTIN_COMMANDS; i++) {
        if (strcmp(name, builtin_commands[i].name) == 0) {
            return &builtin_commands[i];
        }
    }

    /* Check external commands */
    for (int i = 0; i < num_external_commands; i++) {
        if (strcmp(name, external_commands[i].name) == 0) {
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
int shell_read_line(char *buf, int max_len)
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
 * Shell public API
 * ============================================================================ */

/*
 * Initialize the shell subsystem.
 */
void shell_init(void)
{
    line_pos = 0;
    num_external_commands = 0;

#if defined(PLATFORM_QEMU_VIRT)
    /* Register network commands (ping, ifconfig, netstat) */
    net_shell_init();
#endif

    /* Register Lua scripting command */
    lua_shell_init();

    /* Register platform-specific commands */
#if defined(PLATFORM_X86_64)
    {
        extern void pci_register_shell_commands(void);
        extern void nvidia_gpu_register_shell_commands(void);
        pci_register_shell_commands();
        nvidia_gpu_register_shell_commands();
    }
#endif

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

    /* Copy to modifiable buffer (bounded to prevent stack overflow) */
    size_t len = strlen(cmdline);
    if (len >= SHELL_MAX_LINE) {
        uart_printf("Command too long (%u chars, max %d)\r\n",
                    (unsigned)len, SHELL_MAX_LINE - 1);
        return -1;
    }
    strcpy(buf, cmdline);

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
        int len = shell_read_line(line_buffer, SHELL_MAX_LINE);

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
