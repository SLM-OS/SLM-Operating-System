## Cell D-removal-instr-uefi

Counted: `D-removal-instr-uefi-run01.txt` .. `D-removal-instr-uefi-run20.txt` (20 files); head 8fc7c88a.
Format v2; mode sustained, depth 16, work 8, sustain_ms 2500, cpus 6.

### T1 per CPU (`D-removal-instr-uefi-run01.txt` .. `D-removal-instr-uefi-run20.txt` (20 files))

| CPU | median share | pooled share | exec | att | ok | ok % att | iso % att | f_stale % att | f_pd % att | f_empty % att | f_other % att | first success median / max (ms) | runs with ok=0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 17.6% | 17.6% | 4343 | 364084 | 4338 | 1.2% | 0.0% | 0.0% | 0.0% | 98.8% | 0.0% | 0.0 / 0.0 (20 runs) | 0 of 20 |
| 1 (owner) | 16.7% | 16.6% | 4108 | 1254 | 0 | 0.0% | 0.0% | 0.2% | 5.7% | 94.1% | 0.0% | none | 20 of 20 |
| 2 | 16.8% | 16.6% | 4093 | 6544 | 4073 | 62.2% | 0.0% | 1.3% | 11.5% | 24.9% | 0.0% | 4.4 / 9.8 (20 runs) | 0 of 20 |
| 3 | 16.4% | 16.3% | 4024 | 6764 | 4019 | 59.4% | 0.0% | 0.9% | 15.3% | 24.4% | 0.0% | 5.5 / 9.8 (20 runs) | 0 of 20 |
| 4 | 16.6% | 16.6% | 4094 | 5983 | 4094 | 68.4% | 0.0% | 1.1% | 5.8% | 24.7% | 0.0% | 5.4 / 9.9 (20 runs) | 0 of 20 |
| 5 | 16.3% | 16.3% | 4011 | 6155 | 4011 | 65.2% | 0.0% | 0.5% | 9.3% | 25.0% | 0.0% | 5.4 / 9.9 (20 runs) | 0 of 20 |

Best thief by median share: CPU 0 (17.6%). Ratio of each thief's median share to it: CPU 2 0.955, CPU 3 0.935, CPU 4 0.942, CPU 5 0.930.
Runs with a non-owner CPU at ok=0: 0 of 20.

### T2 thieves against CPU 1's preempt_disabled window and tick (`D-removal-instr-uefi-run01.txt` .. `D-removal-instr-uefi-run20.txt` (20 files))

Inside window = attempts that started with preempt_disabled[w] set. Phase columns are % of the attempts that had a watched tick.

| thief | att | inside window | in T | in C | in P | in O | in B | in, no window | f_pd % att |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 364084 | 0.0% (16) | 14 | 0 | 0 | 1 | 0 | 1 | 0.0% |
| 2 | 6544 | 6.1% (396) | 394 | 0 | 0 | 0 | 0 | 2 | 11.5% |
| 3 | 6764 | 8.1% (548) | 539 | 0 | 0 | 1 | 0 | 8 | 15.3% |
| 4 | 5983 | 0.0% (0) | 0 | 0 | 0 | 0 | 0 | 0 | 5.8% |
| 5 | 6155 | 0.0% (0) | 0 | 0 | 0 | 0 | 0 | 0 | 9.3% |

Offset into the open window (w_off), % of in-window attempts:

| thief | <1us | <2us | <4us | <8us | <16us | <32us | <64us | >=64us |
|---|---|---|---|---|---|---|---|---|
| 0 | 78.6% | 21.4% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| 2 | 96.7% | 3.3% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| 3 | 96.7% | 3.3% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| 4 | - | - | - | - | - | - | - | - |
| 5 | - | - | - | - | - | - | - | - |

Uniform reference uses CPU 1's median tick period 10000.3 us (20 runs).

