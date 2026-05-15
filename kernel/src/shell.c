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
#include "blob_autoload.h"
#include "littlefs_slm.h"
#include "help.h"
#if defined(ENABLE_NETWORKING)
#include "net.h"
#endif
#include "lua_slm.h"
#include "string.h"
#include <stddef.h>

/* The line-edit loop in shell_read_command auto-submits the line once
 * `pos >= max_len - 1` — same behavior as shell_read_line, but the
 * recall path can fill the buffer in a single keystroke. Pinning that
 * SHELL_MAX_LINE strictly exceeds SHELL_HISTORY_LINE_MAX makes the
 * recall always fit with at least one byte of headroom for further
 * editing, so an arrow-key recall can never wedge the loop into the
 * "auto-submit before the user can press Enter" path. If
 * SHELL_HISTORY_LINE_MAX ever grows past SHELL_MAX_LINE this assert
 * fires before the next user discovers it interactively. */
static_assert(SHELL_MAX_LINE > SHELL_HISTORY_LINE_MAX,
              "SHELL_MAX_LINE must strictly exceed SHELL_HISTORY_LINE_MAX so a"
              " recalled history entry leaves room for further edits");

/* ============================================================================
 * Command table
 *
 * **CONVENTION (enforced by reviewers, mirrored by `cmd_help` output):**
 *
 *   1. Group entries by `category`. Categories appear in the order
 *      defined by `shell_cmd_category_t` in shell.h.
 *   2. **Alphabetize by `name` within each category.**
 *   3. Each category gets a single-line separator comment of the form
 *      "--- Category Name ---" (see below) so a human can scan the
 *      list in the same order the `help` output produces.
 *
 * Adding a new command:
 *   - Pick the closest category (see shell.h for the enum and what
 *     each value covers).
 *   - Insert it in strict alphabetical order within that category's
 *     block. Conditional (`#if`-gated) entries are interleaved by
 *     name like everything else — the gates go inline per-entry, not
 *     in a "conditional block at the bottom".
 *
 * `cmd_help` groups + alphabetizes the output at runtime regardless,
 * so violating this rule will not break user-visible behaviour — but
 * it does defeat the point of having the source layout match the
 * help output, and the test
 * `test_builtin_commands_grouped_and_sorted` (kernel/tests/test_shell.c)
 * fires when the source drifts out of compliance.
 *
 * External commands registered via `shell_register_command` (lua,
 * net, hailo, kernel, gpu/pci on x86-64) follow the same convention
 * in their respective registration files.
 * ============================================================================ */

