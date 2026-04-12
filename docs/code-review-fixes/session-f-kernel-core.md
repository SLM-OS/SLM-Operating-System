# Session F — Kernel Core / Shell / Strings / VFS / GPU / Lua / Tests

**Source:** `docs/code-review-2026-04-12.md` §7
**Scope:** 16 issues (3 CRITICAL, 7 HIGH, 4 MEDIUM, 2 LOW)
**Files touched:** `kernel/src/elf.c`, `kernel/src/string.c`, `kernel/src/lua_stubs.c`, `kernel/src/kprintf.c`, `kernel/src/lua_slm.c`, `kernel/src/shell.c`, `kernel/src/vfs.c`, `kernel/src/semihosting.c`, `kernel/gpu/gpu_nvidia.c`, `kernel/fs/littlefs_vfs.c`, `kernel/tests/test_shell.c`, `kernel/tests/test_harness.c`

## Mission

Broadest session — fixes span the shell, VFS node pool, ELF argv copy, Lua
C bindings, string-function duplication, and test coverage gaps. Many small
fixes rather than one large architectural change.

**Primary carry-over from April 2:** duplicated string functions between
`string.c` and `lua_stubs.c`. This remains unfixed and is the single most
systemic risk in this group (linker symbol collision).

## Working rules

- Read `CLAUDE.md` (root) and `kernel/CLAUDE.md`. The kernel's C23 freestanding
  constraints and `__asm__` requirements are relevant for any string-ops
  rewrites.
- String-function dedup (CORE-C3) is invasive — it moves code between files,
  may require linker script review, and must not break Lua. Proceed in a
  worktree, test every platform, and have John review before merge.
- Shell and VFS fixes are mostly additive (new tests, new guards) — low risk.
- Work in a worktree: `git worktree add ../slm-os-session-f -b fix/session-f-core`.
- Ask John before committing.

## Issues

### CRITICAL

#### CORE-C1 — `strcpy` without bounds in `elf_setup_argv`
- **File:** `kernel/src/elf.c:411`
- **Problem:** `strcpy(str_ptr, argv[i])` relies on prior `strings_size`
  math; any mismatch overflows the stack.
- **Fix:** Track `remaining` explicitly:
  ```c
  size_t remaining = strings_size;
  for (int i = 0; i < argc; i++) {
      size_t need = strlen(argv[i]) + 1;
      if (need > remaining) return ELF_ERR_ARGV_TOO_LARGE;
      memcpy(str_ptr, argv[i], need);
      str_ptr += need;
      remaining -= need;
  }
  ```
- **Verification:** Add a test in `kernel/tests/test_harness.c` or new
  `test_elf_argv.c` that passes an oversized argv and expects rejection.

#### CORE-C2 — GPU probe reads firewall-blocked MMIO on Jetson
- **File:** `kernel/gpu/gpu_nvidia.c:41-75`
- **Problem:** `nv_gpu_probe()` unconditionally reads NV_PMC_BOOT_0 etc. On
  Jetson the CBB firewall blocks this and the read faults.
- **Fix investigation first:** Verify that Jetson builds use `gpu_stub.c`,
  not `gpu_nvidia.c`. Check CMakeLists.txt:
  ```
  grep -n "gpu_nvidia\|gpu_stub" CMakeLists.txt kernel/gpu/*
  ```
  If the Jetson build correctly uses the stub, this issue is
  **downgraded to MEDIUM** — add a defensive check inside `nv_gpu_probe`:
  ```c
  int nv_gpu_probe(uintptr_t mmio_base) {
  #ifdef PLATFORM_JETSON_ORIN_NANO
      WARN("nv_gpu_probe called on Jetson; CBB firewall blocks MMIO");
      return -1;
  #endif
      /* ... */
  }
  ```
- **Verification:** Jetson boot doesn't trigger CBB abort.

#### CORE-C3 — Duplicated string functions (carry-over from April 2)
- **Files:** `kernel/src/string.c`, `kernel/src/lua_stubs.c`
- **Problem:** Both define `strlen`, `strcmp`, `strncpy`, `strncat`,
  `memcpy`, `memset`, `memcmp`, `memmove`, and others. Linker picks whichever
  comes first; if Lua stubs win, kernel code silently uses different
  implementations.
