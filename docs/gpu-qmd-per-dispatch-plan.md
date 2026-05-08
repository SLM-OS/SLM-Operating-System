# Per-dispatch QMD construction on GA10B (Option 3)

Plan for SLM-OS to author its own Ampere QMD descriptors per inference
dispatch instead of replaying the Linux-helper-baked chain. Rationale,
diagnostic data, and implementation phases.

Issue: [#558](https://github.com/SLM-OS/SLM-Operating-System/issues/558) (GPU
inference returns stale results — off-by-one between dispatches).

---

## Status (2026-05-07)

**Done.** End-to-end v7 + HMMA dispatch verified on jetson-nano-2 with
`SLMOS_GEMM_TIER=hmma slmos-kexec --no-gpu-suspend`. Landed in
[PR #694](https://github.com/SLM-OS/SLM-Operating-System/pull/694)
(merged commit `4ef44135`).

| Phase | State | Notes |
|---|---|---|
| 1. Port encoder | ✅ | `kernel/gpu/nvidia/ga10b_qmd.{c,h}` mirrors the helper's encoder. |
| 2. Handoff v7 | ✅ | `qmd_pool_phys/gpu_va/size_bytes/n_slots` tail; size pinned at 256 B. |
| 3. QMD pool alloc | ✅ | Helper maps via `gpu_alloc_qmd_pool`; default 1024 slots. |
| 4. Byte-compare selftest | ✅ | `test_qmd_selftest_reference_matches_encoder` + the v7-stride pinning test. |
| 5. Switch dispatch to fresh QMD | ✅ | `ga10b_dispatch_v7_pipeline` builds a fresh QMD per launch via `ga10b_qmd_pool_prepare`. |
| 6. Hardware re-probe | ✅ | jetson-nano-2: vertical-bar input → argmax=1, zero input → argmax=5; helper standalone shows max\|err\|=0.000410 vs CPU FP32. |
| 7. Buffer / unknowns | ✅ | Closed in flight: scanner-level v7 acceptance (#694), v7 tail-field copy on inherit (#710), version-aware pipeline-output stride (#710), GA10B LTC cross-dispatch coherency (#715 / #722). |

**Bonus delivery (out of original plan scope).** HMMA tier — FP32-activation
× FP16-weight tensor-core GEMM at MNIST op 6 — added in the same PR.
The QMD encoder propagates `smem_size_bytes`, `slm_size_bytes`, and
`barrier_count` from the v7 op, which the HMMA SASS requires
(`SHARED_MEMORY_SIZE = 2048`, `BARRIER_COUNT = 3`). Without that
propagation the WMMA chain stalls op[N+1] silently. Tensor cores
demonstrably executing under SLM-OS on real GA10B silicon.

**Per-dispatch cost (post-#722).** The 3-point `ga10b_l2_evict_sysmem`
that closed the LTC staleness adds **~120 µs typical / ~6 ms worst
case** of IRQ-off latency per inference. (Derivation: each evict
is 4 UFLUSH ops; per-op typical is <10 µs and worst case is the
100-retry × ~5 µs busy-wait in `ga10b_uflush_op` ≈ 500 µs. So one
evict is ~40 µs typical / ~2 ms worst, multiplied by three sites
per dispatch.) That's fine for MNIST at 1–10 inf/s. It is **not**
the right shape for SLM workloads: at Qwen 2.5 1.5B's ~370
ops/token × 10 tokens/sec = 3700 launches/sec, the evict overhead
alone is ~440 ms/sec — clearly unworkable. The async-batched
dispatch architecture in #573 is what unblocks SLMs, and a
different barrier strategy (per-batch, not per-launch) will need to
replace the 3-point evict on that path. Do not paste this pattern
into the SLM forward path without the architectural rework.

The narrowing experiment — gate each of the three sites behind a
cmdline flag and isolate which is strictly required — is tracked
separately in #723. If only one or two sites turn out to matter,
the per-dispatch overhead drops proportionally.

**Outstanding follow-ups** (all out of scope here, tracked separately):
- [#573](https://github.com/SLM-OS/SLM-Operating-System/issues/573) — async batched dispatch architecture for SLM workloads (per-launch poll overhead from the §7 risk note; deferred per the plan's exit criteria).
- [#702](https://github.com/SLM-OS/SLM-Operating-System/issues/702) — `compute_ready` audit (eligibility gate is a static `#ifdef PLATFORM_JETSON_ORIN_NANO`, decoupled from actual handoff state; not a v7-specific issue but exposed during this work).
- [#692](https://github.com/SLM-OS/SLM-Operating-System/issues/692) — original "GPU MNIST returns CPU-identical logits" finding now largely explained by suspend-kexec masking (compute_ready=1 even when inherit failed silently); kept open until #702 resolves.

---

## 1. Why a fresh QMD per dispatch

Hardware-collected probe data on jetson-nano-2 (issue #558 comments,
2026-04-29) shows:

- Dispatch is reproducible under fixed input — 9 of 10 byte-identical
  results when input doesn't change.
- First dispatch of a session returns all-zero output (kernel didn't
  write the result buffer).
- After an input change, subsequent dispatches return the *previous*
  input's result, deterministically off by one.

Same symptom is observed even though the channel-prelude pushbuffer
emits `INVALIDATE_SKED_CACHES`, `INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI`,
and `SEND_SIGNALING_PCAS2_B` with `INVALIDATE_COPY_SCHEDULE` every
launch (`scripts/gpu-launch-common.c:520-546`), and even though the
QMD itself has all the per-QMD invalidate bits set
(`gpu-launch-common.c:449-460`):

```
QMD_INVALIDATE_TEXTURE_{HEADER,SAMPLER,DATA}_CACHE_BIT
QMD_INVALIDATE_SHADER_DATA_CACHE_BIT
QMD_INVALIDATE_INSTRUCTION_CACHE_BIT
QMD_INVALIDATE_SHADER_CONSTANT_CACHE_BIT
```

The leading hypothesis: the helper's QMD is built **once on Linux
pre-kexec** and SLM-OS replays the same byte-identical QMD pointer
on every dispatch via `SEND_PCAS_A`. Ampere's SKED (compute scheduler)
can elide reprocessing of an unchanged QMD, so the per-QMD invalidate
bits don't fire on launches 2..N. Issuing a fresh QMD pointer per
dispatch forces SKED to redecode, which forces the invalidate bits to
fire each time.

This is what every production driver does. Mesa's NVK builds a fresh
QMD per dispatch (`nvk_cmd_upload_qmd` in
`~/slmos-ref/mesa/mesa-nvk_cmd_dispatch.c:159`), CUDA
runtime does the same, nouveau does the same. The Linux-helper-baked
QMD chain was an SLM-OS shortcut for getting compute working without
authoring QMDs from scratch on the bare-metal side; fixing the off-by-
one means undoing that shortcut.

---

## 2. What we already have

`scripts/gpu-launch-common.c:gpu_populate_qmd_at()` (lines 395-481) is
a complete, working C QMD encoder for Ampere. It mirrors NVK's
`Qmd3_0::new()` + `fill_qmd()`
(`~/slmos-ref/mesa/mesa-nak_qmd.rs:499-528, 616-655`) and
produces the same 256-byte QMD format that Linux uses for
`AMPERE_COMPUTE_B`.

It runs on the Linux helper today. Porting it to SLM-OS is mostly a
move + cache-flush primitive swap (`msync(MS_SYNC)` →
`gsp_platform->cache_clean()`) — there is no new bit-encoding work.

The QMD format itself is documented in
`~/slmos-ref/mesa/mesa-clc7c0qmd.h` (NVIDIA's auto-
generated header for the `clc7c0` class — Ampere compute) and the bit
positions are pinned in `scripts/gpu-launch-common.h:108-141`.

---

## 3. The constraint the helper still owns

A QMD is just bytes; firing it requires the GPU to be able to *reach*
those bytes through the channel's GMMU. SLM-OS post-kexec doesn't have
the ability to mint new GMMU mappings — that requires understanding
the channel's page tables and getting the kernel mode driver's
permissions, both of which the helper handles.

So SLM-OS can't allocate a brand-new physical page and dispatch from
it; the page has to be GMMU-mapped already.

**The cheap solution:** the helper pre-allocates a *pool* of QMD slots,
maps the entire pool through the channel's GMMU, and exposes the pool
via the handoff. SLM-OS writes a fresh QMD into the next free slot and
advances. No new GMMU work in SLM-OS; the helper does the mapping
once, lifetime-of-channel.

**Pool sizing.** The 256 B per QMD is chip-fixed (Ampere `QMDV03_00`).
Slot count is the design parameter. Two competing constraints:

- **Floor:** ensure consecutive launches use distinct GPU VAs (so
  SKED can't elide on pointer identity). Submit-and-poll is
  synchronous — only one launch is in flight at a time — so 2 slots
  is the strict minimum.
- **Defense in depth:** if SKED keys on `(gpu_va, content_hash)`
  rather than `gpu_va` alone, "op-position i in dispatch K" and
  "op-position i in dispatch K+1" produce byte-identical QMDs (same
  shader VA, same cbuf VA, same dims) and could still be elided even
  with pointer rotation. Mitigated by ensuring the same op-position
  doesn't return to the same slot across dispatches — pool size ≥
  `2 × max_ops_per_chain` does that, with cycle-by-2 alternation.

`max_ops_per_chain` is model-dependent. Rough estimates:

| Model | Ops per inference (forward pass per token) |
|---|---|
| MNIST CNN (today) | 8 |
| Qwen 2.5 1.5B (target) | ~370 (28 transformer layers × ~13 ops + embedding + LM head) |

Sizing for the model we're actually enabling rather than the demo:

```
QMD_POOL_SLOTS    = 1024   (≥ 2× Qwen's per-pass op count + headroom)
QMD_POOL_BYTES    = 1024 × 256 = 256 KiB
```

256 KiB is rounding error against the multi-GB weight pool the SLM-OS
allocator already manages. Sized once at channel setup; constant cost
across all dispatches.

Pool advance is `(slot + 1) % POOL_SLOTS` — old slot bytes get
overwritten on cycle-back. Helper allocates the pool once at channel
setup; SLM-OS does no further GMMU work. 16 KiB is negligible against
the existing GPU-mapped allocations the helper already manages.

If hardware behavior surprises us (e.g., SKED elides on content-hash
and 64 slots × 256 B isn't enough margin), we widen the pool or
inject a nonce into a reserved QMD field. Both are localised changes
to the same encoder.

---

## 4. Handoff changes

`struct ga10b_pipeline_op` today (24 B,
`kernel/gpu/nvidia/ga10b_channel_handoff.h:190`):

```c
struct ga10b_pipeline_op {
    uint64_t qmd_gpu_va;        /* GPU VA of helper-baked QMD */
    uint64_t output_phys;
    uint32_t expected_payload;
    uint32_t flags;
};
```

For per-dispatch QMD construction SLM-OS needs the QMD's input
parameters per-op: shader VA, cbuf VA, register count, grid/block
dims, smem/SLM sizes, barrier count. Either:

**A. Helper extracts and pins them in the handoff (preferred).**
Bump handoff version → v7. Add fields to `ga10b_pipeline_op`:

```c
struct ga10b_pipeline_op_v7 {
    /* v6 fields (unchanged) */
    uint64_t qmd_gpu_va;        /* still useful for byte-compare validation */
    uint64_t output_phys;
    uint32_t expected_payload;
    uint32_t flags;

    /* v7: QMD construction inputs */
    uint64_t shader_gpu_va;
    uint64_t cbuf_gpu_va;
    uint32_t register_count_v;
    uint32_t grid_x, grid_y, grid_z;
    uint16_t block_x, block_y, block_z;
    uint32_t smem_size_bytes;
    uint32_t slm_size_bytes;
    uint16_t barrier_count;
    uint16_t _pad;
};

/* Plus a top-level handoff pool descriptor */
struct ga10b_handoff_v7 {
    /* ... existing v6 fields ... */
    uint64_t qmd_pool_phys;
    uint64_t qmd_pool_gpu_va;
    uint32_t qmd_pool_size_bytes;
    uint32_t qmd_pool_n_slots;   /* qmd_pool_size_bytes / 256 */
};
```

Static asserts pin the new sizes; `ga10b_bringup_inherit` and the
pipeline runner accept v6 (helper-baked path) OR v7 (per-dispatch
path), with a runtime toggle.

**B. SLM-OS parses the helper-baked QMD bytes** to extract the inputs,
then encodes a fresh QMD with the same inputs at a different GPU VA.
No handoff change. Slightly slower (a parse step at init), but
self-contained.

**Recommendation:** A. The handoff bump is small and explicit; the
helper already knows these values when it builds its QMDs. Parsing
QMD bit fields out of the baked bytes is a needless decode step and
loses the build-time invariants. v6 stays supported as a build-time
toggle for A/B comparison and regression fallback.

---

## 5. Validation strategy

The byte-compare-vs-baked-QMD test is the primary correctness gate.
The flow:

1. Helper continues to bake its QMD chain at known
   `qmd_gpu_va`. Linux pre-kexec.
2. SLM-OS, given the same inputs the helper used, calls
   `slmos_gpu_populate_qmd_at(slot, op)` to build a fresh QMD for
   the same op.
3. Diff: read the baked QMD bytes from `op->qmd_gpu_va` (via the
   identity-mapped CPU view), read the freshly-built QMD bytes from
   `qmd_pool[slot]`, `memcmp` them.
4. **Expect byte-identical for the same op.** Any divergence is a
   bug in the encoder — fix before shipping.

This test runs at boot (or via a `gpu qmd-selftest` shell verb) and
asserts on mismatch. It's the same gate Mesa uses internally and the
single highest-leverage check we can apply before touching the
dispatch path. If the encoder emits wrong bits, byte-compare fails
immediately and points at exactly which field — versus the alternative
of "dispatch fails silently in some new way."

Once byte-compare passes for all 8 MNIST ops, switch the dispatch
path from "use `op->qmd_gpu_va` directly" to "build into pool[slot],
advance slot, dispatch from pool". Re-run the probe data
(constant-input × 10, alternating × 10) and confirm:

- 10 byte-identical logits on constant input (same as today)
- 10 byte-identical logits on first input, 10 byte-identical on
  second when alternating (no off-by-one)
- First dispatch returns the right logits (not all zeros)

---

## 6. Implementation phases

| Phase | Scope | Estimated effort |
|---|---|---|
| **0. Reading** | Done. | (today) |
| **1. Port encoder to SLM-OS** | Move `gpu_populate_qmd_at` + `gpu_qmd_set_bits` + the QMD bit-position constants from `scripts/gpu-launch-common.{c,h}` and `scripts/gpu-qmd-bits.h` into `kernel/gpu/nvidia/ga10b_qmd.{c,h}`. Cache-flush calls swap from `msync` to `gsp_platform->cache_clean`. No new logic. | 0.5 day |
| **2. Handoff v7** | Add the new fields to `ga10b_handoff` and `ga10b_pipeline_op`. Update the static asserts and field-offset pins. Helper writes them; SLM-OS reads them when v7 is signalled; v6 still accepted. | 0.5 day |
| **3. QMD pool allocation** | Helper allocates and GMMU-maps the QMD pool. Populates pool descriptor in handoff. | 0.5 day |
| **4. Byte-compare selftest** | `gpu qmd-selftest` shell verb that runs the encoder against each MNIST op's params and asserts byte-identical to the baked QMD. Run at boot too. | 0.5 day |
| **5. Switch dispatch to fresh QMD** | In `ga10b_bringup_launch_kernel` (`kernel/gpu/nvidia/ga10b_bringup.c:1739`), replace `op->qmd_gpu_va` with `qmd_pool[next_slot()]` after building the QMD bytes there. Build-time toggle to keep the helper-baked path reachable for A/B. | 0.5 day |
| **6. Re-run probes** | Constant-input × 10 + alternating × 10 + the original `mnist_loop.lua` 10-image pass. Confirm correctness against CPU NEON path. Update issue #558 with results. | 0.5 day |
| **7. Buffer / unknowns** | Whatever shows up. | 1 day |

Total: ~4 days of focused work, with a 1-day buffer.

---

## 7. Risks and unknowns

- **GMMU-mapping for the pool.** The helper's existing baked-QMD
  region works because the helper allocated it through the kernel
  driver's regular nvgpu allocator. Adding a separate pool may
  require the helper to call `nvgpu_dma_alloc_map` (or whatever the
  current API is named) for a new region. Likely small, but worth
  confirming on day 1 of phase 3.

- **Cache attributes on the pool memory.** The CPU writes the QMD
  bytes; the GPU reads them. The helper currently calls
  `msync(MS_SYNC)` after writing; SLM-OS will call
  `gsp_platform->cache_clean`. Both should hit the Point of
  Coherence. If we observe a *different* off-by-one after the switch
  (where the QMD pointer changes but the GPU still reads stale QMD
  bytes), the issue is CPU↔GPU coherency on the pool memory itself —
  a separate fix (likely an explicit GPU L2 invalidate in the
  pushbuffer prelude before `SEND_PCAS_A`). This would point us back
  toward Option 2 layered on top.

- **Pool sizing.** 1024 slots × 256 B = 256 KiB is generous for
  Qwen 2.5 1.5B's ~370 ops/token, but rests on the assumption that
  SKED elides on `gpu_va` (the more common driver-cache key). If
  SKED elides on content, pointer rotation alone won't help —
  consecutive launches at the same op-position produce byte-
  identical QMDs regardless of slot. Mitigations if we hit that:
  inject a per-launch nonce into a reserved QMD field, or emit an
  explicit cache-invalidate pushbuffer method between launches.
  Both are local changes to the same encoder.

- **Synchronous dispatch model — tracked in #573.** `submit_and_poll`
  (`kernel/gpu/nvidia/ga10b_bringup.c:1482`) busy-polls a semaphore
  per launch — ~30-50 µs of pure CPU/IO overhead per dispatch
  before the GPU starts. Fine for MNIST's 8 ops. At Qwen's ~370
  ops/token × 10 tokens/sec = ~3700 launches/sec, the per-launch
  overhead alone is ~120-180 ms/sec of CPU on top of the GPU's
  actual work, scaling linearly with token rate. Production drivers
  batch many launches into the pushbuffer, fire one doorbell, and
  use async signaling (interrupts/fences/MSI-X) for completion of
  *groups* of launches rather than per-launch polling. **Out of
  scope for this fix.** Tracked separately — see issue for "GPU
  dispatch architecture: async batched launch for SLM workloads."

- **Production drivers also use between-launch barriers.** Even with
  per-dispatch QMDs, NVK and nouveau emit `MEM_BARRIER` /
  `INVALIDATE_*_CACHE` methods between consecutive dispatches in the
  pushbuffer. Phase 7 may discover SLM-OS needs the same — that's
  Option 2 from the original analysis. Budget a half-day for it in
  the buffer if so.

---

## 8. References

| File | Lines | What it is |
|---|---|---|
| `scripts/gpu-launch-common.c` | 395-481 | Existing C QMD encoder (Linux-side helper). Mirrors NVK's `Qmd3_0`. Direct port target for SLM-OS. |
| `scripts/gpu-launch-common.h` | 108-141 | QMD bit-position constants for QMDV03_00 (Ampere). |
| `scripts/gpu-qmd-bits.h` | — | `gpu_qmd_set_bits` helper. Pure-logic, host-testable, already used by both the helper and by SLM-OS test code. |
| `kernel/gpu/nvidia/ga10b_bringup.c` | 1739-1850 | Pipeline runner. Where the dispatch site lives. |
| `kernel/gpu/nvidia/ga10b_channel_handoff.h` | 190-217 | `struct ga10b_pipeline_op` definition + size assert. v7 bump goes here. |
| `~/slmos-ref/mesa/mesa-nak_qmd.rs` | 499-528, 616-655 | NVK's Rust QMD encoder for Ampere (`Qmd3_0`). Cross-reference for any field we're unsure about. |
| `~/slmos-ref/mesa/mesa-nvk_cmd_dispatch.c` | 159-260 | NVK's `nvk_cmd_upload_qmd` — model for the upload pattern (allocate buffer, fill QMD, dispatch). |
| `~/slmos-ref/mesa/mesa-clc7c0qmd.h` | — | Authoritative Ampere QMD field definitions, auto-generated from NVIDIA's `open-gpu-doc`. The reference if we hit a "what is this bit?" question. |

---

*Written 2026-04-29 against issue #558 evidence.*
