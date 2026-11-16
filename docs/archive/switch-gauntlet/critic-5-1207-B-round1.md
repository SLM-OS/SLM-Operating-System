FAIL — the rows and counts reproduce (one wrong number in Entry 48), but one inference is stated as fact, the new verifier can fault on Jetson while dumping, and three in-tree texts overstate.

# Critic B: Part B (#1207), fix/pmm-coalesce 647c2c33 against 661204e6

Stage 1 derivation: `stage1-derivation.md` (same directory), written before the claims were opened.
Nothing was run; all results are read from the captures, the code and the ELFs.

## What reproduces (matches)

- QEMU x50 loops: 50 of 50 for every test in every loop at 661204e6, e3c22600, f650b695, f55c5293, 647c2c33.
- QEMU full suites: base 20 runs, PMM `48/45/0/3` in 20, 4 failing runs (run14 net_burst; run15, run20 litmus; run18 litmus +
  Lua data abort + timeout). e3c22600: 8 of 20 fail on litmus only (runs 01-04, 06-08, 19). f650b695: 2 of 13. A/B: base 10
  of 10, f55c5293 8 of 10 (run05 litmus, run08 runner_parks). 647c2c33: 5 of 5. PMM_CHECK=ON 107 walks ok. x86-64 pass. CI 11 steps rc=0 twice.
- Old code fails the new tests: 5 of 5, at the lines quoted; 7b0af7d0 has the same tree as 21303db1 (`git diff` empty; disclosed in B-README).
- Halt signature in QEMU: Data Abort (WRITE), ESR 0x96000044, FAR 0x1000004071633f, x0 = -1; PMM_CHECK panics at the merge (0x420d6000, order 1).
- Jetson old allocator + PMM_CHECK: 2 of 2 complete, 14 failures, 107 walks ok, reserves 189 / 190, 0 unaligned, 0 unmerged, 533 / 610 blocks.
- Jetson f55c5293: PMM halt in boots 01 (order 7, count 29; 30 at boot) and 05 (order 5, count 48; 50 at boot); complete 03, 06, 09, 10; lost in the scheduler suite 02, 04, 07, 08. 2 in 10.
- Jetson 647c2c33: PMM halt in boot 04 (order 4, count 61); complete 03, 05, 07, 08, 09; lost 01, 02, 06, 10. 1 in 10. Findings other than the self-test: none in 9 boots, two in boot 04.
- Coalescing pair: FAIL in 2 of 2 old-allocator boots, PASS in 20 of 20 fix boots and 3 of 3 Pi 5 boots.
- Complete Jetson boots: the same 11 failing names (12 failures) in all 9; the old-allocator 14 are those plus the pair.
- Boot 04: every quoted line number, address, count, time and byte pattern; 0x139580000 is 0x120000 above the pushbuf and in no printed range.
- Trigger logs: 2 + 10 + 10 triggers for 22 saved boots; ELF sha prefixes match the ELFs in `$SP/b/img`. The one loop with 0 triggers is disclosed in B-README.
- Pi 5: 3 of 3 complete, 18 failures, the same 18 names in each, PMM `59/58/0/1`, self-test finding only; restore banner b42b2c1d.
- Layout: `.data`, `.bss`, `__kernel_end` equal between the f55c5293 and 647c2c33 Jetson suite images.
- Demo: tick finding at 2420 ms, pop finding at 3961 ms, then the panic.

## Findings

1. **Wrong number (low). Entry 48: "the three earlier halts of this session have no bytes."** There are two earlier halts
   (f55c5293 boot01, boot05). B-finding-boot04.txt counts three including boot 04 itself, which is the one that has the
   bytes. Fix: "the two earlier halts".

