Lab session #1263 / #1207, Part B (builder B): PMM halt (#1207) and the two coalescing tests.
Branch fix/pmm-coalesce, stacked on fix/kexec-dtb-reserve 661204e6 (PR #1265, = main dc51107c + #1244).

Heads
  661204e6  base (unmodified): "base" files
  df481476  + PMM_CHECK build option only (allocator unchanged)
  21303db1  + five new tests and two test hooks (allocator unchanged; the tests fail here)
            The "old" files are labelled 7b0af7d0: that is this commit before a message-only amend;
            both have tree 6ee8b6b33abe26fd6c2851823e618a35d4066d9b.
  e3c22600  + the allocator fix, the coalescing-test fix, docs
  f650b695  + the test hooks built into every image (e3c22600's production kernels did not compile;
            the test kernel's .text is byte-identical, B-text-testkernel-f650b695-vs-e3c22600.txt)

Every QEMU file is one `make test ...` (ARM64 QEMU unless the name says x86_64) run under the session's
QEMU lock by B-qrun.sh.txt: a header (worktree, head, command, date, make's exit code), the last 40 lines
of make's output, then the whole QEMU serial log. B-qsum.sh.txt prints one summary line per file.
"x50" = TEST_REPEAT=50 with the TEST_FILTER in the header (the filter of PR #1243).

Baseline, 661204e6 (B-base-loops.sh.txt, B-suite-loop.sh.txt)
  B-qemu-base-661204e6-coalesce-x50.txt           test_coalesce_enables_larger_allocation, test_recursive_coalescing x50
  B-qemu-base-661204e6-checker-exhaust-x50.txt    test_checkerboard_fragmentation, test_exhaustion_recovery x50
  B-qemu-base-661204e6-fullsuite-run01..20.txt    full suite x20 (the PMM group in suite order; the filter holds
                                                  16 names at most and the group has 48, so the whole suite was run)

Tests fail on the old allocator, 21303db1 (labelled 7b0af7d0) (B-old-proof.sh.txt)
  B-qemu-old-7b0af7d0-newtests.txt                the five new tests, one pass over them
  B-qemu-old-7b0af7d0-halt-signature.txt          test_fragmentation_recovery_large, test_free_does_not_merge_with_split_buddy x2:
                                                  the second pass dies with the #1207 data abort
  B-qemu-old-7b0af7d0-pmmcheck-on.txt             the same with PMM_CHECK=ON: panic at the merge

Fix head (B-fix-rows.sh.txt; f650b695 files by the same script with a suite count of 3)
  B-qemu-fix-e3c22600-coalesce-x50.txt, -checker-exhaust-x50.txt, -newtests-x50.txt
  B-qemu-fix-e3c22600-fullsuite-run01..20.txt     full suite x20
  B-qemu-fix-e3c22600-pmmcheck-on-fullsuite.txt   full suite, PMM_CHECK=ON
  B-qemu-fix-e3c22600-x86_64-fullsuite.txt        make test PLATFORM=X86_64
  B-qemu-fix-e3c22600-check-braces.txt            make check-braces: FAILED at e3c22600 (test_pmm.c unparsed), the defect f650b695 fixes
  B-qemu-fix-f650b695-*.txt                       the same rows at the final head (full suite x3 as -fullsuite-run1..3,
                                                  x10 more as -fullsuite-b-run01..10), check-braces 0 violations

Build comparisons
  B-text-pmmcheck-off-df481476-vs-661204e6.txt    production kernels, option off, Jetson / Pi 5 / QEMU (B-prodbuild.sh.txt, B-textcmp.sh.txt)
  B-text-testkernel-f650b695-vs-e3c22600.txt      QEMU test kernel .text identical across the last commit
  B-images.txt                                    hardware images built for the lab phases (sha256, banner, PMM_CHECK state)

Lab scripts prepared (previous session's, paths changed): B-trigger.sh.txt, B-save.sh.txt, B-savebatch.sh.txt, B-hwimg.sh.txt

Added after the B0 report (interim work, no lab)
  679a09c7, f55c5293  PMM_CHECK prints the reserve list at pmm_init (check-option code only) + its doc line.
                      Fix head is now f55c5293. diag/1207-check-oldalloc 17daedda = df481476 + 679a09c7
                      (old allocator, full PMM_CHECK report): the image for the first B1 boots.
  B-images.txt        rebuilt at 17daedda and f55c5293 (B-hw-all.sh.txt); B-images-superseded-f650b695.txt is the earlier list.
  B-qemu-ab-base-661204e6-run01..10.txt, B-qemu-ab-fix-f55c5293-run01..10.txt
                      full suite, base and fix interleaved (base, fix, base, fix, ...), both kernels built first,
                      no build of this builder running (B-ab-litmus.sh.txt, B-ab-litmus.out.txt has the load before each pair)
  B-ci-f55c5293-step01..11-*.txt   the /ci skill's steps at f55c5293 (B-ci.sh.txt, B-ci.out.txt), QEMU steps under the lock
  B-qemu-fix-f55c5293-{coalesce-x50,checker-exhaust-x50,newtests-x50,pmmcheck-on-fullsuite,x86_64-fullsuite}.txt   the filter rows again at f55c5293 (check-braces at this head is /ci step 8)

Phase B1, jetson-nano-2, kexec (one boot per boot_test call, pattern "Task 'main' \(id=\d+\) exiting", host-side trigger
B-trig1.sh.txt / B-trigger.sh.txt, saved by B-b1step.sh.txt / B-sstep.sh.txt through B-save.sh.txt)
  B-kexec-chk-oldalloc-17daedda-boot01,02.txt   old allocator + PMM_CHECK=ON (diag/1207-check-oldalloc 17daedda), integration suite
  B-kexec-suite-f55c5293-boot01..10.txt         fix head f55c5293, PMM_CHECK off, integration suite: the required row
  B-trigger-kexec-chk-oldalloc.log.txt, B-trigger-kexec-suite-fix.log.txt   trigger logs (boot_id, ELF sha256 prefix per boot)
  B-jetson-asfound-b1.txt, B-jetson-post-state-b1.txt                     /root as found and as left

After B1 (interim, no lab): head 647c2c33 adds the free-list verifier (every build, no table)
  B-qemu-verify-demo-647c2c33.txt, B-demo-patch.py.txt   an uncommitted demo patch leaves a cleared order-0 link in place:
                                                        the timer-tick walk, free_list_pop and the panic each report it
  B-sizes-verifier-647c2c33-vs-f55c5293.txt             Jetson suite image: .data/.bss sizes and __kernel_end unchanged
  B-qemu-fix-647c2c33-*.txt                             filter rows x50, PMM_CHECK suite, x86-64 suite, full suite x5 (run1..5)
  B-ci-647c2c33-step01..11-*.txt, B-ci2.out.txt          /ci at 647c2c33
  B-candidates-writers.txt                              where the damaged blocks are and candidate writers (code reading only)
  B-images.txt                                          now also the 647c2c33 images; B-images-b1-f55c5293.txt is the list used in B1
  B-save-v2.sh.txt, B-b1step-v2.sh.txt                  save scripts for the next lab rows (summary lines also pick up [pmm-verify]); B1 used B-save.sh.txt / B-b1step.sh.txt

Phase B1b, jetson-nano-2, kexec, head 647c2c33 (one boot per boot_test call, same pattern and trigger as B1; B-s2.sh.txt)
  B-kexec-suite-647c2c33-boot01..10.txt        integration suite with the free-list verifier
  B-finding-boot04.txt                         the one finding (boot 04): observations, inferences, next checks
  B-trigger-kexec-suite-ver.log.txt            trigger log (10 triggers; one loop at 14:22 ended with none: its boot_test
                                               call was never issued, the session was stopped there and resumed 6.5 h later)
  B-jetson-asfound-b1b.txt, B-jetson-post-state-b1b.txt
Phase B2, pi-5-1: on gauntlet-5-hw-pi5, same directory (B-pi5-suite-647c2c33-boot01..03.txt, B-pi5-restore-check-boot01.txt,
  B-pi5-card-and-restore.txt)

After the blind critic's round 1 (host only; the lab connection was down): head 02ebdad8
  B-qemu-fix-02ebdad8-coalesce-x50.txt, -checker-exhaust-x50.txt, -newtests-x50.txt, -verify-x50.txt, -pmmcheck-on-fullsuite.txt,
  -x86_64-fullsuite.txt                          rows at 02ebdad8 (B-rows5.sh.txt, B-rows5.out.txt)
  B-qemu-ab2-base-661204e6-run01..15.txt, B-qemu-ab2-fix-02ebdad8-run01..15.txt   full suite, base and head interleaved, 15 pairs
  B-qemu-verify-demo-02ebdad8.txt                the cleared-link demonstration again: free_list_pop now panics after its report
  B-ci-02ebdad8-step01..11-*.txt, B-ci-02ebdad8-step04-test-arm64-attempt1-litmus.txt, B-ci3-attempt1.out.txt, B-ci3b.out.txt
                                                 /ci at 02ebdad8: step 4 failed once on test_litmus_patterns_pass, rerun from step 4 passed
  B-ivac-callers.txt                             every DC IVAC call site that can run on Jetson, and what each can reach
  B-finding-boot04.txt                           corrected: what the boot 04 capture bounds and what it does not

After the critic's round 2: head 122befe0 (comments and docs only)
  B-text-testkernel-122befe0-vs-02ebdad8.txt     QEMU test-kernel .text identical to 02ebdad8
  B-qemu-fix-122befe0-fullsuite-run1.txt, B-qemu-fix-122befe0-check-braces.txt   make test once, check-braces
  B-ivac-callers.txt, B-finding-boot04.txt       second versions: the in-tree record on DC IVAC / DC ISW on these cores, two call sites added
