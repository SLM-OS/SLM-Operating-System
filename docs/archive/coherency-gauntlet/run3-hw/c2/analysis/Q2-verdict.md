# Q2 (cell B against cell A), C2, coherency run 3

Files: A = `A-ncoff-instr-kexec-run01.txt` .. `-run20.txt` (20 files, head
6608cbd0); B = `B-ncoff-instr-v1-kexec-run01.txt` .. `-run20.txt` (20 files,
head 579b155a). Bar output: `analysis/Q2-A-vs-B.txt` (bars.py q2, strict
file check). Labelled sensitivity view: `analysis/Q2-sensitivity.txt`.

## File check

bars.py and analyze.py leave out any file with a c2t line whose chk is not
0 or 1, or whose att differs from diag_att. All 20 A files pass. Of the 20
B files, 17 are left out (16 for chk=-1 on one or two CPUs; run 08 also has
CPU 4 att 9216 against diag_att 9215; run 16 CPU 1 prints att=175 with
f_empty=176). The strict B count is 3 files (runs 01, 04, 14).

chk is printed by the bench as att - (ok + iso + f_stale + f_pd + f_empty +
f_other), each term a difference of two non-atomic snapshots of per-CPU
counters (`kernel/src/shell_sys.c`, the c2t print loop). An attempt that
starts after att is read and finishes before the outcome counters are read
gives -1; the reverse order gives +1. The check admitted +1 as "an attempt
in flight" (run 2's hist.py) but not -1. B's thieves (CPUs 2 to 5) attempt
939 to 25,667 times per run (A: 182 to 508), so the race appears in most B
runs. The sensitivity view drops the chk filter for both cells;
it is labelled as such and is not used for the verdict.

## Clauses (strict count; sensitivity values in brackets)

1. B, CPUs 2 and 3 within 10 pp of CPUs 4 and 5 (median share): largest
   gap 0.18 pp over 3 files (B medians over 20 files: CPU 2 16.83%, CPU 3
   16.86%, CPU 4 16.84%, CPU 5 16.87%). Holds.
2. f_pd share of attempts, A about 75% to B under 10%: CPU 2 A 5139/7797 =
   65.9% to B 136/42869 = 0.3% [872/299593 = 0.3%]; CPU 3 A 4904/7303 =
   67.2% to B 73/18758 = 0.4% [637/183404 = 0.3%]. B under 10% holds; the
   A values are 65.9% and 67.2%, below the "about 75%" the bar cites.
3. First-success median, A above 200 ms to B under 1 ms: CPU 2 A 204.5 ms
   (20 runs) to B 0.0 ms; CPU 3 A 167.3 ms (17 runs with a success; 195.4
   ms over 20 runs if the 3 runs with ok=0 count as later than the run) to
   B 0.0 ms. CPU 3's A value is not above 200 ms under either reading.
   Fails.
4. A window data: CPU 2 4160/7797 = 53.4% and CPU 3 4167/7303 = 57.1% of
   attempts start inside CPU 1's window; within 200 us of CPU 1's tick
   5505/7797 = 70.6% and 5025/7303 = 68.8% (uniform 2.0%); tick offset to
   CPU 1 median 0 us. Unstalled thieves: CPU 4 2/4068 = 0.0% and CPU 5
   3/4059 = 0.1% inside, 48/4068 = 1.2% and 50/4059 = 1.2% within 200 us,
   offset median 5401 us and 5402 us (range 932 to 6547 us over the 20
   runs); CPU 0 43/55878 = 0.1% inside, offset median 4502 us (range 45 to
   9836 us). Stalled thieves are clustered at a fixed phase; unstalled
   thieves sit away from CPU 1's window, CPU 0 spread across the period,
   CPUs 4 and 5 over 932 to 6547 us. My reading: holds.

## Verdict

All four clauses are required. Clause 3 fails on cell A's CPU 3 baseline
(167.3 ms, `A-ncoff-instr-kexec-run01.txt` .. `-run20.txt`), so H3 is not
shown as written. Cell G is not run and Fix C is not started.
