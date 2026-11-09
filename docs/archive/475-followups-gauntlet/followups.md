# #475 follow-ups gauntlet log

Goal: close out the GPU-inference follow-ups to #475 on jetson-nano-2 under UEFI-direct boot, overnight
and unattended. Rules: [builder-rules.md](builder-rules.md). Criteria: [criteria.md](criteria.md). Raw evidence:
`~/slmos-ref/derivatives/475-followups/<issue>/`.

## Budget

- Started 2026-11-08 05:42 UTC. Limit 10 h (15:42 UTC). Integration starts by 13:15 UTC at the latest.
- At most 4 critic rounds per issue. Move on when the same failure recurs twice without a new strategy, or
  two rounds in a row show no gain.
- #1161 investigation capped at 2 h.

## State found at start (main @ c69438a2)

- jetson-nano-2: SD Linux (up 1:55), nvpmodel 25W, recovery_mode released, no claims.
- NVMe ESP `\EFI\SLMOS\SLMOS.efi` sha256 789cba62..., copied to `/root/SLMOS.efi.found-20261108`; no
  BOOTAA64.efi. BootOrder 0008,0001,0007,0009,000D,000C,000B,000A,0003,0005,0002,0004,0000,0006
  (`/root/efibootmgr-found-20261108.txt`).
- `/root/slmos.elf` 098f3163...; `/root/slmos.elf.prev` a531a785... was already there (left alone).
- Claim taken 05:46 UTC by `claude-code-475-followups`, expires 15:46 UTC.

## Plan

- GPU-chain order (one owner at a time, stacked branches, handoff by `git merge`): #1173, then #1159 if a
  fix is needed, then #1171, then #1165's hardware part if it needs chain files, then #1167's integration.
- Hardware queue in the user's priority order; slots of at most 45 min unless extended.
- #522, #1165 (op vector), #1167 (kernel authoring), #1161 (reading) run in parallel off the chain.

## Issues