- **Fix:**
  1. Pick canonical implementations in `string.c` (already the intent).
  2. Remove duplicates from `lua_stubs.c` — replace each removed function
     with a `#include "string.h"` reference, or mark `lua_stubs.c` to
     explicitly use the kernel versions.
  3. Verify link order in `CMakeLists.txt` and linker scripts (`kernel.ld`,
     etc.) ensures `string.c` is picked first.
  4. After link, inspect symbols: `nm build/kernel/slmos.elf | grep -E 'T strlen|T strcmp|T memcpy'`
     — there should be exactly one definition per symbol.
- **Verification:** `make test`, `make test PLATFORM=X86_64`,
  `make kernel PLATFORM=RASPI5`, `make kernel PLATFORM=JETSON_ORIN_NANO`
  — all must compile and link cleanly. Full regression run.
- **Risk:** If Lua relies on subtly different string semantics, removing its
  copies may break Lua tests. Run `test_lua.c` specifically.

### HIGH

#### CORE-H1 — `kprintf` return count not clamped to INT_MAX
- **File:** `kernel/src/kprintf.c:545`
- **Fix:** At the end of `uart_vsnprintf`:
  ```c
  if (out.count > (size_t)INT_MAX) return INT_MAX;
  return (int)out.count;
  ```

#### CORE-H2 — Lua stack not cleared after error
- **File:** `kernel/src/lua_slm.c:585-615`
- **Fix:** After handling each error path, add `lua_settop(L, 0);` to clear
  the entire stack. Alternatively, save top at function entry and
  `lua_settop(L, saved)` on exit.

#### CORE-H3 — Shell path resolution has no mount-escape defense
- **File:** `kernel/src/shell.c:125-227`
- **Status:** Capstone-optional. If time permits, track the mount context
  during `..` traversal; reject paths that would exit the mount. If deferred,
  file a GitHub issue with `P2-medium` and link it here.

#### CORE-H4 — Shell path-resolution untested
- **File:** `kernel/tests/test_shell.c`
- **Fix:** Add a `test_shell_resolve_path` group with cases:
  - `/foo/../bar` → `/bar`
  - `/foo/./bar` → `/foo/bar`
  - `/../etc` → `/etc` (or error, depending on spec)
  - `/foo//bar` → `/foo/bar`
  - empty string
  - path overflow
  - trailing slash
- **Verification:** `make test` runs and the new tests pass.

#### CORE-H5 — VFS node pool has no ref counting
- **File:** `kernel/src/vfs.c`
- **Fix (minimal, capstone-ready):** Add a `uint32_t generation` to node
  struct. Increment on free. File descriptors carry the generation they were
  opened under; operations check that the node's generation matches.
- **Fix (full, post-capstone):** Real ref counting with `vfs_node_get` /
  `vfs_node_put` around all handle ops.
- **Verification:** Add test that opens a file, closes the fd, opens a
  different file (which may reuse the slot), and verifies the first fd's
  further operations fail cleanly.

#### CORE-H6 — Lua API called with potentially NULL state
- **File:** `kernel/src/lua_slm.c:36` (`l_print`) — audit all `l_*` callbacks
- **Fix:** Add `if (L == NULL) return 0;` at the top of every C function
  registered with Lua that receives `lua_State *L`.

#### CORE-H7 — Semihosting probe faults on real hardware
- **File:** `kernel/src/semihosting.c:33-36`, callers in `test_harness.c`
- **Fix:** Guard `semihosting_available()` with a compile-time
  `#ifdef QEMU_BUILD` or runtime platform check before executing `hlt #0xF000`.
  Make the test harness call `semihosting_available()` only when QEMU is the
  target.
- **Verification:** Pi 5 boot does not trap; QEMU tests still work.

### MEDIUM

#### CORE-M1 — `atoi` silently overflows
- **File:** `kernel/src/string.c:175-201`
- **Fix:** Audit shell callers (`kernel/src/shell_exec.c`, `shell_sys.c`,
  etc.) — replace `atoi` with the existing `shell_parse_uint` where numeric
  range matters. Deprecate `atoi` with a `/* deprecated: use shell_parse_uint */`
  comment.

