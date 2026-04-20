/*
 * shell.c - Shell core for SLM-OS
 *
 * Command dispatch, line editing, path resolution, and public API.
 * Command handlers are in shell_sys.c, shell_fs.c, shell_exec.c,
 * and shell_component.c.
 */

#include "shell.h"
#include "shell_internal.h"
#include "shell_io.h"
#include "shell_session.h"
#include "pi_mutex.h"
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
#if defined(ENABLE_NETWORKING)
#include "net.h"
#endif
#include "lua_slm.h"
#include "string.h"
#include <stddef.h>

/* ============================================================================
 * Command table
 * ============================================================================ */

const shell_cmd_t builtin_commands[] = {
    {"help",   cmd_help,   "List available commands", false},
    {"mem",    cmd_mem,    "Show memory statistics", false},
    {"tasks",  cmd_tasks,  "List all tasks", false},
    {"cpu",    cmd_cpu,    "Show CPU status", false},
    {"uptime", cmd_uptime, "Show system uptime", false},
    {"vmm",    cmd_vmm,    "Show virtual memory info", false},
    {"ipc",    cmd_ipc,    "Show IPC statistics", false},
    {"model",  cmd_model,  "Model management (load/list/info/unload/pools)", true},
    {"dtb",    cmd_dtb,    "Show device tree info", false},
#if !defined(PLATFORM_X86_64)
    /* x86-64 registers a richer `gpu` command via
     * nvidia_gpu_register_shell_commands() with init/sec2/vram/regs
     * subcommands. find_command() checks built-ins before externals,
     * so registering this generic info-only built-in on x86-64 would
     * permanently shadow the NVIDIA dispatcher (the bug that hid
     * `gpu init` from PR #268's kexec-inheritance experiment until
     * this entry was guarded). On Jetson the built-in still carries
     * `gpu read <hex-offset>` for the integrated GA10B aperture. */
    {"gpu",    cmd_gpu,    "Show GPU info (gpu [read <hex-offset>])", false},
#endif
    {"peek",   cmd_peek,   "Read physical memory (peek <phys-hex> [count])", false},
    {"poke",   cmd_poke,   "Write 32-bit word (poke <phys-hex> <val-hex>)", true},
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"nvgpu",  cmd_nvgpu,  "Jetson nvgpu bringup (nvgpu <prepare|run|info>)", true},
    {"xhci",   cmd_xhci,   "Show Tegra XHCI controller info (#266 Phase 3A)", false},
#endif
    {"elftest", cmd_elftest, "Test ELF loader", true},
    {"run",    cmd_run,    "Run a program (run <name>)", true},
    {"kill",   cmd_kill,   "Terminate a task by ID", true},
    {"ls",     cmd_ls,     "List directory (ls [path])", false},
    {"cd",     cmd_cd,     "Change directory (cd [path])", false},  /* per-session cwd only */
    {"pwd",    cmd_pwd,    "Print working directory", false},
    {"cat",    cmd_cat,    "Show file contents (cat <path>)", false},
    {"write",  cmd_write,  "Write to file (write <path> <content>)", false},  /* VFS locks internally */
    {"mkdir",  cmd_mkdir,  "Create directory (mkdir <path>)", false},
    {"rm",     cmd_rm,     "Remove file/dir (rm <path>)", false},
    {"mv",     cmd_mv,     "Move/rename (mv <src> <dst>)", false},
    {"df",     cmd_df,     "Filesystem stats (df [path])", false},
    {"truncate", cmd_truncate, "Truncate file (truncate <path> <size>)", false},
    {"append", cmd_append, "Append to file (append <path> <content>)", false},
    {"cp",     cmd_cp,     "Copy file (cp <src> <dst>)", false},
    {"touch",  cmd_touch,  "Create empty file (touch <path>)", false},
    {"stat",   cmd_stat,   "Show file info (stat <path>)", false},
    {"tree",   cmd_tree,   "Recursive directory listing (tree [path])", false},
    {"wc",     cmd_wc,     "Count lines/words/bytes (wc <path>)", false},
    {"hexdump", cmd_hexdump, "Hex dump file (hexdump <path> [offset] [len])", false},
    {"grep",   cmd_grep,   "Search in file (grep <pattern> <path>)", false},
    {"find",   cmd_find,   "Find files (find <path> <pattern>)", false},
    {"component", cmd_component, "Component system (list/register/status)", true},
    {"msg",       cmd_msg,       "Message router (send/list/subscribe)", true},
    {"sleep",  cmd_sleep,  "Sleep for N ms (sleep <ms>)", false},
    {"bench",  cmd_bench,  "Performance benchmarks (bench <context|irq|ipc|stats|all>)", true},
    {"sched",  cmd_sched,  "Scheduler (sched [policy [<name>] | stats])", true},
    {"eviction", cmd_eviction, "AI eviction (eviction [policy [<name>] | stats])", true},
    {"top",    cmd_top,    "Live dashboard (top [-n <iter>] [refresh_secs])", false},
    {"clear",  cmd_clear,  "Clear screen", false},
    {"reboot", cmd_reboot, "Restart the system", true},
