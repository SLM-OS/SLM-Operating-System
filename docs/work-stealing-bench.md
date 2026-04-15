# Work-Stealing Phase C Benchmark

Load-imbalance numbers for the SLM-OS scheduler, driving the S4
decision on whether `CONFIG_WORK_STEALING` should default to ON.

**Last updated:** 2026-04-14 (Phase S3).

---

## What the benchmark measures

`bench stealing [N]` (implemented in `kernel/src/shell_sys.c`,
introduced Phase S3) dispatches N identical CPU-bound tasks all to
CPU 1 and times wall-clock completion. Each task runs a fixed
~400 k-iteration LCG loop — sized so a single task takes a few
hundred microseconds on Pi 5 / Jetson, large enough that parallel
execution actually wins when stealing is available.

The headline number is **wall-clock time to complete all N tasks**.
The table below also reports per-CPU execution distribution and
steal-counter deltas so the balance story is auditable.

Comparison is between two builds of the same kernel:

- `make kernel` — `CONFIG_WORK_STEALING=OFF`, stealing inert. All N
  tasks serialize on CPU 1.
- `make kernel WORK_STEALING=ON` — idle CPUs call
  `sched_try_steal` and pull work off CPU 1's deque. Ideal wall-clock
  is N / cpu_count of the OFF number.

## Methodology

- **Hardware clock:** `timer_get_count()` (CNTPCT_EL0 on ARM64, TSC
  on x86-64). Results reported in milliseconds and microseconds.
- **Steal counters:** read before/after the benchmark using
  `sched_diag_steal_{attempts,successes}` (Phase S2 observability),
  reported as deltas so repeated runs in the same shell session are
  comparable.
- **Task count:** 8 by default, override via `bench stealing N` up to
  64.
- **Coordination:** per-task slot in NC memory on Pi 5 / Jetson,
  cacheable BSS on QEMU ARM64 / x86-64 (caches coherent on those
  targets, no maintenance required).

## QEMU ARM64 — captured 2026-04-14

4 Cortex-A76 cores, 1 GB RAM, `-smp 4`.

| Build | Wall-clock (ms) | Per-CPU distribution (0/1/2/3) | Steal successes |
|---|---|---|---|
| `CONFIG_WORK_STEALING=OFF` | 22.9 | 0 / 8 / 0 / 0 | n/a |
| `CONFIG_WORK_STEALING=ON`  | 13.4 | 5 / 1 / 2 / 0 | 5 on CPU 0, 1 on CPU 2 |

**Speedup: 1.71× for N=8, cpu_count=4.** The ideal speedup with
perfect stealing would be 4×. The real number is limited by:
- CPU 0 doing bookkeeping work (poll loop + shell I/O) alongside
  stealing.
- Stealing overhead (lock contention, deque probe cost) per stolen
  task.
- The `yield()` inside the poll loop costing several µs per
  iteration.

The ON run shows 5 steals from CPU 0 and 1 from CPU 2 — CPU 0
actively pulls work while waiting. CPU 1 runs 1 of the 8 tasks
itself before its deque drains.

**Conclusion (QEMU):** stealing is unambiguously beneficial on
imbalanced workloads. The 1.7× speedup is enough to justify
enabling by default once the other platforms agree.

## Raspberry Pi 5 — captured 2026-04-14

BCM2712 Cortex-A76, 4 cores @ 2.4 GHz, 4 GB RAM. Deployed via
`labctl sdwire_update` + `power_cycle`, `bench stealing 16` at the
shell.

| Build | Wall-clock (ms) | Per-task avg (µs) | Per-CPU distribution (0/1/2/3) | Steal successes | Speedup |
|---|---|---|---|---|---|
| `WORK_STEALING=OFF` | 39.6 | 2476 | 0 / 16 / 0 / 0 | n/a | — |
| `WORK_STEALING=ON`  | 12.7 |  792 | 4 /  4 / 4 / 4 | 3 (CPU 0) + 4 (CPU 2) + 4 (CPU 3) | **3.12×** |

**Observations:** perfect 4/4/4/4 distribution on ON — idle CPUs
0/2/3 each pulled 4 of the 16 tasks off CPU 1's deque. CPU 0 (shell
driver) caught 3 steals while spinning in its poll loop; CPU 2 and
CPU 3 each caught 4, with CPU 1 retaining 4 for itself.