const shell_cmd_t builtin_commands[] = {
    /* --- Shell session --- */
    {"clear",  cmd_clear,  "Clear screen",                                       false, SHELL_CAT_SHELL},
    {"help",   cmd_help,   "List available commands",                            false, SHELL_CAT_SHELL},
    {"reboot", cmd_reboot, "Restart the system",                                 true,  SHELL_CAT_SHELL},

    /* --- Filesystem --- */
    {"append",   cmd_append,   "Append to file (append <path> <content>)",          false, SHELL_CAT_FILESYSTEM},
    {"cat",      cmd_cat,      "Show file contents (cat <path>)",                   false, SHELL_CAT_FILESYSTEM},
    {"cd",       cmd_cd,       "Change directory (cd [path])",                      false, SHELL_CAT_FILESYSTEM},  /* per-session cwd only */
    {"cp",       cmd_cp,       "Copy file (cp <src> <dst>)",                        false, SHELL_CAT_FILESYSTEM},
    {"df",       cmd_df,       "Filesystem stats (df [path])",                      false, SHELL_CAT_FILESYSTEM},
    {"find",     cmd_find,     "Find files (find <path> <pattern>)",                false, SHELL_CAT_FILESYSTEM},
    {"grep",     cmd_grep,     "Search in file (grep <pattern> <path>)",            false, SHELL_CAT_FILESYSTEM},
    {"hexdump",  cmd_hexdump,  "Hex dump file (hexdump <path> [offset] [len])",     false, SHELL_CAT_FILESYSTEM},
    {"ls",       cmd_ls,       "List directory (ls [path])",                        false, SHELL_CAT_FILESYSTEM},
    {"mkdir",    cmd_mkdir,    "Create directory (mkdir <path>)",                   false, SHELL_CAT_FILESYSTEM},
    {"mv",       cmd_mv,       "Move/rename (mv <src> <dst>)",                      false, SHELL_CAT_FILESYSTEM},
    {"put",      cmd_put,      "Write binary hex to file (put [-a] <path> <hex>)",  false, SHELL_CAT_FILESYSTEM},
    {"pwd",      cmd_pwd,      "Print working directory",                           false, SHELL_CAT_FILESYSTEM},
    {"rm",       cmd_rm,       "Remove file/dir (rm <path>)",                       false, SHELL_CAT_FILESYSTEM},
    {"stat",     cmd_stat,     "Show file info (stat <path>)",                      false, SHELL_CAT_FILESYSTEM},
    {"touch",    cmd_touch,    "Create empty file (touch <path>)",                  false, SHELL_CAT_FILESYSTEM},
    {"tree",     cmd_tree,     "Recursive directory listing (tree [path])",         false, SHELL_CAT_FILESYSTEM},
    {"truncate", cmd_truncate, "Truncate file (truncate <path> <size>)",            false, SHELL_CAT_FILESYSTEM},
    {"wc",       cmd_wc,       "Count lines/words/bytes (wc <path>)",               false, SHELL_CAT_FILESYSTEM},
    {"write",    cmd_write,    "Write to file (write <path> <content>)",            false, SHELL_CAT_FILESYSTEM},  /* VFS locks internally */
    {"xget-bin", cmd_xget_bin, "Direct binary download (xget-bin <path> [skip])",      false, SHELL_CAT_FILESYSTEM},
    {"xput",     cmd_xput,     "Framed upload (xput begin|chunk|status|finish|abort)", false, SHELL_CAT_FILESYSTEM},
    {"xput-bin", cmd_xput_bin, "Direct binary upload (xput-bin <path> <total>)",       false, SHELL_CAT_FILESYSTEM},

    /* --- System info --- */
    {"canary",    cmd_canary,    "Check task stack canaries (#601 Bug B diagnostic)",  false, SHELL_CAT_SYSINFO},
    {"cpu",       cmd_cpu,       "Show CPU status",                                    false, SHELL_CAT_SYSINFO},
    {"dtb",       cmd_dtb,       "Show device tree info",                              false, SHELL_CAT_SYSINFO},
    {"ipc",       cmd_ipc,       "Show IPC statistics",                                false, SHELL_CAT_SYSINFO},
    {"mem",       cmd_mem,       "Show memory statistics",                             false, SHELL_CAT_SYSINFO},
    {"telemetry", cmd_telemetry, "Admin telemetry feed (telemetry [stats|list-topics])", false, SHELL_CAT_SYSINFO},
    {"top",       cmd_top,       "Live dashboard (top [-n <iter>] [refresh_secs])",    false, SHELL_CAT_SYSINFO},
    {"uptime",    cmd_uptime,    "Show system uptime",                                 false, SHELL_CAT_SYSINFO},
    {"vmm",       cmd_vmm,       "Show virtual memory info",                           false, SHELL_CAT_SYSINFO},

    /* --- Process / scheduling / AI runtime --- */
    {"bench",    cmd_bench,    "Performance benchmarks (bench <context|irq|ipc|stats|all>)", true, SHELL_CAT_PROCESS},
    {"eviction", cmd_eviction, "AI eviction (eviction [policy [<name>] | stats])",          true, SHELL_CAT_PROCESS},
    {"kill",     cmd_kill,     "Terminate a task by ID",                                    true, SHELL_CAT_PROCESS},
#if !defined(PLATFORM_X86_64)
    {"mmaptest", cmd_mmaptest, "Run the EL0 sys_mmap/munmap smoke task (mmap follow-up)",   false, SHELL_CAT_PROCESS},
#endif
    {"model",    cmd_model,    "Model management (load/list/info/unload/swap/pools)",       true, SHELL_CAT_PROCESS},
    {"sched",    cmd_sched,    "Scheduler (sched [policy [<name>] | model ... | stats])",   true, SHELL_CAT_PROCESS},
    {"sleep",    cmd_sleep,    "Sleep for N ms (sleep <ms>)",                               false, SHELL_CAT_PROCESS},
    {"slm",      cmd_slm,      "Small language model (load/list/info/launch/prompt/stats)", true, SHELL_CAT_PROCESS},
    {"tasks",    cmd_tasks,    "List all tasks",                                            false, SHELL_CAT_PROCESS},
#if !defined(PLATFORM_X86_64)
    {"userelf",  cmd_userelf,  "Run the embedded EL0 hello ELF (ELF loader follow-up)",     false, SHELL_CAT_PROCESS},
    {"usertest", cmd_usertest, "Run the EL0 smoke task (#697 PR-4)",                        false, SHELL_CAT_PROCESS},
#endif

    /* --- Components & message router --- */
    {"component", cmd_component, "Component system (list/register/status)",                  true, SHELL_CAT_COMPONENTS},
    {"msg",       cmd_msg,       "Message router (send/list/subscribe)",                     true, SHELL_CAT_COMPONENTS},

    /* --- Scripting & programs --- */
    {"elftest", cmd_elftest, "Test ELF loader",                                              true, SHELL_CAT_SCRIPTING},
    {"run",     cmd_run,     "Run a program (run <name>)",                                   true, SHELL_CAT_SCRIPTING},

    /* --- Hardware control & diagnostics ---
     * Alphabetized strictly by name, with #if gates inline per-entry. The
     * test_builtin_commands_grouped_and_sorted regression test enforces
     * this ordering. */
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"bpmp",      cmd_bpmp,      "BPMP IPC smoke test (PING + clock query)",                  false, SHELL_CAT_HARDWARE},
#endif
#if defined(PI5_IRQ_DIAG)
    {"diag",      cmd_diag,      "Pi 5 IRQ-delivery diagnostics (diag <el2|vec|fiq|all>)",    false, SHELL_CAT_HARDWARE},
