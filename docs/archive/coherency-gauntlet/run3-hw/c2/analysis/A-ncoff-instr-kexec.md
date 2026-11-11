## Cell A-ncoff-instr-kexec

Counted: `A-ncoff-instr-kexec-run01.txt` .. `A-ncoff-instr-kexec-run20.txt` (20 files); head 6608cbd0.
Format v2; mode sustained, depth 16, work 8, sustain_ms 2500, cpus 6.

### T1 per CPU (`A-ncoff-instr-kexec-run01.txt` .. `A-ncoff-instr-kexec-run20.txt` (20 files))

| CPU | median share | pooled share | exec | att | ok | ok % att | iso % att | f_stale % att | f_pd % att | f_empty % att | f_other % att | first success median / max (ms) | runs with ok=0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 18.5% | 19.2% | 3485 | 55878 | 3483 | 6.2% | 0.0% | 0.0% | 0.1% | 93.6% | 0.0% | 0.1 / 0.1 (20 runs) | 0 of 20 |
| 1 (owner) | 17.7% | 18.1% | 3293 | 329 | 0 | 0.0% | 0.0% | 0.0% | 42.9% | 56.8% | 0.0% | none | 20 of 20 |
| 2 | 13.8% | 12.9% | 2344 | 7797 | 2310 | 29.6% | 0.0% | 0.3% | 65.9% | 4.2% | 0.0% | 204.5 / 1470.1 (20 runs) | 0 of 20 |
| 3 | 14.2% | 11.5% | 2083 | 7303 | 2079 | 28.5% | 0.0% | 0.3% | 67.2% | 4.1% | 0.0% | 167.3 / 1332.1 (17 runs) | 3 of 20 (A-ncoff-instr-kexec-run06.txt, A-ncoff-instr-kexec-run15.txt, A-ncoff-instr-kexec-run20.txt) |
| 4 | 18.3% | 19.2% | 3481 | 4068 | 3480 | 85.5% | 0.0% | 1.2% | 4.9% | 8.4% | 0.0% | 5.5 / 9.8 (20 runs) | 0 of 20 |
| 5 | 18.4% | 19.2% | 3484 | 4059 | 3483 | 85.8% | 0.0% | 0.8% | 5.7% | 7.7% | 0.0% | 5.5 / 9.8 (20 runs) | 0 of 20 |

Best thief by median share: CPU 0 (18.5%). Ratio of each thief's median share to it: CPU 2 0.748, CPU 3 0.767, CPU 4 0.991, CPU 5 0.994.
Runs with a non-owner CPU at ok=0: 3 of 20 (A-ncoff-instr-kexec-run06.txt, A-ncoff-instr-kexec-run15.txt, A-ncoff-instr-kexec-run20.txt).

### T2 thieves against CPU 1's preempt_disabled window and tick (`A-ncoff-instr-kexec-run01.txt` .. `A-ncoff-instr-kexec-run20.txt` (20 files))

Inside window = attempts that started with preempt_disabled[w] set. Phase columns are % of the attempts that had a watched tick.

| thief | att | inside window | in T | in C | in P | in O | in B | in, no window | f_pd % att |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 55878 | 0.1% (43) | 41 | 0 | 0 | 2 | 0 | 0 | 0.1% |
| 2 | 7797 | 53.4% (4160) | 4152 | 0 | 0 | 0 | 0 | 8 | 65.9% |
| 3 | 7303 | 57.1% (4167) | 4166 | 0 | 0 | 0 | 0 | 1 | 67.2% |
| 4 | 4068 | 0.0% (2) | 2 | 0 | 0 | 0 | 0 | 0 | 4.9% |
| 5 | 4059 | 0.1% (3) | 3 | 0 | 0 | 0 | 0 | 0 | 5.7% |

Offset into the open window (w_off), % of in-window attempts:

| thief | <1us | <2us | <4us | <8us | <16us | <32us | <64us | >=64us |
|---|---|---|---|---|---|---|---|---|
| 0 | 11.6% | 18.6% | 27.9% | 32.6% | 9.3% | 0.0% | 0.0% | 0.0% |
| 2 | 46.1% | 9.0% | 36.7% | 8.1% | 0.0% | 0.0% | 0.0% | 0.0% |
| 3 | 31.4% | 4.8% | 39.7% | 24.1% | 0.0% | 0.0% | 0.0% | 0.0% |
| 4 | 0.0% | 50.0% | 0.0% | 0.0% | 50.0% | 0.0% | 0.0% | 0.0% |
| 5 | 33.3% | 0.0% | 0.0% | 33.3% | 33.3% | 0.0% | 0.0% | 0.0% |

