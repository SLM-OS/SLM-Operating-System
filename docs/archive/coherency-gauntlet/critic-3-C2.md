# Critic verdict, coherency run 3, component C2 (#1216, #1203)

Critic: `coherency-3-C2-critic`. Brief received about 09:31 UTC. Sections 1
to 8 (own count, reproduction, classifications, failure modes) were written
and saved at 10:06 UTC (draft sha256 7f2d39d5...) before any builder
analysis file or note was opened; sections 9 and 10 were added after, and
sections 1 to 8 are unchanged from that draft apart from this paragraph.

Evidence read: `origin/gauntlet-3-hw-jetson` @ 68f5690e,
`docs/archive/coherency-gauntlet/run3-hw/c2/` (excluding `analysis/`), and
the critic's own reproduction pushed to the same branch @ 4e07dc57 under
`run3-hw/critic-c2/`. Image hashes checked against
`run3-tools/c0/cells-MANIFEST.txt` on `origin/gauntlet/coherency-3`. The
critic's parser is `run3-hw/critic-c2/tools/c2parse.py` (with `summ.py`,
`firstok.py`, `order2.py`); the builder's `run3-tools/c2/` was not run.
All paths below are under `docs/archive/coherency-gauntlet/run3-hw/c2/`
unless they start with `critic-c2/`.

## 1. Completeness (definition of done row 1)

Counted files, not table rows. Every file was parsed for: the
`cat /sys/version` reply before the bench, `commit:` equal to the cell
head, a `c2t-format v2` header, exactly six `c2t cpu=` rows, every one of
the 48 fields on every row, the right bin count for every vector field,
`Completed: N / N`, sum of `exec` equal to `tasks`, each CPU's `exec`
equal to its line in the per-CPU distribution, and the printed `chk`
equal to `att - (ok+iso+f_stale+f_pd+f_empty+f_other)` recomputed. The
in-kernel `run=` counter was checked against the file number, and the
labctl call times and task ids for monotonic order inside each cell.

| Cell | Head (banner in every file) | Files | Problems found | `run=` equals file number | Boot log |
|---|---|---|---|---|---|
| C | 1b27eea4 | `C-nc-instr-kexec-run{01..20}.txt`, 20 | none | 20/20 | `C-nc-instr-kexec-boot.txt`, banner 1b27eea4, ends at `slmos>` |
| D | 8fc7c88a | `D-removal-instr-uefi-run{01..20}.txt`, 20 | none | 20/20 | `D-removal-instr-uefi-boot.txt`, banner 8fc7c88a, ends at `slmos>` |
| B | 579b155a | `B-ncoff-instr-v1-kexec-run{01..20}.txt`, 20 | none | 20/20 | `B-ncoff-instr-v1-kexec-boot.txt`, banner 579b155a |
| A | 6608cbd0 | `A-ncoff-instr-kexec-run{01..20}.txt`, 20 | none | 20/20 | `A-ncoff-instr-kexec-boot.txt`, banner 6608cbd0 |
| E | 8fc7c88a | `E-removal-instr-kexec-run{01..20}.txt`, 20 | none | 20/20 | `E-removal-instr-kexec-boot.txt`, banner 8fc7c88a |
| F | 1b27eea4 | `F-nc-instr-uefi-run{01..20}.txt`, 20 | none | 20/20 | `F-nc-instr-uefi-boot.txt`, banner 1b27eea4, ends at `slmos>` |
| G | 0dfaae0c | none | not run | - | - |

All 120 files are distinct (120 distinct md5 sums). Each cell is one boot:
task ids rise across the 20 files of a cell without overlap.

Image hashes of the cell images in the scratchpad match the manifest: C
elf 77803fb2, B elf d325404d, A elf 48eb2cc2, E elf d740b83f, D bin
061645b3, F bin 405c7b43, step 0 elf 556bb724 and bin f5df040b. The
staging and trigger logs (`stage-log.txt`, `trigger-log.txt`,
`uefi-deploy-log.txt`) name the same hashes for every cell that was run.

Branch contents relevant to the comparisons: B (579b155a) is A (6608cbd0)
plus one commit, `V1 (#1216 experiment): idle WFE so the push SEV wakes
idle thieves`, a 6/2-line change to `kernel/sched/sched.c` that replaces
`wfi` with `wfe` in the SECONDARY_PREEMPT idle wait. B against A is
therefore a one-variable comparison. A and C do not share a base (143
files differ between 6608cbd0 and 1b27eea4), so A against C is not an
NC-only comparison; no definition-of-done row depends on A against C.

### Off-by-one `chk` and `att` against `diag_att`

Nonzero `chk` rows, all of magnitude 1: C 12 rows in 10 files, D 1, B 41
rows in 19 files (plus one `att - diag_att = +1`, B run08 CPU 4), A 1, E
0, F 6. No row anywhere is off by more than 1. Source on 1b27eea4:
`sched_try_steal()` increments `sched_diag_steal_attempts[cpu]` and then
`tr->att` at entry and the outcome counter at exit
(`kernel/sched/sched.c`, `steal_trace_note`), and `s3_take_snap()`
(`kernel/src/shell_sys.c`) copies the per-CPU trace structs with plain
loads while idle CPUs keep calling `sched_try_steal()`. A snapshot taken
while a thief is between entry and exit gives +1 in that snapshot; when it
is the pre-run snapshot the difference shows -1. B has the most rows
because its thieves call `sched_try_steal()` 20 to 50 times more often
than in A or C (pooled `att` in the tables below), so a snapshot lands mid-attempt more often. The one
`att - diag_att` mismatch has the same cause (`diag_att` is read before
the struct copy). These are snapshot artifacts of non-atomic per-CPU
counters, not incomplete captures, and every file counts. The same rule
is applied to every cell; no file is dropped in any cell.

## 2. Own tables per cell (definition of done row 2)

Share is `exec` over the run's `tasks` (equal to the sum of `exec` in
every file), median over the cell's files, with min and max. `att`, `ok`
are sums over the files; failure fractions are pooled sums over pooled
`att`. First-ok median and max are over the files where that CPU had at
least one success (see section 4 for the other conventions). "Attempts in
CPU 1 window" is `w_in` by reason over `att`; reasons C and B are 0 in
every file of every cell and are not shown. CPU 1 is the owner (it pushes
all tasks) and is listed in every table.

