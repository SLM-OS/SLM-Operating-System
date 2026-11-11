## Cell B-ncoff-instr-v1-kexec

Counted: `B-ncoff-instr-v1-kexec-run01.txt` .. `B-ncoff-instr-v1-kexec-run14.txt` (3 files); head 579b155a.
Left out (17 files):
- `B-ncoff-instr-v1-kexec-run02.txt`: cpu2 chk=-1; cpu3 chk=-1
- `B-ncoff-instr-v1-kexec-run03.txt`: cpu3 chk=-1; cpu5 chk=-1
- `B-ncoff-instr-v1-kexec-run05.txt`: cpu4 chk=-1
- `B-ncoff-instr-v1-kexec-run06.txt`: cpu1 chk=-1; cpu2 chk=-1
- `B-ncoff-instr-v1-kexec-run07.txt`: cpu5 chk=-1
- `B-ncoff-instr-v1-kexec-run08.txt`: cpu1 chk=-1; cpu3 chk=-1; cpu4 att 9216 != diag_att 9215
- `B-ncoff-instr-v1-kexec-run09.txt`: cpu5 chk=-1
- `B-ncoff-instr-v1-kexec-run10.txt`: cpu1 chk=-1; cpu4 chk=-1
- `B-ncoff-instr-v1-kexec-run11.txt`: cpu1 chk=-1
- `B-ncoff-instr-v1-kexec-run12.txt`: cpu3 chk=-1
- `B-ncoff-instr-v1-kexec-run13.txt`: cpu1 chk=-1; cpu5 chk=-1
- `B-ncoff-instr-v1-kexec-run15.txt`: cpu5 chk=-1
- `B-ncoff-instr-v1-kexec-run16.txt`: cpu1 chk=-1; cpu4 chk=-1
- `B-ncoff-instr-v1-kexec-run17.txt`: cpu3 chk=-1; cpu5 chk=-1
- `B-ncoff-instr-v1-kexec-run18.txt`: cpu1 chk=-1; cpu3 chk=-1
- `B-ncoff-instr-v1-kexec-run19.txt`: cpu1 chk=-1; cpu4 chk=-1
- `B-ncoff-instr-v1-kexec-run20.txt`: cpu2 chk=-1
Format v2; mode sustained, depth 16, work 8, sustain_ms 2500, cpus 6.

### T1 per CPU (`B-ncoff-instr-v1-kexec-run01.txt` .. `B-ncoff-instr-v1-kexec-run14.txt` (3 files))

| CPU | median share | pooled share | exec | att | ok | ok % att | iso % att | f_stale % att | f_pd % att | f_empty % att | f_other % att | first success median / max (ms) | runs with ok=0 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 16.4% | 16.4% | 514 | 3700 | 514 | 13.9% | 0.0% | 0.0% | 0.2% | 85.9% | 0.0% | 0.1 / 0.1 (3 runs) | 0 of 3 |
| 1 (owner) | 16.2% | 16.2% | 508 | 14228 | 0 | 0.0% | 0.0% | 0.0% | 0.3% | 99.6% | 0.0% | none | 3 of 3 |
| 2 | 16.8% | 16.8% | 527 | 42869 | 526 | 1.2% | 0.0% | 0.0% | 0.3% | 98.4% | 0.0% | 0.0 / 0.0 (3 runs) | 0 of 3 |
| 3 | 16.9% | 16.9% | 528 | 18758 | 527 | 2.8% | 0.0% | 0.0% | 0.4% | 96.8% | 0.0% | 0.0 / 0.0 (3 runs) | 0 of 3 |
| 4 | 16.7% | 16.8% | 525 | 40846 | 525 | 1.3% | 0.0% | 0.0% | 0.4% | 98.3% | 0.0% | 0.0 / 0.1 (3 runs) | 0 of 3 |
| 5 | 16.9% | 16.9% | 529 | 20819 | 529 | 2.5% | 0.0% | 0.0% | 0.4% | 97.0% | 0.0% | 0.0 / 0.1 (3 runs) | 0 of 3 |

