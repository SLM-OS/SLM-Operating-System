# ADR 0013: embed a Lua 5.4 interpreter in the kernel for scripting

Status: accepted (backfilled).

## Context

The shell (ADR 0012) runs one command per line with no control flow, variables or files of commands. Before Lua landed, `FUTURE.md` listed "Lua Scripting Runtime" (effort 2 to 3 weeks; "Runtime configurability, rapid prototyping, user-defined automation") next to "Full POSIX Shell" (2 to 3 months, needs fork/exec and user/kernel separation), as of the parent of 653a5087. Lua was chosen as the best available option and as simpler than writing a scripting language (user, 2026-11-14).

## Decision

Lua 5.4.7 is vendored unmodified in `kernel/lib/lua/` and linked into every kernel image. It arrived in 653a5087 "Refactor test infrastructure and add Lua scripting support" (2025-12-27), marked "Lua Integration (WIP)"; `git log -- kernel/lib/lua/src` shows no later change to the upstream sources.

- Build: a separate `lua` static library (`CMakeLists.txt`, `add_library(lua ...)`) so Lua may use floating point while the kernel builds with `-mgeneral-regs-only`. It contains the Lua core, `kernel/src/lua_stubs.c` (freestanding libc: allocator, strings, math, printf to UART), `kernel/src/lua_slm.c` (bindings), `kernel/src/lua_shell.c` (shell verbs) and `setjmp.S` for arm64 and x86_64.
- Configuration (`kernel/include/slm_luaconf.h`): `LUA_32BITS`, `LUA_USE_C89`, `LUAI_MAXSTACK` 15000. Heap: a 1 MB first-fit pool (`docs/lua.md`).
- Bindings: the `slm` module, about 124 registered functions in `lua_slm.c` (tasks, scheduler, components, message router, eviction, model memory, inference, Hailo, camera, telnetd).
- Two entry points (ec3a2e9d "Land Lua admin split, demo fixes, and dynamic policy loading plan", 2026-04-22): `lua` exposes session-local, read-mostly bindings safe across concurrent shell sessions; `lua-admin` serializes global mutators (task control, hot-swap, telnetd control).
- Scripts in `scripts/*.lua` (`demo.lua`, `admin.lua`, camera and Hailo demos) are embedded at boot when `EMBED_DEMO_SCRIPTS=ON` and written to `/mnt/files`.

## Alternatives considered

- eLua, named in the same `FUTURE.md` item as "for smaller footprint": not evaluated (user, 2026-11-14).
- Other embeddable interpreters: not evaluated (user, 2026-11-14).

## Consequences

- Lua inherits the kernel's freestanding libc. Missing prototypes went unnoticed because upstream Lua was built with `-Wno-error`: `memchr` had no declaration in `kernel/include/string.h`, so GCC treated it as returning `int` and sign-extended the pointer. On Jetson, where the heap lies above `0x80000000`, `string.find(..., true)` page-faulted (#1199); QEMU RAM is below that address, so `make test` did not see it. 8ec0e330 (2026-11-13) declares `memchr`, `strcoll` and `strerror`, builds the Lua library with `-Werror=implicit-function-declaration`, `-Werror=int-conversion` and `-Werror=int-to-pointer-cast`, and adds `test_lua_string_find_above_2gb`, which uses an allocator returning addresses with bit 31 set.
- Limits recorded in `docs/lua.md`: scripts must fit a 16 KB buffer; no coroutine, debug or os library; Taylor-series math; `math.asin`, `acos`, `atan`, `atan2` return 0; no Lua subscribers or Lua-defined tasks.
- `lua-admin` releases its lock while waiting for input, so a script must re-read global state after a prompt.
- Lua is not listed in `LICENSES.md` (its MIT notice is at the end of `kernel/lib/lua/src/lua.h` and in `kernel/lib/lua/doc/readme.html`).

## Evidence

- `kernel/tests/test_lua.c` (145 test functions, 4360 lines) and `kernel/tests/test_lua_stubs.c`. `test_lua.c` is excluded from the x86-64 test build (`CMakeLists.txt`, `$<$<NOT:$<STREQUAL:${PLATFORM},X86_64>>:...>`), though the library is linked there.
- `docs/lua.md`, `docs/architecture.md` "Lua Scripting Engine".
- #1199 (closed): "Test kernel on Jetson: test_lua_client_gets_the_reply page-faults in memcmp (Lua string.find), halting the suite".
