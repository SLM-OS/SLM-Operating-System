# Context-Switch Cost Ledger

A running record of what each scheduler change costs in `bench context`
(one round trip between two tasks, nanoseconds per switch, mean of the
run means).

**Baseline** (`bench context` at main dc51107c, 2026-11-14, switch gauntlet
run 5 close-out, Entry 39's files):

| Board | Boot path | Baseline |
|---|---|---|
| Jetson Orin Nano (jetson-nano-2) | kexec | **88.0 ns** |
| Raspberry Pi 5 (pi-5-1) | SD | **100.7 ns** |

## Rule

Every pull request that touches `kernel/sched/`, the context switch
(`kernel/arch/*/context.S`, `switch_to`, the exception-return trampoline) or
the task lifecycle (`task_create`, `task_exit`, `task_destroy`, wake and
sleep paths) adds one row to the table below, with:

- `bench context` **before and after, on both boards** (Jetson kexec and
  Pi 5), at least **5 raw runs per side**, the raw capture files linked;
- the delta, in ns and percent;
- **who accepted the cost and why**.

The running total below is updated in the same pull request. A change that
measures no difference still adds its row (delta 0, files linked). The
counter resolution is 32 ns per sample on Jetson and about 18.5 ns on Pi 5,
so p50 and p95 move only in whole steps; compare the means of the run
means and state the spread.

## Running total since the baseline

| Board | Baseline | Now | Total change |
|---|---|---|---|
| Jetson kexec | 88.0 ns | 99.0 ns | **+11.0 ns (+12.5%)** |
| Pi 5 | 100.7 ns | 108.7 ns | **+8.0 ns (+7.9%)** |

"Now" is the previous total plus the delta of each row, since the rows are
measured in different sessions against their own same-session "before"
(97.3 + 1.7 and 109.0 - 0.3). The #1263 row's deltas are inside the
run-to-run spread on both boards.

## Rows

| PR | Change | Jetson kexec before → after | Pi 5 before → after | Accepted by, and why |
|---|---|---|---|---|
| #1234 fix (this PR) | Switch-in only after the previous CPU has released the task; pinned CPU id in `schedule()` (ADR 0008) | 88.0 → 97.3 ns, **+9.3 ns / +10.6%** (6 runs per side, sd 2.7 / 2.0, Welch t 6.9) | 100.7 → 109.0 ns, **+8.3 ns / +8.3%** (6 runs per side, sd 1.0 / 1.4, Welch t 11.7) | The user (John Jezl), 2026-11-14: accepted as the cost of closing the switch-in window (#1234, #1238, #1242). |
| #1263 (awaits acceptance) | One cache line per per-CPU lock and scheduler word (placement only): padded `SPINLOCK_ARRAY` elements, `preempt_disabled[]`, `current_task[]`, `switch_in_waiting[]`, per-CPU `sched_diag[]` struct and the others in `docs/scheduler.md` "Per-CPU data layout" | 96.83 → 98.50 ns, **+1.67 ns / +1.7%** (6 runs per side, sd 3.19 / 2.81, Welch t 1.0, ranges overlap; p50 96 and p95 128 on every run of both sides) | 107.67 → 107.33 ns, **-0.33 ns / -0.3%** (6 runs per side, sd 1.37 / 0.82, Welch t -0.5, ranges overlap; p50 111 on every run) | Awaits acceptance by the user. No difference is shown on either board at this sample size. |

### Raw files for the #1234 row

Before = main dc51107c, after = `c2/switch-window-r2` 6a4cfa93 (the fix's
kernel code; later commits on the branch add only a default-off debug
option and documents). Three boots per side, alternating, two runs per
boot, 99 round trips per run.

- Jetson: branch `gauntlet-5-hw-jetson` (aa080076),
  `docs/archive/switch-gauntlet/run5-hw/closeout/`:
  `bench-context-jetson-base-B1-run1.txt`, `-B1-run2.txt`,
  `-B2-runs1-2.txt`, `-B3-runs1-2.txt`;
  `bench-context-jetson-head-H1-run1.txt`, `-H1-run2.txt`,
  `-H2-runs1-2.txt`, `-H3-runs1-2.txt`.
- Pi 5: branch `gauntlet-5-hw-pi5` (62a7bbe6), same directory:
  `bench-context-pi5-base-B1-runs1-2.txt`, `-B2-runs1-2.txt`,
  `-B3-runs1-2.txt`; `bench-context-pi5-head-H1-runs1-2.txt`,
  `-H2-runs1-2.txt`, `-H3-runs1-2.txt`.
- Summary computed from the raw files: `bench-context-summary.txt`
  (script `bench-summary.py.txt`) in the same directory on either branch.

### Raw files for the #1263 row

Before = `c2/switch-window-r2` ae1399ba (the #1266 head, per-CPU words
packed), after = `fix/percpu-cacheline` c0612fa7. Three boots per side,
alternating U1, P1, U2, P2, U3, P3 (on Pi 5 one SD round trip per swap), two
runs per boot, 99 round trips per run; both sides measured in one session
(2026-11-15). Directory `docs/archive/switch-gauntlet/run5-hw/1263-1207/`.

- Jetson: branch `gauntlet-5-hw-jetson`,
  `A-bench-context-jetson-base-U1-runs1-2.txt`, `-U2-`, `-U3-`;
  `A-bench-context-jetson-head-P1-runs1-2.txt`, `-P2-`, `-P3-`; the boots
  are `A-kexec-bench-{U-ae1399ba,P-c0612fa7}-{U,P}{1,2,3}-boot.txt`;
  summary `A-bench-context-summary-jetson.txt`.
- Pi 5: branch `gauntlet-5-hw-pi5`,
  `A-bench-context-pi5-base-U1-runs1-2.txt`, `-U2-`, `-U3-`;
  `A-bench-context-pi5-head-P1-runs1-2.txt`, `-P2-`, `-P3-`; the boots are
  `A-pi5-bench-{U-ae1399ba,P-c0612fa7}-{U,P}{1,2,3}-boot.txt`; summary
  `A-bench-context-summary-pi5.txt`.
- Script: `A-bench-summary.py.txt` on either branch.

This session's "before" (96.83 ns Jetson, 107.67 ns Pi 5) is the same
kernel as the #1234 row's "after" (97.3 ns, 109.0 ns), measured again: the
two sessions differ by 0.5 ns on Jetson and 1.3 ns on Pi 5, which is the
size of difference this bench does not resolve.

What the row does not measure: `bench context` switches between two tasks
on one CPU while the other CPUs idle, so it shows the cost of the padded
layout on the switch path (37 more instructions in `schedule()`, more
lines touched) but not the benefit under cross-CPU contention; that is
the R3 wake result in the pull request (109 slow finishes in 6000
iterations before, 0 in 6000 after).