### Cell C: `C-nc-instr-kexec-run{01..20}.txt`, 20 files, banner 1b27eea4 in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 17.88 [16.58, 21.11] | 40467 | 3435 | 0.0 | 0.2 | 91.3 | 0.0 | 0.0 | 131 us / 146 us | 0 | 0.08 / 0.00 / 0.02 |
| 1 (owner) | 17.38 [16.46, 19.54] | 219 | 0 | 0.0 | 41.6 | 55.3 | 0.0 | 0.0 | - | all 20 | - |
| 2 | 13.02 [6.78, 16.98] | 8850 | 2371 | 0.2 | 70.8 | 2.2 | 0.0 | 0.0 | 5.5 ms / 9.4 ms | 0 | 43.64 / 0.00 / 0.01 |
| 3 | 15.16 [8.62, 18.51] | 8255 | 2779 | 0.4 | 63.2 | 2.7 | 0.0 | 0.0 | 5.5 ms / 9.4 ms | 0 | 38.84 / 0.02 / 0.04 |
| 4 | 18.17 [17.14, 20.99] | 3826 | 3507 | 0.4 | 3.8 | 4.1 | 0.0 | 0.0 | 4.6 ms / 9.8 ms | 0 | 0.10 / 0.00 / 0.00 |
| 5 | 18.27 [17.20, 21.35] | 3883 | 3510 | 0.8 | 4.1 | 4.7 | 0.0 | 0.0 | 4.6 ms / 9.8 ms | 0 | 0.05 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 32.2 (87) | 0.1 | 9.3, 20.9, 25.6, 23.3, 20.9 |
| 2 | 70.8 | 96.6 (6267) | 70.1 | 2.8, 26.5, 37.5, 32.0, 1.2 |
| 3 | 66.5 | 98.2 (5215) | 68.1 | 4.0, 30.6, 35.5, 28.8, 1.0 |
| 4 | 0.1 | 1.4 (146) | 57.6 | 0.0, 0.0, 0.0, 75.0, 25.0 |
| 5 | 0.1 | 1.2 (160) | 57.2 | 50.0, 0.0, 0.0, 50.0, 0.0 |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 5064/0/915/2426/0; win_sum_us = 54123/0/2643/6881/0; win_max_us = 18/0/3/3/0; wph % = [60.3, 0.0, 0.0, 0.0, 0.0, 1.2, 0.4, 1.7, 21.4, 14.9, 0.0] (n=8408); wlog first-128 durations median/max ns: T 10560.0/15616, P 2880/3584, O 2848/3776.

### Cell D: `D-removal-instr-uefi-run{01..20}.txt`, 20 files, banner 8fc7c88a in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 17.56 [16.98, 18.15] | 364084 | 4338 | 0.0 | 0.0 | 98.8 | 0.0 | 0.0 | 14 us / 24 us | 0 | 0.00 / 0.00 / 0.00 |
| 1 (owner) | 16.67 [15.67, 17.56] | 1254 | 0 | 0.2 | 5.7 | 94.1 | 0.0 | 0.0 | - | all 20 | - |
| 2 | 16.77 [15.38, 17.53] | 6544 | 4073 | 1.3 | 11.5 | 24.9 | 0.0 | 0.0 | 4.4 ms / 9.8 ms | 0 | 6.02 / 0.00 / 0.00 |
| 3 | 16.43 [14.90, 17.34] | 6764 | 4019 | 0.9 | 15.3 | 24.4 | 0.0 | 0.0 | 5.5 ms / 9.8 ms | 0 | 7.97 / 0.00 / 0.01 |
| 4 | 16.55 [15.95, 17.65] | 5983 | 4094 | 1.1 | 5.8 | 24.7 | 0.0 | 0.0 | 5.4 ms / 9.9 ms | 0 | 0.00 / 0.00 / 0.00 |
| 5 | 16.34 [15.28, 17.15] | 6155 | 4011 | 0.5 | 9.3 | 25.0 | 0.0 | 0.0 | 5.4 ms / 9.9 ms | 0 | 0.00 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 19.3 (83) | 0.1 | 78.6, 21.4, 0.0, 0.0, 0.0 |
| 2 | 15.5 | 93.8 (755) | 54.9 | 96.7, 3.3, 0.0, 0.0, 0.0 |
| 3 | 21.4 | 95.2 (1032) | 55.8 | 96.7, 3.3, 0.0, 0.0, 0.0 |
| 4 | 0.0 | 0.0 (345) | 53.5 | - |
| 5 | 0.0 | 0.0 (574) | 53.7 | - |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 5049/0/594/3510/0; win_sum_us = 4007/0/113/616/0; win_max_us = 3/0/0/0/0; wph % = [55.2, 0.0, 1.1, 0.1, 0.0, 0.0, 0.0, 15.8, 17.4, 10.4, 0.0] (n=9153); wlog first-128 durations median/max ns: T 736.0/3488, P 192/352, O 160/384.

### Cell E: `E-removal-instr-kexec-run{01..20}.txt`, 20 files, banner 8fc7c88a in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 16.62 [16.28, 17.27] | 600231 | 3432 | 0.0 | 0.0 | 99.4 | 0.0 | 0.0 | 18 us / 28 us | 0 | 0.01 / 0.00 / 0.00 |
| 1 (owner) | 16.26 [15.79, 16.57] | 332 | 1 | 0.3 | 3.0 | 96.4 | 0.0 | 0.0 | 74.0 ms / 74.0 ms | 01,02,03,04,05,06,07,08,09,10,12,13,14,15,16,17,18,19,20 | - |
| 2 | 16.68 [16.16, 17.13] | 4564 | 3425 | 0.7 | 8.2 | 16.1 | 0.0 | 0.0 | 3.5 ms / 9.6 ms | 0 | 6.97 / 0.00 / 0.00 |
| 3 | 16.85 [16.13, 17.25] | 4434 | 3467 | 2.3 | 6.4 | 13.1 | 0.0 | 0.0 | 5.0 ms / 9.5 ms | 0 | 3.54 / 0.00 / 0.00 |
| 4 | 16.85 [16.39, 17.09] | 4159 | 3459 | 1.1 | 5.1 | 10.6 | 0.0 | 0.0 | 7.4 ms / 9.8 ms | 0 | 0.00 / 0.00 / 0.00 |
| 5 | 16.77 [16.10, 17.21] | 4253 | 3448 | 0.9 | 7.6 | 10.5 | 0.0 | 0.0 | 7.4 ms / 9.8 ms | 0 | 0.00 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 27.8 (151) | 0.1 | 65.7, 25.7, 8.6, 0.0, 0.0 |
| 2 | 12.5 | 98.7 (376) | 58.7 | 97.2, 2.5, 0.3, 0.0, 0.0 |
| 3 | 8.3 | 95.8 (284) | 59.0 | 96.2, 3.8, 0.0, 0.0, 0.0 |
| 4 | 0.2 | 0.5 (214) | 58.0 | - |
| 5 | 0.1 | 0.0 (322) | 58.4 | - |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 5059/0/793/2551/0; win_sum_us = 5306/0/175/608/0; win_max_us = 3/0/0/0/0; wph % = [60.2, 0.0, 1.5, 0.8, 0.1, 0.0, 0.0, 2.8, 20.4, 14.2, 0.0] (n=8404); wlog first-128 durations median/max ns: T 928/2880, P 224/480, O 224.0/512.

