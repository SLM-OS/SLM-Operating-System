# Critic protocol, coherency run 3 (#1193, #1216)

`run2-critic-protocol.md` applies unchanged in substance, with
`run3-rules.md` binding critics, verdict files named
`critic-3-C<N>.md` and reproduction captures under `run3-hw/critic-c<N>/`.
This file adds what run 2 showed was missing.

## Blindness, enforced

The critic receives the component's definition of done, the issue
numbers, the evidence branch and path, and the heads to reproduce from,
only after the builder's final push. The builder's log entries are
written by the orchestrator after the critic has filed. The critic does
not open `coherency-3.md`, the builder's PR body or issue comments until
its own count, reproduction and verdict are written. A critic that finds
it has seen the builder's summary says so in the verdict file; the
orchestrator decides whether a second critic is needed.

## Reproduction

- C1: 5 boots per kernel from the same heads (NC a03fc75b, removal as in
  Entry 0), same image hashes.
- C2: 5 sustained runs each of cells C and B from the same images.
- C3: one boot per Jetson path.

## Failure modes, each checked by name

Run 1's six rows (run2 protocol table) plus:

| Seen in | What it looked like | What the critic does |
|---|---|---|
| Run 2, C2 | No critic at all; the orchestrator re-read rows itself | Not the critic's to fix: a component without a verdict is "open" at the gate |
| Run 2, report | Decisive table carried one CPU's row and two others' exec counts | Rebuild every per-CPU table from the raw files for all CPUs before reading the builder's table; an omitted row fails |
| Run 2, #1213 | A number cited with no file behind it | Strike any number without a filename |
| Run 2, #1213 | Observation tree never committed | Every evidence file names a pushed commit; reconstructed heads labelled |
| Run 2, C3 | Critic read the builder's claim first | That verdict is void; say so if it happened |
| Run 2, C3 | Builder read labctl's database and used a private BootNext helper | Disclosed breaks go in the verdict; undisclosed ones fail on process before content |
| Run 2, C2 | Cell built on a stale head | Banner hash against the claimed head, for every file, before counting |

## Verdict file

As in run 2: own count with paths; builder's claim quoted; agree or not
per number; reproduction with run ids and files; one line per failure-mode
row (run 1's six and the seven above); **pass**, **fail** or
**fail-on-completeness**, a fail naming the definition-of-done row and the
file.