| Issue | Builder | Branch / PR | Status | Critic rounds |
|---|---|---|---|---|
| baseline + #1159 | hw-baseline | n/a | done 06:20. #1159: no in normal use; yes in one corner (close deferred during another session's timeout dump drops its pin while PTEs remain) -> small chain fix after #1173 | n/a |
| #1173 | b1173 | gauntlet/1173-gpu-vm-lock, PR #1177 (draft), 30a0a155, Fixes #1173 | HW done 08:06: vmrace 512k rounds 0 bad/0 stale, 20 launches overlapped, wpool 64 OK, p1 identical; G1 median 49.503 (49.493-49.508), TTFT 75, prefill <=72.65; default sampler 40.80-40.86; G3 10/10; G4 identical; /ci green. Critic r1 PASS 08:22, no gap; PR switched to Fixes #1173. COMPLETE (minor items left for integration review) | 1 |
| #1159 | b1159 | gauntlet/1159-deferred-close-pin, PR #1181 (draft, stacked on 1173), 06ce311e, Refs #1159 | HW done 09:20: pre-fix image unloads during a deferred close (57.4 s, not refused); fix image refuses at once, unloads after the hold; G1 49.417-49.519 (median 49.511; p1 ~49.42 all 3), TTFT 75-76, prefill <=73.07; G4 identical (r1 cmp; r2/r3 bytes=); /ci green; G3 deferred to stack/integration. Critic r1 PASS 09:33; fix-ups in 06ce311e (TLB-fail range table, release store, doc row; new test fails on 28af2c8b); issue comment posted. COMPLETE (Refs; CPU-only part open) | 1 |
| #1171 | b1171 | gauntlet/1171-chan-recovery, PR #1183 (draft, stacked on 1159), eae99c99, Refs #1171 | H1+H2 08:09-09:05: 20 hangs + 5 faults recovered without reboot (L2, ~125 ms), consumers correct 157-181 ms, replies identical. Gate 09:42-10:13 (1c392ce5): G1 median 49.517, G4 identical, G3 10/10 (covers 1173 and 1159), 5/5 hangs recovered, but 1 wrong oplib RMSNorm in 18,375 race rounds (#1186). Critic r1 FAIL 10:58 on that result. Diagnostic 10:20-10:41 and round 2 11:19-11:48: control 0 wrong in 81,735; main + harness baseline 0 wrong in 105,368; 14 more hangs 0 wrong in 98,187; overall 1 in 305,173 with 51 recoveries, not separable from main (CI 8e-8 to 1.8e-5 vs main 0 to 3.5e-5); code trace finds no recovery cause. Critic r2 PASS 11:56, no gap. Integration fixes 2952e885/eae99c99 (9 s text, merged 1159 06ce311e). COMPLETE (Refs; close by hand once the 1173/1159 stack lands; #1186 open) | 2 |
| #1165 | b1165 | gauntlet/1165-real-goldens, PR #1176 (draft), c3e005c7, Refs #1165 | HW done 09:27: `slm golden` on GPU program decode 12/12 top tokens max err 35 milli, 64-row batch 12/12 33 milli; drop-bias FAIL (28.8, 4/12), restored; per-op tiers 0 attempts; /ci green (bb708a77). Critic r1 PASS 09:35; fix-ups in c138447b (LICENSES note, comment, provenance corrected: image = bb708a77 + uncommitted 6d9fac3a edit, drop-bias preconditions documented). COMPLETE (Refs; schedule not met) | 1 |
| #522 | b522 | gauntlet/522-sampler-one-pass, PR #1179 (draft), 762582bb, Refs #522 | HW done 09:41: greedy decode 50.07-50.18 (median 50.165 vs main 49.508), TTFT 74-76, prefill <=73.21; default sampler 49.96-49.97 vs main 40.80-40.87 (+22%), TTFT 75; G4 + default replies byte-identical to main; /ci green; G3 deferred to integration. Critic r1 PASS 09:55; doc fix + PR residual line in 02078840; Refs kept (fold impossible at T!=1 without changing tokens; recommend manual close). COMPLETE | 1 |
| #1167 | b1167 | gauntlet/1167-gqa-hmma, PR #1178 (draft, base gauntlet/1171-chan-recovery), ee776b76 | phase A: HMMA GQA beats w8 at every kv (native). Phase B HW 11:51-12:09 on ae024f2e (701e835c): opcheck 0 failed (GQA max 0-1 milli); G1 decode 52.971-52.992 (main 49.44-49.51), TTFT 70-71, prefill 67.73-68.07; G4 p1/p3 identical, p2 differs from byte 679 (near-tie sentence; 1221 vs 1215 B), judge vs exact ref at integration; ctx 2048: 1900-token run 51.78 vs main 37.83 tok/s, late positions 50.50 vs 29.48, step 18.94 vs 26.06 ms; /ci green at ae024f2e. Critic r1 FAIL 12:20 solely on G4 (p2 fork not shown to be a near tie); round 2 sent: exact-reference top-2 gap at the p2 divergence on the host, then W1/S2/S4/S5. Integration fixes W1, S2-S6 pushed 8808f7bf (tests fail without W1/S4/S5) and sent to integ. G4 evidence 74d476da: p2 fork at reply token 140 is a 0.0003-logit tie in the exact reference (creature 23.2004 vs large 23.2000; main took top-1, branch top-2; 140 shared tokens all top-1, min gap 0.0057). Critic r2 FAIL 12:33 only on G2 (no full /ci at 74d476da); G4 PASS (critic verified reference provenance and recount). Round 3: ee776b76 (pos alignment, build-time fallback notice, near-tie prefix check), full /ci green 12:37-12:54 at ee776b76; critic r3 PASS 12:57, no gap; PR switched to Fixes #1167. COMPLETE | 3 |
| #1161 | b1161 | gauntlet/1161-emc-rate, PR #1175 (draft), b525f15a, Fixes #1161 | DONE outcome A: bwmgr floor at bpmp_init, emc_hold checks, guard rejects MRQ_CLK on EMC; 10/10 cold boots 0 drops; G1 median 49.528; text identical; /ci green (flaky test_runner_parks_without_a_lock filed #1184); critic r1 PASS 08:05; minor fix-ups done in 3f998a5e (comment; failed readback = READ_ERROR, new test fails on f8619844); make test/runtime-test/braces green. COMPLETE | 1 |
| #1168 | b1168 | gauntlet/1168-default-sampler-dod3, PR #1182 (draft), 013447ce, Refs #1168 | HW 12:11-12:29: 31 default-sampler runs on GPU program (main build d5aacc3b), greedy p1 and default p1 seed 1 byte-identical to baseline, r31 repeats r01; EMC 3199 / GPC 1020; ADR 0004 addition pushed 68b71217 (count [pending]); 15 EOS / 15 cap, decode median 41.084; 5 replies 1-4 bytes short in capture. Critic r1 PASS 12:39: COHERENT 30/30; ADR filled in 013447ce (30 of 30, built 7745382b = c69438a2 tree). COMPLETE (Refs) | 1 |
| #1169 | b1169 | gauntlet/1169-kexec-retest, PR #1190 (draft, docs), 4db5d892, Refs #1169 | HW 12:31-12:54 on main c69438a2 (kexec): shell reached; xload 780 s at 1.37 MB/s; --gpu cpu and --gpu auto both answer (0.309 tok/s; per-op tiers all CPU until `nvgpu oplib stage`); --gpu on refused with a clear message; after manual nvgpu inherit/channel/oplib stage one RMSNORM dispatch timed out in PBDMA (GP_GET stuck, current_ctx=0): per-op GPU ops do not work after kexec: filed #1189; results posted on #1169 (comment 6721784825); docs PR #1190 (jetson-kexec.md Step 4b). Critic r1 FAIL 13:03 only on Fixes overclaim (per-op path never ran a GPU op; slm load not run); criteria 1-4 PASS, comment matches raw evidence. Round 2: 4db5d892 Refs #1169/#1189, doc claims fixed; critic r2 PASS 13:06, no gap. Out-of-scope doc contradictions filed #1191. COMPLETE (partial: per-op GPU after kexec broken, #1189) | 2 |