### Cell B: `B-ncoff-instr-v1-kexec-run{01..20}.txt`, 20 files, banner 579b155a in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 16.40 [16.11, 16.70] | 27053 | 3415 | 0.0 | 0.3 | 87.1 | 0.0 | 0.0 | 90 us / 131 us | 0 | 0.08 / 0.00 / 0.00 |
| 1 (owner) | 16.21 [15.32, 16.54] | 90674 | 0 | 0.0 | 0.3 | 99.7 | 0.0 | 0.0 | - | all 20 | - |
| 2 | 16.83 [16.73, 17.07] | 299593 | 3504 | 0.0 | 0.3 | 98.5 | 0.0 | 0.0 | 19 us / 48 us | 0 | 0.06 / 0.00 / 0.00 |
| 3 | 16.86 [16.68, 17.00] | 183404 | 3506 | 0.0 | 0.3 | 97.7 | 0.0 | 0.0 | 29 us / 1.6 ms | 0 | 0.07 / 0.00 / 0.00 |
| 4 | 16.84 [16.70, 17.08] | 198306 | 3505 | 0.0 | 0.4 | 97.8 | 0.0 | 0.0 | 31 us / 1.8 ms | 0 | 0.14 / 0.00 / 0.00 |
| 5 | 16.87 [16.70, 17.34] | 193936 | 3523 | 0.0 | 0.4 | 97.8 | 0.0 | 0.0 | 26 us / 64 us | 0 | 0.09 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 24.7 (81) | 0.1 | 4.5, 13.6, 27.3, 31.8, 22.7 |
| 2 | 0.1 | 18.6 (872) | 0.9 | 14.0, 12.4, 22.0, 38.2, 13.4 |
| 3 | 0.1 | 16.8 (637) | 1.3 | 10.8, 13.1, 20.0, 36.9, 18.5 |
| 4 | 0.1 | 26.8 (844) | 1.3 | 18.1, 10.0, 18.1, 34.7, 19.2 |
| 5 | 0.1 | 18.9 (715) | 1.2 | 8.7, 10.4, 17.9, 39.9, 23.1 |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 5052/0/876/2555/0; win_sum_us = 48156/0/1811/5515/0; win_max_us = 18/0/4/3/0; wph % = [59.6, 0.0, 0.0, 0.0, 0.8, 0.9, 0.6, 2.6, 21.1, 14.4, 0.0] (n=8483); wlog first-128 durations median/max ns: T 9408.0/12704, P 2240/3360, O 2272/3232.

Full phase histograms, % of attempts, bins <10us,<20,<50,<100,<200,<500us,<1ms,<2,<5,<10,>=10ms:

| row | <10us | <20 | <50 | <100 | <200 | <500us | <1ms | <2 | <5 | <10 | >=10ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| uniform reference | 0.1 | 0.1 | 0.3 | 0.5 | 1 | 3 | 5 | 10 | 30 | 50 | 0 |
| ph_w cpu0 (n=27053) | 0.1 | 0.1 | 0.3 | 0.4 | 0.8 | 2.5 | 4.3 | 9.1 | 31.2 | 51.2 | 0.0 |
| ph_w cpu2 (n=299585) | 0.1 | 0.1 | 0.2 | 0.4 | 0.9 | 2.9 | 5.9 | 15.4 | 46.1 | 28.0 | 0.0 |
| ph_w cpu3 (n=183394) | 0.1 | 0.1 | 0.2 | 0.4 | 0.8 | 2.6 | 5.2 | 10.1 | 35.9 | 44.8 | 0.0 |
| ph_w cpu4 (n=198304) | 0.1 | 0.1 | 0.4 | 0.7 | 1.4 | 4.0 | 6.6 | 14.4 | 28.9 | 43.3 | 0.0 |
| ph_w cpu5 (n=193936) | 0.1 | 0.1 | 0.3 | 0.5 | 1.0 | 3.1 | 5.7 | 12.0 | 26.4 | 50.7 | 0.0 |
| ph_w_pd cpu0 (n=81) | 24.7 | 2.5 | 0.0 | 0.0 | 0.0 | 0.0 | 4.9 | 11.1 | 29.6 | 27.2 | 0.0 |
| ph_w_pd cpu2 (n=872) | 18.6 | 1.8 | 0.3 | 1.0 | 1.0 | 1.8 | 8.1 | 10.3 | 35.6 | 20.9 | 0.5 |
| ph_w_pd cpu3 (n=637) | 16.8 | 1.9 | 0.0 | 1.6 | 0.8 | 2.2 | 4.7 | 12.9 | 35.5 | 23.5 | 0.2 |
| ph_w_pd cpu4 (n=844) | 26.8 | 3.9 | 0.4 | 1.8 | 2.5 | 0.8 | 5.2 | 7.1 | 26.1 | 25.4 | 0.1 |
| ph_w_pd cpu5 (n=715) | 18.9 | 4.3 | 0.6 | 0.4 | 0.1 | 1.4 | 1.0 | 9.2 | 34.5 | 29.5 | 0.0 |
| ph_own cpu0 (n=27053) | 0.1 | 0.1 | 0.3 | 0.7 | 0.9 | 2.5 | 4.5 | 8.3 | 28.9 | 53.8 | 0.0 |
| ph_own cpu2 (n=299584) | 0.9 | 0.2 | 0.5 | 0.8 | 1.5 | 4.2 | 5.6 | 7.4 | 15.3 | 63.6 | 0.0 |
| ph_own cpu3 (n=183394) | 1.3 | 0.1 | 0.3 | 0.6 | 1.1 | 3.2 | 4.5 | 8.1 | 19.6 | 61.2 | 0.0 |
| ph_own cpu4 (n=198304) | 1.3 | 0.1 | 0.4 | 0.6 | 1.2 | 3.3 | 5.4 | 9.9 | 19.3 | 58.6 | 0.0 |
| ph_own cpu5 (n=193934) | 1.2 | 0.1 | 0.4 | 0.6 | 1.2 | 3.3 | 5.3 | 9.5 | 18.0 | 60.4 | 0.0 |
| wph cpu1 (n=8483) | 59.6 | 0.0 | 0.0 | 0.0 | 0.8 | 0.9 | 0.6 | 2.6 | 21.1 | 14.4 | 0.0 |

### Cell A: `A-ncoff-instr-kexec-run{01..20}.txt`, 20 files, banner 6608cbd0 in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 18.47 [17.10, 25.35] | 55878 | 3483 | 0.0 | 0.1 | 93.6 | 0.0 | 0.0 | 75 us / 96 us | 0 | 0.07 / 0.00 / 0.00 |
| 1 (owner) | 17.65 [16.10, 21.58] | 329 | 0 | 0.0 | 42.9 | 56.8 | 0.0 | 0.0 | - | all 20 | - |
| 2 | 13.82 [2.38, 18.05] | 7797 | 2310 | 0.3 | 65.9 | 4.2 | 0.0 | 0.0 | 204.5 ms / 1470.1 ms | 0 | 53.25 / 0.00 / 0.00 |
| 3 | 14.17 [0.00, 18.07] | 7303 | 2079 | 0.3 | 67.2 | 4.1 | 0.0 | 0.0 | 167.3 ms / 1332.1 ms | 06,15,20 | 57.05 / 0.00 / 0.00 |
| 4 | 18.30 [16.91, 25.07] | 4068 | 3480 | 1.2 | 4.9 | 8.4 | 0.0 | 0.0 | 5.5 ms / 9.8 ms | 0 | 0.05 / 0.00 / 0.00 |
| 5 | 18.35 [17.10, 25.07] | 4059 | 3483 | 0.8 | 5.7 | 7.7 | 0.0 | 0.0 | 5.5 ms / 9.8 ms | 0 | 0.07 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 62.3 (77) | 0.1 | 11.6, 18.6, 27.9, 32.6, 9.3 |
| 2 | 69.9 | 98.5 (5139) | 75.5 | 46.1, 9.0, 36.7, 8.1, 0.0 |
| 3 | 68.4 | 99.0 (4904) | 81.2 | 31.4, 4.8, 39.7, 24.1, 0.0 |
| 4 | 0.1 | 1.0 (201) | 58.1 | 0.0, 50.0, 0.0, 0.0, 50.0 |
| 5 | 0.1 | 1.3 (230) | 57.4 | 33.3, 0.0, 0.0, 33.3, 33.3 |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 5075/0/843/2455/0; win_sum_us = 48031/0/1564/4637/0; win_max_us = 16/0/3/3/0; wph % = [60.7, 0.1, 0.0, 0.0, 0.8, 0.7, 0.2, 2.0, 21.1, 14.5, 0.0] (n=8373); wlog first-128 durations median/max ns: T 9440.0/16000, P 2208/3232, O 2208/3360.

