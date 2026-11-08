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
| C0 | Architecture and firmware facts | building | 0 |
| C1 | Linux reference bar | building | 0 |
| C2 | `coherency` verb and litmus suite | building (QEMU stage) | 0 |
| C3 | Boot-sequence audit and cacheable build | waiting on C0, C1 | 0 |
| C4 | Decision, ADR 0006, draft PR | waiting | 0 |

## Critic verdicts

## Failed approaches

## Evidence

### Lab notes
