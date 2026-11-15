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