Full phase histograms, % of attempts, bins <10us,<20,<50,<100,<200,<500us,<1ms,<2,<5,<10,>=10ms:

| row | <10us | <20 | <50 | <100 | <200 | <500us | <1ms | <2 | <5 | <10 | >=10ms |
|---|---|---|---|---|---|---|---|---|---|---|---|
| uniform reference | 0.1 | 0.1 | 0.3 | 0.5 | 1 | 3 | 5 | 10 | 30 | 50 | 0 |
| ph_w cpu0 (n=55878) | 0.1 | 0.1 | 0.3 | 0.5 | 1.0 | 3.0 | 5.0 | 9.8 | 29.4 | 50.7 | 0.0 |
| ph_w cpu2 (n=7797) | 69.9 | 0.0 | 0.1 | 0.0 | 0.5 | 0.7 | 0.7 | 2.0 | 7.7 | 14.4 | 3.9 |
| ph_w cpu3 (n=7303) | 68.4 | 0.0 | 0.0 | 0.0 | 0.3 | 0.5 | 0.2 | 1.2 | 5.6 | 22.3 | 1.4 |
| ph_w cpu4 (n=4068) | 0.1 | 0.0 | 0.2 | 0.4 | 0.5 | 1.9 | 7.5 | 10.5 | 30.4 | 48.4 | 0.0 |
| ph_w cpu5 (n=4059) | 0.1 | 0.1 | 0.1 | 0.5 | 0.5 | 2.0 | 6.6 | 11.0 | 31.5 | 47.6 | 0.0 |
| ph_w_pd cpu0 (n=77) | 62.3 | 1.3 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 2.6 | 6.5 | 27.3 | 0.0 |
| ph_w_pd cpu2 (n=5139) | 98.5 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.4 | 1.0 |
| ph_w_pd cpu3 (n=4904) | 99.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.6 | 0.4 |
| ph_w_pd cpu4 (n=201) | 1.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 19.4 | 11.9 | 28.9 | 38.8 | 0.0 |
| ph_w_pd cpu5 (n=230) | 1.3 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 9.1 | 10.0 | 29.1 | 50.4 | 0.0 |
| ph_own cpu0 (n=55878) | 0.1 | 0.1 | 0.3 | 0.6 | 0.9 | 2.7 | 4.6 | 9.0 | 30.6 | 51.1 | 0.0 |
| ph_own cpu2 (n=7797) | 75.5 | 0.1 | 0.1 | 0.0 | 0.5 | 0.7 | 0.7 | 2.0 | 7.7 | 12.8 | 0.0 |
| ph_own cpu3 (n=7303) | 81.2 | 0.1 | 0.0 | 0.0 | 0.3 | 0.4 | 0.2 | 1.2 | 5.6 | 10.9 | 0.0 |
| ph_own cpu4 (n=4068) | 58.1 | 0.0 | 0.0 | 0.0 | 0.1 | 0.2 | 0.1 | 0.5 | 8.8 | 32.2 | 0.0 |
| ph_own cpu5 (n=4059) | 57.4 | 0.1 | 0.0 | 0.0 | 0.2 | 0.3 | 0.2 | 0.8 | 8.1 | 32.8 | 0.0 |
| wph cpu1 (n=8373) | 60.7 | 0.1 | 0.0 | 0.0 | 0.8 | 0.7 | 0.2 | 2.0 | 21.1 | 14.5 | 0.0 |

### Cell F: `F-nc-instr-uefi-run{01..20}.txt`, 20 files, banner 1b27eea4 in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 20.38 [18.98, 22.21] | 27080 | 4375 | 0.0 | 0.2 | 83.6 | 0.0 | 0.0 | 100 us / 110 us | 0 | 0.05 / 0.00 / 0.04 |
| 1 (owner) | 19.70 [18.33, 21.21] | 728 | 0 | 0.1 | 21.8 | 77.7 | 0.0 | 0.0 | - | all 20 | - |
| 2 | 8.08 [6.99, 12.21] | 9951 | 1795 | 0.5 | 75.5 | 6.0 | 0.0 | 0.0 | 6.2 ms / 10.0 ms | 0 | 43.72 / 0.00 / 0.04 |
| 3 | 11.33 [5.53, 13.19] | 9897 | 2311 | 0.9 | 68.7 | 7.0 | 0.0 | 0.0 | 6.2 ms / 19.1 ms | 0 | 41.04 / 0.01 / 0.04 |
| 4 | 20.12 [19.07, 22.01] | 5169 | 4349 | 0.5 | 3.6 | 11.8 | 0.0 | 0.0 | 6.1 ms / 9.9 ms | 0 | 0.00 / 0.02 / 0.00 |
| 5 | 20.27 [18.72, 22.01] | 5164 | 4349 | 0.6 | 4.5 | 10.6 | 0.0 | 0.0 | 6.1 ms / 9.9 ms | 0 | 0.00 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 28.6 (49) | 0.1 | 25.0, 33.3, 16.7, 16.7, 8.3 |
| 2 | 79.8 | 99.5 (7517) | 79.8 | 16.7, 18.9, 57.6, 6.9, 0.0 |
| 3 | 74.3 | 99.5 (6797) | 74.3 | 22.0, 22.8, 45.1, 10.1, 0.0 |
| 4 | 0.0 | 1.1 (184) | 52.4 | 0.0, 100.0, 0.0, 0.0, 0.0 |
| 5 | 0.0 | 0.0 (232) | 52.5 | - |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 5044/0/545/3664/0; win_sum_us = 37803/0/1138/7668/0; win_max_us = 13/0/3/3/0; wph % = [54.6, 0.0, 0.0, 0.0, 0.7, 0.2, 0.1, 17.5, 14.9, 11.9, 0.0] (n=9253); wlog first-128 durations median/max ns: T 7456/11776, P 2048.0/2944, O 2080/3392.