#### CORE-M2 — `littlefs_vfs` offset truncation
- **File:** `kernel/fs/littlefs_vfs.c:37-39`
- **Fix:** `if (offset > INT32_MAX) return -1;` before casting for littlefs seek.

#### CORE-M3 — GPU alloc returns NULL silently
- **File:** `kernel/gpu/gpu_nvidia.c:227`
- **Fix:** Either change the API to return an error enum or add
  `DEBUG_ASSERT(gm.cpu_addr != NULL)` after each allocation to surface the
  failure loudly during development.

#### CORE-M4 — Documentation drift
- **Files:** `docs/filesystem.md`, `docs/phase-3-*.md`, any doc with a
  pre-April 2026 "Last updated" footer
- **Fix:** Audit. Refresh or mark superseded. Remove stale file-path
  references exposed by the April 2 review.

### LOW

#### CORE-L1 — Lua stubs return 0.0 for trig functions
- **File:** `kernel/src/lua_stubs.c:742-745`
- **Fix:** Either implement a Taylor-series approximation for `asin`, `acos`,
  `atan`, `atan2`, or make the stubs log a warning on first call. Document
  the limitation in `docs/lua.md`.

#### CORE-L2 — Shell argv parsed twice per line
- **File:** `kernel/src/shell.c:418-426`
- **Fix:** Parse once; reuse `argv[]` across dispatch and error-handling
  paths.

## Suggested work order

1. **CORE-C1** (30 min) — ELF argv bounds
2. **CORE-H1** (5 min) — kprintf INT_MAX clamp
3. **CORE-M2** (5 min) — littlefs offset cast
4. **CORE-M3** (15 min) — GPU alloc assert
5. **CORE-H6** (30 min) — Lua NULL guards
6. **CORE-H2** (15 min) — Lua stack clear
7. **CORE-H7** (20 min) — semihosting platform gate
8. **CORE-C2** (30 min investigate + 15 min fix) — GPU Jetson probe
9. **CORE-H4** (45 min) — shell path tests
10. **CORE-H5** (1 hr) — VFS generation number
11. **CORE-M1** (30 min) — atoi audit + replace
12. **CORE-L1, L2** (30 min) — trig stubs + shell parse optimization
13. **CORE-M4** (1 hr) — doc audit
14. **CORE-C3** (2-3 hr, risky) — string dedup **— do last; do not merge without John's review**
15. **CORE-H3** (file as issue if deferred) — path traversal defense

## Testing requirements

- **Every fix:** `make test`, `make test PLATFORM=X86_64`.
- **CORE-C1:** new `test_elf_argv` or equivalent case.
- **CORE-C3:** full platform matrix build + test + `nm` symbol inspection.
- **CORE-H4:** the new tests are themselves the verification.
- **CORE-H7:** Pi 5 boot smoke test.
- **CORE-C2:** Jetson boot smoke test; confirm no CBB abort during GPU init.

## Deliverable — PR template

```
## Summary
Session F fixes from code-review-2026-04-12: kernel core, shell, VFS, GPU,
Lua, strings, tests.

## Issues fixed
- CORE-C1 (ELF argv), CORE-C2 (GPU Jetson probe), CORE-C3 (string dedup)
- CORE-H1..H7 (kprintf, Lua, shell tests, VFS generation, semihosting)
- CORE-M1..M4 (atoi, offsets, GPU assert, doc refresh)
- CORE-L1, L2 (trig stubs, shell parse)

## Deferred (filed as issues)
- CORE-H3 path-traversal defense (post-capstone)

## Test plan
- [ ] `make test` passes
- [ ] `make test PLATFORM=X86_64` passes
- [ ] `make kernel PLATFORM=RASPI5` deploys; `labctl boot_test --count 5` passes
- [ ] `make kernel PLATFORM=JETSON_ORIN_NANO` compiles; Jetson boot no CBB abort
- [ ] `nm build/kernel/slmos.elf | grep -E 'T (strlen|strcmp|memcpy)'` shows one def each
- [ ] New shell path-resolution tests pass
- [ ] New ELF argv-bounds test passes
- [ ] test_lua.c still passes (regression for string dedup)
```
