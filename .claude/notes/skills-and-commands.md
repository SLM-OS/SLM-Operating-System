# Claude Code Skills vs. Commands — Workflow Reference

Notes from a 2026-04-18 analysis of John's Claude Code sessions from the
preceding week. Captures both concrete workflow-automation proposals and
the general "when to use a command vs a skill vs CLAUDE.md" framework.

---

## 1. Context — session-log analysis findings

A research agent mined ~13,500 user messages across 52 sessions between
2026-04-11 and 2026-04-18 (`/home/john/.claude/projects/*/*.jsonl`,
excluding subagent logs). Top recurring patterns, with rough frequency:

| # | Pattern | Freq | Example |
|---|---|---|---|
| 1 | "Rebase + test/doc audit + PR" combo | ~140× | *"Rebase from main. Then Make sure there are comprehensive functional test for all changes and that all relevant documentation has been updated. Then Open a PR."* — copy-pasted verbatim |
| 2 | "Address all actionable items from the review" | 27+× | Usually chained to `merge` |
| 3 | Deferred confirmations (`yes`, `continue`, `merge`) | 100+× | Human-in-the-loop gate |
| 4 | "Give me a prompt for an agent to do X" | 10+× | Often multi-agent fan-out |
| 5 | Cross-platform status report | 8+× | *"...across all supported platforms (QEMU, Jetson, Pi 5, x86-64). Ignore docs under docs/archive. Validate by code, git logs, and docs."* |
| 6 | Hardware boot-test triad | ~10× | *"Run boot tests on pi-5-1, jetson-nano-2, and test-pc (N boots each)"* |
| 7 | "File issues for review suggestions, don't fix them, then merge" | 8+× | Distinct from #2 |
| 8 | Archive / mark resolved | 6+× | *"update the docs to indicate they are done and change the name to prepend it with 'RESOLVED_'"* |
| 9 | Polling-merge sequence | 3× | Long copy-pasted `gh pr view` polling block |

---

## 2. Recommended commands

All six are **user-initiated discrete operations with predictable
inputs/outputs** — command shape, not skill shape.

### 🥇 `/ship` — single highest-leverage

Absorbs pattern #1 (140 verbatim copy-pastes). Rebases from main, runs
tests, audits test coverage for the diff, audits doc changes, pushes,
opens PR. Flags:

- `--no-pr` — push to branch, skip PR creation
- `--commit-only` — commit without rebase or push
- `--no-audit` — skip test/doc audit (rare; default on)

ROI: eliminates the single most copy-pasted paragraph in the corpus.

### 🥈 `/review-address`

Absorbs pattern #2 (and optionally #7). Fetches the current PR's review,
classifies comments as actionable vs nit, fixes actionable items,
commits. Flags:

- `--then-merge` — commit fixes and merge (default, per the observed
  data)
- `--file-issues-only` — file tickets for suggestions instead of fixing
  (absorbs pattern #7)

### 🥉 `/sitrep [features]`

Absorbs pattern #5. Produces the cross-platform status matrix, validating
against **code + git log + docs**, ignoring `docs/archive`. **Read-only
— must not update docs.** Default feature set when called bare.

### `/agent-prompt [scope]`

Absorbs pattern #4. Drafts a self-contained agent prompt (with handoff
doc if needed). Sub-variant:

- `--multi` — splits one plan into N parallel prompts along
  code-boundary lines