## Hardware plan (revised 08:00)

- Demand after 08:45 exceeds the time to 13:15. Per-PR G3 (10 cold boots) is dropped for #522 (CPU sampler, off the GPU and boot paths) and for #1159 (stacked under #1171; covered by #1171's runs and by the integration G3). The integration critic's G1 + G3 on the merged branch covers every change that ships.
- Order (11:20): b1171 round 2 (to 12:00), b1168 (35), b1167 phase B gate (40, if ready by 12:30), b1169 only if a slot is free before integration, integration HW gate after the integration critic's /ci (~13:45).
- GPU chain: b1171 handed to b1167 at 11:20 (b1171 evidence/docs only from then).
- Integration critic `integ` started 11:38 (merge, /ci, full /slmos-review now; HW gate when given the board, ~13:15).
- Order (12:10): b1167 phase B (to ~12:30), b1168 (~12:30-13:05), b1169 (~13:05-13:45), integration HW gate (~13:45-14:45), restore + final report (~14:45-15:30).

## Hardware log

| UTC | Owner | What |
|---|---|---|
| 06:05 | hw-baseline | owns board: main c69438a2 baseline, hw-gate.md, #1159 demo (75 min max) |
| 06:20 | hw-baseline | released at SD Linux; ESP holds a14d7ede (baseline build) |
| 06:25 | b1161 | owns board, slot to 07:10: bisection boot + one fix candidate |
| 06:43 | b1161 | released at SD Linux. Root cause found: BPMP bandwidth manager owns EMC; MRQ_CLK rate lost on next bwmgr recompute (~250 ms, or any MRQ_BWMGR_INT). Fix: MRQ_BWMGR_INT floor at bpmp_init, workaround removed |
| 06:50 | b1161 | owns board to 07:50: 10/10 boot_test + G1 on f8619844 |
| 07:22 | b1161 | released at SD Linux; BOOTAA64 deleted (empty \EFI\BOOT dir remains, likely pre-existing); ESP holds d0ad1207 |
| 07:25 | b1173 | owns board to 08:45: vmrace boot, G1/G4 boot, G3 10/10 |
| 08:06 | b1173 | released at SD Linux; BOOTAA64 deleted; ESP holds f143a8ab |
| 08:08 | b1171 | owns board: H1 (40 min), may continue into H2 to ~09:25 |
| 09:05 | b1171 | released at SD Linux |
| 09:08 | b1159 | owns board to 09:45: pre-fix demo, fix demo + G1/G4 |
| 09:20 | b1159 | released at SD Linux |
| 09:21 | b1165 | owns board to 10:05: real-model golden on GPU program |
| 09:27 | b1165 | released at SD Linux; ESP holds b7bda36c |
| 09:28 | b522 | owns board to ~10:00: G1/G4 + default sampler |
| 09:41 | b522 | released at SD Linux; ESP holds 7020b79f |
| 09:42 | b1171 | owns board to ~10:45: gate (G1/G4, race + 5 hangs, G3 10/10) |
| 10:14 | b1171 | released at SD Linux; BOOTAA64 removed; ESP holds 1c392ce5 |
| 10:16 | b1171 | owns board to ~10:50: diagnostic, control race vs race with hangs |
| 10:41 | b1171 | released at SD Linux (a 10:37 power cycle booted SLM-OS instead of SD Linux; recovered via F11) |
| 10:43 | orchestrator | read-only check: ESP FAT corrupt (SLMOS.efi chain free, FATs differ, dirty bit); NVMe media errors 0. Raw image kept at /root/slmos-esp-fat-corrupt-20261108.img (d0156c87); `fsck.vfat -a` repaired, SLMOS.efi now 0 bytes, recheck clean. ESP hygiene rule added; filed #1187 |
| 10:46 | b1167 | owns board to ~11:30: native JetPack GQA benchmark, no ESP |
| 10:53 | b1167 | released at SD Linux (report arrived 11:15; board idle ~20 min) |
| 11:20 | b1171 | owns board to ~12:00: round 2, main+race baseline vs 8cfb5c86 with hangs |
| 11:48 | b1171 | released at SD Linux; ESP fsck clean at start, between images and at release |
| 11:51 | b1167 | owns board to ~12:30: phase B opcheck, G1/G4, long-context vs main |
| 12:09 | b1167 | released at SD Linux; ESP fsck clean; ESP holds main build 443ea230 |
| 12:11 | b1168 | owns board to ~12:50: 10 prompts x 3 seeds, default sampler |
| 12:29 | b1168 | released at SD Linux; ESP fsck clean before and after; ESP holds d5aacc3b |
| 12:31 | b1169 | owns board to 13:10 (hard stop): kexec retest of main |
| 12:54 | b1169 | released at SD Linux; /root/slmos-1169.elf deleted; ESP untouched |
| 12:55 | integ | owns board to 14:10 (hard stop): integration HW gate on 7a921db1 |
| 13:39 | integ | released at SD Linux; BOOTAA64 removed; ESP fsck clean; ESP holds ca4e60ae |
| 13:41 | orchestrator | restore: ESP SLMOS.efi back to 789cba62 (as found), empty EFI/BOOT removed (EFI nlink 4 as found), fsck clean, sha256 from a fresh ro mount after drop_caches; forensic image moved to host (1187/), board temp files removed; BootOrder unchanged, no BootNext, 25W, /root/slmos.elf 098f3163 and .prev a531a785 unchanged; recovery_mode open |
| 13:42 | orchestrator | claim released |

