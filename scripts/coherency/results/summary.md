# Linux reference bar: summary (#1193)

Spec revision 3, harness binary sha256
8cc2ec369d2861dfc19fd29850c82778582164dcc387a045918ca6df50815252
(version 1193-c1-v3). Raw output and version headers: `jetson-nano-2.txt`,
`pi-5-1.txt`; the INDEX at the top of each marks the v3 run as the reference
and the earlier v1 and v2 runs as records only (their P1, P2 and S1 rows
carry the false-pass path fixed in revision 3).

Each row gives runs / forbidden / timeouts. A P row passes when forbidden
and timeouts are zero over every run and no row is weak. S1 and S2 are
sensitivity rows, reported, never judged.

## jetson-nano-2: PASS

Linux 5.15.148-tegra, L4T R36.4.7, UEFI 36.4.7, nvpmodel 25W, 6x
Cortex-A78AE r0p1. BL31 `v2.8(release)` built 11:44:32 May 9 2026 (TCU
banner from a cold boot the same night, not from the v3 boot itself).
28 passes, 613.0 s, 5264 rows, 0 weak.

| Row | Result | Coverage |
|---|---|---|
| P1 | 840 / 0 / 0 | min observed 1,945,852 |
| P2 | 840 / 0 / 0 | min observed 656,471 |
| P3 | 840 / 0 / 0 | sensitive on 10 of 30 pairs only |
| P4 | 168 / 0 / 0 | min straddles 100,000 per reader |
| P5 | 840 / 0 / 0 | |
| P6, P6L | 28 / 0 / 0 each | |
| S1 | 34,185,793 relaxed | nonzero on all 30 pairs |
| S2 | 22 relaxed | nonzero on 10 of 30 pairs, 1 of 16 cross-cluster |

CPUECTLR_EL1 0xa000400b40543000 on all CPUs at EL2; CLUSTERECTLR not read.

## pi-5-1: PASS

Raspberry Pi OS kernel 6.18.60+rpt-rpi-2712 from an initramfs on the
SLM-OS card, EEPROM firmware 2024-09-23 (2682625908f5), stock embedded BL31
(the card has no `armstub` line; the repository template
`deploy/pi5/config.txt:46` does). 4x Cortex-A76 r4p1. 265 passes, 600.9 s,
20670 rows, 0 weak.

| Row | Result | Coverage |
|---|---|---|
| P1 | 3180 / 0 / 0 | min observed 862,338 |
| P2 | 3180 / 0 / 0 | min observed 863,230 |
| P3 | 3180 / 0 / 0 | sensitive on 12 of 12 pairs |
| P4 | 1060 / 0 / 0 | min straddles 100,000 per reader |
| P5 | 3180 / 0 / 0 | |
| P6, P6L | 265 / 0 / 0 each | |
| S1 | 353,880,832 relaxed | nonzero on all 12 pairs |
| S2 | 3,616 relaxed | nonzero on all 12 pairs |

CPUECTLR_EL1 cpu0 0x2000000961563000 (bit 61, MXP_EN), cpu1-3
0x0000000961563000; CLUSTERECTLR_EL1 0x500; all CPUs at EL2.

## What the evidence supports

Coherence (P1, P2, P4, P5, P6) is shown on every ordered CPU pair of both
boards, including the 16 Jetson cross-cluster pairs, and P2 is sensitive on
every pair. Store-buffering ordering under DMB ISH (P3) is shown on all 12
Pi 5 pairs, but only on 10 of 30 Jetson pairs (1 of 16 cross-cluster); on
the other 20 Jetson pairs the harness never observed SB reordering, so P3
says nothing there. P3 and S2 values are not row-tagged, so P3 tests
ordering, not stale lines.
