# Blind critic report: #1284, Entry 56

**Verdict: PASS (0 blocking findings; 6 MEDIUM, 6 LOW).**
Every number in Entry 56 that I recounted from the capture files is correct, the user's decision rule for "Test race" is met literally, and the entry already discloses the two main weaknesses (one event; B differs from A in two things). The conclusion is a decision by exclusion, not a direct observation of the race. Findings 1 to 4 say what the entry still claims beyond the files.

Paths: `EVJ` = `.wt/evj/docs/archive/switch-gauntlet/run5-hw/1284/`, `EVP` = `.wt/evp/docs/archive/switch-gauntlet/run5-hw/1284/`, both under `/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/Decide-1284/`.

## Step A: invariant derived from ae1399ba (before reading the witness)

Sources: `kernel/sched/sched.c` (schedule 2574-2976, scheduler_start 2988-3102, scheduler_tick 3107-3190, coop_preempt_maybe_tick 2316-2372, slm_preempt_check_and_yield 2397-2434), `vectors.S` 555-651, `task.c` 113-160, `preempt.c` 168-233, ADR 0008.

1. **At the instant `switch_to()` returns into the incoming task in `schedule()`: `preempt_disabled[X]` must be 1**, X being the CPU the task now runs on.
   - X is the CPU that switched it in. That CPU wrote `preempt_disabled[this_cpu] = 1` (sched.c:2872) with the tick masked since `sched_irq_save()` at entry, so `this_cpu` was X. The other setters are `scheduler_start` (3050) for the first switch and the trampoline (vectors.S:596-599) before its `bl schedule`.
   - Nothing may clear it in between. `rq_unlock_irqrestore` opens IRQs in the window, but `scheduler_tick` returns at 3168 and `maybe_arm_resched_trampoline_on` at preempt.c:204 while the flag is 1, so no nested `schedule()` runs and the task cannot move. `switch_to` restores the incoming task's DAIF, so IRQs are normally open here too, still with the flag at 1.
2. **Right after the post-switch clear: 0, on the CPU the task is on.** `now_cpu = cpu_id()` (2964) is read while the flag is still 1, so the task is pinned and the clear is local. From the next instruction on the task is preemptible, so "0" holds only for a read made with the tick masked.
3. **Writers of `preempt_disabled[X]`.** By design all are CPU X itself:
   - `schedule()` set (2872, masked) and clear (2966, pinned by the flag);
   - `scheduler_start` set (3050, boot context);
   - `task_entry_trampoline` clear (task.c:139/156, first run, flag still 1);
   - `resched_trampoline` set (IRQs masked) and clear (after `msr daifset, #2`, CPU recomputed);
   - `coop_preempt_maybe_tick` save/set/restore (COOP builds only, masked);
   - outside the scheduler: `shell_sys.c:11574/11909` and `test_scheduler.c:5839/5845` (a different test, not in a filtered run).
   - No path at ae1399ba writes another CPU's slot. A remote write therefore needs a stale CPU id: the #1229 class (clear) or the #1242 class (set). Under `PI5_FIQ_TIMER` (default OFF) the trampoline masks only I around its clear, so an FIQ tick could make that clear remote; this can only produce a wrong 0.
