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

/* Current working directory (set by cmd_cd, read by resolve_path) */
extern char shell_cwd[VFS_MAX_PATH];

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
int cmd_clear(int argc, char **argv);
int cmd_reboot(int argc, char **argv);
int cmd_vmm(int argc, char **argv);
int cmd_ipc(int argc, char **argv);
int cmd_model(int argc, char **argv);
int cmd_dtb(int argc, char **argv);

/* Filesystem commands (shell_fs.c) */
int cmd_ls(int argc, char **argv);
int cmd_cd(int argc, char **argv);
int cmd_pwd(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_write(int argc, char **argv);
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

#endif /* SHELL_INTERNAL_H */