### Cell C reproduction: `critic-c2/C-repro-run{01..05}.txt`, 5 files, banner 1b27eea4 in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 21.00 [19.03, 22.51] | 11889 | 877 | 0.0 | 0.2 | 92.4 | 0.0 | 0.0 | 133 us / 158 us | 0 | 0.08 / 0.00 / 0.01 |
| 1 (owner) | 19.39 [18.15, 21.60] | 70 | 0 | 0.0 | 50.0 | 48.6 | 0.0 | 0.0 | - | all 5 | - |
| 2 | 7.26 [3.82, 10.35] | 2409 | 317 | 0.3 | 84.4 | 2.2 | 0.0 | 0.0 | 5.9 ms / 9.9 ms | 0 | 48.78 / 0.00 / 0.00 |
| 3 | 12.89 [8.96, 15.53] | 2282 | 544 | 0.4 | 73.5 | 2.2 | 0.0 | 0.0 | 5.9 ms / 9.9 ms | 0 | 50.44 / 0.00 / 0.04 |
| 4 | 19.33 [11.13, 20.98] | 1382 | 778 | 0.4 | 38.0 | 5.3 | 0.0 | 0.0 | 5.6 ms / 9.6 ms | 0 | 35.31 / 0.00 / 0.00 |
| 5 | 21.12 [19.03, 22.38] | 975 | 871 | 0.7 | 3.1 | 6.8 | 0.0 | 0.0 | 5.6 ms / 9.6 ms | 0 | 0.00 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 32.0 (25) | 0.1 | 0.0, 20.0, 20.0, 50.0, 10.0 |
| 2 | 84.5 | 99.0 (2032) | 84.5 | 3.1, 22.6, 29.5, 44.4, 0.3 |
| 3 | 74.8 | 98.4 (1678) | 75.5 | 5.7, 36.4, 24.0, 32.8, 1.1 |
| 4 | 29.3 | 75.2 (525) | 64.2 | 3.1, 18.0, 28.5, 35.7, 14.8 |
| 5 | 0.0 | 0.0 (30) | 58.1 | - |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 1269/0/169/663/0; win_sum_us = 13645/0/512/1974/0; win_max_us = 18/0/3/3/0; wph % = [60.5, 0.1, 0.0, 0.0, 0.1, 0.9, 0.4, 1.8, 21.9, 14.3, 0.0] (n=2102); wlog first-128 durations median/max ns: T 10624/15296, P 3136.0/3744, O 2976/3872.

### Cell B reproduction: `critic-c2/B-repro-run{01..05}.txt`, 5 files, banner 579b155a in every file

| CPU | share median [min, max] % | att (sum) | ok (sum) | stale % | pd % | empty % | other % | iso % | first ok median / max | runs with 0 ok | attempts in CPU 1 window % (T / P / O) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 16.57 [16.44, 16.68] | 7477 | 860 | 0.0 | 0.3 | 88.2 | 0.0 | 0.0 | 92 us / 107 us | 0 | 0.09 / 0.00 / 0.00 |
| 1 (owner) | 16.30 [15.71, 16.57] | 52210 | 0 | 0.0 | 0.4 | 99.6 | 0.0 | 0.0 | - | all 5 | - |
| 2 | 16.78 [16.76, 16.95] | 75204 | 873 | 0.0 | 0.4 | 98.4 | 0.0 | 0.0 | 12 us / 34 us | 0 | 0.09 / 0.00 / 0.00 |
| 3 | 16.83 [16.67, 16.87] | 55741 | 867 | 0.0 | 0.8 | 97.6 | 0.0 | 0.0 | 36 us / 39 us | 0 | 0.50 / 0.00 / 0.00 |
| 4 | 16.78 [16.67, 17.05] | 58677 | 873 | 0.0 | 0.4 | 98.1 | 0.0 | 0.0 | 27 us / 38 us | 0 | 0.08 / 0.00 / 0.00 |
| 5 | 16.76 [16.68, 16.83] | 64686 | 871 | 0.0 | 0.3 | 98.3 | 0.0 | 0.0 | 31 us / 6.1 ms | 0 | 0.09 / 0.00 / 0.00 |

Share of each thief's histogram in the first bin (<10 us after the tick); uniform phase over a 10 ms tick puts 0.1 % there:

| CPU | ph_w <10 us % | ph_w_pd <10 us % (n) | ph_own <10 us % | w_off % (<1, <2, <4, <8, <16 us into the window) |
|---|---|---|---|---|
| 0 | 0.1 | 30.4 (23) | 0.1 | 14.3, 14.3, 28.6, 28.6, 14.3 |
| 2 | 0.1 | 22.4 (299) | 0.9 | 15.5, 12.7, 18.3, 38.0, 15.5 |
| 3 | 0.5 | 61.8 (445) | 1.3 | 38.7, 52.0, 2.5, 4.7, 2.2 |
| 4 | 0.1 | 20.1 (229) | 1.1 | 14.3, 12.2, 20.4, 38.8, 14.3 |
| 5 | 0.1 | 24.3 (226) | 0.9 | 14.0, 17.5, 15.8, 38.6, 14.0 |

CPU 1 windows (sum over files): win_n T/C/P/O/B = 1263/0/235/647/0; win_sum_us = 11750/0/560/1538/0; win_max_us = 13/0/3/3/0; wph % = [58.9, 0.1, 0.0, 0.1, 0.8, 0.6, 0.5, 2.9, 21.3, 14.8, 0.0] (n=2145); wlog first-128 durations median/max ns: T 9312/11680, P 2272/3424, O 2352.0/3264.


## 3. Question 1 (definition of done row 3)

Bar: "pre-existing" if each of CPU 2 and CPU 3 has a median share at or
below 60% of the best thief's median share, or any run has a non-owner CPU
at 0 successes; otherwise "exposed by the removal". Thieves are CPUs 0,
2, 3, 4, 5 (CPU 1 is the owner).

| Cell | Best thief (median share) | CPU 2 median, ratio | CPU 3 median, ratio | Non-owner runs with 0 ok | Classification |
|---|---|---|---|---|---|
| C | CPU 5, 18.27% | 13.02%, 0.713 | 15.16%, 0.830 | none | exposed by the removal |
| D | CPU 0, 17.56% | 16.77%, 0.955 | 16.43%, 0.935 | none | exposed by the removal |
| E | CPU 3, 16.85% | 16.68%, 0.990 | 16.85%, 1.000 | none | exposed by the removal |
| F (reference) | CPU 0, 20.38% | 8.08%, 0.397 | 11.33%, 0.556 | none | pre-existing |
| A (reference) | CPU 0, 18.47% | 13.82%, 0.748 | 14.17%, 0.767 | CPU 3 in runs 06, 15, 20 | pre-existing |

- C: "exposed by the removal", deciding numbers CPU 2 at 0.713 and CPU 3
  at 0.830 of CPU 5's median, with no non-owner 0-success run, files
  `C-nc-instr-kexec-run{01..20}.txt`. C is the NC (pre-removal) kernel;
  the label is the bar's output as written, not a statement that the
  removal caused anything in C.
- D: "exposed by the removal", CPU 2 at 0.955 and CPU 3 at 0.935 of CPU
  0's median, no non-owner 0-success run,
  `D-removal-instr-uefi-run{01..20}.txt`.
- E: "exposed by the removal", CPU 2 at 0.990 and CPU 3 at 1.000 (CPU 3
  is itself the best thief), no non-owner 0-success run,
  `E-removal-instr-kexec-run{01..20}.txt`. The one owner success (CPU 1,
  E run 11, first ok 74.0 ms) is not a non-owner CPU.

