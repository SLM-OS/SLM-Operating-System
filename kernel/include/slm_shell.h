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

/* `slm stats --gpu`'s GPU-time lines, for tests. */
#include "slm_ffi.h"
void slm_stats_print_gpu_busy(const SlmTelemetryC *t);
/* `slm stats --gpu`'s decode DRAM bandwidth line (ADR 0005), for tests. */
void slm_stats_print_dram_bw(const SlmTelemetryC *t, uint64_t emc_hz);

/* The `[slm] ttft=... total=...` line after a prompt: total is ttft plus
 * decode. `tokens` and `bytes` are what the prompt printed. */
void slm_prompt_print_summary(const SlmStatsC *st, uint64_t tokens, uint64_t bytes);

#endif /* SLM_SHELL_H */
