# x86-64 hardware validation — 2026-04-15

**Gap closed:** P1-2 in `docs/x86-64-capstone-gap-closure-plan.md`.

**Test-pc:** Gigabyte H610M S2H V2, i7-6700 (8 logical CPUs), 16 GB DDR4,
RTX 3050 (GA107). UEFI boot order reconfigured to prefer the SDWire SD
card before the external Ubuntu SSD.

**Image:** `build/kernel/slmos-x86.img` built from
`worktree-x86-64-capstone-gap-work` HEAD after the `mathf::{sqrtf,tanhf}`
workaround for #141 (branch `workaround/issue-141-libm-x86`, commit
`fdc1f5e`). `make x86-disk-verify` passes 9/9 structural checks.

Flashed via `labctl sdwire flash --no-reboot test-pc build/kernel/slmos-x86.img`
(26.3 s, 5.1 MB/s). Boot tested via `labctl boot_test` and the multi-task
preemption check via a ser2net-driven Python harness.

---

## Results

### 1. Boot reliability

`labctl boot_test --count 10 --expect 'slmos>' --timeout 45` → **10/10 PASS**.

| Run | Time | Status |
|---:|---:|---|
| 1  | 17.2 s | PASS |
| 2  | 18.7 s | PASS |
| 3  | 18.6 s | PASS |
| 4  | 18.7 s | PASS |
| 5  | 18.7 s | PASS |
| 6  | 18.7 s | PASS |
| 7  | 18.7 s | PASS |
| 8  | 18.7 s | PASS |
| 9  | 18.7 s | PASS |
| 10 | 18.7 s | PASS |

Average successful boot time: **18.5 s** (power-on to `slmos>` prompt).
Zero SLM-OS-side failures. An earlier run with the Kasa power strip
(8/10) showed 2 Kasa `AuthenticationError` failures at 0.0 s that never
reached SLM-OS; this is a known labctl infrastructure flake unrelated
to the kernel. Filed tracking: none yet — surface to the labctl repo
if it recurs.

Exceeds the gap-closure acceptance bar of ≥ 9/10.

### 2. Multi-task preemption (sleep while `component run echo` active)

For each iteration: start the echo service, issue `sleep 2000`, measure
wall-clock from shell send to next prompt return via ser2net.

| Run | Wall-clock | In [2000, 2200] ms |
|---:|---:|---|
| 1 | 2007 ms | ✅ |
| 2 | 2008 ms | ✅ |
| 3 | 2007 ms | ✅ |
| 4 | 2008 ms | ✅ |
| 5 | 2007 ms | ✅ |

**5/5 in-range** (~0.4 % over target, well inside the 10 % acceptance
window). The echo-service task is `COMPONENT_PRIORITY_IDLE` — shares
the round-robin tier with the shell so the shell's `sleep 2000`
actually gets preempted off when echo is runnable, and the timer
ISR + scheduler return control in time.

### 3. SMP cross-CPU dispatch (`bench smp`)

```
slmos> bench smp

SMP Cross-CPU Dispatch Test
===========================
  Dispatched 'smp1' to CPU 1
  Dispatched 'smp2' to CPU 2
  Dispatched 'smp3' to CPU 3
  Dispatched 'smp4' to CPU 4
  Dispatched 'smp5' to CPU 5
  Dispatched 'smp6' to CPU 6
  Dispatched 'smp7' to CPU 7
```

**7/7 secondary CPUs dispatched.** Matches the 8/8 online count reported
at boot (CPU 0 runs the dispatching shell itself). The per-CPU
completion-flag verification at the tail of the bench command is
gated behind `PLATFORM_HAS_NC_MEMORY` and prints "(NC memory
required)" on x86-64; for the P1-2 acceptance the "dispatches" line
is what the plan asks for, but see finding #2 below for the followup.

---

## Findings

1. **`Slept N ms` display overflow on x86-64** — the `sleep` shell
   command prints `Slept 18446744070117 ms (requested 2000 ms)` on
   test-pc. Root cause is `slm_get_time_ns()` in `kernel/src/slm_ffi.c`
   computing `ticks * 1e9 / freq`: on i7-6700 with a ~3.4 GHz TSC,
   `ticks` exceeds `UINT64_MAX / 1e9 ≈ 1.84e10` after ~5 s of uptime,
   so the multiply wraps before the divide. Wall-clock behavior is
   fine (verified above); only the display is wrong.
   **Fixed 2026-04-15** (#171): split the computation into
   `secs * 1e9 + (frac_ticks * 1e9) / freq` — both multiplies
   bounded for any realistic uptime on supported timer frequencies.
   Pure helper `slm_time_ticks_to_ns()` extracted so the overflow
   boundary is unit-testable (`test_slm_time_ticks_to_ns_no_overflow`
   in `test_scheduler.c`).

2. **`bench smp` completion wait is ARM64-only** — the per-CPU done
   flags live in NC memory; x86-64 prints "(NC memory required)" and
   skips the wait. P1-2's acceptance is "dispatches to all CPUs",
   which passed, but the bench should grow an x86-64 completion path
   (cacheable BSS flags are fine since x86-64 caches are coherent).
   **Filed as a follow-up issue.**

3. **Kasa power-plug auth flakes** — one in every ~20 `power_cycle`
   calls gets an `AuthenticationError`. Transient labctl
   infrastructure issue, not an SLM-OS finding. Monitor; escalate to
   labctl repo if frequency rises.

---

## Reproducibility

Dev-machine steps (assumes labctl configured, test-pc UEFI
boot-orders SD first):

```bash
make x86-disk PLATFORM=X86_64
labctl sdwire flash --no-reboot test-pc build/kernel/slmos-x86.img
labctl boot-test test-pc --expect 'slmos>' --runs 10 --timeout 45
python3 scripts/tests/x86-multitask-boot-test.sh   # or the .sh wrapper
```

Or one-shot:

```bash
make x86-hw-validate PLATFORM=X86_64 SLMOS_LABCTL=1
```

---

## Acceptance

- [x] 5/5 `sleep 2000` + `component run echo` runs return within 10 %
  of target — observed 5/5 within 0.4 %.
- [x] `boot_test --count 10` ≥ 9/10 PASS — observed 10/10.
- [x] `bench smp` dispatches to all available CPUs — 7/7 secondaries
  dispatched.
- [x] Dated results document committed to `docs/testing/`.

**P1-2 CLOSED.**
