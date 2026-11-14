# ADR 0012: the in-kernel shell is the integration surface for every subsystem

Status: accepted (backfilled).

## Context

Bring-up of each driver and subsystem (GPU, Hailo, camera, USB, PCIe, BPMP, clocks, thermal, scheduler, eviction, SLM inference) needs a way to trigger it, read its state and demonstrate it on hardware. The debug shell arrived in e9a5c41d "Implement debug shell, improve test verification, add Jetson prep" (2025-12-22). `docs/shell.md` describes it as "purely a development/demo tool": a table-driven command loop running as a kernel task pinned to CPU 0.

## Decision

Subsystem bring-up, diagnostic and demo verbs are added as shell commands in the kernel image, compiled into `builtin_commands[]` (`kernel/src/shell.c`) or registered at run time through `shell_register_command()` (`kernel/include/shell.h`). The same REPL is reachable from three transports: the UART console, telnet on port 2323 (db204cd7, PR #259, 2026-04-17; per-session state in `kernel/include/shell_session.h`, up to `MAX_TCP_SHELL_SESSIONS` = 16) and SSH on port 2222 when built with `NET_SSHD=ON` (904fa48d, #891 / PR #915, 2026-05-25; see ADR 0014). Hardware verification drives the shell over serial through labctl `serial_send` / `serial_capture`, waiting for the `slmos>` prompt (`.claude/skills/deploy-and-test/SKILL.md`; root `CLAUDE.md` post-change checklist step 4). Lua (ADR 0013) is reached from the shell (`lua`, `lua-admin`).

Motivation for putting every verb in one kernel-resident shell instead of a user-space tool or a separate debug channel: unknown, ask the user.

Rules that have accumulated on shell verbs:

- `builtin_commands[]` stays grouped by `shell_cmd_category_t` and alphabetized; `test_builtin_commands_grouped_and_sorted` enforces it (`docs/shell.md`).
- GPU-channel calls from `kernel/src/shell_sys.c` run only inside a `*_locked_cb` passed to `slm_gpu_call_locked`, and the callback does not print (IRQs are masked). Added for #1111 by 0f5dfe93 "Run shell GPU verbs under the dispatch lock (#1142)" (2026-11-05); enforced by `scripts/check-gpu-shell-lock.py`, a prerequisite of `make test`.
- No unconditional UART output on the per-dispatch GPU path (root `CLAUDE.md`, #844 recovery prints, about 146 ms per dispatch).
- lwIP raw-API calls made by shell-reachable network code hold `lwip_core_lock()` (`kernel/CLAUDE.md`, #1065).
- Rust locks reached from IRQs-off paths are IRQ-saving or try-locked; a telnet shell preempted while holding stats locks deadlocked CPU 0 in `net_pump` (#1093, `runtime/CLAUDE.md`).

## Alternatives considered

- A full POSIX shell (fork/exec, pipes, "Port BusyBox ash or dash shell") was listed as a future item in `FUTURE.md` before 653a5087 (2025-12-27); it was not built. Whether it was ever weighed against the in-kernel shell: unknown, ask the user.

## Consequences

- The shell concentrates change. `kernel/src/shell_sys.c` is 13205 lines on 8ec0e330, the largest first-party source file (next: `runtime/src/lib.rs`, 10752; only vendored FatFs `ffunicode.c` is larger). It began at 395 lines when f7d308ee "Split shell.c into 5 focused modules (CORE-L9)" (2026-04-03) split a 2718-line `shell.c`. 235 of 1325 non-merge commits that touch `.c`/`.h`/`.rs`/`.S` files touch it (17.7 percent).
- 78 entries in `builtin_commands[]`, many platform-gated hardware probes (`peek`, `poke`, `nvgpu`, `bpmp`, `xhcidiag`, `rcediag`, `timdiag`).
- The shell task and `net_pump` both run on CPU 0, so shell verbs share lock and IRQ constraints with the network path (#1093, #1104 open).
- Telnet is unauthenticated; `docs/shell.md` limits it to trusted networks. Telnet stops serving after about 50 sessions (#1107, open).
- `docs/shell.md` still says SSH (#199) is deferred, while `docs/ssh.md` reports it shipped in May 2026; #199 remains open.

## Evidence

- Files: `kernel/src/shell.c`, `kernel/src/shell_sys.c`, `kernel/include/shell_internal.h` (added in f7d308ee), `kernel/src/shell_io_tcp.c`, `kernel/src/shell_session.c`, `docs/shell.md`.
- Tests: `kernel/tests/test_shell.c`, `test_shell_session.c`, `test_shell_history.c`, `scripts/tests/test-check-gpu-shell-lock.sh`.
- Line and commit counts: `wc -l`, `git log --no-merges --format=%h -- kernel/src/shell_sys.c | wc -l` against the same with `'*.c' '*.h' '*.rs' '*.S'`, on 8ec0e330.
