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
| Shell I/O bindings | `slm.print`, `slm.read_line`, `slm.try_getc`, `slm.term_size` | Same | Same | Same |
| Embedded scripts | Lua demos via `.incbin` (demo.lua and friends) | Same | Same | Same |
| Bytecode trust boundary | Currently trusted; documented hazard before TCP shell lands | Same | Same | Same |

## Skipped / Blocked

- **Bytecode trust boundary** — `l_task_create` and similar bindings can execute arbitrary Lua bytecode that can invoke syscall-ish kernel APIs. Safe today only on trusted systems; now more relevant because multi-session TCP shell is live on Pi 5 lab images and SSH/authentication (#199) is still outstanding.
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

*Last updated: 22 April 2026*
