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

## Entry 3: Track B critic (Fable)

- Fable critic, blind, read-only. It checked `architecture.md` at f920c8d2
  against `origin/main` 8ec0e330. Result: 170 claims, 154 CONFIRMED (7 of
  them with caveats), 6 REFUTED, 10 UNCONFIRMED. All 335 backticked tokens
  were matched against the tree. Full table:
  `scratchpad/critic-B.md` (session scratch, not committed).
- REFUTED:
  - (1) "every path exists on main": the ADR README and 0009 to 0017 are
    new in this PR.
  - (2) "one toolchain file per platform": there are two.
  - (3) the order of steps in `boot.S`: the EL2/VHE blocks run before
    `msr daifset` at `primary_cpu`.
  - (4) "the main task starts the shell": `kernel_main` calls
    `shell_start()` itself.
  - (5) "every platform runs LDAXR/STXR": x86-64 uses
    `__atomic_exchange_n`.
  - (6) "Hailo-8" should be Hailo-8L.
- UNCONFIRMED: 8 claims of the form "ADR 0009 to 0017 records X". These
  can't be checked against main because the ADRs exist only on the PR
  branch. One concerns whether `armstub8-2712.S` is on the shipped Pi 5
  boot path, and one the December 2025 document, which is not in the repo.
- Action: the refuted claims and caveats went back to Track B to correct
  on #1245. The ADR bodies remain unchecked by any critic.

## Entry 4: Track B corrections

- #1245 updated to 6b00c3b3 (`git log -1 origin/structural/architecture-record`).
- All 6 refuted claims are corrected, along with the 7 caveats and
  unconfirmed claim #39. The Pi 5 boot row now names the TF-A BL31 loaded
  as `armstub8-2712.bin`, per `deploy/pi5/config.txt`. The PR body has a
  "Critic pass" section.
- Still open: the 8 "ADR 0009 to 0017 records X" claims. No critic has
  checked them.
