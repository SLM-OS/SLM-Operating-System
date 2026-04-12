# Session C — IPC + Syscall + Component System

**Source:** `docs/code-review-2026-04-12.md` §3
**Scope:** 13 issues (4 CRITICAL, 3 HIGH, 4 MEDIUM, 2 LOW)
**Files touched:** `kernel/ipc/ipc.c`, `kernel/src/syscall.c`, `kernel/src/msg_router.c`, `kernel/src/component_runtime.c`, `kernel/src/component.c`, `kernel/src/shell_component.c`

## Mission

Close memory-safety and synchronization holes at the syscall boundary and in
the C-side publish/subscribe message router. The parallel Rust-side router
(`runtime/src/msg_router.rs`) is covered in Session D — keep semantics
consistent between the two.

## Working rules

- Read `CLAUDE.md` and `kernel/CLAUDE.md`. No runtime-side changes here.
- This session's changes are mostly local to IPC and component files — low
  collision risk with other sessions.
- Coordinate with Session D if priority or wildcard subscription semantics
  change.
- Work in a worktree: `git worktree add ../slm-os-session-c -b fix/session-c-ipc`.
- Ask John before committing.

## Issues

### CRITICAL

#### IPC-C1 — Integer overflow in `sys_infer_handler`
- **File:** `kernel/src/syscall.c:136`
- **Problem:** `in_len * 4` and `out_len * 4` overflow if `in_len > UINT32_MAX/4`;
  subsequent `validate_user_ptr` passes with a tiny size.
- **Fix:**
  ```c
  if (in_len > UINT32_MAX / 4 || out_len > UINT32_MAX / 4)
      return -1;
  if (!validate_user_ptr(input, in_len * 4) || !validate_user_ptr(output, out_len * 4))
      return -1;
  ```
  Audit other syscall handlers for the same pattern.
- **Verification:** Add a test passing `in_len = 0x40000001`; expect return `-1`.

#### IPC-C2 — Echo mailbox data-before-ready ordering
- **File:** `kernel/src/component_runtime.c:564-575`
- **Problem:** Plain stores to `echo_mailbox.data[]` followed by an atomic
  release on `ready`. The plain stores are not ordered by the atomic store
  alone on ARM64.
- **Fix:**
  ```c
  /* Copy data with plain stores */
  for (int i = 0; i < len; i++) echo_mailbox.data[i] = message[i];
  /* Release fence ensures all preceding stores are visible before ready=1 */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(&echo_mailbox.ready, 1, __ATOMIC_RELAXED);
  ```
  The fence + relaxed store is equivalent to release semantics and makes the
  data-flag dependency explicit.
- **Verification:** Existing echo service tests; if flaky on Pi 5, add a
  stress test that publishes → consumes many times.

#### IPC-C3 — Message router has no SMP synchronization
- **File:** `kernel/src/msg_router.c` (entire file)
- **Problem:** Static `topics[]` accessed from publish and subscribe with no
  locking. The C side was not covered by commit 3c2d9a1 (Rust only).
- **Fix:** Add `static spinlock_t msg_router_lock = SPINLOCK_INIT;` at file
  scope. Wrap **every** public function body:
  ```c
  int msg_router_subscribe(...) {
      irq_flags_t f = spin_lock_irqsave(&msg_router_lock);
      int ret = msg_router_subscribe_unlocked(...);
      spin_unlock_irqrestore(&msg_router_lock, f);
      return ret;
  }
  ```
  Refactor existing bodies into `*_unlocked` helpers. Keep mailbox writes
  (which should stay non-blocking) outside the lock where possible.
- **Verification:** Stress test publish + subscribe from multiple CPUs.

#### IPC-C4 — Component cleanup context array reused
- **File:** `kernel/src/component_runtime.c:541-543`
- **Problem:** `cleanup_ctxs[comp_idx]` shared across task lifetimes. If
  component index is reused after unload, A's surviving task fires B's
  cleanup.
- **Fix:** Replace the static array with per-task heap allocation:
  ```c
  struct component_task_ctx {
      int component_idx;
      /* ... any other cleanup state ... */
  };
  struct component_task_ctx *ctx = pmm_alloc_page();  /* or a slab */
  ctx->component_idx = comp_idx;
  task_set_cleanup(task, component_task_cleanup, ctx);
  /* In cleanup callback: use ctx, then free */
  ```
  Ensure `task_set_cleanup` callback receives the ctx pointer and free path
  is correct on task exit.
- **Verification:** Test that creates + unloads components in a loop,
  reusing indices.

### HIGH

#### IPC-H1 — `str_copy` at message router boundary reads past non-NUL-terminated input
- **File:** `kernel/src/msg_router.c:64-69`, callers at `112, 172-173, 206`
- **Problem:** `str_copy` does bound to `max`, but still reads `src[i]` up to
  `max-1` times. If the caller-provided buffer is shorter than `max` and not
  NUL-terminated, the reads wander.
