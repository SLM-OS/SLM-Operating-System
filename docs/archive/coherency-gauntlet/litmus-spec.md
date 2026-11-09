# Coherency litmus specification (#1193)

One specification shared by the Linux harness (`scripts/coherency/`, C1) and
the SLM-OS suite (`kernel/tests/test_coherency.c` and the `coherency` verb,
C2), so results compare row for row. All shared variables are on cacheable
Normal Write-Back Inner Shareable memory, one variable per 64-byte line
(128-byte alignment on Jetson is harmless and allowed), with no cache
maintenance instructions anywhere in a pattern. Only `DMB ISH*`, `LDAR`,
`STLR`, `LDXR`/`STXR` (or LSE `LDADD`) are used, as stated per pattern.

Every pattern runs over every ordered pair of distinct online CPUs (Pi 5:
12 pairs; Jetson: 30 pairs, which includes the cluster 0 / cluster 1 pairs
between CPUs 0-3 and 4-5), except P4 and P6, which use all online CPUs at
once. Each result row reports: pattern, CPU pair or set, iterations,
forbidden-outcome count, timeouts, and elapsed time.

| Id | Pattern | Code | Forbidden outcome (any count > 0 fails) |
|---|---|---|---|
| P1 | MP+rel+acq | Writer for i = 1..N: `data = i; STLR flag = i`. Reader loops: `f = LDAR flag; d = data`. | `d < f` |
| P2 | MP+dmbs | As P1 with plain accesses: writer `data = i; DMB ISHST; flag = i`; reader `f = flag; DMB ISHLD; d = data`. | `d < f` |
| P3 | SB+dmbs | Rounds r = 1..N separated by a sense-reversing spin barrier: T0 `x = r; DMB ISH; r0 = y`; T1 `y = r; DMB ISH; r1 = x`. | `r0 < r && r1 < r` |
| P4 | CoRR | One writer increments `x` with plain stores; every other CPU reads `x` twice in a row with plain loads. Writer rotates through all CPUs. | second read < first read |
| P5 | Ping-pong | Two CPUs alternate on one counter: CPU A waits for an even value and writes value+1, CPU B waits for odd and writes value+1, plain loads, `STLR` stores, N round trips. | a lost or duplicated step (final != 2N), or no progress within the timeout |
| P6 | Atomic sum | All online CPUs add 1 to one word N times with exclusive or LSE atomics. | final != CPUs x N |
| S1 | MP+none (sensitivity) | As P2 with both DMBs removed. | none: relaxed outcomes are ALLOWED on Armv8. Report the count to show the harness can observe reordering. |
| S2 | SB+none (sensitivity for P3) | As P3 with both DMBs removed. | none: `r0 < r && r1 < r` is ALLOWED without barriers. Report the count per pair. |

P4 coverage (revision 2): each P4 reader also counts `pairs` (read pairs
taken) and `straddles` (pairs whose two reads returned different values,
that is, pairs that observed a write in between). A P4 row reports both, and
P4 runs for a time budget instead of a fixed count: the writer keeps
incrementing until every reader has at least 10^5 straddles or 2 s pass. A
P4 row with zero straddles is reported as `weak`, not as a pass.

Timeouts: any wait loop that makes no progress for 1 s counts as a timeout
and is reported as a failure of that row (a stale line that never updates
shows up here). Iteration counts: at least 10^6 per pair for P1, P2, P4 and
S1, 10^5 rounds per pair for P3 and P5, 10^6 per CPU for P6; the Linux run
loops the whole set for at least 10 minutes per board.

Pass criterion for a board: every P row has zero forbidden outcomes and
zero timeouts, and every P4 row has nonzero straddles. S1 and S2 are
reported, never judged, but a P3 or P2 result counts as sensitive on a CPU
pair only if S2 or S1 respectively showed relaxed outcomes on that pair.

Revision 2 (2026-11-09, after C1 critic round 1): adds S2 and the P4
coverage counters. The checkers for every forbidden predicate (P1-P6) and
the path from a failing row to the final FAIL verdict must be fault-injected
in the self-test.
