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
