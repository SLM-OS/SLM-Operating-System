# Session B — Scheduler + SMP + Concurrency

**Source:** `docs/code-review-2026-04-12.md` §2
**Scope:** 12 issues (3 CRITICAL, 3 HIGH, 4 MEDIUM, 2 LOW)
**Files touched:** `kernel/sched/*.c`, `kernel/sched/ai/*.c`, `kernel/ipc/pi_mutex.c`, `kernel/arch/arm64/context.S`, `kernel/include/spinlock.h`, `kernel/include/task.h`

## Mission

Close race conditions and cross-CPU coherency holes in the scheduler, SMP
bring-up path, and the priority-inheritance mutex. Several issues stem from
the Pi 5 cache coherency model (no SMPEN, per-core L2) — fixes either route
through NC memory or insert explicit cache maintenance.

## Dependency on Session A

If Session A is running concurrently and decides to relocate NC memory
structures (unlikely but possible), rebase onto A's branch before starting
SCHED-C2. If A has already merged, pull main. Otherwise, this session is
independent.

## Working rules

- Read `CLAUDE.md` and especially `kernel/CLAUDE.md` §NC-Memory, §Idle-Task-DAIF,
  and §Pi-5-Timer-IRQs. These describe non-obvious invariants the fixes must
  preserve.
- **Struct layout rule:** `struct task`'s `context` field must stay at offset
  `0x20` (see `context.S`). If you add fields, add them AFTER `context` OR
  update `TASK_CONTEXT_OFFSET`.
- Pi 5 testing is non-negotiable for this session — races only appear under
  real cross-CPU load. `labctl boot_test --count 10` is the baseline gate.
- Work in a worktree: `git worktree add ../slm-os-session-b -b fix/session-b-sched-smp`.
- Ask John before committing.

## Issues

### CRITICAL

#### SCHED-C1 — Secondary CPU infinite busy-wait without timeout
- **File:** `kernel/sched/smp.c:353`
- **Problem:** `while (!scheduler_is_initialized()) { for (volatile int d = 0; d < 100000; d++) {} }` — no timeout.
- **Fix:**
  ```c
  for (int retry = 0; retry < SCHED_INIT_MAX_RETRIES; retry++) {
      if (scheduler_is_initialized()) break;
      for (volatile int d = 0; d < 100000; d++) {}
  }
  if (!scheduler_is_initialized()) {
      /* Write diagnostic to NC memory before panicking */
      *(volatile uint32_t *)(NC_MEM_BASE + NC_MEM_SIZE - 256 + logical_cpu_id * 4) = 0xDEAD0001;
      panic("CPU %u: scheduler init timeout", logical_cpu_id);
  }
  ```
  Pick `SCHED_INIT_MAX_RETRIES` so total wait ≈ 5 seconds at Pi 5 clock.
- **Verification:** Boot test on Pi 5.

#### SCHED-C2 — `current_task[cpu]` cross-CPU coherency
- **File:** `kernel/sched/task.c:465-479` (`task_current`, `task_set_current`)
- **Problem:** On Pi 5 without SMPEN, `cache_invalidate(&current_task[cpu])` in
  the reader path does not guarantee visibility of writes from another CPU's L2.
- **Fix option A (preferred, requires NC layout change):** Relocate
  `current_task[MAX_CPUS]` into NC memory at boot, via a new
  `ncmem_alloc` call in `task_table_init` or a new `current_task_init`.
  Callers then access it directly — no cache maintenance needed.
- **Fix option B (if layout constraints block A):** In `task_set_current`,
  after the write, `cache_clean(&current_task[cpu]); dsb ish`. In `task_current`,
  before the read, `cache_invalidate(&current_task[cpu]); dsb ish`.
  **Risk:** DC CIVAC semantics mean option B can still lose writes under
  specific orderings (see `kernel/CLAUDE.md` DC CIVAC rule).
- **Verification:** Pi 5 `bench smp` dispatches correctly to all CPUs; full
  test suite; `labctl boot_test --count 10`.
- **Cross-session check:** Coordinate with Session A if NC layout changes.

#### SCHED-C3 — Task teardown race between `task_exit` and `schedule`
- **File:** `kernel/sched/task.c:446` (`task_exit`), `kernel/sched/sched.c:1013` (picker)
- **Problem:** `task_exit` sets state outside `rq_lock`; scheduler may pick
  a TERMINATED task. On Pi 5 the race is worsened by the split between NC
  `task->state` and cacheable `task->next`.
- **Fix:** In `task_exit`:
  ```c
  irq_flags_t flags = rq_lock_irqsave(assigned_cpu);
  task->state = TASK_TERMINATED;
  scheduler_remove_task(task);   // must be safe to call under rq_lock
  rq_unlock_irqrestore(assigned_cpu, flags);
  schedule();                     // then context-switch away
  ```
  Audit `scheduler_remove_task` to confirm it doesn't recursively take
  `rq_lock` or call anything that yields.
- **Verification:** Stress test that creates + exits many tasks concurrently
  on multiple CPUs.

### HIGH

#### SCHED-H1 — `pi_mutex` wait loop interrupt-unsafe
- **File:** `kernel/ipc/pi_mutex.c:91-98`
- **Problem:** After unlocking `mutex->guard` and restoring IRQs, the spin
  loop yields with IRQs enabled. A timer ISR → `schedule()` can reach back
  to `pi_mutex` paths and deadlock on `guard`.
- **Fix (conservative, capstone-ready):** Mask IRQs in the wait loop:
  ```c
  irq_flags_t f;
  while (mutex->locked) {
      f = irq_save();
      if (!mutex->locked) { irq_restore(f); break; }
      irq_restore(f);
      yield();
  }
  ```
