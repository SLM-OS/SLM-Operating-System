# GPU SLM Handoff — Design Spec

**Status:** M6.A scaffolding only — schema, Rust backend skeleton, and FFI
shim. All SASS kernel authoring (M6.B / M6.C / M6.D) and the pre-kexec
loader (M6.A-2) and bare-metal pushbuffer dispatch (M6.A-3) are deferred.

**Companions:**

- Plan: [`docs/plans/slm-integration-plan.md`](../plans/slm-integration-plan.md) §M6
- Spec: [`docs/specs/slm-integration.md`](slm-integration.md) §"GPU
  Integration on Orin Nano"
- MNIST precedent: [`scripts/gpu-kernel-launch.c`](../../scripts/),
  PR #376
- Ampere bringup: [`kernel/gpu/nvidia/ga10b_bringup.c`](../../kernel/gpu/nvidia/)

---

## §1 Overview

The Orin Nano's GA10B Ampere GPU has 1024 CUDA cores and 32 third-gen
tensor cores; the headline 67 TOPS sparse-INT8 number is dominated by the
tensor cores. SLM-OS reaches the GPU post-kexec via a handoff bridge: an
L4T-side loader stages a descriptor page (weights, channel resources, a
SASS kernel pool, a per-op plan) and SLM-OS reads it after kexec to
dispatch transformer layers without re-running channel allocation.