Best thief by median share: CPU 3 (16.9%). Ratio of each thief's median share to it: CPU 0 0.973, CPU 2 0.995, CPU 4 0.990, CPU 5 1.000.
Runs with a non-owner CPU at ok=0: 0 of 3.

### T2 thieves against CPU 1's preempt_disabled window and tick (`B-ncoff-instr-v1-kexec-run01.txt` .. `B-ncoff-instr-v1-kexec-run14.txt` (3 files))

Inside window = attempts that started with preempt_disabled[w] set. Phase columns are % of the attempts that had a watched tick.

| thief | att | inside window | in T | in C | in P | in O | in B | in, no window | f_pd % att |
|---|---|---|---|---|---|---|---|---|---|
| 0 | 3700 | 0.1% (3) | 3 | 0 | 0 | 0 | 0 | 0 | 0.2% |
| 2 | 42869 | 0.0% (21) | 21 | 0 | 0 | 0 | 0 | 0 | 0.3% |
| 3 | 18758 | 0.1% (10) | 9 | 0 | 0 | 1 | 0 | 0 | 0.4% |
| 4 | 40846 | 0.1% (47) | 46 | 0 | 1 | 0 | 0 | 0 | 0.4% |
| 5 | 20819 | 0.1% (18) | 17 | 0 | 0 | 1 | 0 | 0 | 0.4% |

Offset into the open window (w_off), % of in-window attempts:

| thief | <1us | <2us | <4us | <8us | <16us | <32us | <64us | >=64us |
|---|---|---|---|---|---|---|---|---|
| 0 | 33.3% | 0.0% | 33.3% | 0.0% | 33.3% | 0.0% | 0.0% | 0.0% |
| 2 | 9.5% | 9.5% | 14.3% | 42.9% | 23.8% | 0.0% | 0.0% | 0.0% |
| 3 | 10.0% | 10.0% | 20.0% | 40.0% | 20.0% | 0.0% | 0.0% | 0.0% |
| 4 | 19.1% | 2.1% | 21.3% | 36.2% | 21.3% | 0.0% | 0.0% | 0.0% |
| 5 | 5.6% | 11.1% | 27.8% | 27.8% | 27.8% | 0.0% | 0.0% | 0.0% |

Uniform reference uses CPU 1's median tick period 10000.4 us (3 runs).

ph_w: attempt time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 3700 | 0.1% | 0.1% | 0.2% | 0.3% | 0.6% | 1.7% | 3.4% | 9.3% | 34.9% | 49.4% | 0.0% | 1.4% |
| 2 | 42867 | 0.1% | 0.1% | 0.2% | 0.4% | 0.9% | 3.3% | 7.6% | 16.7% | 38.3% | 32.5% | 0.0% | 1.7% |
| 3 | 18757 | 0.0% | 0.1% | 0.2% | 0.3% | 0.7% | 3.9% | 10.4% | 16.2% | 40.1% | 28.1% | 0.0% | 1.2% |
| 4 | 40846 | 0.1% | 0.1% | 0.4% | 0.6% | 1.3% | 3.8% | 6.1% | 14.2% | 27.9% | 45.4% | 0.0% | 2.6% |
| 5 | 20819 | 0.1% | 0.1% | 0.3% | 0.6% | 1.4% | 2.9% | 6.6% | 17.4% | 27.9% | 42.8% | 0.0% | 2.5% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_w_pd: attempts ending on the early-out, time after CPU 1's last tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 9 | 33.3% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 11.1% | 22.2% | 33.3% | 0.0% | 33.3% |
| 2 | 136 | 16.2% | 0.7% | 0.0% | 0.7% | 5.9% | 0.0% | 19.9% | 15.4% | 20.6% | 19.1% | 1.5% | 23.5% |
| 3 | 73 | 9.6% | 1.4% | 0.0% | 0.0% | 4.1% | 0.0% | 1.4% | 39.7% | 41.1% | 2.7% | 0.0% | 15.1% |
| 4 | 155 | 23.9% | 4.5% | 0.0% | 1.3% | 10.3% | 0.0% | 5.8% | 12.3% | 17.4% | 24.5% | 0.0% | 40.0% |
| 5 | 82 | 14.6% | 3.7% | 0.0% | 0.0% | 0.0% | 0.0% | 0.0% | 43.9% | 14.6% | 23.2% | 0.0% | 18.3% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

