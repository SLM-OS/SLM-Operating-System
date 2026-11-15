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
| Jetson kexec | 88.0 ns | 97.3 ns | **+9.3 ns (+10.6%)** |
| Pi 5 | 100.7 ns | 109.0 ns | **+8.3 ns (+8.3%)** |

## Rows

| PR | Change | Jetson kexec before → after | Pi 5 before → after | Accepted by, and why |
|---|---|---|---|---|
| #1234 fix (this PR) | Switch-in only after the previous CPU has released the task; pinned CPU id in `schedule()` (ADR 0008) | 88.0 → 97.3 ns, **+9.3 ns / +10.6%** (6 runs per side, sd 2.7 / 2.0, Welch t 6.9) | 100.7 → 109.0 ns, **+8.3 ns / +8.3%** (6 runs per side, sd 1.0 / 1.4, Welch t 11.7) | The user (John Jezl), 2026-11-14: accepted as the cost of closing the switch-in window (#1234, #1238, #1242). |

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