## Critic verdicts

- #1169 r2 (13:06) PASS, no gap: closingIssuesReferences empty, no closing keyword; doc claims now separate measured from inferred and trace to raw files.

- #1169 r1 (13:03) FAIL on criterion 5 only: Fixes #1169 not earned (the per-op path ran with zero GPU ops; forced GPU dispatch fails, #1189; `slm load` from /boot/models fails after kexec, xload used). Criteria 1-4 PASS: shell reached at c69438a2, CPU and --gpu auto answer, --gpu on refused clearly, every number in the #1169 comment matches raw files, docs only. Minor doc claims: 0.41 tok/s comparison, Wi-Fi, an inferred timeout behaviour. Out of scope but now contradicted: slm-integration.md:60 (ramdisk 1.25 GB) and jetson-uefi-direct.md:471-478 (kexec operator library works).

- #1167 r3 (12:57) PASS, largest gap none. /ci at ee776b76 verified step by step from each log; delta checks: pos/out alignment refused, fallback notice only at program build under the (non-spin) program lock, never IRQs-off or per dispatch. Fixes #1167 earned (Done-when met: kv 2048 decode 53.8 vs 619.4 us/layer, prefill 279 vs 9004; SLM-OS 51.8 vs 37.8 tok/s at ~1900 positions; recorded).

- #1168 r1 (12:39) PASS, COHERENT 30/30. Diff is ADR 0004 only; built tree equals c69438a2; b1/b2 byte-identical to baseline; r31 == r01; 31/31 banners on the GPU program with the default sampler; critic re-extracted all replies from raw captures. Only repetition: r02 one item twice (within rule). Capture loss = stripped trailing whitespace. Largest gap: cpu_tokens=0 read for one session only; per-run GPU path shown by banners and decode rate. Minor: ADR names c69438a2 while the banner says 7745382b; one long paragraph.

