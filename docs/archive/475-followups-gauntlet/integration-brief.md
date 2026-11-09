# Brief for the #475 follow-ups integration critic (2026-11-08)

You are a fresh, harsh integration critic. You did not build any of this and you judge only from what you
run and read. Read `.gauntlet/builder-rules.md` (hardware and git rules apply to you too) and
`.gauntlet/criteria.md` (section G) in
/home/john/projects/CS-496-Capstone-SLM-Operating-System/.claude/worktrees/bridge-cse_014Q9as26ypABfoSv9WkYp6q/.

Do not read PR bodies, PR comments, issue comments or the gauntlet log; commit messages only for the
closing-keyword check. You may read issue bodies, code, docs and the evidence directories.

## Step 1: scratch branch

In your own worktree: `git fetch origin`, `git checkout -b gauntlet/followups-integration origin/main`, then
`git merge --no-ff` each branch below in this order. Resolve only textual conflicts whose correct result is
unambiguous, and record each resolution (file, both sides, result). On any semantic conflict, stop and report.
Do not push the scratch branch.

The branch list and heads are in your prompt.

## Step 2: /ci

Every step of `.claude/commands/ci.md` on the scratch branch, in order, outputs saved. Use a private copy of
any helper script under your own scratch directory.

## Step 3: hardware regression gate (only once the orchestrator gives you the board)

Build `make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO` (CUDA 12.0 shader line, record sha256),
then follow `/home/john/slmos-ref/derivatives/475-followups/hw-gate.md` exactly:
- G1 + G4: p1..p3 x3 greedy, every run decode >= 49.0 tok/s, TTFT <= 80 ms, prefill <= 75 ms; text
  byte-identical to baseline/text-pN.txt; `clocks` EMC 3199 / GPC 1020 after the runs.
- Default sampler p1 x3 (decode and byte comparison with the baseline's default replies).
- Recovery smoke in the same boot: `nvgpu cold race` running, 3 `slm gpu inject hang` during short prompts,
  every recovery without reboot and every consumer correct again within 5 s; `nvgpu cold check` at the end;
  then one more p1 at G1 speed.
- G3: 10/10 cold boots with BOOTAA64 (builder-rules.md procedure, cleanup back to SD Linux, no BOOTAA64 left).
- G4 note: the merged branch includes #1167's HMMA attention, which may change greedy text at near ties. If the
  text differs from main's, judge it against the exact CPU reference instead: build `make jetson-real-golden`
  from the scratch branch and run `slm golden 0` and `slm golden 0 batch` (#1165's real-model golden on the GPU
  program, 0.1 logits against the exact reference, llama.cpp top token); compare the text with #1167's own
  hardware run (evidence 1167/) as well. Follow the ESP hygiene rule in builder-rules.md for every ESP write.
- Expected from the parts: decode above main's 49.5 tok/s (sampler and attention gains), EMC holding 3199.
Write raw outputs to `/home/john/slmos-ref/derivatives/475-followups/integration/` with a MANIFEST.md and a
`g1-table.txt` (`prompt run decode_tok_s ttft_ms prefill64_ms`).

## Step 4: /slmos-review, once, in full

Invoke the `slmos-review` skill on the scratch branch against origin/main. Record every finding with the
PR (branch) it belongs to.

## Report (under 60 lines)

Merge results; /ci table; hardware table; review findings grouped by branch with severity; `BLOCKERS:` list
(critical or major, each with its branch); `VERDICT: READY` or `VERDICT: NOT READY`.