#endif
    {"dtb-dump",  cmd_dtb_dump,  "Dump firmware-passed DTB as hex (#414 investigation)",      false, SHELL_CAT_HARDWARE},
#if defined(PLATFORM_RASPI5)
    {"emmc-bringup", cmd_emmc_bringup, "Run Pi 5 SDHCI bring-up on demand (diag for #414)",   true,  SHELL_CAT_HARDWARE},
#endif
#if !defined(PLATFORM_X86_64)
    /* x86-64 registers a richer `gpu` command via
     * nvidia_gpu_register_shell_commands() with init/sec2/vram/regs
     * subcommands. find_command() checks built-ins before externals,
     * so registering this generic info-only built-in on x86-64 would
     * permanently shadow the NVIDIA dispatcher (the bug that hid
     * `gpu init` from PR #268's kexec-inheritance experiment until
     * this entry was guarded). On Jetson the built-in still carries
     * `gpu read <hex-offset>` for the integrated GA10B aperture. */
    {"gpu",       cmd_gpu,       "Show GPU info (gpu [read <hex-offset>])",                   false, SHELL_CAT_HARDWARE},
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"hspdiag",   cmd_hspdiag,   "HSP dimensioning + BPMP doorbell probe",                    false, SHELL_CAT_HARDWARE},
    {"imx219",    cmd_imx219,    "Read IMX219 CHIP_ID via cam_i2c (expect 0x0219)",           false, SHELL_CAT_HARDWARE},
#endif
#if defined(PLATFORM_RASPI5) && defined(ENABLE_NETWORKING)
    {"macbdiag",  cmd_macbdiag,  "MACB IRQ delivery diagnostic",                              false, SHELL_CAT_HARDWARE},
#endif
#if defined(PLATFORM_RASPI5)
    {"mboxclk",   cmd_mboxclk,   "Probe Pi firmware clocks (mboxclk [<id> [on|off]])",        true,  SHELL_CAT_HARDWARE},
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"nvcsi",     cmd_nvcsi,     "Bring up NVCSI receiver for IMX219-A and dump intr status", false, SHELL_CAT_HARDWARE},
    {"nvgpu",     cmd_nvgpu,     "Jetson nvgpu bringup (nvgpu <prepare|run|info>)",           true,  SHELL_CAT_HARDWARE},
    {"pcietrain", cmd_pcietrain, "Tegra PCIe C8 host init + link train + EP probe",           false, SHELL_CAT_HARDWARE},