The classification of C, D and E does not change if CPU 0 is left out of
"best thief" or if CPUs 2 and 3 are left out of it (C: best CPU 5 either
way; D: best becomes CPU 4 at 16.55%, ratios 1.01 and 0.99; E: best
becomes CPU 4 at 16.85%).

The stall signature is present in C without meeting the bar: CPU 2 and 3
end 70.8% and 63.2% of their attempts on the preempt_disabled early-out,
43.6% and 38.8% of their attempts start inside CPU 1's tick window, and
96.6% and 98.2% of their preempt_disabled hits fall less than 10 us after
CPU 1's tick, against 3.8% and 4.1% early-outs and 1.4% and 1.2% in the
first bin for CPUs 4 and 5 (`C-nc-instr-kexec-run{01..20}.txt`). The
share clause asks for 60% and C's medians are 71% and 83%; that is a
property of the bar, recorded here and not reinterpreted.

## 4. Question 2, B against A (definition of done row 4)

H3 is shown only if all four clauses hold.

| Clause | A (`A-ncoff-instr-kexec-run{01..20}.txt`) | B (`B-ncoff-instr-v1-kexec-run{01..20}.txt`) | Holds |
|---|---|---|---|
| 1. In B, CPUs 2 and 3 reach CPU 4 and 5's share within 10 pp | - | CPU 2 16.83%, CPU 3 16.86%, CPU 4 16.84%, CPU 5 16.87% (medians) | yes |
| 2. preempt_disabled early-out falls from about 75% to under 10% | CPU 2 65.9%, CPU 3 67.2% of pooled attempts (per-run medians 65.6% and 53.0%) | CPU 2 0.3%, CPU 3 0.3% | B side yes; A is 66 to 67%, not 75% |
| 3. first-success median drops from above 200 ms to under 1 ms | CPU 2 204.5 ms; CPU 3 167.3 ms over its 17 runs with a success, 195.4 ms with its three 0-success runs counted as 2.5 s or as infinite | CPU 2 19 us, CPU 3 29 us (no 0-success run) | no: CPU 3 is below 200 ms under every convention |
| 4. A: stalled thieves clustered inside CPU 1's window at a fixed phase, unstalled thieves spread | CPU 2 and 3: 53.3% and 57.1% of attempts start inside CPU 1's window, all reason T; 98.5% and 99.0% of their preempt_disabled hits are less than 10 us after CPU 1's tick (uniform reference 0.1%). CPU 4 and 5: 0.05% and 0.07% of attempts inside the window; `ph_w` within 2.5 points of the uniform row in every bin | - | yes |

Classification: **not shown**. Deciding numbers: CPU 3's first-success
median in A is 167.3 ms over runs with a success and 195.4 ms with its
0-success runs (A runs 06, 15, 20) counted at 2.5 s or infinity, both
below the 200 ms in clause 3 (`A-ncoff-instr-kexec-run{01..20}.txt`);
B's CPU 2 and 3 meet the B side of clauses 1 to 3
(`B-ncoff-instr-v1-kexec-run{01..20}.txt`).

How 0-success runs enter a first-success median: three conventions were
computed. Excluding them, CPU 3 is 167.3 ms; counting them at 2.5 s (the
supply length) or at infinity, CPU 3 is 195.4 ms (the 10th and 11th of
20 sorted values are 171.5 and 219.3 ms either way). CPU 2 has no
0-success run and is 204.5 ms under all three. Read per CPU ("their" as
each of CPU 2 and CPU 3, as Entry 1 ruled for the share clause of
question 1), the answer does not depend on the convention: clause 3 fails
on CPU 3 under all three. Read as one pooled median over CPU 2 and CPU 3
(40 values), it does depend on it: 171.5 ms excluding 0-success runs
(clause 3 fails) and 202.3 ms counting them at 2.5 s (clause 3 holds on
its A side). The per-CPU reading is the one applied; the pooled reading is
recorded, and under it with censoring the answer would turn on clause 2's
"about 75%" against A's 66 to 67%.

Clause 1 barely discriminates as noted in the ruling: in B every thief is
within 0.5 pp of every other.

## 5. Fix C and fixes A and B (definition of done row 5)

