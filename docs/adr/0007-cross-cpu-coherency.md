# ADR 0007: cross-CPU state is cacheable on Pi 5 and Jetson

## Context

Since April 2026 the kernel kept run queues, the task table, current-task
pointers and boot flags in a Normal Non-cacheable block on Pi 5 and Jetson
(`PLATFORM_HAS_NC_MEMORY`), cleaned and invalidated shared fields by hand,
wrapped Pi 5 spinlocks in `DC CIVAC`, and kept secondaries from printing.
The stated reason was that firmware leaves `CPUECTLR_EL1.SMPEN` clear, so
private L2s are incoherent. The record held four incompatible accounts, none
re-tested after the set/way invalidate in `smp_boot.S` (`e0a15259`).

## Decision

Status: proposed. The coherency answer is measured; the removal is not yet
decided for Jetson.

Measured: on both boards the hardware keeps Inner Shareable Write-Back
memory coherent across all CPUs, including between the two Jetson clusters;
the NC axis is not required by the hardware, and the SMPEN account is wrong.

Pi 5: decision (a). CPU-shared kernel state moves to ordinary cacheable
memory with no CPU-to-CPU cache maintenance; ordering uses barriers, acquire
and release. Maintenance stays only where a party does not use the cache:
DMA, page tables written before the MMU is on, and `secondary_mmu_*`, which
a secondary reads with its MMU off. The NC block stays, for DMA only.

Jetson: no decision. The removal build has not met the bar (4 kexec and 2
UEFI boots with captured logs against 10, no integration run), and a
CPU-dependent work-stealing stall (one CPU making 0 successful steals in a
run) appears in both cacheable builds, on CPUs 2-3 under `NC_AXIS=OFF` and
CPU 4 under the removal, and is not shown absent from the NC build. Next
experiment: at least 10 `bench stealing` runs with per-CPU steal deltas per
build (NC and removal) per boot path. A stall in both builds is a separate
stealing defect and the removal can proceed; a stall only without NC is a
removal regression. Because the code is shared, the removal does not land
until Jetson is decided.

## Alternatives considered

- Keep the NC axis on Jetson only (b): no Jetson measurement shows
  incoherence, on either boot path, including all 16 cross-cluster pairs.
- Keep it everywhere with a measured justification (c): no measurement on
  either board, under SLM-OS or Linux, shows incoherence.
- Patch BL31 to set an enable bit: neither core has one.
- Keep `NC_AXIS=OFF` as an option: rejected; it is slower than both the NC
  build and the removal on Jetson stealing (below), and a second path is a
  second thing to test.

## Consequences

Neither core has SMPEN: A76 CPUECTLR bit 6 is RES0 (TRM 100798 13.32),
A78AE bit 6 is PF_MODE[0] (101779 3.2.47); both join coherency at power-up
with no software step (A76 5.9, A78AE 2.4.9, DSU 100453 2.1). Caches-off
loads do not allocate (A76 7.2.3), so the April "pre-MMU reads left stale
lines" account cannot hold. The defect actually found was that
`secondary_mmu_*` were never cleaned before a caches-off read (fixed).
`make test` now rejects new cache maintenance in scheduler, IPC, spinlock
and print code. Open: #1202 (one extra test failure in 12 Pi 5 removal
boots, cause undetermined), #1197 (BSS cleared with caches off, which the
task table now relies on), #1201 (unbounded UART wait under the now shared
lock), and the Jetson steal stall above.

## What would overturn this

A row-tagged litmus row with a forbidden outcome or timeout on any CPU pair
with identical Inner Shareable Write-Back mappings and MMU and caches on, or
#1202 resolving as lost cross-CPU visibility.

## Evidence

- Linux, same boards and firmware, row-tagged suite, 10 min each: 0
  forbidden, 0 timeouts, 0 weak on every ordered pair; message passing
  sensitive on every pair; store buffering sensitive on 12/12 Pi 5 pairs but
  only 10/30 Jetson pairs. SLM-OS reads the same `CPUECTLR_EL1` and
  `CLUSTERECTLR_EL1` as Linux on every CPU.
- SLM-OS, row-tagged suite, NC, `NC_AXIS=OFF` and removal builds, both
  boards, both Jetson paths: every row 0 / 0 / 0 weak. The suite sees
  staleness on hardware: caches-off readers on every same-cluster pair, a
  Non-shareable alias on exactly the 16 Jetson cross-cluster pairs.
- Removal against NC: context switch Pi 5 2318 to 105 ns, Jetson kexec 5456
  to 84 ns, Jetson UEFI 18008 to 66-102 ns; IPC Pi 5 812 to 130 ns, Jetson
  equal; stealing Pi 5 equal (bimodal by task split), Jetson kexec median
  10154 against 10625-11234 us, Jetson UEFI 6976-9802 against 7933-11007 us
  (`NC_AXIS=OFF` alone was slower; its CPUs 2-3 never stole).
- Reliability: Pi 5 removal 10/10 boots plus 11 test-kernel boots
  (integration 16/16 each, the 3 NC-only tests compiled out). Jetson
  removal: 7 kexec boots (4 with logs), UEFI boots below 10, no
  integration; `NC_AXIS=OFF` 10/10 both paths with integration 16/16.
- Full data: `docs/archive/coherency-gauntlet/coherency.md`.