2. **Overstated (high). Entry 48 and B-finding-boot04.txt: "the write happened during test_suite_gpu", and "that rules out
   the xHCI hand-over window for this occurrence".** What the capture shows is that the CPU's view of the header at
   0x139580000 was intact at the walk after test_suite_oplib_gpu_as and zero at the walk after test_suite_gpu. That is a
   bracket on when the CPU saw the change, not on when a device wrote. Two cases break the step from one to the other:
   - A device that does not snoop the CPU caches (kernel/CLAUDE.md says the DMA devices here do not) writes DRAM under a
     line the CPU still holds; the walks keep reading the cached copy. The DRAM content appears when the line leaves the
     cache. The xHCI window, and the log's own preferred writer (the GPU through its page tables), are both of this kind.
   - test_suite_gpu is the suite that drops lines: `cache_invalidate_range` in `kernel/gpu/cache.c:84-98` is DC IVAC
     (discard, no write-back), called by `test_cache_invalidate_executes`, `test_gpu_sync_for_cpu_executes` and
     `test_gpu_buffer_workflow`, and it also allocates and touches megabytes. A header written at pmm_init and re-read by
     every walk can sit dirty in cache for the whole boot; an IVAC that reaches it reverts it to pre-boot DRAM content,
     which here is zero like the 64 bytes before it. That would be a lost CPU write, with no writer at all in the window.
   Neither is shown. Both are consistent with every byte in the capture, and the second explains "kexec only".
   Supported wording: "the block's header read intact after test_suite_oplib_gpu_as and zero after test_suite_gpu; when
   the bytes changed in DRAM is not bounded from below". The xHCI exclusion should be withdrawn or marked as holding only
   for a write the CPU sees at once. "The MNIST GPU path on the inherited channel is the writer" is labelled an inference
   in the log, correctly; it goes no further than "one candidate". The supporting remark in B-finding-boot04.txt that a
   64 KB-aligned 64 KB block "is the size and alignment of the hand-off's own buffers" is empty: every order-4 block is
   64 KB at a 64 KB boundary by construction, and the zeroed extent is known only to be at least 128 bytes.

