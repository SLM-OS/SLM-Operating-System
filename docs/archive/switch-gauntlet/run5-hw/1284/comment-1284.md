Decided by the session's rule as **the test's own race**: no kernel-side witness violation anywhere, `bad>0` with the original test, `bad=0` with the fixed one. It is a decision by exclusion; the race itself was not observed. Single-agent lab session, 2026-11-16; log `gauntlet/switch-5`, `docs/archive/switch-gauntlet/switch-5.md` Entry 56; evidence `gauntlet-5-hw-jetson` (and `gauntlet-5-hw-pi5` for the Pi 5 row), `docs/archive/switch-gauntlet/run5-hw/1284/`.

**Kernel-side witness, independent of the test.** New debug option `PD_WITNESS` (default OFF; same `.text` size when off, QEMU ARM64, `Q-text-size-witness-off.txt`), commit 47c0cf28 on `diag/1284-witness`, not for merge. On the path where `schedule()` switched and the incoming task resumes after `switch_to()`, with local IRQs masked:
- W1, before the post-switch clear: `preempt_disabled[cpu_id()] == 1` (the CPU that switched the task in set its own flag before its window, and nothing clears it before the task does);
- W2, right after the clear: `preempt_disabled[cpu_id()] == 0`, CPU id re-read.

It fires on the #1229 regression (the only mutation class tried; W1 has no dedicated mutation): with the clear reverted to `preempt_disabled[this_cpu] = 0`, QEMU x20 gives `resume=13 exit=111` violations and the test fails 20 of 20 (`Q-qemu-mutation-1229-reverted-witness-ON-x20.txt`). `make test` passes with it on (861078 checks, 0 violations) and off.

**Arms, jetson-nano-2, witness ON, the test alone x1000 per boot, UEFI-direct boots alternating A/B, then one kexec boot each:**

| Arm | Kernel | Iterations | `bad != 0` | Witness checks | Violations |
|---|---|---|---|---|---|
| A | c93f0891 = 6439476f (ae1399ba + the test print) + witness | 7000 UEFI + 1000 kexec | **1** (`A-uefi-c93f0891-x1000-boot02.txt:3805`, `bad=1 (first on cpu 4)`) | 133526 + 18007 | **0** |
| B | 775563b0 = cd1557fe (the masked read below) + witness | 7000 UEFI + 1000 kexec | 0 | 128724 + 18379 | **0** |

The decision comes from the 14 UEFI-direct boots: the observation reproduced in arm A (1 in 7000) while the witness saw no violation, including 18742 checks in that boot; B saw `bad=0` in 7000 with no violation. The kexec rows (0 `bad` both) and the Pi 5 row would on their own be undecided. One Pi 5 boot of arm A (1000 iterations): `bad=0`, 0 violations in 21747 checks.

Limits (Entry 56 and the blind critic, PASS with 0 blocking findings):
- One event cannot separate the arms; nothing in the `bad=1` iteration shows the worker changed CPU between its reads. A direct check would record a re-read `cpu_id()` and the switch count on `pd != 0` (not run).
- The witness is silent on a wrong-CPU *set* (the #1242 class) seen from the CPU whose flag was set, and on writes after `schedule()` returns (notably `resched_trampoline`'s set and clear); the test reads the flag after `task_sleep_ms()` returns.
- B also drops the diagnostic print, so B alone does not isolate the masking; the fixed test without the witness has no hardware row.
- CPU 0 had no worker wake-ups and no witness checks on hardware (QEMU covered it). The CPU in `bad=1 (first on cpu 4)` is the id the worker read, under the race reading the CPU it had left.

**The test fix (test only):** cd1557fe reads `cpu_id()` and `preempt_disabled[c]` with local IRQs masked (FIQ too under `PI5_FIQ_TIMER`), restored right after; the assertion is unchanged. With the #1229 line reverted it still fails 20 of 20 in QEMU (`Q-qemu-mutation-1229-reverted-fixed-test-witness-ON-x20.txt`). `/ci` at cd1557fe: 11 of 11 steps pass (`ci-cd1557fe/`). Pushed to #1266's branch.

**#1285 check:** #1285's head 87061023 + witness + this two-worker fixed test in place of #1285's three-worker test, one UEFI-direct boot x1000 (`C-uefi-1285-34c43dc0-x1000-boot01.txt`): `bad=0`, 0 witness violations in 19938 checks; 6 of 1000 iterations fail on `pdm_moved > 0` (`moved=0`), the padded layout's one-CPU wake-ups (#1263) that #1285's own three-worker test replaces this test for.

This comment does not close the issue.