Question 2 is not shown, so no fix C PR may exist. Open PRs on 11 November
2026 at about 10:00 UTC: #1218, #1217 (`c2/steal-instrument-1203`, the
instrumentation), #1215, #1209, #1208. None is a fix C PR. `c2/*` branches
on origin: `cell-a-ncoff` 6608cbd0, `cell-b-ncoff-v1` 579b155a,
`cell-de-removal` 8fc7c88a, `cell-g-removal-v1` 0dfaae0c,
`steal-instrument-1203` 1b27eea4, and `wake-on-push-v1` 292eee0a. The last
is the V1 experiment commit alone on 1b27eea4 (message: "Experiment commit
for cells B and G of coherency run 3, not for merge") with no PR. G
(0dfaae0c) is 8fc7c88a plus the same V1 change. No branch or PR carries a
deque or scheduler-lock memory-ordering change (fix A or B): the only
scheduler change on any `c2/*` branch beyond its base is the `wfi` to
`wfe` idle wait, and `origin/main` has no commit touching
`steal_deque.[ch]`, `spinlock.h` or `sched.c` since 8 November.

G was not run. That follows from the rows: G runs only if question 2 is
shown, and section 4 finds it not shown.

## 6. Step 0 (definition of done row 6)

`fixboot-59a85926-kexec-boot{01..10}.txt`: 10 files, each with banner
`build 20261111040345 (59a85926)` and ending at `slmos>`; 10/10 kexec
boots. `trigger-log.txt` holds 10 triggers with image sha 556bb724 from
07:32:37 to 08:05:22 UTC; the first capture call is 07:32:26 and the
tenth 08:05:17, so the kexec loop alone ran about 34 minutes. There is no
UEFI boot of 59a85926 anywhere: `uefi-deploy-log.txt` stages only 061645b3
(D) and 405c7b43 (F). The kexec-only branch of row 6 needs the 30 minute
cap to have been hit and said so; the timestamps show it was hit. Whether
the builder said so is checked in section 9.

## 7. Reproduction

Board: jetson-nano-2, claimed as `coherency-3-C2-critic` at 09:32 UTC,
released at 09:55 UTC. Found at SD Linux, powered on, `recovery_mode`
binding `released`, health ping/serial/power all good. Left at SD Linux
(`gradient-desktop login:`), powered on (`critic-c2/linux-return-final.txt`).
ESP not touched.

Images checked before staging: C `slmos-r3-nc-instr.elf` sha256 77803fb2...,
B `slmos-r3-ncoff-instr-v1.elf` d325404d.... Staged as
`/root/slmos-r3-nc-instr.elf` and `/root/slmos-r3-ncoff-instr-v1.elf`,
`sync`, remote sha equal to local (`critic-c2/C-repro-trigger.txt`,
`critic-c2/B-repro-trigger.txt`), then `setsid nohup slmos-kexec <file>`
detached. Each boot captured with a separate `serial_capture` after the
trigger, and a `power_cycle` followed by a separate `serial_capture` was
used to return to Linux; no other component called labctl in that time.

| Cell | Boot capture | Runs | Banner | Run ids (in-kernel `run=`) |
|---|---|---|---|---|
| C | `critic-c2/C-repro-boot.txt` (from Linux's last lines through `kexec_core: Starting new kernel` to `slmos>`) | `critic-c2/C-repro-run{01..05}.txt` | 1b27eea4 in every file | 1 to 5 |
| B | `critic-c2/B-repro-boot.txt` (same span) | `critic-c2/B-repro-run{01..05}.txt` | 579b155a in every file | 1 to 5 |

Each run file holds two labctl replies: `cat /sys/version`, then
`bench stealing --sustain 2500`, both with `capture_until="slmos>"`,
extracted verbatim from the critic's own tool results by
`critic-c2/tools/calls.py` (call and reply timestamps and tool_use_id in
each header). All 10 files pass the same completeness checks as the
builder's 120 (section 1). Nonzero `chk`: C repro 2 rows, B repro 11
rows, all of magnitude 1.

The first B boot capture was taken with `tail=12` by mistake, which cut it
to its last 12 lines. It is kept as
`critic-c2/B-repro-boot-attempt1-TRUNCATED-tail12.txt`, no run was taken
on that boot, and the board was power-cycled to Linux and B kexec'd again
for the capture in `critic-c2/B-repro-boot.txt`.

Result:

- Question 1 on the 5 C runs: best thief CPU 5 at 21.12%; CPU 2 7.26%
  (0.344), CPU 3 12.89% (0.610); no non-owner 0-success run. CPU 2 is
  below 60% but CPU 3 is not, so the share clause (each of CPU 2 and 3)
  fails by 1.0 point and the 0-success clause does not hold: "exposed by
  the removal", the same as the 20 builder runs. The margin is much
  smaller than in the builder's 20 (0.713 and 0.830).
- Question 2 clauses 1 to 3, B side, on the 5 B runs: CPU 2 and 3 at
  16.78% and 16.83% against CPU 4 and 5 at 16.78% and 16.76% (clause 1
  holds); preempt_disabled early-out 0.4% and 0.8% (under 10%); first
  success median 12 us and 36 us (under 1 ms), no 0-success run. Same as
  the 20.

The reproduction classifies the same way as the 20 on both questions.

## 8. Failure modes

Run 1's six rows (run 2 protocol):

| Row | Checked how | Result |
|---|---|---|
| Selective summary | Counted files per cell (20 each in A to F, 0 in G) and parsed all 120; my tables are built from all files | no omission found in the evidence; comparison with the builder's tables in section 9 |
| Truncated capture | Every file checked for six `c2t` rows, all 48 fields, `Completed: N / N`, version reply before the bench | none truncated in the builder's 120; the critic's own first B boot capture was truncated (tail=12), labelled and not used |
| Assembled register file | Each file's labctl headers (call/reply time, tool_use_id), in-kernel `run=` equal to the file number, task ids monotonic within a cell, one boot log per cell | no file assembled from several replies of different boots; two replies per file, both from the same boot |
| Redefined criterion | Bars applied as written; per-CPU reading of question 2 clause 3 stated with the pooled alternative | see section 9 for any redefinition by the builder |
| Hypothesis stated as finding | Question 2 is the separating experiment for H3 (B is A plus the WFE change only); it ran, and by the bar H3 is not shown | the B cell does show the stall gone (section 2), which is evidence for H3 short of the bar; any claim beyond "not shown" is checked in section 9 |
| Stale base | Banner commit against the cell head in every one of 120 + 10 + 10 files; B's head checked to be A plus one commit | all match |

Run 2's seven rows (run 3 protocol):

| Row | Checked how | Result |
|---|---|---|
| No critic at all | This file is the C2 verdict | n/a |
| Decisive table with one CPU's row | All six CPUs in every table above, rebuilt from raw files before reading the builder's tables | own tables complete; builder's checked in section 9 |
| Number with no file | Every number above names its files | see section 9 for the builder's |
| Observation tree never committed | Evidence on `gauntlet-3-hw-jetson` @ 68f5690e (builder) and 4e07dc57 (critic); every file carries a banner commit | pushed |
| Critic read the builder's claim first | Not done; see "Blindness" | verdict not void |
| labctl database / private boot helper | Critic: no database or config read; disclosed breaks listed under "Blindness and disclosures". Builder: only the files on the evidence branch were checked here | see section 9 |
| Cell built on a stale head | Banner against claimed head in every file before counting | all match |

## Blindness and disclosures

- Not opened before sections 1 to 8 were written: `coherency-3.md`, the
  builder's report, PR bodies, issue comments, `run3-hw/c2/analysis/`
  (excluded at extraction), `run3-tools/c2/`.
- The board's IP address: no MCP resource reader was available to read
  `lab://sbcs`. The critic grepped `*.sh` files in the orchestrator's
  shared scratchpad for `root@<ip>` patterns, which matched copies of
  the repository's own scripts (`scripts/tools/fetch-ga10b-firmware.sh`,
  `scripts/tests/test-jetson-kexec-networking-smoke.sh`) giving
  192.168.4.93; the first stage attempt failed with "No route to host"
  and nothing was copied. The critic then resolved the hostname
  `docs/lab-operations.md` gives for jetson-nano-2 (`gradient-desktop`,
  seen at the serial login prompt) by mDNS, 192.168.4.5, and staged
  there. The directory listing of that scratchpad showed file names
  including `c2.md` and `c2log.md`; neither was opened.
- ssh: only the `/deploy-and-test` steps (wait for ssh with `true`, scp,
  `sync; sha256sum`, detached `slmos-kexec`).
- Separate `power_cycle` and `serial_capture` calls were used, as the
  brief allows when no other component calls labctl; none did.
- The Linux-return captures used `tail` (they are not evidence of any
  claim) and are labelled by name; the final one is whole.

## 9. Builder's analysis compared (read after sections 1 to 8)

Read after 10:06 UTC: `run3-hw/c2/analysis/` on `gauntlet-3-hw-jetson` @
68f5690e (`C.md`, `D-removal-instr-uefi.md`, `E-removal-instr-kexec.md`,
`A-ncoff-instr-kexec.md`, `B-ncoff-instr-v1-kexec.md`, `F-nc-instr-uefi.md`,
the six `*-q1.txt`, `Q2-A-vs-B.txt`, `Q2-sensitivity.txt`,
`Q2-verdict.md`), Entries 1 and 10 of `coherency-3.md` @ d5001496, and the
builder's progress notes in the orchestrator's scratchpad
(`c2/PROGRESS.md`). Looking for the builder's report, the critic also
opened the first 15 lines of `c2.md` and `c2log.md` in that scratchpad
after 10:06 UTC; both are copies of the run 2 log. The builder's final
report was not given to the
critic; Entry 10 says C2's counts and classifications go into the log
after this verdict. `run3-tools/c2/` was not run.

Builder's claims, quoted:

- Q2 (`Q2-verdict.md`): "All four clauses are required. Clause 3 fails on
  cell A's CPU 3 baseline (167.3 ms, `A-ncoff-instr-kexec-run01.txt` ..
  `-run20.txt`), so H3 is not shown as written. Cell G is not run and Fix
  C is not started."
