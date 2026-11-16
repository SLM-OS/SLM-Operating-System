VERDICT: PASS — all five round-4 items are resolved, 87061023 touches one document only, the rewritten archive passage is accurate in every line number and claims no more than the two captures show, and no remaining finding.

Scope: Part A (#1263), `fix/percpu-cacheline` 87061023, texts only. PR line numbers are those of `CLAIMS5-draft-pr-body.md`.

## Round-4 items

| # | Status | Where |
|---|---|---|
| 1 archive claim "not new with the padding" | Resolved | PR 144; `docs/scheduler.md` at 87061023; PR line 1 names f3c0d021's sentence as superseded |
| 2 two cited files not on an evidence branch | Resolved | `origin/gauntlet-5-hw-jetson` 4efd7e68 adds the four files; the 2be38017 make-test log is byte-identical to the one judged in round 4; both braces logs now carry a `# command:` line (the 2be38017 one says the header was added afterwards) |
| 3 "critic round 2" in #1284 | Resolved | both comment openings now read "from the #1263 lab session" / "from the #1263 session"; no other change to the issue text |
| 4 c3ce3885's "on any CPU" | Resolved | PR line 1 names it and points at the limits section |
| 5 "A2c" before its definition | Resolved | PR 130 defines A2b at first use; PR 161 defines A2c at first use |

## Commit 87061023

- Parent 2be38017. One file, `docs/scheduler.md`, 12 insertions and 4 deletions, all in the paragraph "Observed effect on where a woken task runs". No code.
- The new paragraph: "On healthy unpadded kernels the regime has not been seen: 0 of these 1400 unpadded iterations and 0 of 822 archived runs after the #1234 change"; the failing line occurs in the two archived boots "but in both only after CPUs 4 and 5 had panicked and halted, and those lines carry no per-CPU histogram"; "those boots do not show the regime on a healthy unpadded kernel. In this pair of kernels the regime follows the padded layout." Each statement matches the captures.

## Make-test log at 87061023

`A-qemu-arm-make-test-87061023.txt` on the evidence branch: banner `SLM-OS v0.4.0 build 20261116030623 (87061023)`, 3051 `[PASS]`, 0 `[FAIL]`, `[PASS] All test suites passed!`, exit code 0. Line 7936 `pdm: moved=21 runs=6/5/11 bad=0`, line 7940 PASS. The sorted test-name list equals the 2be38017 run's (the only diffs are three names whose lines interleave with shell output). The five expected `coherency: FAIL` lines are present as before. `A-check-braces-87061023.txt`: "no changed C lines", exit 0, as expected for a document-only commit.

## The rewritten archive passage (PR 144)

Every line number re-checked against the two captures:

- `c2/hw-base/uefi-r2-boot1.txt`: `[cpu5] KERNEL PANIC` 2965, `[cpu5] System halted.` 3047, `[cpu4] System halted.` 3125, first `moved=0` 3138; 37 such iterations, 38 failed of 200, the 38th `w >= 0` with `moved=4 runs=39/100`. All correct.
- `c2-fix-critic/base-3b369aef-r2-boot02.txt`: `[cpu5] System halted.` 1935, `[cpu4] KERNEL PANIC` 1942, `[cpu4] System halted.` 2014, first `moved=0` 2020; kexec, ends at iteration 34 of 200 with no summary, 13 with the line, 2 more `w >= 0`. All correct.
- "read from the line text only", "boots running on four of six CPUs", "do not show the regime on a healthy unpadded kernel": no more than the captures show.
- "On unpadded kernels with all CPUs alive the line is absent ... 0 of 822 archived runs": three of the `suite-kexec-60c548f3-*` boots contain `System halted.`, but in each it comes after the `pdm:` line (boot01 9006 then 9072, boot09 8983 then 9031, boot12 9007 then 9073), so the statement holds for the test's run.

## Whole-text searches

- "not new", "present again", "before the #1234": only PR line 1 (naming the superseded sentence) and PR 144 (the corrected passage). Nothing else in the PR body, #1284 or the #1262 draft leans on the old claim.
- Closing keywords: "Closes #1263" (PR line 3) only.
- Internal-process references ("critic", "orchestrator", "builder", "round N"): none in the PR body, #1284 or the #1262 draft.

## Items carried as open — each stated plainly

- Wake-placement cost not measured: PR 22 (bold, "What changes"), the "Not measured" list, the ledger row, `docs/scheduler.md`.
- #1284 open: PR section "Open observation on the unpadded kernel: #1284", two readings, "the capture decides neither"; no closing keyword.
- Superseded commit-message sentences: PR line 1 names c0612fa7, f3c0d021 (two sentences) and c3ce3885, with the squash instruction.
- `/ci` last run in full at c3ce3885: PR 190 says so, and that 2be38017 and 87061023 (comments and documents only) have `make test` and check-braces only.
- Ledger row awaits the user's acceptance: PR 107 and the row itself.

## Not checkable

- What caused the cpu4 and cpu5 panics in the two archive boots, and whether the halts produced `moved=0` there (the text claims neither).
- Whether GitHub's live #1284 equals `CLAIMS5-issue-1284.txt`.
- Figures from earlier rounds not re-derived here (R3 table, bench means, the 822 count's composition, suite failure names).