Must preserve explicit scoping instructions (e.g., *"skip 176, 180, 183
for now"*).

### `/hw-verify [--runs N]`

Absorbs pattern #6. Builds all three platforms, deploys via labctl, runs
`boot_test` N times on pi-5-1 + jetson-nano-2 + test-pc, captures
results. Waits for lab claim if needed.

### `/merge-when-ready <PR#>`

Absorbs pattern #9. Fire-and-forget poll-check-reschedule loop. Polls
`gh pr view <PR#> --json mergeStateStatus,statusCheckRollup`; merges when
CLEAN + SUCCESS; reschedules a wake-up when IN_PROGRESS.

### Anti-pattern — do not automate

Pattern #3 (`yes`, `continue`, `merge`) confirmations. They are the
human-in-the-loop gate, used ~110× in the corpus. A skill that skipped
the proposal would remove the checkpoint, not add value.

---

## 3. Skills vs. commands — the distinction

Claude Code has two overlapping mechanisms for workflow automation:

| Feature | Commands | Skills |
|---|---|---|
| File location | `.claude/commands/*.md` or `~/.claude/commands/` | `.claude/skills/<name>/SKILL.md` or `~/.claude/skills/` |
| Invocation | Explicit: `/name [args]` | Relevance-matched on each turn based on skill `description` |
| Predictability | Deterministic | Probabilistic (matcher can miss) |
| Arg handling | Natural `<arg>` and `--flag` parsing | Matches against free-form prose |
| Best for | "I want to do X right now" | "When X comes up, know how to handle it" |

### Command-shape test

A proposal is command-shaped if:

- The user would explicitly type `/name` to invoke it.
- It takes well-defined arguments or flags.
- It produces a predictable sequence of actions.
- Missing it once is worse than invoking it unnecessarily.

All 6 proposals above pass this test.

### Skill-shape test

A proposal is skill-shaped if:

- It's *reference knowledge* Claude should consult when relevant.
- The triggering context is diffuse prose, not a user decision to run it.
- Wrong-timing invocation is cheap; missed-invocation is also cheap.
- Examples: "how to write a NIC driver," "Jetson UPHY clock bring-up
  sequence," "Falcon IMEMT per-page semantics."

---

## 4. Skills vs. CLAUDE.md — what belongs where

Both are "reference content Claude consults," but they differ in
loading semantics:

- **CLAUDE.md**: always loaded. 100% presence. Every turn pays the
  token cost. No relevance gate.
- **Skills**: loaded only when the relevance matcher fires on the skill's
  `description` frontmatter. Tokens paid only when relevant.

### Rule of thumb

| Content | Location |
|---|---|
| Invariants that apply every session | CLAUDE.md |
| Conventions Claude must not forget (git, labctl, etc.) | CLAUDE.md |
| Ambient project context (platforms, build commands, lab setup) | CLAUDE.md |
| Deep procedural knowledge used occasionally (how to write a driver) | Skill |
| Reference material for rare domains (Falcon bringup, UPHY config) | Skill |
| Large bodies of text irrelevant to most turns | Skill |

### The cost/benefit math

The full SLM-OS CLAUDE.md stack (project + kernel + runtime) is ~4-5 KB.
That's 1-2% of a 200k context window. Context savings from migrating to
skills are **real but small**. Risk of a skill miss on a critical
invariant is **small but catastrophic**.

Optimize for **readability and scoping**, not context savings.

### Content currently in CLAUDE.md — keep where it is

- Git commit/push rules ("ask before committing", "never skip hooks")
- labctl as the only hardware interface
- Platform build matrix and QEMU safeguards
- Post-Change Checklist
- Issue-tracking conventions
- ASCII box / TODO emoji formatting
- Writing-style rules

If Claude forgets any of these mid-session, the session goes sideways.
Skills' relevance gating is not reliable enough for that.

### Content currently in CLAUDE.md — candidates for migration to skills

- `kernel/CLAUDE.md` §"x86-64 LAPIC post-kexec gotcha"
- `kernel/CLAUDE.md` §"x86-64 GA10x Falcon — Per-page IMEMT"
- `kernel/CLAUDE.md` §"x86-64 GA10x — SEC2 BROM aperture state"
- Nouveau / nvgpu reference file catalog
- Detailed Jetson CBB peripheral map

All of these are long, narrowly scoped, and often irrelevant to the
current turn. Would be healthier as scoped skills.

### Recommended first experiment

One skill: `~/.claude/skills/kernel-gpu-bringup/SKILL.md` containing the
GSP / Falcon / Booter / ACR / SEC2 material from `kernel/CLAUDE.md`.
Roughly 100 lines of content relevant only during GPU bringup sessions —
not most of them.

Measure whether relevance matching fires reliably over a week of use. If
it does, consider the same treatment for a `platform-boot-nuances`
skill (x86-64 LAPIC gotchas, Pi 5 armstub, Jetson UEFI direct). If
relevance matching fails, leave the content where it is.

---

## 5. Priority order for implementation

1. `/ship` — highest ROI single command
2. `/review-address` — pairs well with `/ship`
3. `/sitrep` — read-only, low-risk, high-signal
4. `/hw-verify` — useful once hardware work resumes
5. `/agent-prompt` — useful but more nuanced to get right
6. `/merge-when-ready` — nice-to-have
7. Migrate `kernel-gpu-bringup` content from `kernel/CLAUDE.md` to a
   skill; evaluate before expanding the pattern

---

*Compiled 2026-04-18.*
