FAIL — narrowly: no code defect blocks a merge and every number I could check is right, but the PR body and the #1207 comment say "every DC IVAC call site was read" when two were missed, they present "a header write lost from the cache" without the tree's own record that these cores write back on DC IVAC, and one row description is inaccurate.

# Critic B, round 2: fix/pmm-coalesce 02ebdad8, CLAIMS2-draft-pr-body.md, CLAIMS2-draft-1207-comment.md

Nothing was run. Rows are read from the raw files on `gauntlet-5-hw-jetson` 608ee693; code from `.wt/b` at 02ebdad8.

## Derived first (all match the drafts)

- Interleaved A/B at 02ebdad8, 15 pairs: base 661204e6 fails 4 of 15 (runs 01, 05, 07, 08), head fails 1 of 15 (run 06), all
  on `test_litmus_patterns_pass`. PMM suite `48/45/0/3` and `55/52/0/3`, no fault, in all 30.
  With round 1's 10 pairs: base 21 of 25 pass (4 litmus), head 22 of 25 (2 litmus, 1 runner_parks).
- x50 at 02ebdad8: coalescing pair, checkerboard + exhaustion, the five allocator tests, and both verifier tests: 50 of 50 each.
- `PMM_CHECK=ON` full suite at 02ebdad8: 107 walks ok, no assertion, the run fails on litmus (rc 2). x86-64: pass, PMM `55/52/0/3`.
- `/ci` at 02ebdad8: steps 1 to 3 rc 0; step 4 rc 2 on litmus only, stop; resumed: steps 4 to 11 rc 0. Runtime 308 and 328 passed.
- Demo at 02ebdad8 + demo patch: tick finding at 1761 ms, pop finding at 3261 ms, then
  `PMM: order 0 free list head 0x420ee000 has next link 0x0 with 5 blocks counted`. The "64 bytes before" line reads
  "not in a free page of the heap, not read" in both findings, so the new guard is exercised.
- Not-interleaved totals: 20 + 20 + 13 + 5 = 58 suites, PMM clean in all. 176 bytes of verifier data (4 + 160 + 8 + 4).
  0.85^30 = 0.0076.
- No closing keyword next to an issue number in either draft.

## Code at 02ebdad8

- `pmm_dump_readable` (`pmm.c:695-706`): in the heap range, 64 bytes in one page, page not BLOCK_ALLOCATED. Holes are marked
  allocated at `pmm_init` and only `pmm_add_region` frees pages, so every readable page is mapped RAM. Correct on Jetson,
  Pi 5, QEMU and x86-64, and at the end-of-`pmm_init` call. Cost: the 64 bytes before a block are no longer shown when
  the page before is in use. Round-1 finding 3: resolved.
- `free_list_pop` (`:959-976`): a non-NULL next must be in the heap, aligned, and carry `BLOCK_FREE_HEAD | order`; a NULL
  next with more than one block counted, or a next that fails the test, is reported and panics. The write through
  `block->next->prev` now only happens on a listed block. No legitimate list state trips it. Round-1 finding 4: resolved.
- The test checks allocated, free, straddling, out-of-range, wrapping and zero addresses. It reads a freed page's state
  after the free without holding it, so another CPU allocating that page in between would fail it; 50 of 50 here.
- Nothing newly broken that I can find.

## Findings

1. **Overstated (medium). Both drafts: "every DC IVAC call site that can run on Jetson was read".** `B-ivac-callers.txt`
   misses:
   - `slm_gpu_sync_for_cpu` (`kernel/src/slm_ffi.c:233-243`), which builds a buffer from a caller's pointer and size and
     reaches `nvidia_sync_for_cpu`, the DC IVAC loop. Its callers are in the Rust runtime:
     `runtime/src/mm/model_mem.rs:1580` (`gpu_unmap`, whole model size) and `runtime/src/inference/gpu.rs:259`.
     The suite's first suite is the model-memory smoke test. Not traced by anyone.
   - the `dc isw` set/way loops on each secondary (`kernel/arch/arm64/smp_boot.S:246, 269`; `SETWAY_INVAL` defaults ON),
     which run after `pmm_init`. The file lists `smp_boot.S:162-175` only.
   Fix: add both to the file and say "the call sites found", or trace them.

