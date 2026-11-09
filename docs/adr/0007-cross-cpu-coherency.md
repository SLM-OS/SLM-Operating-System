# ADR 0007: cross-CPU state is cacheable on Pi 5 and Jetson

## Context

Since April 2026 run queues, the task table, current-task pointers and boot
flags lived in a Normal Non-cacheable block on Pi 5 and Jetson
(`PLATFORM_HAS_NC_MEMORY`), shared fields were cleaned and invalidated by
hand, and Pi 5 spinlocks were wrapped in `DC CIVAC`. The reason given was
that firmware leaves `CPUECTLR_EL1.SMPEN` clear, so private L2s are
incoherent. Nothing re-tested it after the set/way fix in `e0a15259`.

## Decision

Status: proposed.

Measured on both boards: the hardware keeps Inner Shareable Write-Back
memory coherent across all CPUs, including between the Jetson clusters. The
NC axis is not required by the hardware.

Pi 5: decision (a). CPU-shared state is ordinary cacheable memory with no
CPU-to-CPU cache maintenance; ordering uses barriers, acquire and release.
Maintenance stays for DMA, for page tables written before the MMU is on,
and for `secondary_mmu_*`, read by secondaries with the MMU off. The NC
block stays, for DMA only.

Jetson: not decided. The removal build has 4 kexec and 2 UEFI boots with
captured logs, against 10, and no integration run. In both cacheable builds
one CPU sometimes makes 0 successful steals in a `bench stealing` run (CPUs
2-3 under `NC_AXIS=OFF`, CPU 4 under the removal); it is not shown absent
from the NC build. Next: at least 10 runs with per-CPU steal deltas per
build per boot path. A stall in both builds is a separate stealing defect;
a stall only without NC is a removal regression. The code is shared, so the
removal does not land until Jetson is decided.

## Alternatives considered

- Keep the axis on one board, or everywhere with a measured justification
  (b, c): no measurement on either board shows incoherence.
- Set an enable bit in BL31: neither core has one.
- Keep `NC_AXIS=OFF` as an option: it was slower than both the NC build and
  the removal on Jetson stealing, and a second path must be tested twice.

## Consequences

Neither core has SMPEN (A76 CPUECTLR bit 6 RES0, TRM 100798 13.32; A78AE
bit 6 is PF_MODE[0], 101779 3.2.47); both join coherency with no software
step (A76 5.9, A78AE 2.4.9, DSU 100453 2.1). Caches-off loads do not
allocate (A76 7.2.3), so "pre-MMU reads left stale lines" cannot hold. The
defect found was uncleaned `secondary_mmu_*` (fixed). `make test` rejects
new cache maintenance in scheduler, IPC, spinlock and print code. Open:
#1202, #1197, #1201 and the steal stall.

## What would overturn this

A row-tagged litmus failure on any CPU pair with identical Inner Shareable
Write-Back mappings, MMU and caches on, or #1202 resolving as lost
cross-CPU visibility.

## Evidence

- Linux, same boards and firmware, row-tagged suite, 10 min each: 0
  forbidden, 0 timeouts, 0 weak on every ordered pair. SLM-OS reads the same
  `CPUECTLR_EL1` and `CLUSTERECTLR_EL1` on every CPU.
- SLM-OS, NC, `NC_AXIS=OFF` and removal builds, both boards and Jetson
  paths: every row 0 / 0 / 0 weak. The suite sees staleness on hardware:
  caches-off readers on every same-cluster pair; a Non-shareable alias on
  exactly the 16 Jetson cross-cluster pairs.
- Removal against NC: context switch Pi 5 2318 to 105 ns, Jetson 5456 to
  84 ns (kexec), 18008 to 66-102 ns (UEFI); IPC Pi 5 812 to 130 ns, Jetson
  equal; stealing equal on Pi 5, equal or better on Jetson.
- Pi 5 removal: 10/10 boots, 11 test-kernel boots, integration 16/16 each.
- Data: `docs/archive/coherency-gauntlet/coherency.md`.
