## Cell E-removal-instr-kexec

Counted: `E-removal-instr-kexec-run01.txt` .. `E-removal-instr-kexec-run20.txt` (20 files); head 8fc7c88a.
Format v2; mode sustained, depth 16, work 8, sustain_ms 2500, cpus 6.

### T1 per CPU (`E-removal-instr-kexec-run01.txt` .. `E-removal-instr-kexec-run20.txt` (20 files))

| CPU | median share | pooled share | exec | att | ok | ok % att | iso % att | f_stale % att | f_pd % att | f_empty % att | f_other % att | first success median / max (ms) | runs with ok=0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 16.6% | 16.7% | 3441 | 600231 | 3432 | 0.6% | 0.0% | 0.0% | 0.0% | 99.4% | 0.0% | 0.0 / 0.0 (20 runs) | 0 of 20 |
| 1 (owner) | 16.3% | 16.2% | 3343 | 332 | 1 | 0.3% | 0.0% | 0.3% | 3.0% | 96.4% | 0.0% | 74.0 / 74.0 (1 runs) | 19 of 20 |
| 2 | 16.7% | 16.7% | 3439 | 4564 | 3425 | 75.0% | 0.0% | 0.7% | 8.2% | 16.1% | 0.0% | 3.5 / 9.6 (20 runs) | 0 of 20 |
| 3 | 16.9% | 16.9% | 3473 | 4434 | 3467 | 78.2% | 0.0% | 2.3% | 6.4% | 13.1% | 0.0% | 5.0 / 9.5 (20 runs) | 0 of 20 |
| 4 | 16.8% | 16.8% | 3461 | 4159 | 3459 | 83.2% | 0.0% | 1.1% | 5.1% | 10.6% | 0.0% | 7.4 / 9.8 (20 runs) | 0 of 20 |
| 5 | 16.8% | 16.7% | 3449 | 4253 | 3448 | 81.1% | 0.0% | 0.9% | 7.6% | 10.5% | 0.0% | 7.4 / 9.8 (20 runs) | 0 of 20 |

Best thief by median share: CPU 3 (16.9%). Ratio of each thief's median share to it: CPU 0 0.986, CPU 2 0.990, CPU 4 1.000, CPU 5 0.995.
Runs with a non-owner CPU at ok=0: 0 of 20.

### T2 thieves against CPU 1's preempt_disabled window and tick (`E-removal-instr-kexec-run01.txt` .. `E-removal-instr-kexec-run20.txt` (20 files))

Inside window = attempts that started with preempt_disabled[w] set. Phase columns are % of the attempts that had a watched tick.

| thief | att | inside window | in T | in C | in P | in O | in B | in, no window | f_pd % att |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 600231 | 0.0% (37) | 34 | 0 | 0 | 1 | 0 | 2 | 0.0% |
| 2 | 4564 | 7.0% (321) | 318 | 0 | 0 | 0 | 0 | 3 | 8.2% |
| 3 | 4434 | 3.6% (158) | 157 | 0 | 0 | 0 | 0 | 1 | 6.4% |
| 4 | 4159 | 0.0% (0) | 0 | 0 | 0 | 0 | 0 | 0 | 5.1% |
| 5 | 4253 | 0.0% (0) | 0 | 0 | 0 | 0 | 0 | 0 | 7.6% |

Offset into the open window (w_off), % of in-window attempts:

| thief | <1us | <2us | <4us | <8us | <16us | <32us | <64us | >=64us |
|---|---|---|---|---|---|---|---|---|
| 0 | 65.7% | 25.7% | 8.6% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| 2 | 97.2% | 2.5% | 0.3% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| 3 | 96.2% | 3.8% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% |
| 4 | - | - | - | - | - | - | - | - |
| 5 | - | - | - | - | - | - | - | - |

Uniform reference uses CPU 1's median tick period 10000.3 us (20 runs).