- **Fix (better, post-capstone):** Proper sleep queue — block on a wait
  condition, wake on release. File as a GitHub enhancement issue.
- **Verification:** `test_pi_mutex.c` suite; stress with multiple waiters.

#### SCHED-H2 — Zombie cleanup after `rq_unlock` races
- **File:** `kernel/sched/sched.c:998-1010`, `kernel/sched/task.c:502-548`
- **Fix:** Move `task_destroy` inside `rq_lock` OR mark `task->state = TASK_DESTROYED`
  before releasing the lock. The scheduler's pick logic already filters
  TERMINATED; extend it to skip DESTROYED.
- **Verification:** Reaper stress test; combine with SCHED-C3 test.

#### SCHED-H3 — AI scheduler task scan unsynchronized
- **File:** `kernel/sched/sched.c:131-165` (`ai_update_top_tasks`)
- **Problem:** Iterates all CPU run queues without taking any `rq_lock`.
  Partially-modified queues yield garbage task pointers into the inference
  input path.
- **Fix:** Iterate CPUs in ascending order, take each `rq_lock` one at a time,
  copy the top-N task pointers + priorities, release, move on. Avoids
  holding all locks simultaneously (which could starve other CPUs).
- **Verification:** AI scheduler tests; verify no stale pointers reach FFI.

### MEDIUM

#### SCHED-M1 — `preempt_disabled` set after `task_set_current`
- **File:** `kernel/sched/sched.c:1101-1137`
- **Fix:** Move `preempt_disabled[this_cpu] = 1;` to immediately before the
  `task_set_current(next)` call.

#### SCHED-M2 — Ticket lock acquire semantics
- **File:** `kernel/include/spinlock.h:282-287`
- **Fix:** After the WFE returns, re-read the owner with `ldar` (acquire load)
  before comparing against the waiter's ticket. Remove the stale `dmb ish`
  and let `ldar` provide the ordering.
- **Verification:** QEMU stress test with contended lock.

#### SCHED-M3 — Task DAIF init documentation
- **File:** `kernel/sched/task.c:306`, `kernel/arch/arm64/context.S:126-128`
- **Fix:** No code change. Document in `kernel/include/task.h` and
  `docs/smp.md` that tasks on Pi 5 start with IRQ masked (DAIF.I=1) per
  platform requirement (`kernel/CLAUDE.md` §Pi-5-Timer-IRQs).

#### SCHED-M4 — Task unlock ordering on Pi 5
- **File:** `kernel/sched/task.c:34-37` (`TASK_UNLOCK_IRQRESTORE` macro)
- **Fix:** Add `isb` after `dsb ish` on platforms without SMPEN.

### LOW

#### SCHED-L1 — Diagnostic NC writes scattered in scheduler code
- **Files:** `kernel/sched/smp.c:297,357`, `kernel/sched/sched.c:387`, others
- **Fix:** Gate behind `#ifdef SCHED_DEBUG_NC_TRACE` or move to a helper
  in a new `kernel/include/nc_trace.h`.

#### SCHED-L2 — Volatile counter busy-wait portability
- **Files:** `kernel/sched/smp.c:353, 436`
- **Fix:** Replace `for (volatile int d = 0; d < N; d++)` with
  `timer_busy_wait_us(N)` where a timer is available.

## Suggested work order

1. **SCHED-C1** (30 min) — timeout + panic diagnostic
2. **SCHED-M1** (5 min) — preempt flag ordering
3. **SCHED-H3** (1 hr) — AI scanner locking
4. **SCHED-H1** (30 min) — pi_mutex IRQ masking (conservative fix)
5. **SCHED-C3** (1 hr) — task_exit + rq_lock
6. **SCHED-H2** (30 min) — zombie cleanup flag
7. **SCHED-C2** (1-2 hr) — `current_task[]` NC relocation — **coordinate with Session A**
8. **SCHED-M2** (15 min) — ticket_lock acquire
9. **SCHED-M3, M4** (15 min) — docs + ISB
10. **SCHED-L1, L2** (30 min) — diagnostic cleanup

## Testing requirements

- **Every fix:** `make test` (QEMU ARM64). Must pass.
- **SCHED-C1, C2, C3, H2:** Pi 5 `labctl boot_test --count 10`.
  For SCHED-C2 specifically, run `bench smp` from the shell and verify
  even task distribution across all 4 CPUs.
- **SCHED-H1:** `test_pi_mutex.c` suite — add a contended test if missing.
- **SCHED-H3:** enable AI scheduler (`make test AI_SCHED=ON`) and run full
  suite.
- **All SMP changes:** `make test PLATFORM=X86_64` to confirm nothing breaks
  the x86 scheduler path.

## Deliverable — PR template

```
## Summary
Session B fixes from code-review-2026-04-12: scheduler, SMP, concurrency.

## Issues fixed
- SCHED-C1, SCHED-C2, SCHED-C3 (CRITICAL races)
- SCHED-H1, SCHED-H2, SCHED-H3 (lock / cleanup correctness)
- SCHED-M1..M4 (ordering + documentation)
- SCHED-L1, SCHED-L2 (cleanup)

## Test plan
- [ ] `make test` (QEMU ARM64)
- [ ] `make test AI_SCHED=ON`
- [ ] `make test PLATFORM=X86_64`
- [ ] Pi 5: `boot_test --count 10` passes
- [ ] Pi 5: `bench smp` dispatches to all 4 CPUs
- [ ] `test_pi_mutex.c` contention test passes
- [ ] Jetson compile check (`make kernel PLATFORM=JETSON_ORIN_NANO`)
```
