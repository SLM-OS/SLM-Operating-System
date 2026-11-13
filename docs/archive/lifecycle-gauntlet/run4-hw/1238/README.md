# #1238 arm A / arm B on jetson-nano-2 (kexec, test kernel), 2026-11-12

Question: did #1237 introduce the CPU 4 idle-task fault in
`test_preempt_flag_follows_migrated_task` (#1238), or does the test expose a
pre-existing bug (the #1234 window)? Log: `gauntlet/lifecycle-4` Entry 43.

Each `1238-bootNN-<arm>-<head>.txt` is one labctl `boot_test` output_dir file,
copied byte for byte (cmp-checked): the serial log from power-on (MB1) through
UEFI, SD Linux, the kexec handoff and the SLM-OS banner, into the integration
suite. Use `grep -a`. Boot numbers are the global trigger order (A and B
alternate: odd = A, even = B; `triggers.log`).

## Arms

| Arm | Head | What | Jetson test elf sha256 | Banner |
|---|---|---|---|---|
| A (with #1237) | 1b85b315 (`c3/jetson-suite` = main 5b28d065 + #1239's 5 commits) | as run 3 | 61a42e9de6f03ae2 | `build 20261112222231 (1b85b315)` |
| B (without #1237) | fcf91277 (`c1/1238-arm-b` = 6c1f4c9c + #1239's 5 commits + the test copied from 5b28d065) | test-only port, no #1237 kernel code | 8dfaf53896510792 | `build 20261112222231 (fcf91277)` |

Both built with `make kernel-test PLATFORM=JETSON_ORIN_NANO` after
`make kernel-test-clean`, operator library embedded (the same SASS files and
MANIFEST.json as run 3's l4-c3 build). Run 3's elf 95acdf74 was not kept (l4-c3's
build dir was rebuilt for QEMU by /ci), so arm A was rebuilt from the same commit.
Staged as `/root/slmos-l5-1238-{A-1b85b315,B-fcf91277}.elf` (sha checked), removed
at the end.

On arm B the test's own assertion fails by design (`bad>0`): without #1229's fix
the CPU a migrated task resumes on keeps `preempt_disabled` = 1. QEMU `make test`
on fcf91277: 1 failure, exactly this test (`bad=6`).

## Method

No test filter or repeat option exists in the test harness, so this is the
weaker full-suite design, with two changes: (1) `boot_test` stops each capture
shortly after the test of interest (expect `\] test_multiple_cores_isolated` for
boots 01-14, `\] test_priority_ordering_multiple_levels` for boots 15-20, timeout
600 s for boots 01-02 and 480 s after that) instead of running the suite to its
summary, so about 4 minutes per boot instead of 6.5; (2) one host-side trigger loop
(`trigger-ab.sh.txt`, Entry 39's loop with one elf per trigger, nothing installed on
the board) alternated A and B boot by boot. The pattern changed after boot 10:
its capture stopped while a third interleaved fault dump was still printing.

## Results

| Boot | Arm | Reached the test | pdm line | Fault in the test |
|---|---|---|---|---|
| 01 | A | yes | migrations=0 moved=7 bad=0 | - |
| 02 | B | yes | migrations=6248 moved=7 bad=7 | - |
| 03 | A | yes | migrations=44598 moved=7 bad=0 | - |
| 04 | B | yes | migrations=0 moved=7 bad=7 | - |
| 05 | A | yes | migrations=0 moved=7 bad=0 | - |
| 06 | B | no (#1207 PMM halt) | - | excluded |
| 07 | A | yes | migrations=23954 moved=7 bad=0 | - |
| 08 | B | yes | migrations=10290 moved=6 bad=6 | - |
| 09 | A | yes | (pdm_done TIMEOUT) | **fault** |
| 10 | B | yes | migrations=8613 moved=7 bad=7 | **fault** (capture ends inside a third dump) |
| 11 | A | no (#1207 PMM halt) | - | excluded |
| 12 | B | yes | migrations=10128 moved=6 bad=5 | **fault** |
| 13 | A | yes | migrations=8797 moved=6 runs=6/0 bad=0, pdm_done TIMEOUT | **fault** |
| 14 | B | yes | migrations=0 moved=6 bad=6 | - |
| 15 | A | no (#1207 PMM halt) | - | excluded |
| 16 | B | yes | migrations=6264 moved=1 bad=1, pdm_done TIMEOUT | **fault** |
| 17 | A | yes | migrations=24550 moved=7 bad=0 | - |
| 18 | B | yes | (interleaved) | **fault** |
| 19 | A | yes | (interleaved) | **fault** (suite stalls; capture to the 480 s timeout) |
| 20 | B | yes | (interleaved) | **fault** (suite stalls; capture to the 480 s timeout) |

**Arm A: 3 of 8** boots that reached the test faulted in it (09, 13, 19). **Arm B: 5 of 9** (10, 12, 16, 18, 20). Run 3 (arm A's head, elf 95acdf74): 2 of 10.

## Faults (all during `test_preempt_flag_follows_migrated_task`, right after `test_migrate_ready_task_updates_assigned_cpu` passed; the 10 lines before each are the scheduler-suite PASS lines shown in the files)

Symbols from each arm's own elf (`aarch64-none-elf-nm`/`addr2line`). In arm A `scheduler_tick` is at 0x8002c564; in arm B at 0x8002c420; `rq_lock` is at 0x80c26c80 in both; `sched` at 0x80c26a00.

- **09 (A)**: EL1 sync, EC 0x0e illegal execution state, ESR 0x3a000000, FAR 0x0, **ELR 0x80c26c84 = `rq_lock`+4** (.bss), SPSR 0x100009, task pdm_a (id 130), **CPU 2**, SP 0x10ac7fa80. Stack: 0x80013230 (`el1_sync_handler` return), 0x80c26c90/0x80c26c94 (`rq_lock`+0x10/+0x14), 0x8002c60c (**`scheduler_tick`+0xa8**). System halted; pdm_done TIMEOUT.
- **10 (B)**: Instruction Abort at **PC 0x0**, ESR 0x86000006, SPSR 0x600003c9, x30 0x0, task **idle_2 (id 2) on CPU 0 and again on CPU 5** (two dumps, one task); a third, interleaved dump is cut by the capture end.
- **12 (B)**: Instruction Abort at **PC 0x0**, ESR 0x86000006, task **idle_1 (id 3) on CPU 1 and on CPU 4**, x30 0x0; then Data Abort (read) at 0x14, ESR 0x96000006, ELR 0xbeba (not code), **SPSR 0x80c26a80 = `sched`+0x80** (an address, not a PSTATE), task idle_2, **CPU 5**. Two "System halted". Later tests time out (repos_spinner, daif_probe).
- **13 (A)**: EL1 sync, ESR 0x3a000000, **ELR 0x80c26c84 = `rq_lock`+4** again, SPSR 0x100009, task pdm_b (id 131), **CPU 3**; interleaved with an Instruction Abort at **PC 0x0** in idle_1 on **CPU 4**.
- **16 (B)**: PC alignment fault, ESR 0x8a000000, FAR = ELR = 0x1, SPSR 0x80d00089, task pdm_a (id 130), **CPU 2**, SP 0x10b87fa80. Stack the same shape as boot 09: 0x80013230 (`el1_sync_handler`), `rq_lock`+0x10/+0x14, 0x8002c4c8 (**`scheduler_tick`+0xa8**, the same call site as boot 09's). idle_4's lowest 64 stack bytes read 0 (canary BROKEN). Run 3's boot 02 was this exception class (ESR 0x8a000000, ELR 0xa00000003).
- **18 (B)**: PC alignment fault, ESR 0x8a000000, FAR 0x13a47fc90 (a stack address), ELR 0x80049f20 (`timer_handler`), SPSR 0x13a47ffc0 (a stack address), idle task (values read through the interleaving); interleaved with a Data Abort (read). Dump garbled by interleaving.
- **19 (A)**: PC alignment fault on an idle task, interleaved with a second fault; garbled. Suite stalls in the litmus rows (capture to the timeout).
- **20 (B)**: PC alignment fault, ESR 0x8a000000, FAR 0x13b9ffc90, **ELR 0x80049f20 (`timer_handler`)**, **SPSR 0x13b9fffc0** (both stack addresses), task idle_2 (id 2). Suite stalls in the litmus rows (capture to the timeout).

CPUs: the faults are on CPUs 0, 1, 2, 3, 4 and 5, not only cluster 1. Common shape on both arms: a task resumes from a saved context that is not its own (PC 0 or a data address, SPSR holding an address, the same task on two CPUs), entered from the tick path.

## #1207 PMM halts (did not reach the test; excluded)

Boots 06 (B), 11 (A), 15 (A): data abort (write) at 0x1000008096ad97, ESR
0x96000044, ELR `pmm_alloc_pages`, x30 `test_exhaustion_recovery`, task main
(id 7), CPU 0, after `test_checkerboard_fragmentation`. This is run 3's boot 08
signature, and boot 06 shows it on a tree without #1237.