3. **Code defect (medium). `pmm_dump_block` / `pmm_dump_words` (`kernel/mm/pmm.c:683-715`, called from `:789-796`) read
   memory that is not mapped on Jetson.** The only guard is `[heap_start, heap_end)`. On Jetson heap_end is 0x280000000
   (capture: "Heap range: 0x82d80000 - 0x280000000") while RAM is mapped to 0x240000000 and the OP-TEE carveout
   0xBE000000-0xC2000000 is left unmapped (`kernel/mm/vmm.c:1518-1564`). Cases: (a) "64 bytes before" for the free block
   at 0xC2000000, the first block of region 2, reads 0xC1FFFFC0; (b) "the element the link points at" is read for any
   page-aligned bad link in that range, including the carveout and 0x240000000-0x280000000. The result is a data abort
   inside the verifier, under pmm_lock, possibly from the timer tick, in place of the evidence the code exists to print.
   Fix: read a range only when its page is one the allocator manages (the page's `block_state` is not BLOCK_ALLOCATED),
   or check `is_mapped`; skip the "before" read when the previous page fails that test.

4. **Code (medium). `free_list_pop` (`pmm.c:930-946`) detects "head's next link is NULL with blocks still counted",
   prints, and carries on.** The damaged block is handed to the caller, and the orphaned tail still has a `prev` pointer
   into it; a later merge that unlinks that orphan (`free_list_remove`, `:898-899`) writes 8 bytes into the now-allocated
   page. Boot 04 ran 125 ms in that state. The non-NULL bad-link case already panics at the pop; the NULL case should too
   (after the report), or the comment should say why running on is wanted in production builds. This does not mask the
   halt; nothing in the change does.

5. **Overstated in the tree (medium).** These are durable texts and say more than the rows:
   - `kernel/CLAUDE.md:823-825` and `docs/memory-map.md:355`: "about one suite boot in five". This session: 3 in 20; the
     log itself says 1 in 10 against 2 in 10 is no evidence of a change. Write "1 to 2 in 10".
   - `kernel/mm/pmm.c:640-641` "The allocator's own operations cannot do that any more, so the writer is somewhere else",
     `kernel/CLAUDE.md:825` "cleared by something other than this file", `docs/memory-map.md:358`: stated as fact. It is
     shown for one boot that no list operation took the block (its page state was still "listed"); that a writer exists
     at all is finding 2's open question. Also "next link is found NULL" understates boot 04: both links and at least
     128 bytes across a page boundary read zero.
   - `pmm.c:996-1005` (comment on `is_buddy_free`) and `kernel/CLAUDE.md` "the #1207 data abort" attribute the hardware
     abort to the merge defect, as e3c22600's message does (question 6).

6. **Gap (low). The hardware rows do not separate the test fix from the allocator change.** The drain and the allocator
   fix landed in one commit (e3c22600); there is no row of the old allocator with the drained tests. The test-defect
   reading rests on the QEMU reproduction (one iteration at 7b0af7d0, the same two messages at the same lines) and on
   the order-0 counts, which is enough for "supported", not for "shown on hardware by isolation".

7. **Not shown equal (low). QEMU litmus failures: 3 of 30 full suites at base, 11 of 48 at the fix heads.** The log's
   wording ("no evidence that the change moves the rate; too few runs to exclude a small change") is fair. But the head
   that adds a once-a-second walk under pmm_lock with IRQs masked (647c2c33) has only 5 full-suite runs. If a PR text
   says the suite is unaffected, it should give these counts.

8. **Gap (low). `test_verify_reports_a_cleared_link` has no repeat row.** Entry 46's "the new tests x50" is the five
   older tests. The sixth ran once per full suite (5 QEMU, 9 Jetson, 3 Pi 5, all pass).

9. **Generalised from two boots (low). Entry 45: "mechanisms 1 and 3 of Entry 43 have no source at boot here."** Two
   boots, neither a halting boot, with a reserve list that differs boot to boot (189 / 190) and a heap that PMM_CHECK
   moves by 2 MB (647c2c33's own commit message). "Had no source in these two boots."

10. **Not filed, as far as the material shows (low). `nvidia_alloc(GPU_MEM_ALIGN_2MB)` leaks 2 MB per call.** It
    allocates order 10 and `nvidia_free` returns order 9 (`kernel/gpu/gpu_nvidia.c:186-227`); PMM_CHECK prints it as the
    "partial free" (caller 0x8009eee8 = nvidia_free), and free pages drop by 512 after its first use. Pre-existing, not a
    corruption, but it is a defect this session surfaced and it sits in the suite under suspicion.

11. **Trivial. Entry 48: boots 06 to 10 "ran from 21:05 UTC".** The trigger log starts the sixth at 20:56:48 and fires it
    at 20:58:18.

## The seven questions

1. **Coalescing failures are a test defect, fixed.** Supported. QEMU: reproduced with lone free pages on the old code,
   50 of 50 after. Hardware: 2 of 2 fail before, 20 of 20 Jetson and 3 of 3 Pi 5 pass after. Caveat: finding 6.
2. **The merge defect reproduces the signature in QEMU but is not what halts the board.** Right reading. The QEMU
   precondition is built with a test hook; with the exact-order merge in place the board still halts 3 times in 20; two
   PMM_CHECK boots show no unmerged pair at init. What is shown is that the defect is not needed for the halt; that it
   never contributed before is not shown and not claimed.
3. **2 in 10, then 1 in 10: not fixed, no evidence of a rate change.** Right. All 20 boots passed through the PMM suite
   before any scheduler-suite loss, so the denominators hold.
4. **"The write happened during test_suite_gpu."** Partly supported, stated more strongly than the evidence allows
   (finding 2). "MNIST on the inherited channel is the writer": an untested candidate. Not established for boot 04, and
   nothing at all is known about the two f55c5293 halts beyond their position.
5. **Closing keyword for #1207 or #1228.** No; agreed. Not for #1109 either: the PR removes two of its failures and 12
   remain. The seven commit messages use "Refs" only; I found no closing keyword in them.
6. **e3c22600's "That is the data abort recorded on #1207".** History is not rewritten, so three things carry the
   correction: the PR body; the squash-commit message, which GitHub pre-fills from the commit messages and must be
   edited by hand so that sentence does not reach main; and the in-tree comments of finding 5, third bullet. Suggested
   sentence: "This sequence produces #1207's signature in QEMU. It is not shown to occur on jetson-nano-2, and the halt
   persists with it fixed (3 of 20 kexec suite boots)."
7. **Unsafe to merge as it stands?** No production allocation path is made worse, and the count-without-a-list panic is
   an improvement over the wild write. Before merge: finding 3 (a fault in the diagnostic on Jetson), a decision on
   finding 4, and the wording of finding 5. Lower: three test hooks are linked into production images
   (`pmm_test_add_region` lists arbitrary pages if anything ever calls it); commits 21303db1 and e3c22600 do not build
   as production kernels (disclosed; harmless under a squash merge, a trap for bisect otherwise); a pop-path finding
   prints about 35 lines under pmm_lock with IRQs masked.

## Not checkable from the material (or not checked)

- Whether the once-a-second tick walk ran on the Jetson. It is silent without a finding. Timer ticks advance in boot 04
  and the walk fires in the QEMU demo; no hardware line shows it.
- Whether 0x13957f000-0x139590000 is mapped in either channel VM or lies in a gr_ctx extent: those ranges are counted in
  the capture, not listed.
- Entry 43's numbers from the earlier session's captures (104 to 132 free single pages; 5 of 5 / 5 of 5 / 5 of 5): not
  opened.
- That the Pi 5's 18 names equal Entry 39's list: the three boots agree with each other; the earlier file was not opened.
- "16 bytes differ per image, all `__LINE__` immediates": the file gives the count and equal sizes; the nature of the 16
  bytes is not in it.
- That each capture is the verbatim labctl output, the board's as-left state, and that the images were built from clean trees.
- Any effect of the verifier on the halt rate: 1 in 10 against 2 in 10 cannot show one.