2. **Missing fact that changes a candidate (medium). The tree says these cores do not discard on DC IVAC.**
   `kernel/arch/arm64/mmu.S:46-50` and `kernel/sched/smp.c:684-688`: "On Cortex-A76 and A78AE DC IVAC is executed as
   DC CIVAC", i.e. a dirty line is written back; `smp_boot.S:205-213` says the same of DC ISW. If that record is right,
   no DC IVAC on jetson-nano-2 can lose a header write, and candidate 3 of the comment ("a header write lost to a cache
   discard") has no mechanism through any of the listed call sites. The new comment at `pmm.c:645` ("DC IVAC discards
   dirty lines"), `B-ivac-callers.txt`, the revised `B-finding-boot04.txt` and both drafts state the opposite without
   mentioning it. I raised this candidate in round 1 without having read those comments; the correction applies to my
   finding as much as to the builder's text. What survives is the other half: a device that does not snoop the caches
   writing DRAM under a line the CPU holds clean. That needs the header to have been written back before the device
   write, which narrows it. The bound "when DRAM changed is not bounded from below" still holds for that case.
   Fix: cite the in-tree record next to candidate 3 in both drafts and in `pmm.c:645`, and mark the candidate as
   "architecturally possible; excluded on these cores if that record holds". Experiment 1 keeps its value, because it
   also separates stale DRAM from a write of zeros.

3. **Inaccurate (low). PR body: "4 and 4 lost after the PMM suite at `test_preempt_flag_follows_migrated_task`".** In
   647c2c33 boot01 that test prints PASS (line 9019) after a cpu4 fault at line 8946, and the capture ends in the
   coherency litmus; boot02 ends in a cpu1 instruction abort with no line of that test; f55c5293 boot02 likewise has no
   line of it. Accurate: "a CPU faulted or the boot stalled in the scheduler suite, around that test, and the suite did
   not finish".

4. **Open item not in the PR (low, not blocking). `free_list_remove` still follows a listed block's own `prev` and
   `next` unchecked** (`pmm.c:924-934`; merge path and `pmm_alloc_pages_low`). A zeroed header in mid-list makes it set
   the list head to NULL: the later panic is named, but the damaged block is not dumped. The PR's item 3 reads as if
   every path now reports the block. One sentence would carry it.

5. **Wording (trivial).** `docs/memory-map.md:347-350` says the pop panics when the link "is not a block address"; the
   code and the PR say "not a listed block of this order". `B-finding-boot04.txt` still says of the skip-the-MNIST-tests
   experiment "the writer is confirmed"; skipping those tests also removes their cache and allocation activity, so a zero
   rate would narrow the cause to that suite's tail, not name a writer. The drafts do not repeat that sentence.

## Round-1 findings

| # | Status |
|---|---|
| 1, 11 | Log errors; with the orchestrator. Not in the drafts. |
| 2 | Resolved in the drafts, the finding file and the in-tree texts ("what that bounds"), subject to finding 2 above. |
| 3 | Resolved: `pmm_dump_readable`, its test, x50, and the demo. |
| 4 | Resolved: the pop panics; documented. |
| 5 | Resolved: "1 to 2 in 10", "3 of 20", the merge-defect sentence, no asserted writer. |
| 6 | Carried honestly in both drafts. |
| 7 | Carried with new rows: 4 of 25 against 2 of 25 on litmus, and the non-interleaved counts are given. Fair as worded. |
| 8 | Resolved: `B-qemu-fix-02ebdad8-verify-x50.txt`, 50 of 50 for both verifier tests. |
| 9 | Resolved: "in these two boots ... do not settle it for every boot". |
| 10 | Carried: referred to #1240. I could not check that issue's text. |

## The questions put for this round

- **Does the PR say plainly that 02ebdad8 has no hardware rows?** Yes: bold, near the top, and again in the comment's
  first line.
- **Is the argument that the three GPU tests cannot reach a listed block's first line sound?** Yes. `nvidia_alloc` sets
  the size to whole allocated pages; the range is page-aligned at both ends; the invalidate runs while the buffer is
  held, before `gpu_free`; the free then rewrites the header. The 2 MB-aligned test issues no invalidate.
- **The order-10 / order-9 partial free:** after `nvidia_free` the buffer struct is zeroed, so nothing later
  invalidates, writes or frees the other 2 MB; it stays allocated for the rest of the boot. A leak only.
- **Squash message:** the instruction is in the PR body, bold. Sufficient if followed.

## Not checkable or not checked

- #1240, #1274 and #1278 as described; "the 8 of 20 ran beside image builds"; "104 to 132 free single pages"; the Pi 5
  list against the earlier closeout file.
- "8 `mov w2, #line` instructions": the file's diff shows such lines; I saw six of them and did not count the rest.
- Whether the in-tree statement that A76 / A78AE execute DC IVAC as DC CIVAC is itself correct. I take it as the
  project's record (it cites the #1193 audit); I did not verify it against the TRMs.