#endif
    {"peek",      cmd_peek,      "Read physical memory (peek <phys-hex> [count])",            false, SHELL_CAT_HARDWARE},
    {"poke",      cmd_poke,      "Write 32-bit word (poke <phys-hex> <val-hex>)",             true,  SHELL_CAT_HARDWARE},
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"rcediag",   cmd_rcediag,   "Camera RTCPU (RCE) HSP-VM HELLO+PROTOCOL+RESUME handshake", false, SHELL_CAT_HARDWARE},
    {"csidiag",   cmd_csidiag,   "Open NVCSI port A via CAPTURE_PHY_STREAM_OPEN (RCE IVC)",   false, SHELL_CAT_HARDWARE},
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)
    {"rtldiag",   cmd_rtldiag,   "RTL8168 PCIe probe diagnostic",                             false, SHELL_CAT_HARDWARE},
#endif
#if !defined(PLATFORM_X86_64)
    {"timdiag",   cmd_timdiag,   "Timer/interrupt delivery diagnostic",                       false, SHELL_CAT_HARDWARE},
#if defined(PLATFORM_RASPI5) && defined(PI5_IRQ_DIAG)
    {"irqtest",   cmd_irqtest,   "Briefly unmask DAIF.I + check if IRQ vector fires (Pi 5)",  false, SHELL_CAT_HARDWARE},
#endif
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO)
    {"xhci",      cmd_xhci,      "Show Tegra XHCI controller info (#266 Phase 3A)",           false, SHELL_CAT_HARDWARE},
#endif
#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)
    {"xhcidiag",  cmd_xhcidiag,  "Tegra XHCI CBB-at-EL2 probe",                               false, SHELL_CAT_HARDWARE},
    {"cdcdiag",   cmd_cdcdiag,   "CDC-ECM TX path diagnostic (#427)",                         false, SHELL_CAT_HARDWARE},
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

/*
 * REPL line reader (#434). Same per-byte editing rules as
 * shell_read_line but with two extras: it emits the prompt itself
 * (so the caller does not double-print), and it runs an ESC-sequence
 * parser that turns "ESC [ A" / "ESC [ B" into shell_history_prev /
 * shell_history_next calls. A bare ESC followed by an unknown byte —
 * or an unknown CSI parameter byte — is dropped from the parser and
 * the trailing byte falls through to the existing data-byte path,
 * matching the spec's "no Vi-mode toggle to break" rule.
 *
 * Repaint: \r ESC[K to move to column 0 and erase to end of line, then
 * the prompt and the recalled text. The cursor lands at end-of-input
 * (no in-line cursor support yet — left/right arrows are out of scope).
 *
 * One line-edit loop covers both the UART path and every TCP session:
 * the telnet parser delivers ESC bytes (0x1B) to the RX ring as-is
 * (telnet.c only filters IAC sequences starting at 0xFF), so the same
 * ESC[A/B handling here works for telnet clients without changes to
 * shell_io_tcp.c or telnet.c.
 */