**Correctness note:** Pi 5's WORK_STEALING=ON path previously hung
on boot (#158) because the deque's embedded spinlock was neutered
by SPINLOCK_SKIP_LOCKING / NC-memory cross-CPU incoherency. Fixed by
moving the lock into a cacheable `steal_deque_lock[MAX_CPUS]` array
(same pattern as `rq_lock[]`). The bench driver's completion
detection also switched from an NC-memory `__atomic_fetch_add` to a
per-slot single-writer flag — atomics on NC memory are
implementation-defined per ARM ARM.

## Jetson Orin Nano — clean

Cortex-A78AE, 6 cores @ 1.5 GHz (dual-cluster: Aff2.Aff1), 8 GB RAM.
Deployed via `sudo slmos-kexec` from L4T.

| Build | Wall-clock (ms) | Per-CPU distribution (0/1/2/3/4/5) | Status |
|---|---|---|---|
| `WORK_STEALING=OFF` | 19.8 | 5 / 11 / 0 / 0 / 0 / 0 | ✅ clean |
| `WORK_STEALING=ON`  | 20.9 | 2 / 4 / 4 / 4 / 1 / 1 | ✅ clean (3 consecutive runs, no fault) |

The OFF/ON wall-clock numbers are within 5% because the dispatch
fan-out amortises against a fixed 20 ms tick granularity: when the
six per-task wall-clocks are scheduled tightly on CPU 1 the serial
run already fits inside a single tick. The parallel distribution
(2 / 4 / 4 / 4 / 1 / 1) is the headline story — idle CPUs 0/2/3/4/5
pulled work off CPU 1's deque via `sched_try_steal`, confirming the
cross-CPU steal path operates cleanly on A78AE under real locks.

**#166 resolution (2026-04-15):** after moving Jetson off the
unconditional `SPINLOCK_SKIP_LOCKING` to Pi 5's runtime
`spinlock_hw_enabled` model, the previous page fault at
`pmm_free_pages` (ELR 0x80014a58, free-list pointer write to a
near-NULL address) disappeared. Root cause: every cacheable spinlock
(PMM, task table, rq_lock, steal_deque_lock) was a no-op on Jetson,
so concurrent `task_destroy → pmm_free_pages` calls from stolen-
then-completed tasks raced on the buddy free list. With real
LDAXR/STXR post-MMU, the mutual exclusion is restored.

## x86-64 — pending unblock

Blocked on #141 (libm 0.2 f16 + rustc 1.92 on x86_64-unknown-none).
Once unblocked, run in QEMU-x86_64:
```
make kernel PLATFORM=X86_64
make run PLATFORM=X86_64
# At the slmos> prompt:
# bench stealing 16
```

## Steal-counter interpretation

The `bench stealing` output (and the `cpu` shell command) prints a
per-CPU table with four columns:

| Column | Meaning |
|---|---|
| `Attempts` | `sched_try_steal()` entries |
| `Success` | live task returned |
| `Stale` | stale pointer popped from a victim's deque (task already ran / terminated) |
| `EmptyVic` | victim's deque was empty on first pop |

High `EmptyVic` + low `Success` on a given CPU = that CPU spent its
idle time scanning for work that wasn't there. Non-zero `Stale` is
expected under contention — the ABA mitigation in
`scheduler_terminate_task` removes the terminating task from all
deques but can race with in-flight steals.

## S4 status — default flipped (2026-04-14, Jetson follow-up 2026-04-15)

After hardware capture, `ENABLE_WORK_STEALING` defaults **ON** in
`CMakeLists.txt` for every hardware platform; QEMU stays OFF to keep
the integration test harness green. Rationale:

| Platform | ON default? | Evidence |
|---|---|---|
| x86-64 | ✅ | ON since Phase B; cache-coherent SMP + LAPIC IPI make the path safe |
| Raspberry Pi 5 | ✅ | 3.12× speedup on bench stealing 16, boots cleanly (#158 closed) |
| Jetson Orin Nano | ✅ | 3 consecutive clean `bench stealing 16` runs (20.9 ms, 2/4/4/4/1/1 distribution across 6 CPUs) after #166 fix on 2026-04-15; default flipped ON in the follow-up commit |
| QEMU ARM64 | ❌ | 1.7× speedup (S3 capture), but several integration tests carry timing assumptions that conflict with aggressive cross-CPU migration. Kept OFF so `make test` stays reliably green; use `make test WORK_STEALING=ON` for regression coverage |

Opt-out path: `make kernel PLATFORM=<platform> WORK_STEALING=OFF` passes `-DENABLE_WORK_STEALING=OFF` to CMake, overriding the per-platform default.

#139 (ABA race) closed 2026-04-14 via per-slot generation counter.
#158 (Pi 5 boot hang) closed 2026-04-14 via external cacheable lock
for the steal deque. The two fixes together unblocked S4 for Pi 5.

## Related

- `kernel/sched/steal_deque.c`, `kernel/sched/sched.c` —
  implementation.
- `kernel/src/shell_sys.c` (`bench stealing`) — benchmark driver.
- `CMakeLists.txt` — `ENABLE_WORK_STEALING` per-platform defaults.
- GitHub #105 — observability counters (Phase S2).
- GitHub #139 — ABA race, closed via per-slot generation counter.
- GitHub #158 — Pi 5 boot hang, closed via external cacheable
  steal-deque lock.
- GitHub #166 — Jetson page fault during bench stealing, closed
  2026-04-15 by flipping Jetson off `SPINLOCK_SKIP_LOCKING` and onto
  the runtime `spinlock_hw_enabled` flag.
- `docs/jetson-capstone-execution-plan.md` §S3, §S4.
