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
| libc stubs | `kernel/src/lua_stubs.c` — fopen/fread/stdio + ARM64 math; x86-64 adds glibc `__ctype_b_loc` / `__ctype_to{upper,lower}_loc` | Same | Same | Same (x86 ctype tables have lazy SMP-safe init via `__atomic_*`) |
| State factory — safe surface | `lua_slm_newstate()` loads `slm_lib_safe` only — read-only / observability bindings | Same | Same | Same |
| State factory — admin surface | `lua_slm_newstate_admin()` loads safe + `slm_lib_admin` + `slm.hailo` namespace | Same | Same | Same (no `slm.hailo` — Hailo backend gated out) |
| `slm.*` binding surface (safe) | print, uptime, uptime_us, mem_stats, tasks, sleep, yield, version, cpu_count, cpu_id, cpu_info, term_size, vmm_stats, ipc_stats, sched_policy, sched_stats, sched_policy_list, ai_sched_stats, ai_sched_decision, eviction_policy, eviction_stats, model_stats, model_find, model_list, model_info, infer_stats, gpu_status, component_count, component_list, component_find, msg_publish, msg_publish_priority, msg_subscribe, msg_unsubscribe, msg_drain, read_line, try_getc, telnetd_status, telnetd_sessions | Same | Same | Same |
| `slm.*` binding surface (admin) | component_run, component_hot_swap, component_hot_swap_stateful, model_load, model_load_mnist, model_pin, model_unpin, model_preload, model_preload_wait, model_infer, model_bench, sched_set_policy, task_create, task_kill, task_set_priority, task_pin, task_migrate, eviction_set_policy, shell_exec, telnetd_start, telnetd_stop, telnetd_kick | Same | Same | Same |
| `slm.hailo.*` namespace | — (no Hailo) | ✅ admin-only: load, infer, status, unload | — (no Hailo) | — (gated out at compile time) |
| Embedded scripts | Lua demos via `.incbin` (demo.lua and friends) | Same | Same | Same |
| Boundary regression test | `test_slm_safe_state_lacks_admin_bindings` asserts non-admin states cannot reach admin bindings or `slm.hailo` | Same | Same | Same |
| Bytecode trust boundary | Mitigated by safe/admin split — non-admin scripts cannot invoke mutators; bytecode loaded by an admin shell still trusted | Same | Same | Same |

## Skipped / Blocked

- **Bytecode trust boundary (admin path)** — bytecode loaded under an admin state can still invoke any `slm_lib_admin` binding. The safe/admin split provides defense in depth: non-admin TCP/telnet sessions cannot reach the mutator surface even if bytecode is hostile. The remaining hazard is on the admin path itself — same trust assumption as physical UART access today.
- **Debug library (`debug.*`)** — not compiled in (`LUA_USE_DEBUG` off); not a concern for demo workloads.
- **LuaJIT / alternative VMs** — not in scope; stock Lua 5.4 interpreter.
- **Hot-reload of running scripts** — no. Script runs to completion; re-invoke to re-run.
- **Coroutine-based cooperative tasking** — Lua coroutines work inside a single state; not hooked into the kernel scheduler.
- **Test harness `lua_State` cleanup on assertion failure** — Unity's `TEST_ASSERT_*` long-jumps past `lua_slm_close(L)`, leaking the state. One real failing test cascades into apparent failures of subsequent tests via heap exhaustion. Tracked: #374.

## See also

- `docs/lua.md` (narrative, binding catalog)
- `kernel/src/lua_slm.c` — binding implementation (safe/admin split at `slm_lib_safe` / `slm_lib_admin`)
- `kernel/src/lua_stubs.c` — libc stubs
- `kernel/src/lua_shell.c` — REPL
- `kernel/tests/test_lua.c` — `test_slm_safe_state_lacks_admin_bindings` security boundary regression
- Issues: #152 (Lua audit — closed), #374 (Unity tearDown follow-up)

*Last updated: 25 April 2026*
