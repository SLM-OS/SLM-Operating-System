/*
 * shell_internal.h - Internal declarations shared between shell_*.c files
 *
 * Not part of the public shell API (see shell.h for that).
 * Used to share helper functions, variables, and command handler
 * declarations across the split shell implementation files.
 */

#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"        /* VFS_MAX_PATH */
#include "config.h"     /* SHELL_MAX_LINE, SHELL_MAX_ARGS */

/* ============================================================================
 * Shared State
 * ============================================================================ */

/* The current working directory lives on the per-session struct
 * (shell_session.h). Read it via shell_session_current()->cwd and
 * mutate it only from command handlers that run on the session's
 * shell task (cd). */

/* ============================================================================
 * Shared Helper Functions (defined in shell.c)
 * ============================================================================ */

/*
 * Parse an unsigned integer from a string.
 * Returns 0 on success, -1 on error. Result stored in *out.
 */
int shell_parse_uint(const char *str, uint32_t *out);

/*
 * Resolve a path relative to the current working directory.
 * Handles absolute paths, relative paths, ".", "..", and "//" normalization.
 * Returns 0 on success, -1 on error. Result stored in out.
 */
int shell_resolve_path(const char *input, char *out, size_t out_size);

/* ============================================================================
 * Command Handler Declarations
 *
 * All cmd_* functions have the signature: void cmd_xxx(int argc, char **argv)
 * They are defined in their respective shell_*.c files and referenced
 * by the command table in shell.c.
 * ============================================================================ */

/* System commands (shell_sys.c) */
int cmd_help(int argc, char **argv);
int cmd_mem(int argc, char **argv);
int cmd_tasks(int argc, char **argv);
int cmd_cpu(int argc, char **argv);
int cmd_uptime(int argc, char **argv);
int cmd_canary(int argc, char **argv);
int cmd_clear(int argc, char **argv);
int cmd_reboot(int argc, char **argv);
int cmd_sleep(int argc, char **argv);
#if defined(PI5_IRQ_DIAG)
int cmd_diag(int argc, char **argv);
#endif
int cmd_vmm(int argc, char **argv);
int cmd_ipc(int argc, char **argv);
int cmd_model(int argc, char **argv);
int cmd_dtb(int argc, char **argv);
int cmd_gpu(int argc, char **argv);
int cmd_peek(int argc, char **argv);
int cmd_poke(int argc, char **argv);
int cmd_dtb_dump(int argc, char **argv);
int cmd_emmc_bringup(int argc, char **argv);
#if defined(PLATFORM_JETSON_ORIN_NANO)
int cmd_nvgpu(int argc, char **argv);
int cmd_xhci(int argc, char **argv);
#endif
int cmd_bench(int argc, char **argv);
int cmd_sched(int argc, char **argv);
int cmd_eviction(int argc, char **argv);
#if !defined(PLATFORM_X86_64)
int cmd_timdiag(int argc, char **argv);
#endif

#if defined(PLATFORM_RASPI5) && defined(ENABLE_NETWORKING)
int cmd_macbdiag(int argc, char **argv);
#endif

#if defined(PLATFORM_RASPI5)
int cmd_mboxclk(int argc, char **argv);
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(ENABLE_NETWORKING)
int cmd_rtldiag(int argc, char **argv);
int cmd_xhcidiag(int argc, char **argv);
int cmd_cdcdiag(int argc, char **argv);
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO)
int cmd_hspdiag(int argc, char **argv);
int cmd_bpmp(int argc, char **argv);
int cmd_pcietrain(int argc, char **argv);
int cmd_imx219(int argc, char **argv);
int cmd_nvcsi(int argc, char **argv);
int cmd_rcediag(int argc, char **argv);
int cmd_csidiag(int argc, char **argv);
#endif

/* Dashboard command (shell_top.c) */
int cmd_top(int argc, char **argv);

/* Filesystem commands (shell_fs.c) */
int cmd_ls(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_write(int argc, char **argv);
int cmd_put(int argc, char **argv);
int cmd_xput(int argc, char **argv);
int cmd_xput_bin(int argc, char **argv);
int cmd_mkdir(int argc, char **argv);
int cmd_rm(int argc, char **argv);
int cmd_mv(int argc, char **argv);
int cmd_df(int argc, char **argv);
int cmd_truncate(int argc, char **argv);
int cmd_append(int argc, char **argv);
int cmd_cp(int argc, char **argv);
int cmd_touch(int argc, char **argv);
int cmd_stat(int argc, char **argv);
int cmd_tree(int argc, char **argv);
int cmd_wc(int argc, char **argv);
int cmd_hexdump(int argc, char **argv);
int cmd_grep(int argc, char **argv);
int cmd_find(int argc, char **argv);

/* Execution commands (shell_exec.c) */
int cmd_elftest(int argc, char **argv);
int cmd_run(int argc, char **argv);
int cmd_kill(int argc, char **argv);

/* Component commands (shell_component.c) */
int cmd_component(int argc, char **argv);

/* Message router commands (shell_component.c) */
int cmd_msg(int argc, char **argv);

/* Telemetry feed introspection (shell_sys.c — admin & telemetry suite, M4) */
int cmd_telemetry(int argc, char **argv);

#endif /* SHELL_INTERNAL_H */
