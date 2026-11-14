# test_litmus_patterns_pass (#1230) in every C1 full make test, with host load

Load = 1-minute load average at the start of the run (from /proc/loadavg or
uptime, recorded in each file's header). "n/r" = not recorded.

| Run | Tree | Suite | Load at start | Result | MP weak rows | File |
|---|---|---|---|---|---|---|
| try1 | 8ec0e330-dirty (pre-6e39f425) | default | 3.71 | FAIL | 1 | pre-commit-try1-unfiltered.log |
| full | 6e39f425 | default | 3.05 | PASS | 0 | unfiltered-c1-6e39f425.log |
| base run1 | 8ec0e330 | default | 3.74 | FAIL | 4 | unfiltered-base-8ec0e330-run1-1230.log |
| base run2 | 8ec0e330 | default | 2.05 | PASS | 0 | unfiltered-base-8ec0e330.log |
| /ci 4 | 6e39f425 | default | n/r (4.31 at /ci start, 23:47) | FAIL | 1 | ci-6e39f425/04-test-output.log.txt |
| /ci 5 | 6e39f425 | AI_SCHED | n/r | FAIL | 3 | ci-6e39f425/05-test-output.log.txt |
| 04r | 165cb992 | default | 5.70 | FAIL | see file | ci-165cb992/04r-test-output.log |
| 05r | 165cb992 | AI_SCHED | 6.93 | FAIL | see file | ci-165cb992/05r-test-output.log |
| base1 | 8ec0e330 | default | 4.09 | FAIL | 1 | litmus-ab/base1.log.txt |
| head1 | 165cb992 | default | 3.79 | PASS | 0 | litmus-ab/head1.log.txt |
| base2 | 8ec0e330 | default | 4.67 | FAIL | 1 | litmus-ab/base2.log.txt |
| head2 | 165cb992 | default | 5.67 | FAIL | 1 | litmus-ab/head2.log.txt |
| 05r2 | 165cb992 | AI_SCHED | 2.56 | PASS | 0 | ci-165cb992/05r2-test-output.log |
| head3 | 165cb992 | default | 1.46 | PASS | 0 | litmus-ab/head3.log.txt |
| head3ai | 165cb992 | AI_SCHED | 2.75 | PASS | 0 | litmus-ab/head3ai.log.txt |
| base3 | 8ec0e330 | default | 2.96 | PASS | 0 | litmus-ab/base3.log.txt |

Load under 3.1: head 4 of 4 PASS (full, 05r2, head3, head3ai), base 2 of 2
PASS (base run2, base3). Load 3.7 and above: head (incl. try1) 5 of 6 FAIL,
base 3 of 3 FAIL. C0's base /ci at load about 2 passed both
(run5-hw/c0/ci-base-8ec0e330/). The failure tracks host load on both
trees; these counts show no change in rate from C1, within their small size.