4. **Yes: a task in its own context with IRQs open can read `preempt_disabled[c]` non-zero with no kernel defect, when `c` is stale.**
   - The task reads `c = cpu_id()` = A. A tick on A arms the trampoline, which sets flag[A] = 1 and calls `schedule()`; the task is requeued READY on A.
   - A thief, `sched_migrate_task` or the rebalance tick moves it to B. It resumes on B, returns through the trampoline to the interrupted instruction and loads `preempt_disabled[A]`.
   - That slot is 1 whenever A is inside a switch window or its own trampoline at that moment.
   - With no migration between the two reads the task cannot see 1: every setter on its own CPU either runs with the tick masked or is the task's own trampoline or `schedule()` call, which clears before returning to task code.
   - So a non-zero read means one of: stale id (the test's race), a wrong-CPU clear, a wrong-CPU set, or an out-of-scheduler writer.

## Findings

### MEDIUM

**1. The decision is by exclusion; no file shows the race happening.**
- Evidence: `EVJ/A-uefi-c93f0891-x1000-boot02.txt:3805-3810` is the only `bad != 0` iteration in 9000 arm-A iterations. Nothing in the capture shows the worker changed CPU between its two reads.
- The entry's title "decided: the test's own race" and "needs nothing outside the documented design" rest on witness silence plus the code argument.
- No rate argument is given. Arm A on Jetson made 74,106 worker reads (my sum of `runs=`) for 1 event. The race needs a tick inside a window of a few tens of instructions out of a 10 ms period, then a migration, then the old CPU inside a window. My rough estimate is of order 1 expected event or fewer for this sample: not excluded, not shown.
- Fix: one more arm-A variant that, on `pd != 0`, records `cpu_id()` re-read after the flag load and the task's `switches` delta across the two reads. A stale id proves the race; a `bad` with the same CPU and no switch is a kernel defect the witness missed. Until then, word it as "decision rule met; the race itself not observed".

**2. Witness silent cases are only partly disclosed.** (`git show 47c0cf28`, `pd_witness.h`, sched.c call sites.) The witness checks my invariants (1) and (2) at the right points; I found no race of its own:
- W1 and W2 read the id and the flag under `sched_irq_save`.
- A tick between the clear and W2 cannot heal a wrong-CPU clear, because the stuck flag blocks preemption.
- If the flag were wrongly 0 before W1 and the task were preempted first, W1 still reads 0 on whichever CPU it lands.
- `pdw_checks[cpu]++` after `irq_restore` is safe only while the flag is 1, which is the non-violating case.

It stays silent in these cases, each of which could produce the observation:
- (a) A wrong-CPU **set** (the #1242 class, CPU Y sets flag[X]) is never checked on X, where the worker reads it. It is seen only indirectly, as a W1 "0" on Y's incoming task, and not at all if that incoming task is on its first run (`task_entry_trampoline` has no check). The stuck 1 on X is then cleared by X's next switch-in without a trace.
- (b) Any write after `schedule()` returns: the trampoline's set and clear, `shell_sys.c:11574`. The entry's "a path other than the resume path" covers this but does not name the trampoline, which is the one path that sets the worker's own CPU's flag in task context.
- (c) The early return of `schedule()` (disclosed).
- Fix: name (a) and the trampoline in section 4.

**3. Mutation coverage is one defect class.** Both mutation files revert the #1229 clear:
- `EVJ/Q-qemu-mutation-1229-reverted-witness-ON-x20.txt:1806`: `checks=705 violations resume=13 exit=111`.
- `EVJ/Q-qemu-mutation-1229-reverted-fixed-test-witness-ON-x20.txt:1806`: `checks=1106 violations resume=12 exit=120`.
- These validate W2. W1 fires only as a side effect. No mutation exercises a wrong-CPU set (for example the #1242 pin removed), the other route to "1 where 0 is expected".
- "It fires on the #1229 mutation" is true; "it checks the resume path directly" claims more than was tested.
- Fix: add a #1242-style mutation row, or say W1 has no dedicated mutation.

**4. The "`.text` unchanged when off" claim is not shown; only the size is.**
- `EVJ/Q-text-size-witness-off.txt`: size 0x22c658 for both, but "cmp -l count of differing bytes: 43452 ... not analysed further".
- The two ELFs were built in different worktree paths (`.wt/ft/...` and `.wt/b/...`), so the comparison cannot show byte identity.
- The entry's cause "(line numbers, layout)" is asserted, not measured. Path-length shifts in `.rodata` references are at least as likely.
- Commit 47c0cf28's message, `pd_witness.h:4-5` and the CMake comment say ".text is unchanged". At source level the OFF macros are empty and that is very likely true, but the evidence file contradicts the literal claim. It is QEMU ARM64 only; no Jetson or Pi 5 OFF comparison exists.
- Fix: build both in one path with `__LINE__` neutralised and compare objects, or reword to "same `.text` size; no code under the option when off".

**5. The witness-ON `make test` row is not a 47c0cf28 build.**
- `EVJ/Q-qemu-arm-make-test-witness-ON-47c0cf28.txt:82`: banner `(ae1399ba-dirty)`. The file's own header says so ("built from the working tree before the commit, identical content").
- The entry row says "(47c0cf28 tree)" without the banner. The same applies to the `kt-pdw-on` ELF in the text-size file.
- "Identical content" cannot be checked from the files.
- Fix: state the banner in the row, or rerun at the commit.

**6. Scope of the decision sentence.**
- Kexec: A 0 bad, B 0 bad. By the user's rule those two rows alone are "Undecided".
- Pi 5: one A boot, 0 bad, no B boot. Also undecided.
- The entry folds them in ("1 of 8000", "8000 with kexec"), which reads as wider support than exists.
- The fixed test as it would merge (cd1557fe, no witness) has no hardware row. B is cd1557fe plus the witness.
- The witness build reproduced at 1 in 8000 against Entry 50's 1 in 1400. With single events that is neither confirmation nor contradiction, but it is not stated.
- Fix: say the decision is from the UEFI-direct rows on jetson-nano-2 only, and list what has no row.

### LOW

**7.** "The two `bad` events were on CPU 5 and CPU 4." `first on cpu 4` is the id `c` the worker read. Under the race reading that is the CPU it had left, not the one it was on. Reword.

**8.** Header "written 10:30 UTC" conflicts with the body: `/ci` ran to 10:36:48 (`EVJ/ci-cd1557fe/summary.txt`), pi-5-1 was released at 10:49, and the fixed-test mutation banner is build 20261116103814.

**9.** The `/ci` test steps archive make output only. `step04/05/06` end in the Makefile's `PASSED - All tests passed`; no serial log, so the files do not show that the changed test ran. "ARM64, x86-64 and Jetson real-weights builds (main and test)": only step 03 (Jetson) builds a test kernel. The warnings claim checks out: outside `kernel/lib/lua/` only `lua_slm.c` `-Wcomment` and two `ld` warnings.

**10.** "Every capture is from power-on": true. Each file also carries an 8 to 18 line `#` header added by `script-save.sh.txt`, so the line numbers quoted in the entry (for example 3805) are evidence-file lines, not labctl run-file lines. Worth one clause.

**11.** Stage lines in the trigger logs carry the board's clock ("08:51:00 staged" at host 08:56:32, earlier than the A image's build time 08:52:32Z in `images.txt`). The hash check against the host image makes this harmless. A note would stop a reader suspecting a stale stage.

**12.** #1285 row is correct: 6 failures, all `test_scheduler.c:2392 - pdm_moved > 0`, iterations 17, 18, 19, 26, 30, 33 (`EVJ/C-uefi-1285-34c43dc0-x1000-boot01.txt:3001-3039`), each about 1.02 s with `moved=0`. The row uses B's two-worker test, not #1285's own test (disclosed). It says nothing about #1285's own test; one more word would make that plain.

## Step C: the fixed test (cd1557fe)

- Test file only. `cpu_id()` and `preempt_disabled[c]` are read as a pair under `pdm_irq_save()`.
- The assertion is unchanged: `TEST_ASSERT_EQUAL_UINT32(0, pdm_bad)`, plus `w >= 0` and `pdm_moved > 0`.
- It still tests the property: a task that resumed from `schedule()` reads its own CPU's flag as 0.
- Masking removes the race of A(4) on every build: `irq_save` masks the IRQ tick (QEMU, Jetson, Pi 5 default; `cli` on x86), and I+F under `PI5_FIQ_TIMER`. With the tick masked there is no trampoline, so no migration between the two loads.
- Masking cannot hide the defect the test was written for. A wrong-CPU clear leaves the worker's own flag stuck at 1 until that CPU's next switch-in, and a masked read sees it: 20 of 20 failures, `bad` 4 to 8 in every iteration of the fixed-test mutation file.
- What the masked read no longer reports is exactly the stale-id read, which is not a property of the kernel.
- I found no real defect the mask would hide.

## Recount (from the capture files, not `summary-counts.txt`)

Every capture: 1000 `pdm:` lines, one SLM-OS banner matching the arm's commit, the stated `Boot source:`, last line `slmos>`, and 0 `[PDW] violation` lines. All Jetson files have an MB1 line in the header window; the Pi 5 file starts at the kernel banner block.

| File | bad != 0 | [FILTER] | [PDW] checks | Boot source |
|---|---|---|---|---|
| A-uefi boot01 | 0 | 1000/0 | 18354 | UEFI-direct |
| A-uefi boot02 | 1 (line 3805, bad=1, ITER 282) | 999/1 | 18742 | UEFI-direct |
| A-uefi boot03 | 0 | 1000/0 | 20341 | UEFI-direct |
| A-uefi boot04 | 0 | 1000/0 | 19908 | UEFI-direct |
| A-uefi boot05 | 0 | 1000/0 | 18480 | UEFI-direct |
| A-uefi boot06 | 0 | 1000/0 | 18799 | UEFI-direct |
| A-uefi boot07 | 0 | 1000/0 | 18902 | UEFI-direct |
| B-uefi boot01..07 | 0 in each | 1000/0 in each | 18271, 18458, 18729, 18278, 18695, 18062, 18231 | UEFI-direct |
| A-kexec boot01 | 0 | 1000/0 | 18007 | direct load |
| B-kexec boot01 | 0 | 1000/0 | 18379 | direct load |
| C-uefi-1285 boot01 | 0 | 994/6 | 19938 | UEFI-direct |
| A-pi5 boot01 (EVP) | 0 | 1000/0 | 21747 | direct load |

- Banners: A `20261116085116 (c93f0891)`, B `20261116085241 (775563b0)`, C `20261116100543 (34c43dc0)`, Pi 5 `20261116100700 (c93f0891)`; all match `images.txt`.
- Sums: A UEFI 133526, B UEFI 128724, A total 151533, B total 147103, all 18 captures 340321. All as in the entry.
- `moved` ranges and the CPU 0 statement (histogram column 0 sums to 0 in all nine A captures; no `cpu0` PDW line in any hardware file) are correct.
- Order and completeness: 14 UEFI triggers, 08:56:33 to 09:47:06, strictly A, B, A, B, ...; staged hashes `8310ea0c20eb` (A) and `160a2ec0da4a` (B) match `images.txt`. Kexec A 09:56:27 reply `cfc0033c9a4650cc`, B 10:00:42 reply `ee71665b238495bd`, matching the ELF hashes. C 10:09:50 `d6a682e5d762`. No capture missing or duplicated; all PDW totals are distinct.
- Arm identity: `git diff c93f0891 775563b0` touches `kernel/tests/test_scheduler.c` only (masked read, and the absent histogram and print); kernel and witness are identical in A and B.
- Not checked: `patches/` contents against the commits.