- #1167 r2 (12:33) FAIL on G2 only (no full /ci at 74d476da). G4 PASS: near_tie.cpp uses slm_cpu_ref.h (ADR 0004's exact reference), teacher-forced 204-row batch, critic recounted first difference at reply index 140. Delta checks pass: telemetry rule identical to slm_prog.c's, boundary pinned, alignment and fallback tests fail without fixes. Minor: pos pointer alignment unchecked; silent W8 fallback; MANIFEST not updated.

- #1167 r1 (12:20) FAIL on G4 only. Blind A/B (X=branch, Y=main): branch 7% better on decode (52.979 vs 49.508), TTFT (70 vs 75) and prefill (67.82 vs 72.54), far beyond spread. Criteria 1, 2, 4, G1, G2 PASS (critic recomputed medians from 588 samples per cell; 28/28 WIN at kv 64-4096). G4 PARTIAL: p2 differs from byte 680 with no top-2 gap evidence. Minor: no fallback to w8 for stale SASS; boundary 16 unpinned; no alignment check; telemetry; P.V still uses truncating tensor-core add (3.77 of 4.0 ULP headroom at kv 4096); 'clocks after' missing from evidence. Fixes #1167 defensible.

- #1171 r2 (11:56) PASS, largest gap none. Critic recomputed Clopper-Pearson bounds (main 0/105,368 [0, 3.5e-5]; recovery 1/305,173 [8.3e-8, 1.83e-5]; ~914k rounds per arm needed); showed from the gate's 181 ms first-correct times that the wrong result was not a first post-recovery call; own code trace of the RMSNorm path finds no recovery mechanism that yields a wrong value. 51/51 injections recovered across all runs. Round-1 minor items closed; ADR 0006 now 552 words. Refs #1171 judged honest.

- #1171 r1 (10:58) FAIL. Blind A/B (X=branch, Y=main): indistinguishable. PASS: 25/25 recoveries (L2, ~125 ms), replies identical, consumers 157-181 ms, mutation-c fails 7 tests, golden assert, remaining items, G1/G3/G4, kexec path keeps #987 forced reload (g_handoff_local false), RECOVERING gate under the dispatch lock, bounded waits, DEAD path. PARTIAL: criterion 1 (1 wrong oplib RMSNorm in 18375 gate rounds; 269,546 clean rounds since; no main baseline for that consumer), ADR 0006 1243 words. Largest gap: that wrong result. Minor: false level-2 comments; NO_RELOAD reset untested; 6 s vs 9 s docs; race sched under REAL weights returns 1 unconditionally.

- #522 r1 (09:55) PASS. Blind A/B (X=branch, Y=main, all three): host branch faster beyond spread in 28/36 cells (TopK/TopKTopP 10.5-16.5x, greedy 2.0x), never slower beyond spread; hw greedy 50.165 vs 49.508 (separated); hw default 49.962 vs 40.851 (separated), TTFT 75 vs 79-80. Criterion 1 PASS as the achievable optimum: expf counts equal main's in all 52 cells, max reductions 2 -> 1; critic verified no bit-exact fold exists at T != 1. Largest gap: literal "one exp pass at T != 1" not met (wording, not work). Minor: slm-integration.md:144 overstates; legacy oracle transitive for edge params; M1/M7 survive the digest tests (unit tests catch them).

- #1165 r1 (09:35) PASS, no A/B. Bias vector from pinned ggml ca89968a regenerates byte-identical; M1/M2 in production `project_with_bias` fail host + both QEMU; no golden or tolerance changed. HW golden ran the GPU program at 1 and 64 rows, drop-bias reaches mapped weights before build and fails, restored; no CPU fallback (per-op tiers 0). Largest gap: HW image banner bb708a77-dirty, not pinned to head 6d9fac3a (only a brace differs). Minor: LICENSES.md note stale for the real-model fixture; drop-bias not guarded against a concurrent CPU prompt or a second borrowing slot; test_harness comment; truncated summary line. Refs #1165 honest (schedule not met).

- #1159 r1 (09:33) PASS. Blind A/B: X=branch, Y=main; tie (median diff 0.003; branch p1 runs ~0.09 below its p2/p3, inside spread). Corner confirmed at base (slm_prog.c:849-862 defer, gpu_program.rs:1330-1335 pin drops after Drop), fix and mutations M1/M2 confirmed, no new lock edge, leaked sessions do not block unload forever. Largest gap: HW demo used a synthetic holder (vmhold) on a borrowed model; real holder and owned-page free shown only in code and host/QEMU tests. Minor: TLB-invalidate failure clears `open`; mixed atomic access; doc table lacks vmhold; vmhold verb in production image (test-only, gated); prefix image log lacks nvcc line; fix-boot capture is a hand-assembled summary.

