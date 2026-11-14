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

## Entry 5: Track A result (Rec 7)

Source: the Track A pass, snapshot 2026-11-13 about 23:25 PT. The data
came from `gh issue list --state open --limit 500 --json
number,title,body,labels,milestone,createdAt,comments`, `gh issue list
--state all --limit 2000` and `gh pr list --state merged --limit 2000`.
Months are UTC calendar months. UTC reproduces the review's figures
exactly; PT months differ slightly.

### Labels and milestones applied

- Open issues: 236 (the review counted 214 on 2026-11-08 13:15 PT).
- Before the pass, 138 issues had no milestone and 64 had no type label
  (the review counted 63).
- Applied: 472 origin and kind labels (one of each on all 236 issues), 64
  type labels, and 138 milestones (77 Backlog, 61 Icebox). Seven labels
  were created: origin:{agent-followup, golden, review, hardware, user}
  and kind:{gap, regression}. A query after the pass found 0 open issues
  missing any of the four fields. No issue was closed, commented on or
  had a label removed.
- Classification: 183 valid, 35 fixed, 13 not-worth-doing, 5 duplicate.
  Origin: 71 review, 58 agent-followup, 57 hardware, 47 user, 3 golden.
  Kind: 224 gap, 12 regression. Confidence: 99 high, 110 medium, 27 low.
  Origin is judged from the text alone, because every issue was filed from
  one account.

### Opened per PR merged (UTC)

| Month | Issues opened | PRs merged | Ratio | Review |
|---|---|---|---|---|
| 2026-04 | 288 | 284 | 1.01 | 1.01 |
| 2026-05 | 145 | 260 | 0.56 | 0.56 |
| 2026-06 to 09 | 0 | 0 | n/a | n/a |
| 2026-10 | 13 | 14 | 0.93 | 0.93 |
| 2026-11, 1st to 8th 21:15Z | 91 | 45 | 2.02 | 2.02 |
| 2026-11, to the 13th | 131 | 64 | 2.05 | n/a |
| Since the review cut | 40 | 19 | 2.11 | n/a |

### Hub share of open issues

The review did not record its keyword patterns, so Track A wrote its own:

- NC: `\bNC\b|non-?cacheable|coheren|cache[ -]maintenance|\bDC\s+(CVAC|CIVAC|IVAC|CISW|CSW|ISW)\b|set/way|cache_(clean|inval|flush)\w*|cache\.h\b`
- Configuration: `\b[A-Z][A-Z0-9_]{2,}=(ON|OFF)\b|build (flag|option|variant)s?|config(uration)? (variant|matrix|flag|option)s?|CMake option|feature flag|#ifdef|\bAI_SCHED\b|JETSON_HW_TICK|SECONDARY_PREEMPT|COOP_PREEMPT|WORK_STEALING`
- FFI: `\bFFI\b|\brust_[a-z0-9_]+|extern\s+"C"|no_mangle|cbindgen|bindgen`
- Shell: the `sub:shell` label, plus a separate count of issues that name
  `shell_sys\.c`.

The patterns match against title and body. They were applied to the
review-time open set rebuilt from the data (exactly 214) and to the open
set now. Columns 3 and 4 are the like-for-like comparison.

| Hub | Review, own patterns (of 214) | Track A patterns, review set (of 214) | Now (of 236) |
|---|---|---|---|
| NC, coherency, cache maintenance | 21 (9.8%) | 24 (11.2%) | 34 (14.4%) |
| Build flags and configuration | 20 (9.3%) | 21 (9.8%) | 31 (13.1%) |
| FFI, rust_*, extern "C" | 30 (14.0%) | 23 (10.7%) | 23 (9.7%) |
| Shell (`sub:shell` label) | 12 (5.6%) | 12 (5.6%) | 13 (5.5%) |
| Names `shell_sys.c` | n/a | 6 (2.8%) | 8 (3.4%) |