ph_w: attempt time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 600231 | 0.1% | 0.1% | 0.3% | 0.5% | 1.1% | 3.3% | 5.6% | 10.5% | 27.4% | 51.1% | 0.0% | 2.1% |
| 2 | 4564 | 12.5% | 0.1% | 0.4% | 0.5% | 0.5% | 0.6% | 5.7% | 7.5% | 30.4% | 41.7% | 0.2% | 13.9% |
| 3 | 4434 | 8.3% | 0.0% | 0.2% | 0.1% | 6.1% | 44.3% | 0.1% | 1.5% | 12.2% | 26.5% | 0.7% | 14.7% |
| 4 | 4159 | 0.2% | 0.1% | 0.4% | 0.5% | 0.9% | 5.0% | 5.6% | 15.3% | 36.8% | 35.2% | 0.0% | 2.1% |
| 5 | 4253 | 0.1% | 0.1% | 0.2% | 0.4% | 0.6% | 5.3% | 6.1% | 15.7% | 37.0% | 34.6% | 0.0% | 1.4% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_w_pd: attempts ending on the early-out, time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 151 | 27.8% | 0.0% | 0.0% | 0.0% | 2.0% | 24.5% | 4.6% | 8.6% | 15.9% | 15.9% | 0.7% | 29.8% |
| 2 | 376 | 98.7% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.8% | 0.5% | 98.7% |
| 3 | 284 | 95.8% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 4.2% | 95.8% |
| 4 | 214 | 0.5% | 0.0% | 0.0% | 0.0% | 0.0% | 7.9% | 4.7% | 22.4% | 50.9% | 13.6% | 0.0% | 0.5% |
| 5 | 322 | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 9.3% | 8.1% | 25.8% | 46.0% | 10.9% | 0.0% | 0.0% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_own: attempt time after the thief's own tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 600228 | 0.1% | 0.1% | 0.3% | 0.5% | 0.9% | 2.6% | 4.3% | 8.9% | 30.6% | 51.8% | 0.0% | 1.8% |
| 2 | 4564 | 58.7% | 0.0% | 0.3% | 0.3% | 0.2% | 0.2% | 0.1% | 1.3% | 12.2% | 26.7% | 0.0% | 59.4% |
| 3 | 4434 | 59.0% | 0.0% | 0.2% | 0.3% | 0.2% | 0.3% | 0.0% | 1.7% | 12.9% | 25.4% | 0.0% | 59.8% |
| 4 | 4159 | 58.0% | 0.0% | 0.0% | 0.1% | 0.2% | 0.1% | 0.0% | 0.4% | 12.1% | 29.0% | 0.0% | 58.3% |
| 5 | 4253 | 58.4% | 0.0% | 0.0% | 0.0% | 0.2% | 0.0% | 0.0% | 0.5% | 10.4% | 30.5% | 0.0% | 58.6% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

### T3 tick phase against CPU 1 (from tick_first_us) (`E-removal-instr-kexec-run01.txt` .. `E-removal-instr-kexec-run20.txt` (20 files))

Offset = (tick_first_us[c] - tick_first_us[w]) mod period, us.

Watched tick period median 10000.3 us over 20 runs.

| CPU | runs | offset median | min | max |
|---|---|---|---|---|
| 0 | 20 | 4144 | 244 | 9501 |
| 1 | 20 | 0 | 0 | 0 |
| 2 | 20 | 5129 | 0 | 9999 |
| 3 | 20 | 318 | 0 | 9997 |
| 4 | 20 | 3452 | 377 | 5697 |
| 5 | 20 | 3452 | 377 | 5697 |

### T4 CPU 1's windows (`E-removal-instr-kexec-run01.txt` .. `E-removal-instr-kexec-run20.txt` (20 files))

| reason | windows | total us | mean us | longest us | % of wall-clock |
|---|---|---|---|---|---|
| T | 5059 | 5306 | 1.0 | 3 | 0.010% |
| C | 0 | 0 | 0.0 | 0 | 0.000% |
| P | 793 | 175 | 0.2 | 0 | 0.000% |
| O | 2551 | 608 | 0.2 | 0 | 0.001% |
| B | 0 | 0 | 0.0 | 0 | 0.000% |
| all | 8403 | 6089 | 0.7 | 3 | 0.012% |

Window entry after CPU 1's own tick (wph), % of 8404 windows:

| <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 60.2% | 0.0% | 1.5% | 0.8% | 0.1% | 0.0% | 0.0% | 2.8% | 20.4% | 14.2% | 0.0% | 62.6% |

wlog entries: 2560 over 20 runs; by reason: O 754, P 253, T 1553.