- #1173 r1 (08:22) PASS, largest gap none. Blind A/B: X=branch, Y=main; indistinguishable. Critic re-grepped every ga10b_vm_* caller, checked every *_locked_cb for VM writers, confirmed vm_waited_on_map counts real overlap, cmp'd all replies. Done-when fully met -> Fixes #1173. Minor, left for integration review: map_at BUSY mid-pool reported as PMM exhaustion (oplib_gpu_as.c:235-242); note_wait under-counts; vmrace leaks VA per round (test-only); BUSY folded into ERR_ARG in pipeline/MNIST embed; deferred close finished on a non-CPU-0 writer prints on a silent CPU.

- #1161 r1 (08:05) PASS. Blind A/B: X=main, Y=branch; not distinguishable (decode diff 0.020 below either spread; Y nominally better). All of A1-A3, C1-C5 PASS on raw evidence (critic re-ran the guard on main and cmp'd all 9 replies). Largest gap: the ~250 ms BPMP recompute is inferred, not located; moot for A2. Minor: emc_hold.h comment says MRQ_CLK; failed readback counting; boot wiring not covered by a test; hook globals boot-only; final build log reused SASS (no nvcc line).

## Integration hardware gate (scratch 7a921db1, image ca4e60ae)

- Verdict 13:40: READY (code and hardware), subject to accepting #522's criterion-1 deviation (at T != 1 with top-p a second exp over the kept tokens remains; same count as main; cannot be removed while keeping main's tokens). G3 10/10 (56.1-56.3 s, model verified, EMC 3199 each). Review: 0 critical; 6 warnings, 5 fixed and re-verified, 1 is the #522 deviation. Open suggestions: 1161 S1 (IRQs-off across MRQ round trips), 1165 S3 (drop-bias order), 1167 telemetry under w8 fallback, ADR 0006 note that L1 never succeeded, #1188.

- 13:19: boot 52 s, model verified, EMC 204 -> 3199 via bwmgr floor (51 checks, 0 drops). G1 9/9: decode 53.612-53.710 tok/s (main median 49.508), TTFT 70-71 ms, prefill 67.786-68.120 ms; after the runs EMC 3199 / GPC 1020, 360 hold checks 0 drops, cpu_tokens=0, `slm stats --gpu` GQA tier-1 HMMA, tensor-core share 100.00 %. G4: p1, p3 identical to main; p2 identical to #1167's p2 (the 0.0003-logit near tie), so the real-golden boot was skipped. Default sampler p1 x3: 53.490/53.488/53.495 tok/s, TTFT 70-71 (main 40.80-40.87). Recovery smoke: 4 hangs, all recovered at level 2 (1.59 s first, then 125 ms), 13-token GPU replay 115-118 ms, same answer; race consumers correct 157-181 ms after injections 2 and 4 (1627-1650 ms after the first), 0 wrong in 54,899 rounds; injection 3 fell outside a race run (no consumer timing); cold check PASS; fresh p1 53.702 tok/s, identical to main. Console: TCU input mangled first lines of several multi-line sends; two p3 r3 attempts saved as invalid. G3 10/10.

## Integration review findings (as they arrive)

