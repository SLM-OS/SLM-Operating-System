# Code Review Fix Sessions — April 12, 2026

Six parallel work streams derived from `docs/code-review-2026-04-12.md`. Each
session fixes a coherent slice of the codebase. Run each in a separate Claude
Code session for review granularity and context freshness.

## Sessions

| Session | Area | File | Issues | Dependency | Status |
|---------|------|------|--------|------------|--------|
| A | Memory + Boot | [session-a-memory-boot.md](session-a-memory-boot.md) | 20 | Independent | ✅ (merged in #75) |
| B | Scheduler + SMP | [session-b-scheduler-smp.md](session-b-scheduler-smp.md) | 12 | Independent | ☐ |
| C | IPC + Syscall + Components | [session-c-ipc-syscall.md](session-c-ipc-syscall.md) | 13 | Independent | ✅ (9 live-code issues fixed; 4 dead-code in `msg_router.c` removed. Pi 5 + Jetson 5/5 boot; test-pc blocked by #82) |
| D | Rust Runtime | [session-d-rust-runtime.md](session-d-rust-runtime.md) | 12 | Independent (Rust-only) | ✅ (Pi 5 hardware verified; publish-with-ack stress blocked by pre-existing #80) |
| E | Drivers | [session-e-drivers.md](session-e-drivers.md) | 9 | Independent | ☐ |
| F | Kernel Core / Shell / Strings | [session-f-kernel-core.md](session-f-kernel-core.md) | 16 | Independent | ☐ |

## Cross-session coordination

All six sessions can run in parallel. The only file-level overlap:

- **F ∩ A on `kernel/src/elf.c`:** A's BOOT-H2 touches `elf_load()` (~line 165);
  F's CORE-C1 touches `elf_setup_argv()` (~line 411). Different functions,
  same file. Whichever branch merges second should rebase.
- **C ↔ D:** Session C's audit found `kernel/src/msg_router.c` was no longer
  compiled (Rust router had taken over); C removed the dead file in commit
  `8c53ada` and redirected the four `msg_router.c`-scoped issues (IPC-C3,
  IPC-M1, IPC-M3, IPC-H1) to Session D's Rust counterpart. Session D owns
  `runtime/src/msg_router.rs` end to end.
- **A ∩ B on `kernel/include/spinlock.h` (conditional):** Only if A tackles
  MM-C2 (runtime-flag spinlock refactor) and B tackles SCHED-M2 (ticket_lock
  acquire). MM-C2 is likely deferred, so this probably won't materialize.
- **E ∩ F on `kernel/src/kprintf.c` (minor):** E's DRV-L2 touches comments at
  lines 43-53; F's CORE-H1 touches the return clamp near line 545. Different
  regions, trivial merge.

## Ground rules (all sessions)

- Read `CLAUDE.md` at project root and `kernel/CLAUDE.md` or `runtime/CLAUDE.md`
  as relevant. Those override the review's guidance.
- **Never commit without asking** (per `CLAUDE.md`).
- **Never bypass labctl** for any hardware interaction.
- **Post-change checklist** from `CLAUDE.md` is mandatory for every fix:
  1. Regression test covering the fix
  2. Update affected docs
  3. `make test` passes (QEMU)
  4. For Pi 5-affecting changes: deploy and verify through the board's supported model
     (`labctl sdwire_update` on SDWire-equipped boards, maintenance-OS / dual-boot on no-SDWire boards)
  5. Commit with descriptive message
- If a review finding turns out to be a **false positive** or the fix is
  wrong, flag it and skip — don't force a broken change.
- **One commit per logical fix.** Don't batch unrelated issues.
- Commit message format: `<ISSUE-ID>: <short description>` (e.g.,
  `MM-H1: invalidate TLB after vmm_map_block descriptor write`).

## Session workflow

Each session should:

1. **Read** the session doc and the referenced section of the main review.
2. **Propose** a work order; confirm with John before touching code.
3. **Work in a worktree** (keeps main clean for other sessions):
   ```
   git worktree add ../slm-os-session-X -b fix/session-X-<area>
   ```
4. **Fix → test → commit → repeat** for each issue ID.
5. **Open one PR per session** (not per issue) when all planned work is done.
6. **Wait for John's review** before merging.

## Verification order of operations

For each fix:
- QEMU build (`make test`) — non-negotiable baseline
- Pi 5 (`make kernel PLATFORM=RASPI5` + deploy through the board's supported
  path + `boot_test`) — required for memory, scheduler, driver, or ARM64 boot
  changes
- Jetson (`make kernel PLATFORM=JETSON_ORIN_NANO`) — verify build; hardware
  testing optional unless the fix is Jetson-specific
- x86-64 (`make test PLATFORM=X86_64`) — required for IDT, LAPIC, or x86 boot
  changes

## Priority within session

All sessions: start with **CRITICAL**, then **HIGH**, then **MEDIUM**, then
**LOW**. Skip LOW if time is tight and focus on correctness.

## What to do if you can't complete the session

- Merge what's done; file GitHub issues (per `CLAUDE.md` issue-tracking rules)
  for the remainder.
- Update the session doc with a "Deferred" section listing what's left.
- Don't leave half-finished fixes in the branch.
