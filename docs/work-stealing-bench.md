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

## Raspberry Pi 5 — pending hardware capture

```
make kernel PLATFORM=RASPI5                      # OFF baseline
labctl sdwire_update kernel build/kernel/slmos.bin
labctl power_cycle pi5-1
labctl serial_send pi5-1 "bench stealing 16"
# Capture result, then rebuild with WORK_STEALING=ON:
make kernel PLATFORM=RASPI5 WORK_STEALING=ON
labctl sdwire_update kernel build/kernel/slmos.bin
labctl power_cycle pi5-1
labctl serial_send pi5-1 "bench stealing 16"
```

| Build | Wall-clock (ms) | Per-CPU distribution | Speedup |
|---|---|---|---|
| OFF | TBD | TBD | — |
| ON  | TBD | TBD | TBD |

## Jetson Orin Nano — pending hardware capture

```
make kernel PLATFORM=JETSON_ORIN_NANO
scp build/kernel/slmos.elf jetson-nano-2:/tmp/
ssh jetson-nano-2 sudo slmos-kexec /tmp/slmos.elf
labctl serial_send jetson-nano-2 "bench stealing 24"  # 6 cores
# Repeat with WORK_STEALING=ON build.
```

| Build | Wall-clock (ms) | Per-CPU distribution (0/1/2/3/4/5) | Speedup |
|---|---|---|---|
| OFF | TBD | TBD | — |
| ON  | TBD | TBD | TBD |

**Jetson-specific caveat:** the known ABA race in the work-stealing
deque (#139) is still present. `bench stealing` at N ≤ 32 has not
reproduced it in testing, but if the benchmark hangs or reports
task counts ≠ N, rebuild with `WORK_STEALING=OFF` and re-run to
isolate.

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

## Recommendation for S4 (default flip)

Based on QEMU ARM64 alone:

- ✅ 1.7× speedup on a clean load-imbalance test.
- ✅ No observed correctness regressions under the regression
  suite (`make test` passes in both configurations).
- ⏳ Pi 5 / Jetson / x86-64 data still pending.

**Blocker for flipping the default:** Jetson #139 (ABA race) is the
last known bug. The S4 plan requires at least two platforms' data to
be green before flipping; once Pi 5 and Jetson numbers confirm
speedup without reviving #139, `CONFIG_WORK_STEALING` should move
from default-OFF to default-ON.

## Related

- `kernel/sched/steal_deque.c`, `kernel/sched/sched.c` —
  implementation.
- `kernel/src/shell_sys.c` (`bench stealing`) — benchmark driver.
- GitHub #105 — observability counters (Phase S2).
- GitHub #139 — known ABA race on Jetson `bench smp` with
  `WORK_STEALING=ON`.
- `docs/jetson-capstone-execution-plan.md` §S3 — phase description.