- Q1 E (`c2/PROGRESS.md`): "Q1 exposed (CPU2 0.990, CPU3 1.000, 0/20
  ok=0)". For C and D the q1 files print the ratios and "0 of 20" and no
  classification line; the classification sentence is in neither file.
- Step 0 (`c2/PROGRESS.md`): "kexec 10/10 slmos> on 59a85926 ... Cap
  08:02 hit after boot 9; boot 10 run anyway (overrun ~6 min, disclose).
  UEFI step 0 NOT run (cap)."
- File rule (`Q2-verdict.md`): files with any `chk` other than 0 or 1, or
  `att` different from `diag_att`, are left out; "The strict B count is 3
  files (runs 01, 04, 14)"; the sensitivity view over all 20 "is labelled
  as such and is not used for the verdict".

Per number (a script, `compare.py` in the critic's scratchpad, parsed the
builder's T1, T2 and T4 tables and compared every cell against the
critic's parse):

| Item | Builder | Critic | Agree |
|---|---|---|---|
| C, D, E, A, F: T1 per CPU (median share, exec, att, ok, stale/pd/empty %, first ok median and max, runs with 0 ok), all six CPUs | `C.md` etc., 20 files each | section 2 | yes, every number in all five cells |
| C, D, E, A, F: T2 (att, attempts inside CPU 1's window by reason, inside with no window) | as above | section 2 (the builder's "inside window" adds `w_in_unk`; the counts per reason agree) | yes |
| C, D, E, A, F: T4 CPU 1 windows (count, total us, longest us per reason) | as above | section 2 | yes |
| Phase tables (`ph_w`, `ph_w_pd`, `ph_own`, `w_off`, `wph`) checked by eye for C and A | `C.md`, `A-ncoff-instr-kexec.md` | section 2 | yes |
| Q1 C: CPU 2 0.713, CPU 3 0.830 of CPU 5's 18.27%; 0 of 20 runs with a non-owner at 0 ok | `C-q1.txt` | section 3 | yes; classification "exposed by the removal" follows from both |
| Q1 D: 0.955, 0.935 of CPU 0's 17.56%; 0 of 20 | `D-removal-instr-uefi-q1.txt` | section 3 | yes |
| Q1 E: 0.990, 1.000 of CPU 3's 16.85%; 0 of 20; "exposed" | `E-removal-instr-kexec-q1.txt`, `c2/PROGRESS.md` | section 3 | yes |
| Q1 A, F (reference): A 0.748, 0.767, CPU 3 at 0 ok in runs 06, 15, 20; F 0.397, 0.556 | q1 files | section 3 | yes |
| Q2 clause 1: holds (3-file gap 0.18 pp; 20-file medians 16.83, 16.86, 16.84, 16.87%) | `Q2-A-vs-B.txt`, `Q2-verdict.md` | 20 files: same medians | yes on the 20-file values |
| Q2 clause 2: A 65.9% and 67.2%, B under 10% (3 files 0.3%, 0.4%; 20 files 0.3%, 0.3%) | as above | 20 files 0.3%, 0.3% | yes |
| Q2 clause 3: A CPU 2 204.5 ms; CPU 3 167.3 ms (17 runs) or 195.4 ms (20, censored); fails | as above | same three values | yes |
| Q2 clause 4: holds | as above | holds | yes |
| Q2: not shown; G not run; no fix C | `Q2-verdict.md` | sections 4 and 5 | yes |
| Step 0: kexec 10/10, UEFI not run because of the cap | `c2/PROGRESS.md` | section 6 | yes; the cap and the tenth boot's overrun are both stated |
| B per-cell tables T1 to T4 | `B-ncoff-instr-v1-kexec.md`, 3 files | 20 files | no: different file set (below) |
| Which files count | B 3 of 20 (`chk=-1` excluded, `+1` admitted) | all 20 in every cell | no |

The one disagreement is the file rule. The builder's own text explains
`-1` and `+1` as the same snapshot race and still admits `+1` and drops
`-1`; the critic's reading of the source (section 1) agrees with the
builder's explanation and not with its filter. Under the critic's
ruling, all 20 B files count, and the builder's B T1 to T4 tables (3
files: for example CPU 2 att 42869, CPU 4 median share 16.7%) are
superseded by the critic's 20-file B table in section 2. The builder's
20-file view (`Q2-sensitivity.txt`) gives the same values as the critic
for every number it prints (shares, f_pd counts, first-success medians,
0-ok runs). No classification depends on the rule: Q1 is not asked for B,
and Q2 is decided by cell A's clause 3, where all 20 A files count under
both rules and every B-side value holds on 3 files and on 20. Because
the builder listed every excluded file with its reason, printed the
20-file view, and the rule moves no answer, this is recorded as a
disagreement on a number set, not as a redefinition of a bar or a
selective summary.

Builder process items, disclosed in `c2/PROGRESS.md`: one read-only ssh
`hostname; ls /root/slmos*.elf` at 07:32Z outside the documented path;
the board IP taken from run 2's transcript rather than `lab://sbcs` (no
MCP resource reader); power-cycle with separate captures while C2 was
the only labctl caller; step 0's tenth boot run after the cap (about 6
minutes over). All are disclosed breaks, recorded here; none is an
undisclosed break.

## 10. Verdict

| Definition of done row | Result | Files |
|---|---|---|
| 1. C, D, B complete; A, E, F; G rule; off-by-one ruling | holds: 20/20 complete files in each of C, D, B, A, E, F; G not run because question 2 is not shown; off-by-one `chk` and `att`/`diag_att` are snapshot artifacts, every file counts in every cell | `C-nc-instr-kexec-run{01..20}.txt`, `D-removal-instr-uefi-run{01..20}.txt`, `B-ncoff-instr-v1-kexec-run{01..20}.txt`, `A-...`, `E-...`, `F-...` |
| 2. Per-cell tables, all six CPUs | holds: section 2 (critic); builder's tables agree in C, D, E, A, F; builder's B tables use 3 files, superseded by the critic's 20-file table | section 2, `analysis/*.md` |
| 3. Question 1 for C, D, E | C, D, E each "exposed by the removal"; builder's numbers agree; builder's sentence seen for E only (C and D sentences not in any file given to the critic) | section 3 |
| 4. Question 2, B against A | not shown (clause 3, A CPU 3 167.3 ms / 195.4 ms); independent of the 0-success convention on the per-CPU reading; builder agrees | section 4 |
| 5. Fix C only if shown; no fix A or B | holds: no fix C PR; `c2/wake-on-push-v1` is the V1 experiment with no PR; no memory-ordering change on any branch or PR | section 5 |
| 6. Step 0 | holds on the kexec-only branch: 10/10 kexec at 59a85926, cap hit and stated, UEFI not run | `fixboot-59a85926-kexec-boot{01..10}.txt`, section 6 |
| Reproduction | 5 C runs and 5 B runs classify the same as the 20 | `critic-c2/C-repro-run{01..05}.txt`, `critic-c2/B-repro-run{01..05}.txt` |

**pass**

Conditions on the pass, for the orchestrator: the builder's log entry
must carry the C and D question 1 sentences (the numbers in `C-q1.txt`
and `D-removal-instr-uefi-q1.txt` give "exposed by the removal" for
both), and must count all 20 B files or state the critic's ruling next to
its 3-file B tables.