- **Fix:** This is mostly a boundary issue — fix at the syscall entry point
  (see IPC-H2). If defense-in-depth is desired, make `str_copy` accept a
  caller-provided `src_len` and copy only `min(src_len, max-1)` bytes.

#### IPC-H2 — `validate_user_ptr` doesn't enforce NUL termination
- **File:** `kernel/src/syscall.c:80-91` (`sys_send_handler`), others
- **Fix:** For every string argument, add:
  ```c
  if (len == 0 || ((const char *)data)[len - 1] != '\0')
      return -1;
  ```
  after the `validate_user_ptr` check. Audit all syscall handlers that take
  a `(ptr, len)` string pair.

#### IPC-H3 — Integer overflow in `msg_queue_create`
- **File:** `kernel/ipc/ipc.c:167-169`
- **Fix:**
  ```c
  if (capacity == 0 || capacity > SIZE_MAX / MSG_PRIO_COUNT) return NULL;
  size_t total_capacity = capacity * MSG_PRIO_COUNT;
  if (msg_size == 0 || total_capacity > SIZE_MAX / msg_size) return NULL;
  size_t buffer_size = total_capacity * msg_size;
  ```

### MEDIUM

#### IPC-M1 — Publisher iterates subscribers unlocked
- **File:** `kernel/src/msg_router.c:166-189`
- **Fix:** Resolved by IPC-C3 if the publish loop runs under
  `msg_router_lock`. Verify the whole subscriber iteration is inside the
  critical section.

#### IPC-M2 — Missing validation of `topic_out` in `sys_recv_handler`
- **File:** `kernel/src/syscall.c:107`
- **Fix:**
  ```c
  if (!validate_user_ptr(topic_out, MSG_ROUTER_TOPIC_LEN))
      return -1;
  ```

#### IPC-M3 — `component_idx` unvalidated at subscribe
- **File:** `kernel/src/msg_router.c:97-140`
- **Fix:** At top of `msg_router_subscribe`:
  ```c
  if (component_idx < 0 || component_idx >= COMPONENT_MAX_COUNT)
      return -1;
  ```

#### IPC-M4 — Global `swap_state_buf` unprotected
- **File:** `kernel/src/component_runtime.c:720-744`
- **Fix:** Add `static spinlock_t swap_state_lock = SPINLOCK_INIT;` and guard
  `component_hot_swap_stateful` + `component_get_swap_state` with it.
  Alternative: document single-threaded invariant and add
  `DEBUG_ASSERT(current_cpu() == 0)`.

### LOW

#### IPC-L1 — `msg_queue_create` has no capacity cap
- **File:** `kernel/ipc/ipc.c:145-150`
- **Fix:** `#define MSG_QUEUE_MAX_CAPACITY 65536`; reject larger.

#### IPC-L2 — Echo mailbox pre-init not atomic
- **File:** `kernel/src/component_runtime.c:531-533`
- **Fix:**
  ```c
  __atomic_store_n(&echo_mailbox.ready, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&echo_running, 1, __ATOMIC_RELEASE);
  ```

## Suggested work order

1. **IPC-C1** (10 min) — syscall overflow
2. **IPC-H3** (15 min) — msg_queue overflow
3. **IPC-M2** (5 min) — topic_out validate
4. **IPC-M3** (5 min) — component_idx range check
5. **IPC-L1** (5 min) — msg_queue cap
6. **IPC-C3** (1 hr) — msg_router spinlock (IPC-M1 dissolves)
7. **IPC-H1, H2** (30 min) — string NUL termination at syscall + router
8. **IPC-C2** (15 min) — release fence in echo mailbox
9. **IPC-L2** (10 min) — atomic mailbox init
10. **IPC-M4** (20 min) — swap_state lock
11. **IPC-C4** (1 hr) — per-task cleanup context

## Testing requirements

- **Every fix:** `make test`. Add a dedicated negative-input test in
  `kernel/tests/test_syscall.c` for IPC-C1, IPC-H2, IPC-H3, IPC-M2.
- **IPC-C3, M1:** add a multi-CPU publish/subscribe stress test in
  `kernel/tests/test_ipc.c` or a new `test_msg_router.c`.
- **IPC-C4:** component lifecycle stress test — create, unload, create
  again, verify cleanup fires on correct component.
- **Pi 5:** `labctl boot_test --count 5` after IPC-C3 and IPC-C2 (cross-CPU
  visibility).

## Deliverable — PR template

```
## Summary
Session C fixes from code-review-2026-04-12: IPC, syscall, components.

## Issues fixed
- IPC-C1..C4 (CRITICAL: overflow, ordering, lock, cleanup)
- IPC-H1..H3 (HIGH: string safety, queue overflow)
- IPC-M1..M4 (MEDIUM: validation, polish)
- IPC-L1, IPC-L2 (LOW: caps + atomics)

## Test plan
- [ ] `make test` passes
- [ ] New test_syscall negative-input cases pass
- [ ] Message router stress test (multi-CPU) passes
- [ ] Component lifecycle reuse test passes
- [ ] Pi 5 `boot_test --count 5` passes
```