int shell_read_command(const char *prompt, char *buf, int max_len)
{
    struct shell_session *s = shell_session_current();
    if (!s || !s->io || max_len <= 0) {
        if (buf && max_len > 0) {
            buf[0] = '\0';
        }
        return -1;
    }
    struct shell_io *io = s->io;

    /* Captured once per call — telnet negotiation can in principle
     * flip mid-line, but the WILL/DONT ECHO exchange is initiated
     * at connect time and slm-put.py never re-negotiates, so a
     * single read avoids the per-char vtable indirection in the
     * hot path. The line-edit loop emits ~SHELL_MAX_LINE writes
     * per command (8 KB post-#581); a per-char check would be a
     * measurable per-byte cost on the bulk-upload path that this
     * field exists to fix. */
    bool echo = shell_io_echo_enabled(io);

    if (prompt && *prompt) {
        io->write(io, prompt, strlen(prompt));
    }

    enum { ES_DATA, ES_ESC, ES_CSI } esc_state = ES_DATA;
    int pos = 0;

    for (;;) {
        if (pos >= max_len - 1) {
            break;
        }
        /* Refill the prefetch when our local view is empty (#597).
         * The TCP backend's `read_buf` drains the rx ring in a single
         * spin_lock_irqsave cycle (vs. one per byte for `read_char`),
         * which is the core of the throughput recovery for large
         * `xput chunk` lines. The session-level buffer means
         * unconsumed bytes after a newline (multi-line paste,
         * pipelined commands) survive the return from this call. */
        if (s->read_prefetch_pos >= s->read_prefetch_len) {
            int got = shell_io_read_buf(io, s->read_prefetch,
                                        (int)sizeof(s->read_prefetch));
            if (got <= 0) {
                buf[pos] = '\0';
                return -1;
            }
            s->read_prefetch_pos = 0;
            s->read_prefetch_len = (uint16_t)got;
        }
        char c = s->read_prefetch[s->read_prefetch_pos++];

        /* ESC-sequence parser. Bare ESC and unknown CSI parameter
         * bytes deliberately fall through to the data path so the
         * trailing byte is processed exactly as if it had arrived
         * standalone. */
        if (esc_state == ES_ESC) {
            if (c == '[') {
                esc_state = ES_CSI;
                continue;
            }
            esc_state = ES_DATA;
            /* Bare ESC discarded; reprocess `c` as data. */
        } else if (esc_state == ES_CSI) {
            esc_state = ES_DATA;
            if (c == 'A' || c == 'B') {
                const char *recall = (c == 'A')
                                   ? shell_history_prev(s)
                                   : shell_history_next(s);
                if (recall) {
                    /* History recall is a no-op for echo-off peers:
                     * a line-mode client won't be sending arrow-key
                     * CSI sequences in the first place (it does its
                     * own local line edit), and the visible recall
                     * has nowhere to go. Skip the visual update,
                     * but still load the recall into `buf` so a
                     * pasted ESC[A doesn't leave the buffer empty. */
                    if (echo) {
                        /* \r → column 0; ESC[K → erase to end of line. */
                        io->write(io, "\r\x1b[K", 4);
                        if (prompt && *prompt) {
                            io->write(io, prompt, strlen(prompt));
                        }
                    }
                    size_t rl = strlen(recall);
                    if (rl > (size_t)(max_len - 1)) {
                        rl = (size_t)(max_len - 1);
                    }
                    if (rl > 0) {
                        memcpy(buf, recall, rl);
                        if (echo) {
                            /* Echo `buf` rather than `recall` so the visible
                             * line and the in-memory buffer stay in lock-
                             * step when the recall is clipped to fit. */
                            io->write(io, buf, rl);
                        }
                    }
                    buf[rl] = '\0';
                    pos = (int)rl;
                }
                continue;
            }
            /* Unknown CSI; let the trailing byte fall through. */
        }

        if (c == 0x1B) {
            esc_state = ES_ESC;
            continue;
        }
        if (c == '\r' || c == '\n') {
            /* CRLF echo is part of the visual line-end for echo-on
             * peers; for echo-off peers it's just extra bytes the
             * client has to skip past in its `read_until_prompt`
             * scan. Harmless but skippable. */
            if (echo) {
                io->write(io, "\r\n", 2);
            }
            break;
        }
        if (c == '\b' || c == 0x7F) {
            if (pos > 0) {
                pos--;
                if (echo) {
                    io->write(io, "\b \b", 3);
                }
            }
            continue;
        }
        if (c == 0x03) {
            if (echo) {
                io->write(io, "^C\r\n", 4);
            }
            shell_history_reset_cursor(s);
            pos = 0;
            break;
        }
        if (c >= 0x20 && c < 0x7F) {
            buf[pos++] = c;
            /* Per-char echo dominates bulk-upload throughput — see
             * shell_io.h::echo_enabled. With slm-put.py (or any
             * line-mode telnet client) negotiating DONT ECHO, this
             * skip removes ~8 KB of TX traffic and 8K spinlock-
             * protected ring writes per `xput chunk`, raising
             * effective bandwidth from ~30 KB/s to wire-rate. */
            if (echo) {
                io->write(io, &c, 1);
            }
            continue;
        }
        /* Other control bytes silently ignored, same as shell_read_line. */
    }

    buf[pos] = '\0';
    return pos;
}

/*
 * Drain the session-level prefetch first, then fall back to the
 * backend's batched read. Required by `cmd_xput_bin` (#597 Option B):
 * the prefetch may hold bytes that arrived in the same TCP segment
 * as the `xput-bin <path> <total>\n` command line. If we read straight
 * from the io's read_buf without first consuming the prefetch, those
 * bytes are lost and the upload skips them.
 */
