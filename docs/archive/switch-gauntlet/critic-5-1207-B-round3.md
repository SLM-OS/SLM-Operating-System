PASS — all five round-2 items are resolved or carried plainly, the eight per-boot descriptions match the captures, the diff is comments and docs only, and I found no wrong number, new overstatement or closing keyword. Two optional one-line additions are listed at the end.

# Critic B, round 3: fix/pmm-coalesce 122befe0, CLAIMS3-draft-pr-body.md, CLAIMS3-draft-1207-comment.md

Nothing was run. Code from `.wt/b` at 122befe0; evidence from `gauntlet-5-hw-jetson` 293f8fa8.

## The five round-2 items

1. **"Every DC IVAC call site was read."** Resolved. Both drafts now say "the call sites found by search ... not a proof
   that there are no others". `B-ivac-callers.txt` (second version) adds `slm_gpu_sync_for_cpu` with its two Rust callers
   and the `dc isw` loops at `smp_boot.S:246, 269`, and marks what was not traced.
2. **The tree's record on DC IVAC.** Resolved. Both drafts, `pmm.c:643-645`, `kernel/CLAUDE.md` and `docs/memory-map.md`
   now cite `mmu.S:46-50`, `smp.c:684-688` and `smp_boot.S:205-213` and call a lost header write "architecturally possible
   ... excluded on these cores if that record holds". The non-snooping-device case is kept as the one that survives. The
   quoted lines match the source.
3. **The eight scheduler-suite losses.** Resolved; checked per boot below.
4. **`free_list_remove` follows links unchecked.** Carried plainly: an "Open item in the code" paragraph in the PR body
   and a clause in the comment, both accurate about the consequence (named panic, no dump of the block).
5. **Wording.** Resolved: `docs/memory-map.md` and `docs/testing.md` say "not a listed block of this order"; the
   "writer is confirmed" sentence is gone from `B-finding-boot04.txt`.

## The eight losses against the captures

All eight have `[PASS] test_migrate_ready_task_updates_assigned_cpu` immediately before the trouble, as the PR says.

| Boot | PR text | Capture |
|---|---|---|
| f55c5293 02 | CPU 4 `idle_1`, PC alignment fault; no line for the test; ends in the litmus | matches (EC 0x22; last line is the litmus table header) |
| f55c5293 04 | CPU 2 `pdm_a`, illegal execution state; FAIL; ends in the litmus | matches |
| f55c5293 08 | same as 04 | matches; the capture also has a second dump (CPU 4, `pdm_b`, same EC) that the text does not mention |
| f55c5293 07 | CPU 4 data abort, write to 0x0; FAIL; last line a later scheduler test | matches |
| 647c2c33 01 | CPU 4 `idle_1`, PC alignment fault; PASS; ends in the litmus | matches |
| 647c2c33 02 | CPU 1 exception, EC 0x0; no line for the test; ends at that dump | matches |
| 647c2c33 06 | no dump; stops at the `pdm:` line | matches |
| 647c2c33 10 | CPU 4 `idle_1` PC alignment fault and two more dumps; FAIL; reaches the component tests, ends at a dump | matches (CPU 5 `idle_3`, CPU 0 `c4_payload`) |

## The diff 02ebdad8..122befe0

Four files: `docs/memory-map.md`, `docs/testing.md`, `kernel/CLAUDE.md`, and one comment block in `kernel/mm/pmm.c`
(five lines replaced by five, so no `__LINE__` moves). No code. `B-text-testkernel-122befe0-vs-02ebdad8.txt`: the QEMU
test kernel's `.text` has the same sha256 and 0 differing bytes at both heads. One `make test` at 122befe0 passes
(PMM `55/52/0/3`); `check-braces` 0 violations, 0 unparsed.

## New text checked

- The rewritten experiment 1 (pattern stamp, clean to DRAM, once-a-second walk that drops its cached copy) is worded as
  a proposal and its readings as "points at"; no result is claimed.
- "On these cores none of them can lose a write if the record above holds" is conditional, as it should be.
- 0.85^30 = 0.0076; the other numbers are unchanged from round 2 and were checked then.
- No closing keyword next to an issue number in either draft.

## Open items and how plainly the PR states them

- **Hardware rows only at f55c5293 and 647c2c33:** bold, near the top of the PR body, and in the comment's first line. Plain.
- **`free_list_remove` unchecked links:** its own paragraph. Plain.
- **Untraced callers:** named one by one as not traced, in both drafts. Plain.
- **`/ci` not re-run at 122befe0:** not said in so many words. The rows are labelled by head (`/ci` at 02ebdad8;
  `make test` once and `check-braces` at 122befe0), so a careful reader gets it; a hurried one may not.

## Optional additions (neither fails the round)

1. Add "`/ci` was not re-run at 122befe0" to the 122befe0 row.
2. f55c5293 boot08 has two dumps (CPU 2 `pdm_a`, then CPU 4 `pdm_b`); the per-boot line mentions one.

## Still not checkable

- Whether the in-tree statement that A76 / A78AE execute DC IVAC as DC CIVAC is correct. The drafts make their
  exclusion conditional on it, which is the right form.
- #1240, #1274 and #1278 as described; the earlier-session numbers (104 to 132 free single pages, the Pi 5 list).
