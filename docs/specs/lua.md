# Lua Scripting — Fact Sheet

In-kernel Lua runtime, SLM bindings, REPL.

## Matrix

| Sub-capability | QEMU (ARM64) | Pi 5 | Jetson | x86-64 |
|---|---|---|---|---|
| Lua version | 5.4.7 | 5.4.7 | 5.4.7 | 5.4.7 |
| Build | Compiled into kernel (~350 KB) | Same | Same | Same |
| Per-session state | ✅ `lua_State *` per shell session | Same | Same | Same |
| REPL | `lua` shell command | Same | Same | Same |
| One-liner (`lua -e "code"`) | ✅ | ✅ | ✅ | ✅ |
| Script file (`lua <path>`) | ✅ reads from VFS | Same | Same | Same |
| libc stubs | `kernel/src/lua_stubs.c` — fopen, fread, stdio | Same | Same | Same |
| `slm.*` binding surface | ~30 bindings covering tasks, msg, components, model, scheduler, ai | Same | Same | Same |
| Task bindings | `slm.task_create`, `slm.task_kill`, `slm.task_list` family | Same | Same | Same |
| Message bindings | `slm.msg_publish`, `slm.msg_subscribe`, `slm.msg_unsubscribe` | Same | Same | Same |
| Model bindings | `slm.model_load`, `slm.model_infer`, `slm.model_list` | Same | Same | Same |
| AI scheduler bindings | `slm.ai_sched_decision` | Same | Same | Same |
| Scheduler bindings | `slm.sched_set_policy`, `slm.sched_stats` | Same | Same | Same |
| Component bindings | `slm.component_run`, `slm.component_swap` | Same | Same | Same |
| Shell I/O bindings | `slm.print`, `slm.read_line` | Same | Same | Same |
| Embedded scripts | Lua demos via `.incbin` (demo.lua and friends) | Same | Same | Same |
| Bytecode trust boundary | Currently trusted; documented hazard before TCP shell lands | Same | Same | Same |

## Skipped / Blocked

- **`lua_msg_subs` spinlock** — the global subscribers table is not spinlock-protected. Acceptable under single-CPU-0 shell access; unsafe when TCP shell sessions start running Lua concurrently on different CPUs.
- **Cross-state `msg_subscribe` ack loss** — `LUA_MSG_SUB_IDX` sentinel is shared across Lua states; the ack-always code path can lose acks when two states subscribe to the same topic. Queued as an issue.
- **Bytecode trust boundary** — `l_task_create` and similar bindings can execute arbitrary Lua bytecode that can invoke syscall-ish kernel APIs. Safe today (shell is single-user, UART-console-only); becomes a concern when TCP shell (unauthenticated) or SSH (authenticated multi-user) land. Flagged in PR #217 review.
- **Debug library (`debug.*`)** — not compiled in (`LUA_USE_DEBUG` off); not a concern for demo workloads.
- **LuaJIT / alternative VMs** — not in scope; stock Lua 5.4 interpreter.
- **Hot-reload of running scripts** — no. Script runs to completion; re-invoke to re-run.
- **Coroutine-based cooperative tasking** — Lua coroutines work inside a single state; not hooked into the kernel scheduler.

## See also

- `docs/lua.md` (narrative, binding catalog)
- `kernel/src/lua_slm.c` — binding implementation
- `kernel/src/lua_stubs.c` — libc stubs
- `kernel/src/lua_shell.c` — REPL
- Issue: #152 (Lua audit — closed)

*Last updated: 18 April 2026*
