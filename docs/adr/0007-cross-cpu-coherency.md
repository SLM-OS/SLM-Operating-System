# ADR 0007: cross-CPU state should be cacheable on Pi 5 and Jetson

## Context

Since April 2026 run queues, the task table, current-task pointers and boot
flags lived in a Normal Non-cacheable block on Pi 5 and Jetson
(`PLATFORM_HAS_NC_MEMORY`) with hand-written cache maintenance. The reason
given was
that firmware leaves `CPUECTLR_EL1.SMPEN` clear, so private L2s are
incoherent. Nothing re-tested it after the set/way fix in `e0a15259`.

## Decision

Status: proposed; the removal is pending on both boards.

Measured: on both boards Inner Shareable Write-Back memory is coherent
across all CPUs, including between the Jetson clusters, under Linux and
SLM-OS. The NC axis is not required for coherency, and the SMPEN account
is wrong. The target is decision (a): CPU-shared state in ordinary
cacheable memory, no CPU-to-CPU cache maintenance, ordering by barriers,
acquire and release; maintenance only for DMA, page tables written before
the MMU is on, and `secondary_mmu_*` (read with the MMU off); the NC block
for DMA only.

The removal does not land until both boards meet the bar:

- Pi 5: the integration "match" criterion is unmet. The removal test kernel
  failed one extra test in 1 of 11 boots (#1202); the NC test kernel has a
  single boot. Next: 10-boot NC test-kernel series and the #1202 experiment.
- Jetson: the removal has 4 kexec and 2 UEFI boots with captured logs, no
  integration run, and a steal stall the NC build does not show (a CPU with
  0 successful steals): `NC_AXIS=OFF` CPUs 2 and 3 in 6 of 6 runs; removal
  CPU 4 in 2 of 2 UEFI runs with per-CPU deltas, 0 of 5 on kexec; NC 0 of
  6. Next: 10+ `bench stealing` runs with per-CPU deltas per build per path.
  A removal-only stall that stays unexplained would make (b), keeping the
  axis on Jetson, the outcome.

## Alternatives considered

- Keep the axis everywhere with a measured justification (c): no
  measurement on either board shows incoherence.
- Keep it on Jetson only (b): open until the steal experiment.
- Keep `NC_AXIS=OFF` as an option: it shows the stall above.

## Consequences

Neither core has SMPEN (A76 CPUECTLR bit 6 RES0, TRM 100798 13.32; A78AE
bit 6 is PF_MODE[0], 101779 3.2.47); a core joins its cluster's coherency
with no software step (A76 5.9, A78AE 2.4.9, DSU 100453 2.1). Coherency
between the Jetson clusters has no public source and rests on measurement.
Caches-off loads do not allocate (A76 7.2.3), so "pre-MMU reads left stale
lines" cannot hold. Defects fixed: D1, D2, D12 and an extern mismatch
that page-faulted the first cacheable build (C3 audit). `make test` rejects new cache maintenance in scheduler,
IPC, spinlock and print code. Open: #1196, #1197, #1201, #1202, the stall.

## What would overturn this

A row-tagged litmus failure on any CPU pair with identical Inner Shareable
Write-Back mappings, MMU and caches on; #1202 resolving as lost cross-CPU
visibility, or an NC 10-boot series passing where the removal repeats it;
a different Pi 5 EEPROM or BL31 (version not determined here).

## Evidence

- Linux, same boards and firmware, row-tagged suite, about 10 min each: 0
  forbidden, 0 timeouts, 0 weak on every ordered pair. SLM-OS reads the same
  `CPUECTLR_EL1` as Linux on each CPU of both boards (Pi 5 CPU 0 differs
  from CPUs 1-3 under both); `CLUSTERECTLR_EL1` compared on Pi 5 only.
- SLM-OS row-tagged suite, every row 0 / 0 / 0 weak: Pi 5 NC, `NC_AXIS=OFF`
  and removal; Jetson NC on kexec, `NC_AXIS=OFF` and removal on both paths
  (removal UEFI kept as a summary only). Staleness seen on hardware:
  caches-off readers on every same-cluster pair; under `NC_AXIS=OFF` a
  Non-shareable alias timed out on exactly the 16 cross-cluster pairs (the
  earlier NC run: 11 of 16); control (a) not run on the removal.
- Removal against NC: context switch Pi 5 2318 to 105 ns, Jetson 5456 to
  84 ns (kexec), 18008 to 66-102 ns (UEFI); IPC Pi 5 812 to 131 ns, Jetson
  equal; stealing equal on Pi 5, equal or better on Jetson; `bench smp`
  completed in every run on both boards.
- Boots: Pi 5 removal 10/10. Jetson `NC_AXIS=OFF` 10/10 per path,
  integration 16/16 on one boot per path.
