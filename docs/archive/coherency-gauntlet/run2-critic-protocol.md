# Critic protocol, coherency run 2 (#1193)

One critic per component, assigned after the builder finishes. The critic
has not read the builder's branch log entries, reports or chat. It receives
the component's definition of done, the issue numbers, the evidence path,
and the branch heads it must reproduce from. Nothing else. `run2-rules.md`
binds critics too.

## Order of work

1. Open the raw artifacts and count. Write the critic's own count.
2. Reproduce (below).
3. Write the verdict against the definition of done.
4. Only then read the builder's claim (the issue comment, PR body and the
   builder's entries in `coherency-2.md`) and compare. Reading the claim
   first grades the prose; do not open `coherency-2.md` before step 3 is
   written down.

## What a pass requires

- Every number in the builder's claim traces to a file in the evidence
  branch, and the critic opened that file and counted. "20/20" means 20
  files or 20 rows found and 20 passes counted.
- Every capture is complete: a `bench stealing` capture has one line per
  CPU for every CPU on the board; a boot log runs from power-on (or kexec
  trigger) to prompt; a test run prints every test name. A cut, filtered
  or reassembled capture is treated as missing, and the component fails on
  completeness before anything else is read.
- The criterion applied is the one in the definition of done. A builder
  argument that 19 of 20 is enough, or that another test is a better proxy,
  is recorded and the component fails. Redefining the bar is the
  orchestrator's decision and does not happen this run.
- The result reproduces. C1 and C2: re-run at least 5 of the 20 on the same
  board from the same branch head and get the same answer. C0: build from
  the branch and run `/ci`. C3: boot the image once.
- A fix is shown failing on the old code. A root cause with no
  demonstration that the old code misbehaves under the new check is a
  hypothesis.

## Run 1 failure modes, each checked by name

| Seen in run 1 | What it looked like | What the critic does |
|---|---|---|
| Selective summary | Passing boots reported, a failing one left out of the table | Count files, not table rows; any mismatch fails |
| Truncated capture | `tail -3` on per-CPU output, dropping stalled CPUs | Reject any capture that is not the whole output |
| Assembled register file | One register dump built from several boots | Require boot id and timestamp on every register read; cross-check the serial log |
| Redefined criterion | "Coherent enough for the removal" for the stated bar | Apply the bar as written; record the proposed redefinition |
| Hypothesis stated as finding | H1 to H3 entered as conclusions without the separating experiment | For every causal claim, name the experiment that separates it from the brief's alternative and check it ran |
| Stale base | Pre-rebase evidence attributed to the rebased branch | Match the boot banner hash to the claimed branch head |

## Hardware

The critic touches a board only when the orchestrator grants it, and uses
labctl MCP tools and `/deploy-and-test` exactly as `run2-rules.md` says.
Reproduction captures go on the same evidence branch under
`run2-hw/critic-c<N>/`.

## Verdict file

`docs/archive/coherency-gauntlet/critic-2-C<N>.md`, returned in the final
report as full text (the orchestrator commits it to `gauntlet/coherency-2`):

1. The critic's own count, with file paths.
2. The builder's claim, quoted.
3. Agree or disagree, per number.
4. Reproduction result with run ids and files.
5. The failure-mode table above, one line per row: checked how, result.
6. Verdict: **pass**, **fail**, or **fail-on-completeness**. A fail names
   the row of the definition of done and the file.
