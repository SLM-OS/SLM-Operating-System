# Coherency gauntlet log (#1193)

Goal: decide by measurement whether the non-cacheable memory axis
(`PLATFORM_HAS_NC_MEMORY`, `kernel/include/ncmem.h`) is required by the
hardware on Pi 5 and Jetson, or is a workaround for a boot-sequencing defect
that has since been fixed. Reference bar: Linux on the same board under the
same firmware, plus the Cortex-A76, Cortex-A78AE and DSU TRMs and TF-A.
Boards: jetson-nano-2 and pi-5-1 only.

## Budget

- Started: 2026-11-08 22:15 UTC. Limit: 12 h (2026-11-09 10:15 UTC). At most
  5 critic rounds per component.
- Stop and report if the same failure recurs twice without a new strategy,
  two critic rounds in a row show no measurable gain, progress needs Jetson
  firmware flashing, C0 and C2 disagree with no source to settle it, or a
  board becomes unreachable.

## State found at start (main @ c69438a2)

- Tracking issue #1193 opened 22:15 UTC.
- jetson-nano-2: on, JetPack Linux 5.15.148-tegra (Ubuntu 22.04.5), up 8.6 h,
  6 CPUs online. `recovery_mode` binding released (last_state open).
- pi-5-1: on, running SLM-OS. Card is the SLM-OS single-partition card
  (one FAT32 partition, label SLMOS); no Raspberry Pi OS on it. `config.txt`
  has no `armstub=`, so the board runs the stock BL31 embedded in the EEPROM
  firmware for every OS booted from this card.
- Both boards claimed by `coherency-gauntlet-1193` until 02:16 UTC.
- `kernel/sched/sched.c`: 84 conditional lines. `PLATFORM_HAS_NC_MEMORY`
  appears in 22 files under `kernel/` and `runtime/`.
- The record holds a fourth explanation besides H1 to H3: the `ncmem.h`
  header comment says the L2s are incoherent "despite SMPEN being set".

## Components

| Id | Component | Status | Critic rounds |
|---|---|---|---|
| C0 | Architecture and firmware facts | PASS (round 3), merged | 3 |
| C1 | Linux reference bar | round 1: measured claim PASS, wording FAIL; round 2 code (S2, P4 coverage) then reruns | 1 |
| C2 | `coherency` verb and litmus suite | round 1 FAIL (false-pass flaw); rev 3 suite being built | 1 |
| C3 | Boot-sequence audit and cacheable build | NC_AXIS=OFF @ c2122932 page-faults at boot (extern mismatch); fix + rev 3 suite in progress | 0 |
| C4 | Decision, ADR 0006, draft PR | waiting | 0 |

### Record check (git history, times -0700)

- `aedf6e1a` 04-05 01:00 SMPEN investigation (H2). `e0a15259` 04-05 14:50
  set/way invalidate after MMU enable on secondaries (H3). `0c229442` 04-05
  21:03 NC memory on Pi 5: its message also records that spinlocks on NC
  memory hang, so `rq_lock[]` stayed cacheable. `f683c770` 22:41 task table
  to NC. `a6ab2c2b` and `0c79eaf4` 04-06 06:12 and 07:40 NC on Jetson.