The coherency and build-flag counts grew by about 10 issues each. Most of
that is the #1193 work, and some of those issues are close candidates
(for example #1131).

### Origin of open issues created since 2026-11-01

131 issues were created since Nov 1. 77 are still open and were
labelled; 54 were already closed and were not labelled.

| Origin | Count (of 77) | Share |
|---|---|---|
| hardware | 32 | 41.6% |
| review | 32 | 41.6% |
| agent-followup | 12 | 15.6% |
| golden | 1 | 1.3% |
| user | 0 | 0% |

The same 77 split into 70 gaps and 7 regressions. For contrast, the 159
open issues created before November split: user 47, agent-followup 46,
review 39, hardware 25, golden 2.

### Close candidates (53, for approval; none closed)

| Issue | Title | Proposed resolution | Evidence |
|---|---|---|---|
| #44 | Network console (telnet/SSH alternative) | fixed (medium confidence) | telnet multi-session shell db204cd7 (PR #259), per-session history (#434), sshd 904fa48d (PR #915) + 5a67cc7e (PR #918); kernel/src/telnet.c, shell_session.c on origin/main |
| #45 | CI: coverage tracking | not-worth-doing (low confidence) | premise is .github/workflows/ci.yml, deleted in aff136a5 ("Delete .github/workflows directory"); CI now runs locally via the ci skill |
| #46 | CI: real hardware test farm (Pi 5 + Jetson) | not-worth-doing (low confidence) | GitHub Actions workflows deleted (aff136a5); a self-hosted runner driving the shared labctl lab conflicts with the lab-sharing policy |
| #47 | EFI stub: boot services integration (memory map, ACPI/DTB, exit BS) | fixed (medium confidence) | eb094b73 (PR #1085, #26): efi_stub_entry uses GetMemoryMap + ExitBootServices and passes the DTB; make test-qemu-uefi covers it |
| #71 | Cache tiling assumes 64KB L1D — verify for Cortex-A55 (32KB) | not-worth-doing (medium confidence) | issue itself concludes impact nil (12KB fits 32KB L1D) and no A55 target exists; rationale comment already at ops.rs:901 |
| #72 | x86-64 SIMD: SSE intrinsics disabled due to LLVM crash on soft-float target | fixed (medium confidence) | 49ce3ae1 added kernel/arch/x86_64/sse_kernels.c (slm_sse_* C kernels called from ops.rs x86_64 cfg blocks); docs/benchmarks.md:622 |
| #199 | Phase 3: SSH server for remote shell access | fixed (medium confidence) | sshd shipped: wolfSSH vendored (kernel/lib/wolfssh), 904fa48d (PR #915) channel->shell_session + autostart, 5a67cc7e (PR #918) hardening/default-on, 983cb0cc (PR #1000) tests; sub-tickets #891/#893/#895/#896 also still open |
| #202 | Pi 5 networking: BCM GENET driver (networking Phase 3) | fixed (medium confidence) | kernel/drivers/macb.c: cf60be14 stages 4-5 RX + DHCP + ping, PR #248/#257; ENABLE_NETWORKING ON for RASPI5 (CMakeLists.txt:402); only optional IRQ stage left |
| #220 | Jetson UEFI doc: rewrite first-person plural to SLM-OS voice for advisor submission | not-worth-doing (medium confidence) | doc moved to docs/archive/investigations/ (archived, superseded by working UEFI boot, PR #1085) |
| #221 | Jetson UEFI doc: clarify 'str w12, [0x0C280000]' shorthand (not valid AArch64 syntax) | not-worth-doing (medium confidence) | same archived doc; superseded by docs/deploy/jetson-uefi-direct.md |
| #222 | Jetson UEFI doc: sharpen crash-PC analysis at 0x10070 (add I-cache staleness hypothesis) | not-worth-doing (medium confidence) | crash analysis moot: UEFI direct boot works (eb094b73, PR #1085); doc archived |
| #223 | CLAUDE.md: refresh 'UEFI direct boot (WIP)' summary to reflect deeper blockers | fixed (high confidence) | CLAUDE.md on origin/main now documents working UEFI direct boot (#26, eb094b73 PR #1085) with a deploy guide pointer |
| #246 | Jetson UEFI-direct: Device-nGnRnE DRAM access raises CBB Interface Error | fixed (high confidence) | UEFI-direct boot works: eb094b73 (PR #1085, #26) moves MMU-off copy/relocation into boot.S; CLAUDE.md UEFI-direct section |
| #266 | Jetson: USB networking (CDC-ECM via XHCI host + dongle; XUDC gadget as fallback) | fixed (high confidence) | Jetson USB CDC-ECM networking shipped: ed15f3dd/PR #326 (kexec path), e7f9aa51/PR #1087 (UEFI-direct); #385 body confirms working; follow-ups in #384/#385/#386/#387 |
| #286 | Jetson xHCI: investigate standalone Tegra XUSB Falcon firmware load (replace Linux cooperation model) | fixed (medium confidence) | e7f9aa51 (PR #1087, #1084) xusb_t234_cold.c brings XUSB up from cold without Linux; falcon boots from IFR firmware, no firmware load needed |
| #334 | Tegra xHCI: full PHY / padctl re-init at SLM-OS xhci_init (#309 option A) | fixed (medium confidence) | e7f9aa51 (PR #1087): xusb_t234_cold.c does BPMP clocks/resets/power + padctl from fuses (UEFI-direct path); kexec path still inherits |
| #380 | Jetson: restore hardware timer IRQ delivery on secondary CPUs (cooperative preemption is the workaround) | fixed (high confidence) | 72728b77 (PR #746) TF-A patch + HW-tick wire-up; ad905eed (#755) JETSON_HW_TICK=ON default; needs patched BL31 |
| #381 | Jetson: UARTA (40-pin header, 0x03100000) blocked by CBB firewall even at EL2 | not-worth-doing (low confidence) | CBB firewall blocks UARTA in fabric; only out-of-band fixes (NVIDIA-unpublished MB2 BCT or custom signed bootloader); UARTC console works on both boot paths |
| #412 | test_blob_validate/stage_rejects_invalid_*_headers fail on main (payload_len > 0) | fixed (high confidence) | afa7e765 "test_eviction: fix MLP payload buffer undersize (long-standing 2 failures)" sizes buffers to EVICTION_MLP_PAYLOAD_BYTES |
| #475 | SLM integration on Jetson Orin Nano — Qwen2.5-1.5B end-to-end (umbrella) | fixed (medium confidence) | c69438a2 (PR #1164) Qwen2.5-1.5B end-to-end on Jetson GPU; 2026-11-07 comment: remaining follow-ups (#1165-#1172) do not block the DoD as amended by ADR 0005; umbrella left open with "Refs #475" |
| #522 | Sampler hot-path allocation pressure (M9) | fixed (high confidence) | 5ef4e69a (#1179, 2026-11-08) SamplerWorkspace + one max reduction, no per-token allocation |
| #535 | Jetson: ai_mlp+gpu-use-sched dispatch wedges box under task burst (sync IRQ-off cliff) | duplicate (high confidence) | duplicate of #651 (same sched-MLP GPU dispatch IRQ-off wedge), closed by 87849d5f rate-limit + try-lock |
| #539 | SLM M6.A-2..6: pre-kexec L4T loader + bare-metal pushbuffer dispatch + smoke-test gate + cache coherency + PTIMER | fixed (medium confidence) | superseded: GPU SLM dispatch delivered on an SLM-OS-built channel via c69438a2 (PR #1164) instead of a pre-kexec loader; kexec retest tracked in #1169 |
| #540 | SLM M6.B / M6.C / M6.D: SASS kernel authoring (HMMA tensor cores + CUDA-core fallback + element-wise) | fixed (medium confidence) | kernels landed (#540 PRs #699-#712: q4k dequant/dot, rmsnorm, swiglu, rope, gqa, embedding) + HMMA Q4_K/Q6_K in c69438a2; 49.5 tok/s >= 25; utilization target retired by ADR 0005; #1167 closed |
| #546 | SLM M5.3.3: multi-block weight-pool allocation for real GGUFs > 2 MB | duplicate (high confidence) | practical blocker fixed by 09bc6c89 (PMM bypass); remaining multi-block model_mem allocator is exactly #550 (per 2026-10-18 comment) |
| #563 | slmos-kexec: helper logs in /tmp aren't rotated across runs | not-worth-doing (medium confidence) | script still writes /tmp/$bin.log truncated per run on tmpfs; issue itself says no operational pain and only matters if run from a long-lived context, which it is not (interactive pre-kexec helper) |
| #606 | parse_onnx intermittent translation fault on test_model_loader (1/10 on QEMU ARM64) | duplicate (medium confidence) | Duplicate of #200 (closed 2026-05-05): own 2026-05-03 comment calls the parse_onnx fault a victim symptom of the #200 scheduler teardown race |
| #618 | net_poll page fault (NULL +0xd8) crashes kernel during slm prompt | duplicate (medium confidence) | Duplicate of #633: 8b941478 (2026-05-05) records the #618 x19 probe did not fire; root cause reassigned to Rust heap fragmentation tracked in #633, mitigated by RUST_HEAP_MB bump |
| #633 | msg_router publish path fragments Rust heap; bigger heap masks but doesn't cure it | not-worth-doing (medium confidence) | Premise no longer holds: msg_router publish path on origin/main is allocation-free (static Mailbox arrays + stack target array, runtime/src/msg_router.rs publish_internal_nowait); heap bump 8b941478 mitigated symptom. Re-file with a fresh repro if alloc failure recurs |
| #635 | Pi 5 / Jetson: extend COOP_PREEMPT to cover pure CPU-bound loops via slm_preempt_point() | fixed (medium confidence) | slm_preempt_point() shipped in 495bce31 (PR #636) with tests; only unchecked box is Pi 5 HW verify of COOP_PREEMPT, which is no longer the default (Pi 5 #777, Jetson #1050 use HW preemption) |
| #655 | Parallel matmul: drop NC-memory + cache mgmt once SMPEN lands | not-worth-doing (high confidence) | Parallel-matmul code (chore/matmul-multithread) never merged to origin/main (no matmul_par/ncmem in runtime/src); NC-memory axis removed by 98e17440 (PR #1225, #1193) |
| #658 | [meta] Tensor-core (HMMA) GPU dispatch path | fixed (high confidence) | All six stage issues #659-#664 closed 2026-05-07; HMMA dispatch verified on jetson-nano-2 per 2026-05-15 comment (d28dcde0 + PR #828); HMMA Q4_K kernels in GPU program c69438a2 (#475) |
| #665 | GPU runtime loader prereq B: shader-page executable mapping + DMA coherence | fixed (low confidence) | SLM-OS-mapped shader/scratch pages dispatch on GPU: ga10b_gmmu_map used by oplib_pool_stage_to_gpu slow path (#832, closed) and cold-channel pipelines (#1088/#1092 closed) |
| #666 | GPU runtime loader prereq A: GMMU virtual-address allocator + page mapper | fixed (high confidence) | GMMU walker/alloc/map/free + TLB invalidate on origin/main ga10b_gmmu.c: fbb30233 (PR #679, Milestone A), 0577c301 (PR #681, C), 4ef64eb8 (PR #687, D) |
| #675 | Jetson Cortex-A78AE: NEON SDOT Q4_K kernel is compute-bound at Qwen2.5 prefill shapes — batched matmul yields no win | not-worth-doing (high confidence) | Negative result: measured 0.99x, theoretical ceiling 1.013x; batched kernel (matmul_q4k_rows_batch/forward_batch) never merged to origin/main |
| #678 | Validate ga10b_gmmu_walk against real handoff data (#666 Milestone A follow-up) | fixed (medium confidence) | Walker validated against real inherited VMs on HW: ga10b_gmmu_enum walks handoff PDBs, 10/10 GPU fastpath boots on jetson-nano-2 (b9de6824, PR #1032); SLM-OS GMMU mappings dispatch in #832 path |
| #691 | GMMU walker: Tegra MC SMMU layer breaks the FECS-CURRENT-CTX-as-phys assumption (#678 deeper finding) | fixed (low confidence) | Path 2 (SLM-OS-owned channel) delivered by UEFI cold bring-up #1088/#1092 (closed); inherited-VM walk from handoff PDB works on HW (b9de6824, PR #1032) |
| #714 | GPU dispatch: register the remaining 6 ops + session-launch probe (B follow-on to A) | fixed (high confidence) | B.1 02248142 (PR #756), B.2 e00af77b (PR #758), smoke all ops 9fa6c40c (PR #759), B.3 RMSNORM hybrid c0359733 (PR #762); remainder tracked in #763/#764 |
| #764 | GPU weights pool: persistent GPU-VA staging for SLM weight tensors (#714 §B.3 architecture) | not-worth-doing (medium confidence) | Superseded: SLM-on-GPU shipped via prebuilt per-session GPU program with HMMA Q4_K/Q6_K on SLM-OS-built channel (c69438a2, #475), not the Linux-helper weights-pool design |
| #891 | SSH 199c: session abstraction integration + sshd autostart | fixed (high confidence) | PR #915 (904fa48d) sshd 199c channel->shell_session + autostart |
| #892 | SSH 199b: littlefs VFS hooks + Ed25519 host key generation and persistence | fixed (high confidence) | PR #992 (4989faec) Ed25519 host key + VFS persistence; kernel/net/ssh/host_key.c |
| #893 | SSH 199a: wolfSSH/wolfCrypt vendoring + lwIP IO hooks + crypto RNG | fixed (high confidence) | PR #902 (358d6358) RNG + vendoring; kernel/lib/wolfssh, wolfcrypt present |
| #895 | SSH 199e: hardening + interop matrix + entropy audit | fixed (medium confidence) | PR #918 (5a67cc7e) 199e hardening docs + autostart default-on; interop matrix scope not separately verified |
| #896 | SSH 199d: password authentication + user management | fixed (high confidence) | PR #916 (3b446bdf) scrypt password auth + passwd shell cmds |
| #950 | make test-ga10b-bringup fails to link: ga10b_phys_in_dram undefined | fixed (high confidence) | PR #972 (73404e9e) test-ga10b-bringup link gap fixed; host stub ga10b_phys_in_dram in test_ga10b_bringup.c |
| #1028 | build-mnist-shaders.sh fails with host CUDA 12.0: cuobjdump names cubins tmpxft_* | fixed (high confidence) | PR #1097 (5f1b3c77) fixed MNIST shader build on CUDA 12.0; script picks cubin by content (tmpxft handled) |
| #1131 | pi_mutex_t fields on Pi 5 / Jetson: cacheable state read across CPUs, untested under contention | fixed (medium confidence) | question answered: ADR 0007 (e73b9b61) measured cacheable IS-WB memory coherent across CPUs on Pi 5 + Jetson; NC axis removed in PR #1225 (98e17440) |
| #1169 | Jetson kexec: retest the SLM path after the UEFI-direct GPU program (#1164) | fixed (medium confidence) | retest done and recorded as Step 4b in docs/deploy/jetson-kexec.md by 5483fbdf (PR #1190); --gpu on refusal meets done-when; PBDMA failure split to #1189 |
| #1171 | SLM GPU program: small follow-ups from the #1164 reviews | fixed (high confidence) | every item fixed: ecd8057a (PR #1183) message says every item is done, left open for manual close; VM writes via 28a4b108 (PR #1177); wrong race result tracked in #1186 |
| #1204 | Jetson UEFI-direct: cross-cluster litmus rows about 2x slower under the #1193 removal build than under NC_AXIS=OFF | not-worth-doing (low confidence) | slow rows only on pre-rebase image 072bd707 (1/5 boots); merged removal 98e17440 is the rebased tree, fast 3/3; deliverable (raw rows) produced and comment leaves it for the user to close |
| #1206 | Jetson UEFI-direct: .got is never copied by the relocation and holds stale DRAM (6 live entries) | fixed (high confidence) | 8ec0e330 (#1224 fix) puts .got/.got.plt inside .data in kernel-jetson.ld and kernel.ld; test-jetson-uefi-layout.sh checks nothing lies past __data_end; same defect as closed #1224 |
| #1211 | test_task_lifecycle memory-leak check failed in 2/20 Pi 5 removal test boots vs 0/10 NC (#1193 run 2) | fixed (medium confidence) | 73d8deb7 (PR #1215) converted the cleanup window to a wall-clock wait; 20/20 NC and 20/20 removal in run 3; old window 0/20 in run 3 |
| #1223 | Pi 5: KERNEL PANIC on CPU 1 right after the core-latency isolation phase at the split head (NC axis off on Pi 5), 3 of 10 integration boots | duplicate (medium confidence) | duplicate of #1213 (closed, fixed 5b28d065 / PR #1237): run-4 C1 traced the same signature to #1213 sequence B; seen only on the unmerged split head e5b24894 (draft PR #1222); Pi 5 suite 5/5 isolated=10/10 at the fix head |

## Entry 6: Track C result (Rec 5, part 1)

Sources: the Track C report, `gh pr view 1251`, `gh run list --branch
structural/ci-matrix` and `gh issue view` for 1246 to 1250.

- Draft PR #1251 on branch `structural/ci-matrix`, head 8c4ccff3. It adds
  `.github/workflows/build-matrix.yml`, the scripts
  `scripts/ci/{build_matrix.py,build_cell.sh,summarize.py}` and a section
  in `docs/building.md`. No build option was retired, folded or changed,
  and no source or build file was touched.
- First run, 43571376728 (push, 61df7871): 93 ok and 26 failed. Every
  X86_64 cell failed on missing host GSP firmware. That was a gap in the
  runner setup, fixed by adding `linux-firmware-nvidia-graphics` to the
  workflow.
- Latest run, 43571998789 (push, 8c4ccff3): 112 ok and 7 failed, out of
  119 built cells (28 QEMU, 34 Pi 5, 34 Jetson, 23 x86), with 35 cells
  skipped. A skipped cell is an option that does not apply to that
  platform; each has its reason in the PR. Every cell is report-only, so
  the run concludes "success".
- Run 43572707956 (pull_request, same SHA) was still queued when this
  entry was written.

### Failing cells (filed, not fixed; all origin:review, kind:gap, Backlog)

| Issue | Prio | Cells | Error |
|---|---|---|---|
| #1246 | P3 | qemu, pi5, jetson, x86 with `ENABLE_NETWORKING=OFF` (4) | link: telemetry server symbols, plus the Pi 5 mailbox symbols |
| #1247 | P2 | `x86-ENABLE_GSP_FIRMWARE=OFF` (1) | `#error` in `nvidia_gsp_firmware.S`; CMakeLists.txt:635 always compiles it, so the default x86 build needs a host firmware package |
| #1248 | P3 | `x86-BUILD_TYPE=Debug` (1) | Rust: `-C lto=thin` is incompatible with Debug's `embed-bitcode=no` |
| #1249 | P3 | `jetson-JETSON_EL1_SMOKE=ON` (1) | link: relocation truncated in `jetson_el1_smoke.S` |

### New warnings (unique lines not in the platform's default cell)

| Cells | New lines each | What |
|---|---|---|
| NET_SSHD on, autostart, demo-allow-all, and the wide cell, on qemu, pi5 and jetson (12) | 7 | implicit declarations in wolfssh and wolfcrypt; the `strtok_r` result is truncated to int (same class as #1199). Filed as #1250 (P3), although no cell fails |
| `DISABLE_EVICTION=ON` on all 4 platforms | 3 | unused `eviction_blob_*` helpers in `lua_slm.c` |
| `x86-AI_SCHED=ON`, `x86-AI_WEIGHTS=real`, `x86-wide` | 1 | linker: `fp_context.S` has no `.note.GNU-stack` |
| `x86-SLM_GPU_GOLDEN=ON` | 1 | the same linker note, for `slm_gpu_golden_embed.S` |

Each default cell carries 164 warning lines (87 unique), almost all
`-Wpedantic` in Lua's `ljumptab.h`.

### Limits and side findings

- Jetson cells build without nvcc: the operator-library shaders are
  missing, so GPU paths are CPU-only. The ga10b firmware ships only with
  JetPack. Hailo blobs, `OPLIB_BLOB`, `MNIST_DIGITS_DIR` and
  `SLM_GOLDEN_MODEL` all need files from outside the repo. The PR lists
  all of these as not buildable in CI.
- The per-platform defaults in `build_matrix.py` were copied by hand from
  CMakeLists.txt and the Makefile at 8ec0e330. They will drift if those
  files change.
- On Pi 5 and Jetson, `-DCOOP_PREEMPT=ON` on its own is silently dropped:
  the `PI5_COOP_PREEMPT` alias defaults OFF and forces it back off. Noted
  in the PR; no issue filed.

## Entry 7: Track A critic (Fable), amendments to Entry 5

- Fable critic, read-only. It checked 30 records drawn at random (seed
  20261113, `random.sample` over the 236 in `classifications.jsonl`)
  against each issue and against `origin/main` 8ec0e330. Full table:
  `scratchpad/critic-A.md` (session scratch, not committed).
- **Agreement: 27 of 30 (90%).** By field: origin 29/30, kind 30/30,
  status 28/30.
- The three disagreements, and what was done about each:
  - **#27** was marked valid; the critic says fixed by a4971f82
    (2026-04-06). `gpu_map` and `gpu_unmap` are at
    `runtime/src/mm/model_mem.rs:1554,1574`, and `SHM_GPU_ACCESSIBLE` is
    at `kernel/ipc/ipc.c:637,759`. **Added as close candidate 54**, below.
  - **#896** was marked fixed by PR #916. The critic says it is only partly
    done: per-IP auth-failure rate limiting (scope item 11) and the no-echo
    password prompt are not delivered. `sshd.c:144` defers the rate
    limiting to #895, but #895 treats it as specified elsewhere, so closing
    #896 would leave it with no owner. **Withdrawn as a close candidate**
    until those items move to #895. The count of candidates stays at 53.
  - **#1240** had origin hardware; the critic says agent-followup, because
    it was found in a QEMU `PMM_DEBUG` run during #1207. **The label was
    changed on GitHub.**
- Noted by the critic, with no action taken:
  - #675 (not-worth-doing) is sound. A close should point to #669 and keep
    the `origin/chore/batched-prefill` branch.
  - #28 is valid, but its stated blockers are stale since #1088.
  - #819's origin (user) is borderline.

| Issue | Title | Proposed resolution | Evidence |
|---|---|---|---|
| #27 | GPU model-handle mapping / SHM_GPU_ACCESSIBLE | fixed (found by the critic) | a4971f82 (2026-04-06): `runtime/src/mm/model_mem.rs:1554,1574`, `kernel/ipc/ipc.c:637,759`, `kernel/tests/test_ipc.c:161` |

Correction to Entry 7: the exact title of #27 is "Jetson GPU Memory Integration (gpu_map, SHM_GPU_ACCESSIBLE)" (`gh issue view 27`).

## Entry 8, 2026-11-14: user answers to the ADR unknowns; new issues

- The user answered the 19 "unknown, ask the user" points in ADRs 0009
  to 0017 (user, 2026-11-14). The changes that affect decisions:
  - 0016 is now "accepted (backfilled), regretted"; runtime loading is
    the preferred direction.
  - 0017 is now "deprecated". The user's part is a Hailo 8; the code is
    to be checked for 8 versus 8L.
  - Kexec is a fallback and is to be retired on Jetson and x86-64,
    including both x86 loader variants.
  - The GPL stack is to be replaced with an MIT- or BSD-compatible one.
  - The shell verbs are to move to user-space programs.
- These go back to Track B for #1245, together with a "Planned direction"
  section in `architecture.md` and the correction of the stale "consumers
  need kexec" line in `docs/deploy/jetson-uefi-direct.md`.
- New issues (all origin:user, kind:gap, Backlog; `gh issue create`):

| Issue | Title | Labels |
|---|---|---|
| #1252 | Epic: retire kexec on Jetson and x86-64 (UEFI-direct only). Gates: (a) UEFI-direct initializes XUSB, PCIe, GPU and camera RCE; (b) the lab and gauntlet tooling works on UEFI-direct only; (c) reliability matches kexec on the hardware suite | enhancement, jetson, x86-64, sub:boot, P2 |
| #1253 | Runtime model-weight loading, replacing compiled-in weights (ADR 0016) | enhancement, all, sub:ai-runtime, P3 |
| #1254 | Hailo: delete or freeze the integration? (decision for the user). Inventory: 114 tracked files outside docs/archive, 3 CMake and 3 Makefile options, the CI cells, 8 docs | investigation, pi5, sub:ai-runtime, P3 |
| #1255 | Licensing: replace GPL wolfSSH/wolfCrypt with an MIT/BSD-compatible stack | tech-debt, all, sub:net, P2 |

- #1255 was not in the request. ADR 0014 was to link "the licence issue",
  and none existed (`gh issue list --state all --search "GPL OR licence
  OR license OR wolfssl OR wolfssh in:title"` found only #1250 and older
  SSH tickets), so it was filed.
- The 114-file Hailo count comes from `git ls-tree -r --name-only
  origin/main | grep -i hailo | grep -v ^docs/archive | wc -l`.
