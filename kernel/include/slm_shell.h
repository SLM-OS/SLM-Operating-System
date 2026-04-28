/*
 * slm_shell.h - `slm` shell command family (Phase SLM, M7).
 *
 * The `slm` verb wires the M1.4 (loader) + M5.2 (session/decoder)
 * Rust FFI through the kernel shell. See kernel/src/slm_shell.c
 * for the dispatcher and per-verb handlers.
 *
 * The command itself is registered through `builtin_commands[]` in
 * kernel/src/shell.c — handler pointer lives in slm_shell.c so the
 * float-arg FFI surface (rust_slm_session_open / rust_slm_prompt)
 * can be called from a translation unit compiled WITHOUT
 * -mgeneral-regs-only. The kernel as a whole keeps the FP-disable
 * for Cortex-A78AE / GA10B safety; only this file has FP enabled.
 */

#ifndef SLM_SHELL_H
#define SLM_SHELL_H

/*
 * `slm <verb> [args]` shell command. Defined in kernel/src/slm_shell.c.
 * Returns 0 on success, negative on error (matches the rest of the
 * shell command surface).
 */
int cmd_slm(int argc, char **argv);

#endif /* SLM_SHELL_H */
