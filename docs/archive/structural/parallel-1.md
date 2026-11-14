# Structural review, parallel session 1

Append-only log. Each entry names its numbers and where they came from.

Source review: "SLM-OS Structural Review: Why the Ticket Count Grows"
(2026-11-08), Recommendations 5 (part 1), 7 and 8. Run alongside
gauntlet Brief 5 (`gauntlet/switch-5`), which measures timing on the same
host.

## Rules for this run

- No labctl calls, no QEMU, no `make test`, no `/ci`. At most one local
  build at a time, under `nice -n 19`, and only if a track needs it.
- Files under `kernel/sched/`, `kernel/arch/`, `kernel/mm/`,
  `kernel/tests/`, the exception and panic path, and `docs/adr/0008*` are
  off limits. So are the `gauntlet/switch-5` and `gauntlet-5-hw-*`
  branches.
- Nothing merges. Draft PRs only. Issues are not closed: close candidates
  go in a table for approval.

## Tracks

| Track | Recommendation | Branch | Output |
|-------|----------------|--------|--------|
| A | 7, tracker triage | none (GitHub labels and milestones) | labels, close-candidates table, metrics |
| B | 8, architecture record | `structural/architecture-record` | draft PR |
| C | 5 part 1, CI build matrix | `structural/ci-matrix` | draft PR, first run |

## Entry 1, 2026-11-13: start

- Base: `origin/main` at 8ec0e330 (`git log -1 origin/main`).
- The review text was read from the user's Claude Docs copy (rev 40).
- The review's baseline figures this run compares against (its tracker
  snapshot, 2026-11-08 13:15 PT): 214 open of 537 issues; 138 open with no
  milestone; 63 with no type label; opened per PR merged 1.01 (April),
  0.56 (May), 0.93 (October), 2.02 (November to the 8th). Open issues per
  hub theme: NC memory/coherency 21, build flags/configuration 20, FFI 30,
  `sub:shell` label 12.
- `docs/adr/` on `origin/main` holds 0001 to 0007 and no `README.md`
  (`git ls-tree origin/main docs/adr/`). `.github/` holds only
  `ISSUE_TEMPLATE` (no workflows).
- The December 2025 design document the review names is in the claude.ai
  Project, not in the repository: no tracked file matches it (search for
  its markers: microkernel, YAML component manifest, TensorRT plan).

## Entry 2: Track B result (Rec 8)

- Draft PR #1245, branch `structural/architecture-record`, commit
  f920c8d2, based on 8ec0e330 (from the Track B report and `gh pr view 1245`).
- 11 files: a rewritten `docs/architecture.md`, ADRs 0009 to 0017 (status
  "accepted (backfilled)"), and a new `docs/adr/README.md` with 0008 listed
  as reserved and the rule to reserve the number in the tracking issue.
- 19 "unknown, ask the user" markers: 0009 1, 0010 3, 0011 2, 0012 2,
  0013 3, 0014 3, 0015 1, 0016 1, 0017 1 (Track B's count). 0014 raises a
  licence point: wolfCrypt is GPL-2.0 and wolfSSH GPL-3.0, and the repo is
  MIT, so `NET_SSHD=ON` images need a decision.
- Not done: the December 2025 design document is not in the repository,
  so it can't be moved. `architecture.md` now says it is superseded and
  held outside the repo.
- Stale text found and left as it was (listed in the PR body):
  `docs/shell.md` (SSH deferred), `docs/deploy/jetson-uefi-direct.md`
  (consumers need kexec), the `spinlock.h` header comment (Jetson skips
  locking), `docs/smp.md` (names `kernel/src/spinlock.c`, which does not
  exist), an `smp_boot.S` HCR_EL2 comment, `LICENSES.md` (lwIP, wolfCrypt,
  wolfSSH and Lua missing), the CLAUDE.md link to
  `docs/jetson-el2-bringup.md` (file missing), the SECONDARY_PREEMPT CMake
  help text (still says "NC-memory platforms"), and the lwIP version
  (commit message says 2.1.3, the tree is 2.2.0).
- ADR citations were spot-checked only. The Fable critic checks
  `architecture.md` only, as the brief asks.
