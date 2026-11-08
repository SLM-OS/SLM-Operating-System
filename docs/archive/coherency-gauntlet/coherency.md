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
| C1 | Linux reference bar | Jetson PASS (47ab164a, 44809f4b); Pi 5 retry with SDWire settle | 0 |
| C2 | `coherency` verb and litmus suite | QEMU stage done (9a13b9ee); Jetson hardware stage running | 0 |
| C3 | Boot-sequence audit and cacheable build | audit + switches building (QEMU stage) | 0 |
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

## Failed approaches

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

### Lab notes

- 23:18 UTC: both claims renewed to 05:18 UTC.
- pi-5-1 needs about 20 s powered off after an SDWire switch to DUT before
  power-on, or it does not boot at all. Deploy rule for this run:
  sdwire_update reboot=false, then power_cycle delay=20.