int shell_session_read_raw(char *dst, int max_len)
{
    if (!dst || max_len <= 0) {
        return -1;
    }
    struct shell_session *s = shell_session_current();
    if (!s || !s->io) {
        return -1;
    }

    if (s->read_prefetch_pos < s->read_prefetch_len) {
        int avail = (int)(s->read_prefetch_len - s->read_prefetch_pos);
        int n = (avail < max_len) ? avail : max_len;
        memcpy(dst, &s->read_prefetch[s->read_prefetch_pos], (size_t)n);
        s->read_prefetch_pos += (uint16_t)n;
        return n;
    }

    return shell_io_read_buf(s->io, dst, max_len);
}

/*
 * Toggle binary-mode pass-through on the current session's I/O
 * backend. Used by `cmd_xput_bin` to disable telnet's CR-LF /
 * CR-NUL swallow for the duration of a binary upload — without
 * this, any 0x0D 0x0A or 0x0D 0x00 pair in the binary stream
 * gets the second byte silently dropped, shifting every
 * subsequent byte by one and corrupting the file.
 *
 * No-op when the session has no io, or when the backend doesn't
 * implement set_binary_mode (UART/serial). Safe to call from
 * any return path — pairs cleanly with set(true) / set(false).
 */
void shell_session_set_binary_mode(bool on)
{
    struct shell_session *s = shell_session_current();
    if (s && s->io && s->io->set_binary_mode) {
        s->io->set_binary_mode(s->io, on);
    }
}

void shell_session_write_raw(const uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return;
    struct shell_session *s = shell_session_current();
    /* Backends without write_raw (UART today) can't deliver bit-exact
     * binary — the cooked write path's CR-LF expansion would corrupt
     * the stream. Drop silently here; callers must use
     * shell_session_supports_write_raw() to gate before starting a
     * binary frame so the operator sees a useful error instead of a
     * truncated download. */
    if (s && s->io && s->io->write_raw) {
        s->io->write_raw(s->io, buf, len);
    }
}

bool shell_session_supports_write_raw(void)
{
    struct shell_session *s = shell_session_current();
    return s && s->io && s->io->write_raw;
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

    /* Register `kernel` admin command (dynamic-kernel-replace #370). */
    {
        extern void kernel_cmd_register_shell(void);
        kernel_cmd_register_shell();
    }

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
        blob_boot_autoload();
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

    /* Telemetry-feed TCP server (port 2325). Brings up the network
     * bridge for `tel.*` msg_router topics if the build set
     * NET_TELEMETRYD_AUTOSTART; no-op otherwise. */
    {
        extern void tcp_telemetry_server_autostart(void);
        tcp_telemetry_server_autostart();
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

bool shell_mutation_pause(void)
{
    if (!pi_mutex_held_by_self(&shell_mutex)) {
        return false;
    }
    pi_mutex_unlock(&shell_mutex);
    return true;
}

void shell_mutation_resume(bool paused)
{
    if (!paused) {
        return;
    }
    pi_mutex_lock(&shell_mutex);
}

/*
 * Shell main loop.
 */
void shell_run(void)
{
    /* Stack-local line buffer so two concurrent sessions (console +
     * TCP) don't corrupt each other's in-progress input. SHELL_MAX_LINE
     * (post-#581: 8 KB) + argv is well within the 64 KB task stack. */
    char line_buffer[SHELL_MAX_LINE];
    char *argv[SHELL_MAX_ARGS];
    int argc;

    /* Cache the bound session for the lifetime of this REPL. The shell
     * task entry binds the session before calling and unbinds after
     * shell_run returns, so the lookup is stable for the entire loop —
     * doing it once here saves the per-iteration call that would
     * otherwise feed shell_history_add. (shell_read_command still does
     * its own internal lookup; the line-edit loop body is platform
     * code and does not get a session-typed parameter.) */
    struct shell_session *sess = shell_session_current();

    while (1) {
        /* shell_read_command emits the prompt + runs the ESC[A/B
         * history parser. Replaces the older shell_puts(SHELL_PROMPT)
         * + shell_read_line() pair so the history parser sees the
         * full byte stream including arrow-key sequences (#434). */
        int len = shell_read_command(SHELL_PROMPT, line_buffer, SHELL_MAX_LINE);

        if (len < 0) {
            /* Session closed (peer disconnect). End the REPL. */
            break;
        }
        if (len == 0) {
            continue;  /* Empty line */
        }

        /* Capture into history before parse_line shreds the buffer with
         * in-place NUL terminators. shell_history_add itself filters
         * whitespace-only and exact-duplicate-of-most-recent so this
         * call site stays simple. */
        shell_history_add(sess, line_buffer);

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
