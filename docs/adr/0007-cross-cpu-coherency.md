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

Measured: Inner Shareable Write-Back memory is coherent across all CPUs on
both boards, including between the Jetson clusters, under Linux and SLM-OS.
The NC axis is not required for coherency; the SMPEN account is wrong. The
target is decision (a): CPU-shared state in ordinary
cacheable memory, no CPU-to-CPU cache maintenance, ordering by barriers,
acquire and release; maintenance only for DMA, page tables written before
the MMU is on, and `secondary_mmu_*` (read with the MMU off); the NC block
for DMA only.

The removal does not land until both boards meet the bar:

- Pi 5: the integration "match" criterion fails. Across all runs, the
  removal test kernel failed `test_slm_stats_live_from_second_session`
  (#1202) in 5 of 21 boots and `test_component_cleanup_ctx_is_per_task`
  (#1205) in 1 of 10; the NC test kernel failed neither in 11. Both waits
  are bounded by yield count, so a much faster context switch may expire
  them early (untested). Next: the #1202 experiment.
- Jetson: boots and integration now meet the bar (rebased removal: 10/10
  log-verified boots and integration 16/16 per path), but a CPU sometimes
  makes 0 successful steals (#1203): `NC_AXIS=OFF` CPUs 2-3 in 6 of 6 runs;
  removal 3 of 17 runs with per-CPU deltas; NC 0 of 6. Next: 10+ such
  runs per build per path. An unexplained removal-only stall makes (b),
  keeping the axis on Jetson, the outcome.

## Alternatives considered

- Keep the axis everywhere with a measured justification (c): no
  measurement on either board shows incoherence.
- Keep it on Jetson only (b): open until the steal experiment.

## Consequences

Neither core has SMPEN (A76 CPUECTLR bit 6 RES0, TRM 100798 13.32; A78AE
bit 6 is PF_MODE[0], 101779 3.2.47); a core joins its cluster's coherency
with no software step (A76 5.9, A78AE 2.4.9, DSU 100453 2.1). Coherency
between the Jetson clusters has no public source and rests on measurement.
Caches-off loads do not allocate (A76 7.2.3), so "pre-MMU reads left stale
lines" cannot hold. Defects fixed: D1, D2, D12 and an extern mismatch
that page-faulted the first cacheable build (C3 audit). `make test`
rejects new cache maintenance in scheduler, IPC, spinlock and print code.
Open: #1196, #1197, #1201, #1202, the stall.

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
  (removal UEFI kept as a summary only). The suite detects a reader that
  never sees the writer's lines: an NC-alias reader fails every pair, a
  Non-shareable alias the 16 Jetson cross-cluster pairs, both by timeout.
  No forbidden predicate has fired on real hardware, and P3 is not a
  coherence detector, so late or out-of-order lines are not ruled out.
- Removal against NC: context switch Pi 5 2318 to 105 ns, Jetson 5456 to
  84 ns (kexec), 18008 to 66-102 ns (UEFI); IPC Pi 5 812 to 131 ns, Jetson
  equal; stealing equal (Pi 5 bimodal by task split; Jetson medians 10136
  and 9801 against 10627 and 9831 us); `bench smp` always completed.
- Boots, rebased removal: Pi 5 10/10 and 21 test-kernel boots
  (integration 16/16 each); Jetson 10/10 per path, log-verified.
