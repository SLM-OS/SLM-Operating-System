# ADR 0011: C and Rust keep separate spinlock implementations; Rust locks reached from IRQs-off paths must save IRQs or try-lock

Status: accepted (backfilled).

## Context

The kernel (C) and the runtime (Rust, `runtime/`, a `no_std` static library) both guard shared state with spinlocks, and both are called from IRQs-off paths: `spin_lock_irqsave` sections, the IRQ-masking lwIP core lock, the GPU dispatch lock and ISRs (`runtime/CLAUDE.md`, "Locks reached from IRQs-off context").

Until PR #1052 (`9f456e96` 2026-10-31) every task started with `DAIF.I=1` and nothing cleared it, so running tasks on Pi 5 and Jetson took no timer IRQ and were never preempted (#1047). `kernel/CLAUDE.md` records that the lwIP core lock "became necessary in #1065 once #1047 made running tasks preemptible". Whether #1052 is also what exposed the Rust lock deadlock in #1093 is not recorded: unknown, ask the user.

## Decision

**C (`kernel/include/spinlock.h`).** `spinlock_t` (LDAXR/STXR with WFE, released by STLR + SEV), `ticket_lock_t`, and `spin_lock_irqsave` / `spin_unlock_irqrestore`, which save DAIF (EFLAGS on x86-64) and mask IRQs. On ARM64 the lock is barrier-only while `spinlock_hw_enabled` is 0; `vmm_init` sets it to 1 after the MMU is on (`kernel/mm/vmm.c`), because exclusives on pre-MMU non-cacheable memory hang on Cortex-A76 and abort on Cortex-A78AE. x86-64 defines the flag as 1 (`kernel/arch/x86_64/platform_x86.c`). Cross-CPU sections use a plain cacheable `spinlock_t` with `spin_lock_irqsave` (`kernel/CLAUDE.md`; ADR 0007).

**Rust (`runtime/src`).** Each module keeps a hand-written `AtomicBool` compare-exchange lock wrapped in an RAII `SpinGuard` (`runtime/CLAUDE.md`, "SpinGuard RAII pattern"); the heap is `linked_list_allocator::LockedHeap`. These locks do not consult `spinlock_hw_enabled` and leave IRQs enabled by default. The rule for any Rust lock reachable from an IRQs-off path:

- short critical section, no allocation: the guard masks IRQs through the kernel FFI `slm_irq_save` / `slm_irq_restore` (`kernel/src/slm_ffi.c`), as `msg_router` and `mm::model_mem` do;
- lock held across long work (policy consult, inference, GPU dispatch): a one-shot try-lock that skips the data when busy (`mm::eviction::try_with_active_policy`);
- no allocation with IRQs off;
- a Rust guard that prints nests into the kernel UART lock, which is safe only because UART code never calls back into Rust.

## Alternatives considered

- **Rust calls the C `spinlock_t` through FFI, or the kernel uses one shared lock type.** unknown, ask the user.
- **`SPINLOCK_SKIP_LOCKING` on Jetson (barrier-only C locks after boot).** Used until #166: CPUs raced on the PMM free list during `bench stealing`. Removed in PR #184 (`4b6cda4a` 2026-04-15); Jetson now uses the `spinlock_hw_enabled` model introduced for Pi 5 in `03f6eda1` 2026-04-03.
- **Make every Rust lock IRQ-saving.** Not adopted for long-held locks; the registry lock is held while a policy runs, so spinning on it with IRQs masked is never safe (PR #1106).

## Consequences

- Two lock implementations, with different pre-MMU behaviour and different IRQ defaults, must be reasoned about together on every C to Rust call made with IRQs off.
- The audit of the remaining plain Rust locks (`sched/xgb.rs`, `sched/inference.rs`, `mm/eviction/store.rs`, `mm/weight_cache.rs`, `loader/registry.rs`, `inference/engine.rs`, `inference/ops.rs`, `slm/registry.rs`, `slm/session.rs`, `slm/decoder.rs`) is open as #1104.
- The host build of the runtime stubs `slm_irq_save` to return 0 (`runtime/src/lib.rs`), so host tests do not exercise IRQ masking.
- Stale text: the header comment of `spinlock.h` still says Jetson skips locking after kexec, and `docs/smp.md` lists a `kernel/src/spinlock.c` that does not exist.

## Evidence

- #1093 (closed 2026-11-04): after a UEFI-direct boot `bench eviction-e2e` left jetson-nano-2 silent. CPU 0 spun with IRQs masked in `net_pump`'s telemetry publish on `mm::model_mem` and eviction-registry locks held by the preempted telnet shell on the same CPU. `23b140be` PR #1106 (2026-11-04): `model_mem` guard made IRQ-saving, stats path try-locked; before, the board wedged within 1 to 3 runs; with both changes, 8/8 runs twice.
- `msg_router` lock made IRQ-safe: `c79ff843` 2026-04-12 (#66).
- `9f456e96` PR #1052 (2026-10-31) "sched: unmask IRQs at task entry"; `1e8183b1` PR #1073 (2026-11-02) lwIP core lock (#1065).
- Tests: `test_spinlock_hw_enabled_after_boot` (`kernel/tests/test_scheduler.c`); `try_with_active_policy_gives_up_while_locked` (`runtime/src/mm/eviction/registry.rs`).