ph_w: attempt time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 364084 | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.1% | 5.3% | 10.4% | 34.2% | 45.0% | 0.0% | 2.0% |
| 2 | 6544 | 15.5% | 0.1% | 0.4% | 0.1% | 4.0% | 19.8% | 10.5% | 5.8% | 23.4% | 19.8% | 0.8% | 20.0% |
| 3 | 6764 | 21.4% | 0.0% | 0.5% | 0.2% | 0.3% | 1.6% | 0.8% | 6.6% | 27.2% | 39.9% | 1.4% | 22.4% |
| 4 | 5983 | 0.0% | 0.0% | 0.3% | 0.5% | 0.0% | 1.3% | 3.4% | 9.8% | 31.5% | 53.3% | 0.0% | 0.8% |
| 5 | 6155 | 0.0% | 0.0% | 0.2% | 0.4% | 0.0% | 1.0% | 3.3% | 9.8% | 30.7% | 54.5% | 0.0% | 0.6% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_w_pd: attempts ending on the early-out, time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 83 | 19.3% | 0.0% | 0.0% | 0.0% | 3.6% | 6.0% | 6.0% | 7.2% | 26.5% | 28.9% | 2.4% | 22.9% |
| 2 | 755 | 93.8% | 0.0% | 0.1% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.5% | 0.0% | 5.6% | 93.9% |
| 3 | 1032 | 95.2% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.2% | 0.6% | 0.0% | 4.1% | 95.2% |
| 4 | 345 | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 2.9% | 11.9% | 40.9% | 44.3% | 0.0% | 0.0% |
| 5 | 574 | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.3% | 3.0% | 14.5% | 40.9% | 41.3% | 0.0% | 0.0% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_own: attempt time after the thief's own tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 364083 | 0.1% | 0.1% | 0.4% | 0.6% | 1.2% | 3.6% | 5.2% | 10.8% | 30.0% | 48.0% | 0.0% | 2.4% |
| 2 | 6544 | 54.9% | 0.0% | 0.7% | 0.3% | 0.0% | 0.0% | 0.0% | 5.8% | 23.8% | 14.7% | 0.0% | 55.8% |
| 3 | 6764 | 55.8% | 0.0% | 1.1% | 0.3% | 0.0% | 0.0% | 0.0% | 5.9% | 22.4% | 14.5% | 0.0% | 57.1% |
| 4 | 5983 | 53.5% | 0.0% | 0.3% | 0.5% | 0.0% | 0.0% | 0.0% | 4.2% | 26.2% | 15.3% | 0.0% | 54.4% |
| 5 | 6155 | 53.7% | 0.0% | 0.3% | 0.3% | 0.0% | 0.0% | 0.1% | 4.0% | 27.3% | 14.2% | 0.0% | 54.3% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

### T3 tick phase against CPU 1 (from tick_first_us) (`D-removal-instr-uefi-run01.txt` .. `D-removal-instr-uefi-run20.txt` (20 files))

Offset = (tick_first_us[c] - tick_first_us[w]) mod period, us.

Watched tick period median 10000.3 us over 20 runs.

| CPU | runs | offset median | min | max |
|---|---|---|---|---|
| 0 | 20 | 4258 | 29 | 9333 |
| 1 | 20 | 0 | 0 | 0 |
| 2 | 20 | 496 | 0 | 9999 |
| 3 | 20 | 4304 | 0 | 9999 |
| 4 | 20 | 4566 | 974 | 6177 |
| 5 | 20 | 4566 | 974 | 6177 |

### T4 CPU 1's windows (`D-removal-instr-uefi-run01.txt` .. `D-removal-instr-uefi-run20.txt` (20 files))

| reason | windows | total us | mean us | longest us | % of wall-clock |
|---|---|---|---|---|---|
| T | 5049 | 4007 | 0.8 | 3 | 0.008% |
| C | 0 | 0 | 0.0 | 0 | 0.000% |
| P | 594 | 113 | 0.2 | 0 | 0.000% |
| O | 3510 | 616 | 0.2 | 0 | 0.001% |
| B | 0 | 0 | 0.0 | 0 | 0.000% |
| all | 9153 | 4736 | 0.5 | 3 | 0.009% |

Window entry after CPU 1's own tick (wph), % of 9153 windows:

| <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 55.2% | 0.0% | 1.1% | 0.1% | 0.0% | 0.0% | 0.0% | 15.8% | 17.4% | 10.4% | 0.0% | 56.4% |

wlog entries: 2560 over 20 runs; by reason: O 969, P 167, T 1424.