- Correction to the brief's order: `a5e91676` (14:15), `e70a73b9` (16:32)
  and `584ace8f` (17:11) land before `2eab51ae` (19:17, "switch_to fails on
  secondary CPUs regardless" with a cacheable task table). The cacheable-table
  test therefore ran after the scheduler fixes, and still failed then; the
  failure it saw was attributed to `switch_to`, not to memory attributes.
- Spinlock `DC CIVAC` path: first appears with `53515d00` (04-16, #215).

## Critic verdicts

- C0 round 1: FAIL, narrowly. Independent answers to all seven questions
  agree with the builder; about 60 citations opened, all matched, no claim
  wrong. Failed on its own labeling rule: three inferences stated as fact
  (Pi 5 EEPROM BL31 contents, Tegra cross-cluster fabric, the caches-off
  control returning a deterministic stale value). Missing: HCR_EL2.TIDCP,
  DSU 5.9 SoC interconnect caveat, EL3 code before BL31, CPU 0 not reset
  across kexec, the full booting.rst sentence. Overturning observation named
  by the critic: a Jetson message-passing litmus between CPUs 0-3 and 4-5
  that reads stale data with identical IS WB mappings, MMU and caches on.
- C0 round 2: FAIL, narrowly. Eight of nine gaps fixed; all new citations
  matched. One new wrong statement: "on Pi 5 a core is reset only at cold
  boot" (upstream CPU_OFF requests a warm reset through RMR_EL3,
  rpi3_pm.c:184-208). RES0 glossary citation partial; two small unsourced
  statements.
- C0 round 3: PASS. Open nit, not blocking: Q3 says the cited sources do not
  state whether a warm reset invalidates the caches, while (b) step 2 says
  they were invalidated at the last reset; A76 TRM 7.2 does not distinguish
  warm from cold reset.

- C1 round 1: PASS on the measured claim, FAIL as worded. Critic rebuilt
  the binary (hash matched), parsed every ROW: 0 forbidden, 0 timeouts,
  coverage contiguous (Jetson 40 passes x 30 pairs incl. 16 cross-cluster;
  Pi 5 390 passes x 12 pairs), counts add up. Gaps: no SB+none control, so
  P3's zero has no shown sensitivity; P4 got 0.07 to 0.2 percent of runtime
  with no straddle count; P3, P4 and P6 checkers not fault-injected; card
  config not quoted (the repo template `deploy/pi5/config.txt:46` uses
  `armstub=armstub8-2712.bin`, the pi-5-1 card does not); Jetson BL31
  version unrecorded. Spec revision 2 (5e82fdb8) adds S2 and P4 coverage
  counters; the same applies to C2's kernel suite.

- C2 round 1: FAIL. Coherence clause supported (every P row parsed: 0
  forbidden, 0 timeouts; S1 nonzero on every pair; registers match Linux).
  Detection clause not supported: MP and CoRR readers return on `f >= n`
  without checking the value belongs to the row, so a stale reader holding a
  previous row's final value passes at once. Hardware shows it: Jetson UEFI
  control (b) reported 13 of 30 P2 rows and 5 of 6 P4 rows as 0/0 at 0 ms
  through a Non-cacheable alias. Control slots were not initialised, so
  controls (a)/(b) depended on old DRAM; the run-1 "contamination"
  explanation was never isolated. Control (c) missed on some cross-cluster
  pairs, unexplained. The Linux harness shares the reader flaw. Spec
  revision 3 (021f3041): row nonces in every value, an `observed` progress
  rule, sentinel + clean to PoC before controls (a)/(b).

## Failed approaches

- Pi 5 Linux attempts 1 to 3 (console-only result path; then a 20 s settle
  without an SDWire round trip). Cause: rig defect #1195 plus possibly
  earlycon on RP1. Replaced by attempt 4 (round trip + HTTP results).
- NC_AXIS=OFF at c2122932 on Pi 5 (attempt N1): Data Abort on CPU 0 in
  `cpu_supervisor_task_entry` reading 0xaaaf0000aaaa, right after "Enabling
  interrupts", with the NC_AXIS=OFF banner printed and 3/3 secondaries
  online. Cause: `cpu_supervisor.c:57` declares `sched_diag_idle_loops` as a
  pointer while `sched.c:399` defines an array under NC_AXIS=OFF; same in
  `test_integration.c:169, 1066-1071`. A software defect in the switch, not a
  coherency failure; C3's audit missed cpu_supervisor.c. Fix in progress
  with a compile-time check.

## Evidence

### C0 builder result (before critic)

- Builder marks H1 and H3 INCONSISTENT and H2 CONSISTENT on both boards
  (cross-cluster on Jetson unsourced). A76 CPUECTLR bit 6 is RES0, A78AE bit
  6 is PF_MODE[0]; neither core has SMPEN. Filed #1194: `cpu_has_smpen()`
  tests the A72 bit, so "SMPEN NOT set" prints on both boards regardless.
- Safety: CPUECTLR reads from NS-EL2 are safe (SLM-OS already does it every
  boot); any write to it or to other IMP DEF control registers traps to EL3,
  and on Jetson BL31 v2.8 that panics. C1 and C2 told: reads only.
- Negative control: Non-shareable mapping is not guaranteed to show
  staleness (Arm ARM B2.10.1.1). C2 adds a caches-off reader control
  (SCTLR.C=0), which A76 7.2.3 and A78AE 2.6.2.3 say looks up no cache.

### C1 Jetson Linux litmus (builder report, critic pending)

- jetson-nano-2, 5.15.148-tegra, 6 CPUs: 40 passes of the full set, 606 s.
  Every P row forbidden 0, timeouts 0. S1 (no barriers) relaxed outcomes
  82,271,035 of 1.2e9, so the harness observes reordering on this core.
- CPUECTLR module: `msr` count 0 in both .ko files; Jetson cleared to load
  with cluster=0 (DSU-AE CLUSTERECTLR encoding unconfirmed).

### C1 Pi 5 boot, attempt 1

- Raspberry Pi OS kernel 6.18.60+rpt-rpi-2712 plus initramfs from the SLM-OS
  card: 0 serial lines in 1260 s. Cause under test: the lab console is RP1
  UART0 at 0x1F00030000 (`platform.h:591`), while firmware `uart_2ndstage`
  output goes to the BCM2712 debug UART, and the first config did not enable
  RP1 UART0 for Linux. Second attempt adds `dtparam=uart0=on`,
  `pciex4_reset=0` and earlycon on 0x1f00030000. A second silent boot stops
  this approach.

### C2 QEMU stage (builder report, critic after hardware)

- `coherency regs | litmus | neg`, `test_coherency.c` (12 tests), vmm
  `VMM_FLAG_NONSHARE` with 2 PTE tests. QEMU ARM64: 65 litmus rows, 0
  forbidden, 0 timeouts; controls a, b, c all fresh on QEMU (expected, TCG
  models no caches). Adds about 1.5 s to `make test`. x86-64 passes.
- Self-checks: synthetic forbidden observations, frozen flags and skipped
  ping-pong steps are counted, so the checkers can fail.
- CLUSTERECTLR_EL1 is S3_0_C15_C3_4 (DSU TRM 100453 11.8), not the
  S3_0_C15_C3_0 the orchestrator first gave (that is CLUSTERCFR_EL1).

### C3 QEMU stage (builder report, critic after hardware)

- Audit `c3-audit.md`, 12 items. D1 (medium): `secondary_mmu_{ttbr,mair,tcr}`
  are written by CPU 0 with caches on and never cleaned, then read by each
  secondary with its MMU off (`vmm.c:2097-2100`, `smp.c:775-789`,
  `smp_boot.S:161-178`); it works only because A76/A78AE execute DC IVAC as
  DC CIVAC. Fixed with a clean and a test. D2: pre-MMU page tables were
  cleaned, not invalidated; fixed (DC IVAC, as Linux head.S). D7: the
  set/way loop is unnecessary and its comment's reasoning is wrong (caches-off
  loads do not allocate, A76 TRM 7.2.3); kept, `SETWAY_INVAL=OFF` added. D9 is
  #1194, fixed. Filed: #1196 (Jetson CPU 0 may hold dirty WB lines for the NC
  block after kexec/UEFI), #1197 (primary caches-off writes never
  invalidated), #1198 (`mmu_enable` unmasks all of DAIF); D3/D4 on #1128.