The handoff design extends the proven MNIST pattern (PR #376) from "single
op chain" to "multi-op transformer plan with per-op tier selection".

This spec defines the M6.A scaffolding interfaces. The kernels themselves
(M6.B HMMA, M6.C SIMT, M6.D element-wise) are weeks of work and ship in
separate PRs once the scaffolding lands.

---

## §2 Handoff Schema

The header lives at `kernel/include/gpu_handoff.h`. Field-by-field:

| Field | Purpose |
|---|---|
| `magic` | `0x534C4D47` ("SLMG"). Validated **before** any other read. |
| `version` | Schema version. Current: `1`. Mismatched versions abort. |
| `op_count` | Number of `slm_gpu_op_desc_t` entries that follow the header. Capped at `SLM_GPU_HANDOFF_MAX_OPS = 256`. |
| `arch_kind` | `0 = qwen2`, `1 = llama`. Unknown values fall back to CPU. |
| `block_count` … `vocab_size` | Architecture dimensions, mirror of `runtime/src/slm/gguf.rs::ArchInfo`. Avoids re-parsing GGUF post-kexec. |
| `channel_id` | nvgpu channel id inherited from L4T. |
| `pushbuffer_va` / `_size` | Pre-allocated GPU VA region the bare-metal side writes pushbuffer instructions into. |
| `semaphore_page_va` | 4 KB page used for pushbuffer-completion semaphores. Polled by the bare-metal side. |
| `doorbell_page_va` | The channel's doorbell register MMIO page. |
| `weight_pool_va` / `_size` | Contiguous GPU VA region holding all Q4_K-packed weights. Per-op `weight_va` is an offset/VA into this pool. |
| `sass_kernel_pool_va` / `_size` | Read-only / executable GPU VA region with the M6.B / M6.C / M6.D SASS kernels concatenated. Per-op `sass_kernel_offset` selects one. |

Each `slm_gpu_op_desc_t` (64 bytes, packed) carries an `op_kind`, a
`tier`, the offset+size of its SASS kernel within the pool, the offset of
its weights within the weight pool, and the input/output dimensions. Pad
field at the end keeps the descriptor power-of-two for cache-friendly
array striding.

`_Static_assert` guards the header at ≤ 256 bytes and the descriptor at
exactly 64 bytes — bumping either is a schema-version bump.

---

## §3 Tier Model

Three dispatch tiers, layered as a fallback ladder:

| Tier | Constant | Source | Throughput |
|---|---|---|---|
| 1 | `SLM_GPU_TIER_HMMA` | M6.B SASS — `nvcuda::wmma` / `mma.sync.aligned` | 10–15 dense FP16 TFLOPS |
| 2 | `SLM_GPU_TIER_SIMT` | M6.C SASS — plain `fma.f16x2` SIMT  | 2–3 dense FP16 TFLOPS |
| CPU | `SLM_GPU_TIER_CPU` | M4 NEON path | 8–12 tok/s on Qwen2.5-1.5B |

`SLM_GPU_TIER_AUTO` resolves to "Tier 1 if smoke-test passed for that op
kind, else Tier 2, else CPU".

**Smoke-test gate (M6.A-4 — to be authored):** at session launch SLM-OS
dispatches a tiny 16x16 reference matmul through each Tier-1 kernel. A
failure (compile error, dispatch failure, miscompare against the
reference vector) demotes that op to Tier 2 for the lifetime of the
session and writes a `slm gpu` log entry. Per-op granularity means a
single problematic Tier-1 kernel never drags the whole pipeline down.

The Rust backend's `TIER_TABLE` (`runtime/src/inference/gpu_slm.rs`) is
the read side of this gate. It defaults to all-CPU; M6.A-4 populates it
after the smoke probes.

---

## §4 Per-op Kernel Inventory

Mirrors plan §M6.B / M6.C / M6.D:

| Op kind | Tier 1 (HMMA) | Tier 2 (SIMT) | Element-wise (single tier) |
|---|---|---|---|
| `RmsNorm` | — | — | M6.D-1 |
| `Rope` | — | — | M6.D-2 |
| `Embedding` | — | — | gather kernel (single tier) |
| `Q4kDot` | M6.B-1 | M6.C-1 | — |
| `Q4kGemm` | M6.B-2 | M6.C-2 | — |
| `GqaAttn` | M6.B-3 | M6.C-3 | — |
| `SwiGlu` | M6.B-4 | M6.C-4 | — |
| `LmHead` | M6.B-5 | M6.C-5 | — |

Element-wise / reduction ops have only one variant — they don't hit
tensor cores in either tier, so the second variant would be wasted
authoring.

Per-kernel HMMA utilization probe (M6.B-6) reads `%clock64` and the
MMA-issue counter at the kernel prologue/epilogue and returns the busy
fraction via the completion semaphore page.

---

## §5 Pre-kexec Loader (M6.A-2 — to be authored)

`scripts/slm-gpu-bringup.c`, ported from `scripts/gpu-kernel-launch.c`
(MNIST). Pseudocode:

```
parse argv: gguf path, model, options
gguf_open(path) → arch_info, weight tensors
gpu_open_channel() → channel_id, pushbuffer, semaphore, doorbell
weight_pool_va = gpu_map_weights(arch_info, tensors, Q4_K)
sass_kernel_pool_va = gpu_map_sass(get_kernel_blob(arch_info))

handoff = stage_page(slm_gpu_handoff_v1_t)
fill handoff header from arch_info + channel resources
for each op in plan(arch_info):
    desc = handoff.ops[i++]
    desc.op_kind = OpKind::from(op)
    desc.tier = SLM_GPU_TIER_AUTO
    desc.sass_kernel_offset = sass_offset_for(op)
    desc.weight_va = weight_offset_for(op)
    desc.input_dim = …
    desc.output_dim = …
publish_handoff_phys(handoff_pa)  // somewhere SLM-OS can find it
kexec_to_slmos()
```

The page is staged at a known PA (TBD — likely the same kexec-handoff
page used by MNIST, plus an extra reserved range for the op_desc array).
The strong override of `slm_gpu_get_handoff_phys()` lives in this loader
or in a Jetson-specific kernel C file that mirrors the MNIST pattern.

---

## §6 Bare-metal Dispatch (M6.A-3 — to be authored)

The Rust backend's `Backend::execute` impl on top of the staged channel.
Pseudocode for a single op:

```
fn execute(op_kind, tier, weight_va, input, output) -> Result<(), BackendError> {
    let h = Handoff::try_from_kernel().ok_or(NoHandoff)?;
    let desc = h.find_op(op_kind, tier).ok_or(NotAvailable)?;

    sync_for_device(input.as_ptr(), input.len()); // M6.A-5
    let pb = build_pushbuffer(&desc, weight_va, input, output);
    write_pb_at(handoff.pushbuffer_va, pb);
    ring_doorbell(handoff.doorbell_page_va);

    let t0 = ptimer_now();         // M6.A-6
    let sem = handoff.semaphore_page_va as *const u32;
    while semaphore_value(sem) != PB_DONE {
        if ptimer_now() - t0 > TIMEOUT { return Err(Timeout); }
    }
    let busy = ptimer_now() - t0;
    record_telemetry(op_kind, tier, busy);

    sync_for_cpu(output.as_ptr(), output.len()); // M6.A-5
    Ok(())
}
```

Pushbuffer construction follows the same Ampere method/subchannel
patterns as `kernel/gpu/nvidia/ga10b_bringup.c`. Reminder: GA10B uses
`SEND_SIGNALING_PCAS2_B` (method `0x02C0`, action `0xA`), **not**
Turing's `PCAS_B` — see `kernel/CLAUDE.md` "Jetson GA10B" note.

---

## §7 Cache Coherency (M6.A-5)

Reuse the Phase-5 GPU-framework helpers exposed via `slm_ffi.h`:

- `slm_gpu_sync_for_device(addr, size)` — `DC CVAC` flush before GPU
  reads CPU-written data (input tensors, weight tensor changes).
- `slm_gpu_sync_for_cpu(addr, size)` — `DC IVAC` invalidate before
  CPU reads GPU-written data (output tensors, KV-cache append result).

The bare-metal dispatch wraps every input / output buffer with these.
Weights are flushed once at session launch (not per-op) — they don't
change during decode.

---

## §8 PTIMER Telemetry (M6.A-6)

Per-pushbuffer wall-clock timing via the GA10B PTIMER MMIO at
`BAR0+0x9420` (32-bit nanosecond counter). Read fences before and after
each pushbuffer issue; the delta is the GPU-busy time for that op.

`slm stats --gpu` aggregates these into per-op-kind histograms. M6.B-6
adds tensor-core busy fraction on top of this for HMMA kernels (read
the SM `sm__inst_executed_pipe_tensor` equivalent counter via the SASS
prologue/epilogue, return through the semaphore page).

The kernel-side helper (TBD — likely
`slm_gpu_ptimer_ns()` in `slm_ffi.c`) is the bare-metal equivalent of
nvgpu's PTIMER read.

---

## §9 Future Kernel Authoring

The SASS bring-up plan (mirror of plan §M6.B/C/D):

1. **Tier 2 first per kernel.** Author `q4K_dot_f16.tier2.sass` (plain
   SIMT) before `q4K_dot_f16.tier1.sass` (HMMA). The Tier-2 kernel is
   simpler, gives a known-good reference output, and unblocks the Tier-1
   work without losing the GPU path on slip.
2. **Tier 1 against Tier-2 reference vectors.** Each Tier-1 kernel is
   numerically validated against the Tier-2 sibling on a held-out test
   vector before going live. Bit-exact within FP16 epsilon (max-abs ≤
   1e-2).
3. **Authoring toolchain.** CUDA C++ targeting `sm_87`, `nvcc -arch=sm_87`,
   extract SASS via `cuobjdump --dump-sass`, ship the SASS payload.
   Reference: llama.cpp's `ggml-cuda` backend
   (https://github.com/ggerganov/llama.cpp/tree/master/ggml-cuda).
4. **Concatenation.** All kernels concatenated into one SASS pool blob
   shipped pre-kexec. Per-op `sass_kernel_offset` selects the entry
   point. The pool is mapped read-only / executable on the GPU.

Per-op kernel slips fall back to Tier 2 (Tier-1 slip) or CPU (Tier-1 +
Tier-2 slip) at op granularity — the demo still GPU-accelerates the rest
of the pipeline.

---

## §10 Risk Register

(Cribbed from plan §M6 risk note; reproduced here so future kernel
authors have the mitigations close at hand.)

1. **HMMA tile orchestration is the highest-risk single item.** Per-op
   tiering means a failing Tier-1 kernel falls back to its Tier-2 sibling
   at op granularity, not session granularity.
2. **Tier-2 first, Tier-1 second per kernel.** Tier-2 work being cut
   never results in *no* GPU path for that op.
3. **CPU as ultimate floor.** Even if all of M6.B and M6.C slip, the M4
   NEON path runs at 8–12 tok/s.
4. **IMMA / INT8 / Tier-3 is a follow-up.** The tier model leaves a clean
   `Tier0_IMMA` insertion point; if HMMA proves intractable on any op,
   that op moves to the same after-the-fact bucket without restructuring.
5. **Schema bumps.** Bumping `SLM_GPU_HANDOFF_VERSION` is cheap (header
   change + L4T loader rev), but the bare-metal side and the L4T loader
   must rev together at kexec time. The magic check ensures a mismatched
   loader/SLM-OS pair fails closed (no GPU dispatch) instead of corrupting
   GPU state.
6. **`SECONDARY_PREEMPT` on Jetson is not safe yet.** GPU dispatch from a
   non-CPU-0 task hits the dual-cluster MPIDR fold bug (see kernel
   CLAUDE.md "Secondary-CPU preemption" note). M6.A-3 should pin GPU
   dispatch to CPU 0 until plan P3 step 2 lands.

---

*Last updated: 2026-04-27. Authored as part of M6.1 (#482) — see
[`docs/plans/slm-integration-plan.md`](../plans/slm-integration-plan.md) §M6.A.*