Uniform reference uses CPU 1's median tick period 10000.4 us (20 runs).

ph_w: attempt time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 55878 | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 9.8% | 29.4% | 50.7% | 0.0% | 2.1% |
| 2 | 7797 | 69.9% | 0.0% | 0.1% | 0.0% | 0.5% | 0.7% | 0.7% | 2.0% | 7.7% | 14.4% | 3.9% | 70.6% |
| 3 | 7303 | 68.4% | 0.0% | 0.0% | 0.0% | 0.3% | 0.5% | 0.2% | 1.2% | 5.6% | 22.3% | 1.4% | 68.8% |
| 4 | 4068 | 0.1% | 0.0% | 0.2% | 0.4% | 0.5% | 1.9% | 7.5% | 10.5% | 30.4% | 48.4% | 0.0% | 1.2% |
| 5 | 4059 | 0.1% | 0.1% | 0.1% | 0.5% | 0.5% | 2.0% | 6.6% | 11.0% | 31.5% | 47.6% | 0.0% | 1.2% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_w_pd: attempts ending on the early-out, time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 77 | 62.3% | 1.3% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 2.6% | 6.5% | 27.3% | 0.0% | 63.6% |
| 2 | 5139 | 98.5% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.4% | 1.0% | 98.6% |
| 3 | 4904 | 99.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.6% | 0.4% | 99.0% |
| 4 | 201 | 1.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 19.4% | 11.9% | 28.9% | 38.8% | 0.0% | 1.0% |
| 5 | 230 | 1.3% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 9.1% | 10.0% | 29.1% | 50.4% | 0.0% | 1.3% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_own: attempt time after the thief's own tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 55878 | 0.1% | 0.1% | 0.3% | 0.6% | 0.9% | 2.7% | 4.6% | 9.0% | 30.6% | 51.1% | 0.0% | 2.0% |
| 2 | 7797 | 75.5% | 0.1% | 0.1% | 0.0% | 0.5% | 0.7% | 0.7% | 2.0% | 7.7% | 12.8% | 0.0% | 76.1% |
| 3 | 7303 | 81.2% | 0.1% | 0.0% | 0.0% | 0.3% | 0.4% | 0.2% | 1.2% | 5.6% | 10.9% | 0.0% | 81.6% |
| 4 | 4068 | 58.1% | 0.0% | 0.0% | 0.0% | 0.1% | 0.2% | 0.1% | 0.5% | 8.8% | 32.2% | 0.0% | 58.3% |
| 5 | 4059 | 57.4% | 0.1% | 0.0% | 0.0% | 0.2% | 0.3% | 0.2% | 0.8% | 8.1% | 32.8% | 0.0% | 57.7% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

### T3 tick phase against CPU 1 (from tick_first_us) (`A-ncoff-instr-kexec-run01.txt` .. `A-ncoff-instr-kexec-run20.txt` (20 files))

Offset = (tick_first_us[c] - tick_first_us[w]) mod period, us.

Watched tick period median 10000.4 us over 20 runs.

| CPU | runs | offset median | min | max |
|---|---|---|---|---|
| 0 | 20 | 4502 | 45 | 9836 |
| 1 | 20 | 0 | 0 | 0 |
| 2 | 20 | 0 | 0 | 10000 |
| 3 | 20 | 0 | 0 | 9995 |
| 4 | 20 | 5401 | 932 | 6546 |
| 5 | 20 | 5402 | 932 | 6547 |

### T4 CPU 1's windows (`A-ncoff-instr-kexec-run01.txt` .. `A-ncoff-instr-kexec-run20.txt` (20 files))

| reason | windows | total us | mean us | longest us | % of wall-clock |
|---|---|---|---|---|---|
| T | 5075 | 48031 | 9.5 | 16 | 0.095% |
| C | 0 | 0 | 0.0 | 0 | 0.000% |
| P | 843 | 1564 | 1.9 | 3 | 0.003% |
| O | 2455 | 4637 | 1.9 | 3 | 0.009% |
| B | 0 | 0 | 0.0 | 0 | 0.000% |
| all | 8373 | 54232 | 6.5 | 16 | 0.107% |

Window entry after CPU 1's own tick (wph), % of 8373 windows:

| <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 60.7% | 0.1% | 0.0% | 0.0% | 0.8% | 0.7% | 0.2% | 2.0% | 21.1% | 14.5% | 0.0% | 61.6% |

wlog entries: 2560 over 20 runs; by reason: O 731, P 249, T 1580.