#if defined(PI5_IRQ_DIAG)
    {"diag",   cmd_diag,   "Pi 5 IRQ-delivery diagnostics (diag <el2|vec|fiq|all>)", false},
#endif
#if !defined(PLATFORM_X86_64)
    {"timdiag", cmd_timdiag, "Timer/interrupt delivery diagnostic", false},
#endif
#if defined(PLATFORM_RASPI5) && defined(ENABLE_NETWORKING)
    {"macbdiag", cmd_macbdiag, "MACB IRQ delivery diagnostic", false},
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)
    {"rtldiag",  cmd_rtldiag,  "RTL8168 PCIe probe diagnostic", false},
    {"xhcidiag", cmd_xhcidiag, "Tegra XHCI CBB-at-EL2 probe", false},
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"hspdiag",  cmd_hspdiag,  "HSP dimensioning + BPMP doorbell probe", false},
    {"bpmp",     cmd_bpmp,     "BPMP IPC smoke test (PING + clock query)", false},
#endif
};

const int NUM_BUILTIN_COMMANDS = sizeof(builtin_commands) / sizeof(builtin_commands[0]);

/* External command slots for runtime registration */
#define MAX_EXTERNAL_COMMANDS 16
shell_cmd_t external_commands[MAX_EXTERNAL_COMMANDS];
int num_external_commands = 0;

/* Serializes mutating commands across concurrent shell sessions. A
 * priority-inheriting mutex is correct here because (a) a slow mutating
 * command should not block preemption on the whole system, and (b) when
 * a high-priority shell task waits on a command from a lower-priority
 * task, we want PI to avoid deadline inversion. Commands marked
 * `.mutates = false` bypass the lock entirely. */
static pi_mutex_t shell_mutex = PI_MUTEX_INIT;

/* ============================================================================
 * Per-session state notes
 * ============================================================================ */

