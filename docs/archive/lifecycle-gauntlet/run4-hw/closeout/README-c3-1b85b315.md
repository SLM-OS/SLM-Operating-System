# Lifecycle run 4 close-out: c3/jetson-suite 1b85b315 (rebased onto main 5b28d065) integration suite on jetson-nano-2, 2026-11-12

Each `c3-1b85b315-jetson-{kexec,uefi}-suite-bootNN.txt` is one labctl `boot_test` output_dir file, copied byte for byte (cmp-checked): the full serial log from power-on (MB1) to the suite summary match, or to the boot_test timeout for a boot that stopped. Use `grep -a`.

- Image: `make kernel-test PLATFORM=JETSON_ORIN_NANO` at 1b85b315 in l4-c3 (operator library embedded from scripts/cuda/operator_library/MANIFEST.json). elf sha256 95acdf74a930e480 (staged /root/slmos-l4co-test-1b85b315.elf, `jetson-stage-c3-1b85b315-test.txt`), bin 41081a40be9497aa (ESP, `jetson-esp-write-c3-1b85b315-test.txt`). Banner in every file: `SLM-OS v0.4.0 build 20261112192508 (1b85b315)`.
- Method: as `README-1237-jetson.md` (boot_test + host trigger loop; trigger logs `c3-*-triggers-*.log`). Expect `All test suites passed|total failure\(s\)`; timeout 720 s (kexec boots 01-03, UEFI), 600 s (kexec boots 04-10).

## Kexec, 10 boots: 7 reach the last test

| Boot | Reached last test | Summary | Failing checks | Stop |
|---|---|---|---|---|
| 01 | yes (`test_work_stealing_distributes_load`) | 14 total failure(s) | 15, all #1109 | - |
| 02 | **no** | - | 10 so far, all #1109 | **#1238**: EL1 PC alignment fault, ESR 0x8a000000, FAR=ELR 0xa00000003, idle_3 (id 2), CPU 4, in test_preempt_flag_follows_migrated_task; System halted |
| 03 | **no** | - | 12 #1109 + test_regs_every_cpu_reports (5 of 6 CPUs, consequence of the CPU 4 fault) | **#1238**: data abort at 0x3, ESR 0x96000006, ELR 0x8002c5a8 (scheduler_tick), idle_3 (id 5), CPU 4, same test; suite continued on 5 CPUs, timed out at 720 s |
| 04-07 | yes | 14 total failure(s) | 15, all #1109 | - |
| 08 | **no** | - | 9 so far, all #1109 | **#1207** PMM halt: data abort (write) at 0x1000008096add7, ESR 0x96000044, ELR 0x80024b6c (pmm_alloc_pages), x30 in test_exhaustion_recovery, task main (id 7), CPU 0, after test_checkerboard_fragmentation |
| 09-10 | yes | 14 total failure(s) | 15, all #1109 | - |

The 7 complete boots have an identical failing set (sorted `  [FAIL]` lines md5 3f8d2495), #1109's 15 names. test_preempt_flag_follows_migrated_task passed (`bad=0`) in the 8 boots without #1238.

## UEFI-direct, 3 boots: 3 reach the last test

| Boot | Reached last test | Summary | Failing checks |
|---|---|---|---|
| 01-03 | yes (`test_work_stealing_distributes_load`), 351.7-356.2 s | 21 total failure(s) | 22 = 13 of #1109 + 9 of #1231 (identical in all 3, md5 3ab58e46; the same accounting as run 4's 8a3406ea UEFI rows) |

## Board

ESP as found 789cba62 (`jetson-asfound.txt`); written with 33f05c5a production (276ffa90), then 1b85b315 test (41081a40); restored to 789cba62 at the end, fsck clean, 2196 clusters, no BootNext, BootOrder unchanged, staged elfs removed, `/root/slmos.elf` untouched (`jetson-restore.txt`). Released powered on at SD Linux.