- Switches: `NC_AXIS=OFF` (CPU-shared state cacheable; NC block kept for
  DMA as `PLATFORM_HAS_NC_REGION`; Pi 5 spinlock CIVAC path removed, 226
  CIVACs to 0) and `SETWAY_INVAL=OFF`. Default builds object-identical to the
  base except the new test. Several `#else` branches were QEMU-only shortcuts
  and were fixed (`sched.c:609`, `sched.c:2617`, `task.c:155`, USB DMA,
  `bench smp`); `preempt.c` used NC memory with no gate at all.
- `sched.c` conditionals: 84 before, 84 after (the switch keeps both paths;
  removal is C4's PR).
- QEMU: `make test` PASS, x86-64 PASS, check-braces 0, test-build-order
  PASS; new `test_nc_axis.c` walks live page tables to check where run
  queues, task table and `current_task` live.

### Suite revision 2 in the kernel (c2-rev2, 09028b54)

- S2 rows, SENS lines and P4 coverage in C1's format; 19 self-tests,
  including end-to-end fault injection for P3, P4 b<a, weak P4 and P6, each
  ending in `coherency: FAIL`. Integration branch `c3-hw-1193` @ cf7866f2
  (C3 a6c2242a + 09028b54): QEMU `make test` PASS, with the real run and the
  no-fault control passing and the four injected runs failing, as designed.
- On QEMU, S2 is nonzero on 11 of 12 pairs while S1 stays 0, so QEMU cannot
  show P2 sensitivity; only hardware can.

### C2 Jetson progress (builder note, 01:55 UTC)

- kexec path complete: regs 6/6, litmus PASS twice, neg a/b/c, perf x3,
  integration 19/19 on a BOOT_TESTS_HW_SET test kernel (the full Jetson test
  kernel page-faults in `test_lua_client_gets_the_reply`, 3 of 3, #1199),
  boots 10/10. The brief's "20 integration tests" is 19 here.
- First neg run was contaminated (alias rows reused lines, leaving stale
  Non-shareable copies in reader caches); fixed in 62a0217e with fresh lines
  per row. With the fix, control (c) (caches-off reader) shows p1_stale on
  30 of 30 pairs, so the litmus suite can see staleness on this board.
- UEFI-direct: regs, litmus PASS, neg done; perf, boots, integration in
  progress. bench context about 18 us on UEFI vs 5.4 us on kexec.

### c3-hw-1193 @ c2122932

- Merge of C2's 62a0217e (fresh lines per neg row) and 18ad1210
  (BOOT_TESTS_HW_SET) onto suite rev 2; conflicts resolved keeping both.
  QEMU `make test`: coherency 20/20, real run and no-fault control PASS,
  four injected runs FAIL. RASPI5 and JETSON build, default and NC_AXIS=OFF.
- Checked that a cached NC_AXIS=OFF cannot leak into a default build: the
  Makefile only passes `-DNC_AXIS=OFF`, but a signature change wipes the
  build directory (`Makefile:349-358`).

### NC baseline, SLM-OS (C2 builder reports, critic running)

| | Jetson kexec | Jetson UEFI | Pi 5 |
|---|---|---|---|
| Build | 9a13b9ee / 62a0217e | 62a0217e | 9a13b9ee |
| Litmus P rows forbidden / timeouts | 0 / 0 (2 runs) | 0 / 0 | 0 / 0 (2 runs) |
| S1 relaxed (sensitivity) | 1,227,299; 1,178,323 | 1,397,948 | 391,659; 394,730 |
| Control a, Non-shareable alias | same cluster 0; cross 16/16 time out | same cluster 0; cross 7 forbidden, 4 timeouts | 0 on all 12 pairs |
| Control b, NC alias | 30/30 time out (memory read 0) | 15 forbidden, 2 timeouts | P2 forbidden 1 on each of 12 pairs |
| Control c, caches-off reader, phase 1 stale | same 14/14, cross 16/16 and 10/16 | same 14/14, cross 13/16 and 14/16 | 12/12 |
| Integration | 19/19 (hwset kernel) | 19/19 | not captured |
| bench stealing (us) | 10625 / 10627 / 11234 | 9831 / 11007 / 7933 | 13062 / 13266 / 17035 |
| bench context avg (ns) | 5464 / 5457 / 5448 | 18063 / 17927 / 18034 | 2321 / 2318 / 2315 |
| bench ipc avg (ns) | 128 / 112 / 113 | 163 / 86 / 92 | 817 / 810 / 812 |
| bench smp | 5/5 | 5/5 | completes; tasks for CPUs 2, 3 report CPU 1 |
| Boots to shell | 10/10 | 10/10 | 8 of 20 attempts (0 partial, 10 silent, 2 unclassified) |

- Registers: SLM-OS reads the same CPUECTLR_EL1 and CLUSTERECTLR_EL1 as
  Linux on every CPU of both boards (Jetson 0xa000400b40543000 / 0x4501;
  Pi 5 cpu0 0x2000000961563000, cpu1-3 0x0000000961563000 / 0x500). All CPUs
  at EL2 with E2H=1 TGE=1 and identical TCR_EL2 0x12b5193519, MAIR_EL2
  0xff440400 on both boards. Jetson CPU 0 SCTLR_EL2 differs (0x30c01805
  after kexec, 0x30c5183d after UEFI; others 0x30c51835).
- Jetson BL31 from the TCU banner: `v2.8(release)`, built 11:44:32 May 9 2026.
- The Non-shareable control (brief's required negative control) does fail
  on hardware, but only between Jetson clusters; inside a DSU cluster it
  stays coherent, as C0 predicted. The caches-off control shows staleness on
  every same-cluster pair on both boards, and on 10 to 16 of 16 cross-cluster
  pairs on Jetson (unexplained variation).
- Unexplained, reported: Jetson context switch about 3.3x slower after UEFI
  than after kexec; Pi 5 `bench smp` tasks for CPUs 2 and 3 report CPU 1.
- #1199 filed: full Jetson test kernel page-faults in a Lua test.

### Lab notes

- 23:18 UTC: both claims renewed to 05:18 UTC. 01:51 UTC: renewed to 09:51 UTC.
- pi-5-1 boots only after a read-only SDWire round trip while off, then a
  20 s off period (#1195; the first "20 s settle" rule was incomplete).
  Deploy rule: sdwire_update reboot=false, sdwire_info, power_cycle
  delay=20. `boot_test` cannot do the round trip between boots, so Pi 5
  reliability counts use this sequence by hand.