/* The current working directory lives on the shell_session now — see
 * shell_session_current()->cwd. shell_resolve_path and cwd-reading /
 * cwd-mutating commands route through it. The REPL's line buffer is a
 * stack local inside shell_run() so two concurrent sessions can read
 * input independently — it must not be a file-level static. */

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

    /* Resolve relative paths against the current session's cwd. Falls
     * back to "/" if there is no session bound (e.g. early boot /
     * unit-test init before shell_session_init). */
    struct shell_session *sess = shell_session_current();
    const char *cwd = (sess && sess->cwd[0]) ? sess->cwd : "/";

    /* Empty path means current directory */
    if (*path == '\0') {
        size_t cwd_len = strlen(cwd);
        if (cwd_len >= max_len) return -1;
        strcpy(out, cwd);
        return 0;
    }

    /* Start with cwd for relative paths, empty for absolute */
    if (path[0] != '/') {
        /* Relative path - start with cwd. Do NOT append a separator here;
         * the component-append loop below inserts one as needed. Previously
         * this branch appended '/' eagerly, which combined with the loop's
         * own separator produced "/foo//bar" for cwd=/foo, path=bar. */
        size_t cwd_len = strlen(cwd);
        if (cwd_len >= sizeof(work)) return -1;
        strcpy(work, cwd);
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
 * Dispatch a command, acquiring the shell mutex around mutating
 * ones. Returns whatever the handler returns.
 *
 * Non-recursive: if the calling task already holds shell_mutex (this
 * happens when a mutating handler like `lua` dispatches another
 * command via shell_execute — e.g. slm.exec / slm.model_preload),
 * skip the acquire. Serialization is still preserved because the
 * outer lock is held for the whole duration; a recursive lock would
 * deadlock since pi_mutex_lock is not re-entrant.
 */
static int dispatch_cmd(const shell_cmd_t *cmd, int argc, char *argv[])
{
    if (!cmd->mutates || pi_mutex_held_by_self(&shell_mutex)) {
        return cmd->handler(argc, argv);
    }
    pi_mutex_lock(&shell_mutex);
    int ret = cmd->handler(argc, argv);
    pi_mutex_unlock(&shell_mutex);
    return ret;
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
 * Read a line from the current session's I/O with basic line editing.
 * Supports: backspace, enter, Ctrl+C
 * Returns line length (excluding null terminator), or -1 if the
 * session has closed (peer disconnected).
 */
int shell_read_line(char *buf, int max_len)
{
    struct shell_session *s = shell_session_current();
    if (!s || !s->io) {
        buf[0] = '\0';
        return -1;
    }
    struct shell_io *io = s->io;

    int pos = 0;
    for (;;) {
        if (pos >= max_len - 1) {
            break;
        }
        int ch = io->read_char(io);
        if (ch < 0) {
            /* EOF / closed */
            buf[pos] = '\0';
            return -1;
        }
        char c = (char)ch;

        if (c == '\r' || c == '\n') {
            /* Enter pressed */
            io->write(io, "\r\n", 2);
            break;
        } else if (c == '\b' || c == 0x7F) {
            /* Backspace or DEL */
            if (pos > 0) {
                pos--;
                io->write(io, "\b \b", 3);
            }
        } else if (c == 0x03) {
            /* Ctrl+C - cancel line */
            io->write(io, "^C\r\n", 4);
            pos = 0;
            break;
        } else if (c >= 0x20 && c < 0x7F) {
            /* Printable character */
            buf[pos++] = c;
            io->write(io, &c, 1);  /* Echo */
        }
        /* Ignore other control characters */
    }

    buf[pos] = '\0';
    return pos;
}

/* ============================================================================
 * Per-session I/O wrappers
 * ============================================================================ */

void shell_puts(const char *s)
{
    struct shell_session *sess = shell_session_current();
    if (sess && sess->io) {
        shell_io_puts(sess->io, s);
    } else {
        /* Early boot or unbound task — fall back to UART. */
        uart_puts(s);
    }
}

void shell_putc(char c)
{
    struct shell_session *sess = shell_session_current();
    if (sess && sess->io) {
        sess->io->write(sess->io, &c, 1);
    } else {
        uart_putc(c);
    }
}

int shell_printf(const char *fmt, ...)
{
    struct shell_session *sess = shell_session_current();
    va_list args;
    va_start(args, fmt);
    int n;
    if (sess && sess->io) {
        n = shell_io_vprintf(sess->io, fmt, args);
    } else {
        n = uart_vprintf(fmt, args);
    }
    va_end(args);
    return n;
}

int shell_getc(void)
{
    struct shell_session *sess = shell_session_current();
    if (sess && sess->io) {
        return sess->io->read_char(sess->io);
    }
    return (int)(unsigned char)uart_getc();
}

int shell_try_getc(void)
{
    struct shell_session *sess = shell_session_current();
    if (sess && sess->io) {
        return sess->io->try_read_char(sess->io);
    }
    return uart_try_getc();
}

/* ============================================================================
 * Shell public API
 * ============================================================================ */

/*
 * Initialize the shell subsystem.
 */
void shell_init(void)
{
    num_external_commands = 0;

    /* Ensure the console session exists before any shell_* wrapper
     * is called by command registration or banner output. */
    shell_session_init();

#if defined(ENABLE_NETWORKING)
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
#else
    {
        extern void hailo_register_shell_commands(void);
        hailo_register_shell_commands();
    }
#endif

    /* #64: boot-time model preloading from /mnt/files/preload.conf.
     * Must run after the scheduler is up (we're in the shell task).
     * Skip during boot-tests — tests expect an empty model registry. */
#if !defined(ENABLE_BOOT_TESTS)
    {
        extern void model_boot_preload(void);
        model_boot_preload();
    }
#endif

#if defined(ENABLE_NETWORKING) && !defined(ENABLE_BOOT_TESTS)
    /* Phase 3: bring telnetd up automatically if /etc/telnetd.conf
     * says to, or if NET_TELNETD_AUTOSTART was set at build time.
     * Skipped in the boot-test image — tests spawn their own nets. */
    {
        extern void telnetd_autostart(void);
        telnetd_autostart();
    }
#endif

    shell_puts("\r\n");
    shell_puts("SLM-OS Debug Shell\r\n");
    shell_puts("Type 'help' for available commands.\r\n");
    shell_puts("\r\n");
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
        shell_printf("Command too long (%u chars, max %d)\r\n",
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
        shell_printf("Unknown command: %s\r\n", argv[0]);
        return -1;
    }

    return dispatch_cmd(cmd, argc, argv);
}

/*
 * Shell main loop.
 */
void shell_run(void)
{
    /* Stack-local line buffer so two concurrent sessions (console +
     * TCP) don't corrupt each other's in-progress input. 1024 bytes
     * + argv is well within the 64 KB task stack. */
    char line_buffer[SHELL_MAX_LINE];
    char *argv[SHELL_MAX_ARGS];
    int argc;

    while (1) {
        /* Print prompt */
        shell_puts(SHELL_PROMPT);

        /* Read line */
        int len = shell_read_line(line_buffer, SHELL_MAX_LINE);

        if (len < 0) {
            /* Session closed (peer disconnect). End the REPL. */
            break;
        }
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
            shell_printf("Unknown command: %s\r\n", argv[0]);
            shell_puts("Type 'help' for available commands.\r\n");
            continue;
        }

        /* Execute command (serialized if .mutates) */
        int ret = dispatch_cmd(cmd, argc, argv);
        if (ret != 0) {
            shell_printf("Command returned error: %d\r\n", ret);
        }
    }
}

/*
 * Shell task entry point.
 */
static void shell_task_entry(void *arg)
{
    (void)arg;

    /* Bind this task to the console session so shell_puts / shell_printf
     * route through shell_io_uart for the duration of the REPL.
     * shell_session_console() lazily initializes the session on first
     * call; shell_init() below ensures explicit init too. */
    shell_session_bind(task_current(), shell_session_console());

    shell_init();
    shell_run();

    /* If shell_run() returns, the session closed — normally unreachable
     * for the console session. */
    shell_session_unbind(task_current());
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

    /* Kernel log — always goes to the physical console. */
    uart_puts("[SHELL] Shell task started\r\n");
}
