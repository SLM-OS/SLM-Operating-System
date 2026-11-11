# ADR 0007: cross-CPU state should be cacheable on Pi 5 and Jetson

## Context

Since April 2026 run queues, the task table, current-task pointers and boot
flags lived in a Normal Non-cacheable block on Pi 5 and Jetson
(`PLATFORM_HAS_NC_MEMORY`) with hand-written cache maintenance. The reason
given was that firmware leaves `CPUECTLR_EL1.SMPEN` clear, so private L2s
are incoherent. Nothing re-tested it after the set/way fix `e0a15259`.

## Decision

Status: accepted; implementation in PR #1225.

Measured: Inner Shareable Write-Back memory is coherent across all CPUs on
both boards, including between the Jetson clusters, under Linux and SLM-OS.
The NC axis is not required for coherency; the SMPEN account is wrong. The
target is decision (a): CPU-shared state in ordinary cacheable memory, no
CPU-to-CPU cache maintenance, ordering by barriers, acquire and release;
maintenance only for DMA, page tables written before the MMU is on, and
`secondary_mmu_*` (read with the MMU off); the NC block for DMA only.

The removal does not land until both boards meet the bar:

- Pi 5: the integration "match" criterion fails. The removal test kernel
  failed `test_slm_stats_live_from_second_session` (#1202) in 5 of 22
  boots and `test_component_cleanup_ctx_is_per_task` (#1205) in 1 of 22;
  the NC test kernel failed neither in 11. Both waits are bounded by yield
  count, so a much faster context switch may expire them early (untested).
  Next: the #1202 and #1205 experiments.
- Jetson: 10/10 boots per path reached the shell (tail-only logs) and
  integration passed 16/16 on one hardware-set boot per path, which does
  not run the #1202 or #1205 tests. A CPU sometimes makes 0 successful
  steals (#1203): `NC_AXIS=OFF` CPUs 2-3 in 6 of 6 runs; removal UEFI 3 of
  7, kexec 0 of 10; NC 0 of 6. Next: 10+ runs with per-CPU deltas per build
  per path. An unexplained stall absent under NC makes (b), keeping the
  axis on Jetson, the outcome.

## Alternatives considered

- Keep the axis everywhere with a measured justification (c): no
  measurement on either board shows incoherence. (b) is open, above.

## Consequences

Neither core has SMPEN (A76 CPUECTLR bit 6 RES0, TRM 100798 13.32; A78AE
bit 6 is PF_MODE[0], 101779 3.2.47); a core joins its cluster's coherency
with no software step (A76 5.9, A78AE 2.4.9, DSU 100453 2.1). Coherency
between the Jetson clusters has no public source and rests on measurement.
Caches-off loads do not allocate (A76 7.2.3), so "pre-MMU reads left stale
lines" cannot hold. Defects fixed: D1, D2, D12 and an extern mismatch that
page-faulted the first cacheable build (C3 audit). Open: #1196, #1197,
#1201, #1202, #1203, #1204, #1205.

## What would overturn this

A row-tagged litmus failure on any CPU pair with identical Inner Shareable
Write-Back mappings, MMU and caches on; #1202 or #1205 resolving as lost
cross-CPU visibility; a different Pi 5 EEPROM or BL31 (version not
determined here). The NC 10-boot condition was met (NC 0 of 10, removal
4 of 10), so Pi 5 stays pending until those experiments separate a timing
bound from lost visibility.

## Run 2 (2026-11-10)

#1202 and #1205 are timing: both waits were bounded by yield counts, and
a yield costs 2.30 us under NC against 0.07 to 0.13 us under the removal.
Lost visibility is excluded by a forced variant of #1205 (0 of 120
cleanups lost; every one completed after the bound expired, on both
kernels), and #1202's flag was seen 1.3 and 1.5 ms after expiry. With
wall-clock bounds both passed 9 of 9 boots per kernel, short of the 20 of
20 bar. #1203's run-1 evidence is superseded by #1216's sustained rows;
run 1's attempt counts were read after the results printed and are
inflated. #1216 (a thief failing on the `preempt_disabled[victim]`
early-out for 2.5 s, NC_AXIS=OFF) is unmeasured on NC and the removal.
Status unchanged.

## Run 3 (2026-11-11)

The removal cleared #1216's signature on Jetson. In 20 runs per cell of
`bench stealing --sustain 2500` with victim-window instrumentation, CPUs 2
and 3 executed 0.713 and 0.830 of the best thief's median share under the
NC kernel over kexec and 0.397 and 0.556 over UEFI-direct, ending 63 to
76% of their steal attempts on the `preempt_disabled[victim]` early-out;
under the removal they executed 0.990 and 1.000 (kexec) and 0.955 and
0.935 (UEFI-direct), with no thief at 0 successes in any run. #1216 stays
open as an NC-kernel scheduler defect; its wake-path mechanism is a
hypothesis with a separate fix. Pi 5 met its bar: 20 of 20 integration
boots under each kernel, #1202 and #1205 passing with wall-clock waits.
Evidence: `coherency-3.md`, `critic-3-C1.md` and `critic-3-C2.md` on
`gauntlet/coherency-3`; raw rows under
`docs/archive/coherency-gauntlet/run3-hw/` on `gauntlet-3-hw-jetson` and
`gauntlet-3-hw-pi5`.

## Evidence

- Linux, same boards and firmware, row-tagged suite, about 10 min each: 0
  forbidden, 0 timeouts, 0 weak on every ordered pair. SLM-OS reads the same
  `CPUECTLR_EL1` as Linux on each CPU of both boards; `CLUSTERECTLR_EL1`
  compared on Pi 5 only.
- SLM-OS row-tagged suite, every P row 0 / 0 / 0 weak: Pi 5 NC,
  `NC_AXIS=OFF` and removal; Jetson NC on kexec, `NC_AXIS=OFF` and removal
  on both paths. It detects a reader that never sees the writer's lines: an
  NC-alias reader fails every pair, a Non-shareable alias the 16 Jetson
  cross-cluster pairs, both by timeout. No rev 3 P1-P6 forbidden predicate
  has fired on real incoherence, and P3 is not a coherence detector, so
  late or out-of-order lines are not ruled out.
- Removal against NC, pre-rebase: context switch Pi 5 2318 to 105 ns,
  Jetson 5456 to 84 ns (kexec), 18008 to 66-102 ns (UEFI); IPC Pi 5 812 to
  131 ns, Jetson equal. Stealing medians: Pi 5 removal 13021 us
  pre-rebase, 16146 post-rebase, NC 15715 (bimodal by task split, not
  distinguishable at 5 runs); Jetson post-rebase 10136 and 9801 against NC
  10627 and 9831. `bench smp` always completed.
- Boots: Pi 5 rebased removal 10/10 and 22 test-kernel boots (10 rebased,
  12 pre-rebase; integration 16/16 each); Jetson as above.