- #1165 (rev1165, 11:49): W1 SLM_REAL_GOLDEN missing from check-braces wide lists; S2 BiasMutation::restore unguarded against a reloaded slot; S3 drop-bias mutates before NoBackend is known; S4 uninitialised report struct. W1, S2, S4 sent to b1165; fixed in c3e005c7 (probe shows check-braces now sees the gated code; restore guard with failing-without test; zero-init).
- #1161 (rev1161, 11:50): W1 emc_hold_set_target stores the raw requested target even on failure, so `clocks emc 3200` makes the 2 s fan-task check WARN forever; S1 ~300 ms IRQs-off worst case; S2 rc hidden; S3 boot_begin without BPMP; S4 wording. W1, S2, S3, S4 sent to b1161; fixed in b525f15a (target only on grant, min(requested, readback); clocks refuses > 3199; tests fail with old logic). Reviewer reports route to main; relayed to integ.
- integ 11:55: scratch branch built (8 merges, one additive Makefile conflict), /ci steps 1-4b green. Cluster #1173/#1159/#1171: W stale 6 s lock-wait text (shell_sys.c:6601, jetson-uefi-direct.md:552, test_slm_prog.c:1808, slm_prog.h) after #1171 raised it to 9 s; sent to b1171 (text only).
- #522 (fork reviewer, 11:52): W T==1 draw reuses pre-cut exps when the tie sort cuts the max (stream can differ from main in a contrived near-tie); S stale comment, digest regeneration note, pre-existing drift fallback. Sent to b522, relayed to integ.
- #1167 (rev1167, 11:53): Ready if W1 fixed. W1 telemetry counts GQA MACs as SIMT (tensor-core share wrong now); S2 DEC/PF boundary unpinned; S3 KV-length rows missing; S4 HMMA alignment not refused; S5 stale SASS disables hd-128 programs instead of falling back to w8; S6 docs. Sent to b1167 for after its board slot, relayed to integ.
- integ interim 12:08: scratch c6400764 (8 merges, Makefile conflict additive), /ci 13 steps all green on the merged branch, review 0 critical / 5 warnings (#1159-#1171 stale 9 s text, #1167 W1, #1161 W1, #1165 W1, #522 W), #1173 no findings. Deltas sent 12:10: 1171 eae99c99, 1165 c3e005c7.
- integ 12:14: 1171/1165 deltas merged and delta-reviewed clean. rev522 disagrees with the fork on #522's T==1 corner: main also uses the pre-cut max, so the branch matches main; the suggested change would break criterion 3. Correction sent to b522 (keep main's behaviour, pin against main's algorithm, rename the misnamed exp-count test). Done in 762582bb: main c69438a2 sampler.rs:196-197/:447 also uses the pre-cut max at T=1; kept; pinned against a verbatim copy of main's sampler; test renamed; docs qualified; sent to integ. rev522 S3: pre-existing NaN + non-total comparator in sort_unstable_by may panic on Rust >= 1.81; integ checking toolchain, will file.
- integ 12:14: deltas 1171 eae99c99 + 1165 c3e005c7 merged, affected /ci steps green (runtime-test 306+326, braces 12/12 with SLM_REAL_GOLDEN, Jetson build); 1161 b525f15a merged and delta-reviewed (W1, S2-S4 addressed; S1 stands as suggestion), steps re-running. Filed #1188 (pre-existing NaN sort-comparator panic path; Rust 1.94.1 pinned).
- integ 12:30: 1161 b525f15a affected steps green; 1167 8808f7bf merged (scratch 01f270fc, additive help-text conflict), delta-reviewed clean (W1, S2, S4, S5 verified; suggestion: stats still reports tensor-core GQA if the w8 fallback is taken); full /ci re-running 12:28-12:55, its Jetson build becomes the HW-gate image.
- integ 12:48: full /ci green on scratch 01f270fc (all 8 branches + deltas through 1167 8808f7bf): runtime-test 307+327, braces 12/12, layout 22 checks, test-qemu-uefi PASS. Gate + real-golden images building; 1167 74d476da (test-only) and 522 762582bb (production sampler unchanged) and 1168 013447ce (docs) merging with affected steps.

## Final state (13:45 UTC, about 8 h used of 10)

| Issue | PR | Outcome | Line |
|---|---|---|---|
| #1173 | #1177 | Closed by the PR: one VM lock for every runtime cold-VM writer; fake-backend tests; 20 overlapped launches on hardware | Fixes #1173 |
| #1159 | #1181 (on 1177) | Partial by scope: the GPU deferred-close corner was real and is fixed; the CPU-only part stays open | Refs #1159 |
| #1171 | #1183 (on 1181) | Done: GPU hang recovery without reboot, 51/51 + 4 integration injections, consumers back in 157-181 ms; all small items done. Refs because the VM-lock item lands with #1177; close by hand after the stack merges. #1186 tracks one unexplained wrong result | Refs #1171 |
| #1165 | #1176 | Partial by scope: Q/K/V bias vector in /ci; real-model golden as a hardware target (GPU program, exact reference); no schedule | Refs #1165 |
| #522 | #1179 | Done in substance: zero per-token allocation, one max reduction, default sampler 40.85 -> 49.96 tok/s (53.49 integrated); the second exp pass at T != 1 cannot be removed without changing main's tokens (deviation for the user to accept); close by hand | Refs #522 |
| #1167 | #1178 (on 1183) | Closed by the PR: HMMA GQA, decode 49.5 -> 53.0 tok/s, prefill 72.5 -> 67.8 ms, ctx 2048 37.8 -> 51.8 tok/s; p2 greedy text forks at a 0.0003-logit tie | Fixes #1167 |
| #1161 | #1175 | Closed by the PR: root cause (BPMP bandwidth manager owns EMC); floor at bpmp_init; 10/10 cold boots 0 drops | Fixes #1161 |
| #1168 | #1182 | Partial by scope: DoD 3 holds under the default sampler, 30/30 coherent, recorded in ADR 0004; gate and loop guard not built | Refs #1168 |
| #1169 | #1190 | Partial: retest done and posted; per-op GPU dispatch after kexec times out (#1189) | Refs #1169 |

Integration (scratch gauntlet/followups-integration, not pushed, final db304b6a): /ci green on every step; hardware gate on 7a921db1: decode 53.61-53.71 tok/s, TTFT 70-71 ms, prefill 67.8-68.1 ms, default sampler 53.49 tok/s, G4 identical except the measured p2 tie, 4/4 recoveries, G3 10/10; full /slmos-review 0 critical, 6 warnings (5 fixed and re-verified, 1 the #522 deviation). Verdict READY.

Merge notes: #1177 -> #1181 -> #1183 -> #1178 are stacked (each PR's base is the previous branch); merge in that order. The others are independent of the stack. #1165 and #1167 both add Makefile targets and help lines (additive conflict, both kept). GitHub only applies closing keywords to PRs targeting main, so the stacked PRs close their issues once retargeted after their base merges.

Issues filed during the run: #1180 (slow `slm unload`), #1184 (flaky test_runner_parks_without_a_lock), #1186 (one wrong RMSNorm under the race), #1187 (NVMe ESP FAT corruption), #1188 (NaN sort-comparator panic path), #1189 (per-op GPU dispatch after kexec times out), #1191 (docs contradicted by the kexec retest).

Board: jetson-nano-2 as found (SD Linux, 25W, ESP image 789cba62, BootOrder unchanged, no BootNext, /root/slmos.elf and .prev unchanged, recovery released); claim released 13:42 UTC.

## Failed approaches

- #1161: an MRQ_CLK `set_rate` on EMC holds only until BPMP's bandwidth manager recomputes (about 250 ms, or any MRQ_BWMGR_INT request); replaced by a bandwidth-manager floor.
- #1171: re-enabling the channel with FORCE_CTX_RELOAD after recovery left it DEAD from the second hang; recovery and the cold channel's v7 dispatch path now enable without it (the kexec path keeps #987's reload).
- #1171: level 1 (TSG preempt) never recovered an injected hang (-10 every time); level 2 (GR engine reset) did the work.
- #1167: adding into the running score through the tensor core truncated (up to 11.3 FP16 ULP at large scores); each score mma now starts from zero and sums in FP32. FP16-only P variants miss the ULP rule and were not adopted.
- #522: the fork reviewer's T == 1 change (subtract the kept max) would have broken token identity with main; main itself uses the pre-cut max, so it was not applied.
- Lab: per-PR 10-boot runs did not fit the board time; #522 and #1159 relied on stacked and integration runs (stated in their PRs).

## Evidence notes

### Critic watch list
- #1171: H1 forced two changes outside recovery: recovery re-enables the channel without FORCE_CTX_RELOAD, and the v7 dispatch path no longer forces a context reload on the cold channel (with it, the channel went DEAD from the second hang). Critic must check this does not undo #987's FECS mb6 recovery on the kexec path, and that consumers stay correct. ADR 0006 added.

### Baseline, main c69438a2 (a14d7ede), `slm launch 0 --ctx 512 --sampler greedy --gpu on`
- Greedy p1..p3 x3: decode median 49.508 tok/s (49.440 to 49.512, slow end is p1 r1, first run of the boot), TTFT 75 ms (75 to 76), prefill 72.536 ms (72.483 to 72.855). GPC 1020, EMC 3199.
- Default sampler (topkp, temp 0.7, k 40, p 0.9, seed 1), p1 x3: decode 40.851 tok/s (40.799 to 40.865), TTFT 79 to 80 ms. About 4.3 ms per token in the CPU sampler.
- Greedy text identical across runs: p1 45828358 (1376 B), p2 9495f74c (1215 B), p3 df0c03b6 (1277 B).
- On one of two boots EMC read 204 MHz when the program opened (after the main.c workaround); passed to #1161.
- Filed #1180: `slm unload` of the 1.5B model takes 57 to 85 s with no output.