ph_own: attempt time after the thief's own tick, % per bin; last column = within 200 us

| thief | n | <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 3700 | 0.2% | 0.1% | 0.4% | 0.8% | 0.9% | 2.9% | 5.4% | 8.8% | 22.5% | 58.1% | 0.0% | 2.4% |
| 2 | 42866 | 0.9% | 0.1% | 0.4% | 0.7% | 1.4% | 3.8% | 5.0% | 7.5% | 20.1% | 60.1% | 0.0% | 3.5% |
| 3 | 18757 | 1.8% | 0.1% | 0.2% | 0.3% | 0.5% | 1.5% | 2.6% | 3.8% | 14.3% | 75.0% | 0.0% | 2.8% |
| 4 | 40846 | 0.9% | 0.1% | 0.4% | 0.6% | 1.2% | 3.6% | 6.7% | 13.3% | 18.1% | 55.0% | 0.0% | 3.2% |
| 5 | 20819 | 1.7% | 0.1% | 0.3% | 0.5% | 1.0% | 3.1% | 7.6% | 17.0% | 24.8% | 43.9% | 0.0% | 3.5% |
| uniform | - | 0.1% | 0.1% | 0.3% | 0.5% | 1.0% | 3.0% | 5.0% | 10.0% | 30.0% | 50.0% | 0.0% | 2.0% |

### T3 tick phase against CPU 1 (from tick_first_us) (`B-ncoff-instr-v1-kexec-run01.txt` .. `B-ncoff-instr-v1-kexec-run14.txt` (3 files))

Offset = (tick_first_us[c] - tick_first_us[w]) mod period, us.

Watched tick period median 10000.4 us over 3 runs.

| CPU | runs | offset median | min | max |
|---|---|---|---|---|
| 0 | 3 | 4637 | 2296 | 6637 |
| 1 | 3 | 0 | 0 | 0 |
| 2 | 3 | 3506 | 2844 | 4160 |
| 3 | 3 | 8633 | 5383 | 8976 |
| 4 | 3 | 1085 | 1076 | 2469 |
| 5 | 3 | 847 | 87 | 1076 |

### T4 CPU 1's windows (`B-ncoff-instr-v1-kexec-run01.txt` .. `B-ncoff-instr-v1-kexec-run14.txt` (3 files))

| reason | windows | total us | mean us | longest us | % of wall-clock |
|---|---|---|---|---|---|
| T | 758 | 7094 | 9.4 | 12 | 0.094% |
| C | 0 | 0 | 0.0 | 0 | 0.000% |
| P | 131 | 221 | 1.7 | 3 | 0.003% |
| O | 388 | 715 | 1.8 | 3 | 0.009% |
| B | 0 | 0 | 0.0 | 0 | 0.000% |
| all | 1277 | 8030 | 6.3 | 12 | 0.106% |

Window entry after CPU 1's own tick (wph), % of 1277 windows:

| <10us | <20us | <50us | <100us | <200us | <500us | <1ms | <2ms | <5ms | <10ms | >=10ms | <200us |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 59.4% | 0.0% | 0.1% | 0.0% | 0.9% | 1.1% | 0.7% | 3.1% | 20.2% | 14.6% | 0.0% | 60.4% |

wlog entries: 384 over 3 runs; by reason: O 117, P 39, T 228.
