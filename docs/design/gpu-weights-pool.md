# GPU Weights Pool — Persistent GPU-VA staging for SLM model weights

Stage SLM model weight tensors into the inherited GA10B channel's address
space at `slm load` time so the per-op hybrid forward path
(#714 §B.3) can dispatch operators against GPU-resident weights instead
of trying to round-trip multi-MB tensors through 64 KB scratch slots
per call.

**Tracking:** to be filed alongside the first implementation PR.
**Status:** spec only. No code changes proposed in this document.

---

## The problem this fixes

PR #762 wired the first hybrid-forward call site (RMSNORM) through the
GPU operator library. The pattern works because RMSNORM's input is
small — a single 3 KB activation row — and copying it through a
helper-staged 64 KB scratch slot per call is feasible (if not fast).

The remaining 5 SLM ops can't use that same template:

| Op | Largest input | 64 KB slot fits? | Why |
|---|---:|:-:|---|
| EMBEDDING | `token_embd.weight` ≈ 150 MB | ❌ | full vocab table per call is 2400× the slot |
| Q4K_DOT (per matmul) | weight matrix 1.3 MB+ | ❌ | Qwen Q/K/V/O/gate/up/down all multi-MB |
| GQA_ATTN | KV cache 2 MB+ | ❌ | scales with `seq_len`; long contexts blow the slot |
| SWIGLU (element-wise) | activations 17.5 KB | ✅ | but the call site is buried inside `swiglu_mlp_q` |
| Q4K_DEQUANT | depends on caller | n/a | no standalone forward.rs call site today |

The first three rows are the load-bearing ops: EMBEDDING is 1 call per
token, Q4K_DOT is ~7 calls per layer × 30 layers = 211 calls per token,
GQA_ATTN is 1 call per layer = 30 calls per token. Those are the calls
that have to land on the GPU for the SLM-on-GPU demo to be worth
running.

The blocker is shared across all three: **the GPU cannot read CPU-heap
memory directly**. Today's only GPU-readable region is the
helper-staged 1 MB SASS pool — too small for weights, and the wrong
addressing model (slot-allocated scratch) for stable per-tensor VAs.

## Proposed: per-channel weights pool

Extend the Linux-side helper (`scripts/gpu-channel-helper.c`) to
allocate an additional, larger DMA buffer, map it into the channel's
GMMU at a known GPU VA, and publish the descriptor in the
`ga10b_channel_handoff`. SLM-OS at `slm load` time copies model
weights into this pool, splits it into per-tensor regions, and caches
the per-tensor GPU VA on the `LoadedSlm`. Forward.rs's hybrid
wrappers then pass the cached GPU VA to the dispatcher — no per-call
weight copy.

### Sizing

Qwen2.5-1.5B Q4_K_M is the demo target:

| Tensor | Bytes | Notes |
|---|---:|---|
| `token_embd.weight` (Q6_K) | ~191 MB | 151,936 × 1260 (Q6_K is ~0.82 B/elem × 1536 hidden) |
| `attn_*.weight` × 30 layers | ~600 MB | Q/K/V/O × 30 |
| `ffn_*.weight` × 30 layers | ~250 MB | gate/up/down × 30 |
| `output_norm.weight` etc. | <1 MB | small per-layer norms |
| **Total weights** | **~1.0 GB** | matches Q4_K_M file size |

Plus persistent KV cache for GQA_ATTN:
- 30 layers × 2 (K, V) × `n_head_kv=2` × `head_dim=128` × `seq_len_max=4096` × FP16 = **~7.5 MB**

Plus existing SASS pool (1 MB) + QMD pool (256 KB) + scratch slots
(within SASS pool today).

**Recommended initial pool size: 1.5 GB** for Qwen2.5-1.5B with
headroom; smaller models scale down. Jetson Orin Nano has 8 GB shared
RAM — plenty of headroom even with the OS, Linux services pre-kexec,
and SLM-OS itself.

### Lifecycle

The pool is allocated by Linux's nvmap before kexec, so it survives the
handoff to SLM-OS just like the SASS pool does today. SLM-OS treats
the pool as a bump allocator with per-tensor slots:

```
[ token_embd 150 MB ][ layer0 attn_q 2 MB ][ layer0 attn_k 2 MB ] ...
   ↑                   ↑                     ↑
   gpu_va_base         gpu_va_base + 0x9600000   ...
```

Each `(tensor_name, gpu_va, size)` triple goes into `LoadedSlm`'s new
`gpu_tensor_map: BTreeMap<String, GpuTensorRef>`. Tensor names are
constructed at runtime via `format!` against the layer index
(`blk.0.attn_q.weight`, `blk.1.attn_q.weight`, …) so `&'static str`
keys aren't workable; `String` allocates per-key but the population
runs once at `slm load` time and lookups are read-only afterwards.
Lookup is O(log n) on tensor count (~250 tensors for Qwen 30 layers)
— negligible.

`slm unload` frees the slots back into the bump allocator's free list.
For the simplest first cut: no free list, the pool resets when SLM-OS
loses the channel (kexec or shutdown). One model per channel.

### Wire-format extension: handoff `weights_pool_*` fields

```c
struct ga10b_channel_handoff {
    /* … existing v7 fields … */

    /* --- v8 extension: weights pool. ---
     * Zero on v2..v7. Populated by `gpu-channel-helper --weights-pool-size <N>`
     * (default 0 = no weights pool, equivalent to v7). When non-zero,
     * SLM-OS's `slm load` copies model weights into this pool and
     * caches per-tensor GPU VAs in `LoadedSlm`. Backward compatible:
     * v7 handoffs read these as zero (no pool), forward.rs's hybrid
     * wrappers fall through to CPU. */
    uint64_t weights_pool_phys;
    uint64_t weights_pool_gpu_va;
    uint64_t weights_pool_size_bytes;
};
```

`version` field bumps from 7 to 8 when the pool is non-zero. Existing
v7 consumers that ignore the new fields continue to work.

### Helper extension

```
sudo /home/slmos-1/gpu-channel-only/build/scripts/gpu-channel-helper \
    --qmd-pool \
    --weights-pool-size 1610612736 \    # 1.5 GB
    --timeout-secs 600
```

The helper allocates the pool via nvmap (existing pattern), maps it
via `NVGPU_AS_IOCTL_MAP_BUFFER_EX`, and writes the resulting
`(phys, gpu_va, size)` into the handoff. No SASS-pool-style copy: the
pool starts empty; SLM-OS populates it post-kexec.

The helper accepts the size at the command line so test runs can use a
small pool (e.g. 32 MB) for the synthetic Qwen fixture without paying
the 1.5 GB allocation. CI runs at low size; production runs at full
size.

### SLM-OS side: `LoadedSlm` extension

```rust
pub struct LoadedSlm {
    pub gguf: GgufHandle,
    pub arch: ArchInfo,
    /* …existing fields… */

    /// Per-tensor GPU VA cache. Populated at `slm load` time when a
    /// weights pool is available; empty otherwise. Forward.rs's hybrid
    /// wrappers query this map; on miss, fall through to CPU. Keys
    /// are owned `String`s because per-layer tensor names
    /// (`blk.<i>.attn_q.weight`) are constructed at runtime.
    pub gpu_tensor_map: BTreeMap<String, GpuTensorRef>,
}

pub struct GpuTensorRef {
    pub gpu_va: u64,
    pub size_bytes: usize,
}
```

New API on the kernel side:

```c
/* Stage `bytes` into the weights pool starting at the current bump
 * pointer. Returns 0 on success and fills `out_gpu_va`; returns -1
 * if the pool is exhausted or no handoff is staged. The caller's
 * CPU buffer is copied byte-for-byte into the pool's GPU-mapped
 * memory; cache_clean_range handles drain to PoC. */
int slm_runtime_stage_weight(const void *cpu_bytes, size_t bytes,
                              uint64_t *out_gpu_va);
```

Rust wrapper:

```rust
impl LoadedSlm {
    pub fn stage_tensor_to_gpu(
        &mut self,
        name: &str,         /* clones to a String for the map key */
        bytes: &[u8],
    ) -> Result<(), &'static str> {
        let mut gpu_va = 0u64;
        // SAFETY: bytes is valid for bytes.len() bytes; FFI copies and
        // does not retain the pointer.
        let rc = unsafe {
            slm_runtime_stage_weight(bytes.as_ptr() as *const _,
                                      bytes.len(), &mut gpu_va)
        };
        if rc != 0 { return Err("weights pool staging failed"); }
        self.gpu_tensor_map.insert(name.to_string(), GpuTensorRef {
            gpu_va,
            size_bytes: bytes.len(),
        });
        Ok(())
    }
}
```

`slm load` calls `stage_tensor_to_gpu` for the major tensors after
the GGUF parse + arch detect phase. Failed staging is non-fatal —
the model is still loaded; forward.rs falls back to CPU on any tensor
that didn't make it.

### Forward.rs hybrid wiring (post-staging)

For each weight-bearing op, the hybrid wrapper consults
`gpu_tensor_map`:

```rust
fn embedding_lookup_hybrid(
    slm: &LoadedSlm,
    token_id: u32,
    out: &mut [u16],
    cpu_scratch: &mut [f32],
) -> Option<()> {
    // Try GPU path: requires the table is staged AND the boot probe
    // flipped EMBEDDING to Tier::Simt.
    if let Some(table) = slm.gpu_tensor_map.get("token_embd.weight") {
        if select_tier(OpKind::Embedding) == Tier::Simt {
            if OperatorLibraryBackend::dispatch_embedding(
                table.gpu_va, table.size_bytes,
                token_id, out,
                /*embedding_length*/ slm.arch.hidden_size,
            ).is_ok() {
                return Some(());
            }
        }
    }
    // Fall through to CPU.
    let (q, b) = tensor_q(slm, "token_embd.weight")?;
    embedding_lookup_any(q, b, slm.arch.hidden_size, token_id, out, cpu_scratch)
}
```

The pattern is the same as `rmsnorm_hybrid` from PR #762, just with
the GPU VA coming from `gpu_tensor_map` instead of being copied from
CPU per call.

## Implementation PR sequence

| PR | Scope | Lines (est) |
|---|---|---:|
| **W1** | Helper `--weights-pool-size` arg + handoff `weights_pool_*` fields + SLM-OS-side accessors | ~250 |
| **W2** | `slm_runtime_stage_weight` C FFI + Rust `LoadedSlm::stage_tensor_to_gpu` + `gpu_tensor_map` | ~200 |
| **W3** | `slm load` populates `gpu_tensor_map` for the SLM tensor set | ~150 |
| **W4** | EMBEDDING hybrid (FFI shim + Rust backend method + forward.rs `embedding_lookup_hybrid`) | ~250 |
| **W5** | Q4K_DOT hybrid (matmul wrapper + the per-call activation-stage path) | ~350 |
| **W6** | GQA_ATTN hybrid (+ persistent KV cache staging — possibly a sub-PR) | ~400 |
| **W7** | SWIGLU hybrid (refactor `swiglu_mlp_q` to expose the element-wise step) | ~200 |
| **W8** | Q4K_DEQUANT — defer or close as "no integration site" | n/a |

W1-W3 are the architectural work. After W3 lands, W4-W7 follow the
RMSNORM template mechanically. W4 is the natural first hybrid op to
demonstrate the staging — it's a single tensor lookup, easy to verify
bit-exactly against the CPU reference.

## Memory cost

Per-channel pool of 1.5 GB on Jetson Orin Nano (8 GB total). Two
distinct memory windows to track:

1. **Pre-kexec window** (Linux running, helper allocating). Linux
   uses ~2 GB; the helper's nvmap allocation reserves the 1.5 GB
   pool. Concurrent peak ≈ 3.5 GB. Linux still has headroom for
   normal processes.
2. **Post-kexec window** (SLM-OS owns RAM). Linux vacates; SLM-OS's
   static + dynamic allocations (~1 GB Rust heap + ~1 GB ramdisk +
   ~150 MB kernel runtime) plus the inherited 1.5 GB pool ≈ 3.7 GB
   concurrent. Plenty of headroom.

The transient peak isn't either window — it's **at `slm load` time**:
the GGUF mmap-buffer + Rust-heap copy hold the weights once, then
`stage_tensor_to_gpu`'s memcpy transiently holds them again in the
pool. Peak ≈ 2 × weight size = ~2 GB for Qwen 1.5B until the CPU-side
buffer is freed. Doable on Jetson but tight; future optimization
(see "mmap-existing-pages alternative" below) eliminates the
duplication.

For development, smaller test fixtures (e.g. SmolLM2-135M ≈ 90 MB
weights) need a much smaller pool. The `--weights-pool-size` flag lets
the helper tune per workload.

## Activation lifecycle (out of scope for W1-W3)

This doc covers **weight** staging. The activation lifecycle (residual
stream + layer outputs) is a separate question:

- Today: activations live in `ForwardScratch` on CPU heap.
- For one-op-at-a-time hybrid (W4-W7): each hybrid wrapper copies the
  activation in/out of scratch slots — same per-call CPU↔GPU
  round-trip cost as RMSNORM today.
- For multi-op-on-GPU forward (future): activations live on GPU
  permanently across the forward, only crossing to CPU at sample
  time. Requires forward.rs restructure.

The first cut targets the W4-W7 pattern (per-call activation copies);
multi-op-on-GPU is a future arc once the per-op hybrids are all live.

## Risks

- ~~**CMA contiguity at 1.5 GB** (W1 prerequisite).~~ **Resolved by
  the W1 PoC on jetson-nano-1 (2026-05-10).** `scripts/cma-probe.c`
  allocates 1.5 GB via the same nvmap IOVMM-heap path the helper
  uses (`heap_mask=0x40000000`), mmaps the result, touches every
  64 KB chunk to verify real backing — succeeds on the first try.
  Key insight: CmaTotal on Jetson L4T is only 256 MB, but the
  IOVMM heap doesn't actually require physically-contiguous CMA
  pages — the SMMU stitches scattered physical pages into one
  virtually-contiguous IO address. So the 1.5 GB pool fits a
  single (phys, gpu_va, size) triple in the v8 handoff; the
  multi-chunk fallback design isn't needed for the W1 cut.
  Re-probe via `cma-probe` if a future design wants > 1.5 GB or
  a non-IOVMM heap.
- **Bump-allocator alignment policy** (W2 prerequisite). Q4_K
  weight rows have 144-byte super-block alignment requirements
  (already pinned in `EMBEDDING_Q4K_BLOCK_BYTES`). The bump
  allocator must round each `stage_tensor_to_gpu` call up to a
  suitable boundary (256 B for Q4_K-shaped tensors, 4 KB for
  conservative page-aligned everything-else) so per-tensor offsets
  decode correctly under the GPU's GMMU view. Trivial implementation;
  pinned here so W2 doesn't ship an unaligned bump pointer.
- **Helper backward compatibility**: production SLM-OS deployments
  using the old helper without `--weights-pool-size` see
  `weights_pool_size = 0`, fall through to CPU. ✓ safe.
- **Pool exhaustion**: bump allocator, no free; user can't load two
  models that exceed the pool. Acceptable for the v8 cut; multi-model
  needs a real allocator (out of scope).
- **Tensor-name drift**: `gpu_tensor_map` is keyed on tensor name. A
  GGUF file using a non-standard naming scheme (renamed
  `attn_q.weight` etc.) would silently miss the GPU path. The CPU
  fallback covers correctness; surface as a `slm gpu` diagnostic
  ("X of Y tensors staged").
- **Cache coherency**: weights are written CPU-side at `slm load`,
  cache_clean_range issued before first GPU dispatch. GPU writes
  back to weights buffers shouldn't happen (read-only model
  weights), so no GPU→CPU cache_invalidate cost on the hot path.
- **Helper restart cost**: re-running `slm load` while the helper
  is alive in Linux requires the user to power-cycle and re-stage.
  Same constraint as today's SASS pool.

## Future optimization: mmap-existing-pages alternative

The W2 design **copies** CPU heap → GPU pool at `slm load` time.
That doubles peak memory (~2× weight size during the copy window)
and burns ~30 ms of CPU↔DRAM bandwidth per gigabyte of weights.

An alternative is to **map the existing CPU heap pages directly into
the GMMU**, no copy. This requires a kernel API like
`ga10b_gmmu_map_existing(phys, size) → gpu_va` that doesn't exist
today: the current `ga10b_gmmu_alloc` allocates fresh PMM pages and
maps them. Adding `_map_existing` is a contained kernel change
(builds PTE entries from an arbitrary physical-page list).

Saved: 1 GB transient peak at `slm load`, plus the memcpy time.
Cost: weight pages stay locked in PMM (can't be paged out — moot
in a no-paging system) and lifetime management becomes more
delicate (the mapped pages must outlive the channel).

**Sequencing**: the copy-based design (W1-W3 above) is the safe
first cut. If memory pressure on Jetson Orin Nano under the 1.5 GB
pool turns out to bite, file a follow-on for `_map_existing`.
Not blocking for the umbrella tracking issue.

## Validation plan

Each PR plugs into the existing test infrastructure rather than
inventing a new harness:

For W1 (helper + handoff schema):
- QEMU `make test`: handoff struct size + offset `_Static_assert`s
  in `kernel/gpu/nvidia/ga10b_channel_handoff.h` catch wire-format
  drift at compile time.
- Hardware (jetson-nano-1): helper allocates the pool successfully
  with the new flag; SLM-OS post-`oplib stage` prints
  `[oplib-pool] weights region: phys=… gpu_va=… size=…` and the
  values match the helper's stdout.

For W2 (`stage_weight` FFI + Rust API):
- Hosted unit tests in `runtime/src/inference/gpu_slm.rs` (pattern
  from PR #762's `oplib_backend_*` tests): mock the FFI to return
  a deterministic `gpu_va`, verify `stage_tensor_to_gpu` populates
  `gpu_tensor_map` with the right key/value/size triple. Bounds
  checks on pool-exhaustion and the bump-pointer alignment math
  covered here too.
- QEMU `make test`: the FFI's stub return `-1` (no handoff) is
  exercised by every test that loads a model — verifies the
  fallback path stays bit-exact with the pre-W2 behavior.

For W3 (`slm load` populates `gpu_tensor_map`):
- Hosted: the existing `test_slm_load.c` fixtures load synthetic
  Qwen-shaped models; assert that `gpu_tensor_map` is populated
  with the expected tensor names when staging is mocked-success
  and stays empty when mocked-fail.
- Hardware: `slm gpu` shell verb extends to dump the staged tensor
  count + total bytes; expect ~250 / ~1 GB for Qwen2.5-1.5B.

For W4-W7 (per-op hybrids):
- Hardware: bit-exact comparison of GPU output against CPU reference
  per op, using the existing smoke-verb fixtures (#759). Each op's
  hybrid wrapper is exercised twice in the same `slm prompt` run —
  once with the tier flipped to `Tier::Cpu` (forces CPU path), once
  with `Tier::Simt` (forces GPU path); the two outputs must match
  bit-for-bit (modulo documented FP rounding tolerance).
- `boot_test --count 10` on jetson-nano-1 against a fixed prompt
  catches regressions across the full pipeline.

Throughout: the existing `make test` (QEMU) + cross-platform builds
(Pi 5, x86-64) gate every PR; behavior on non-Jetson platforms is
unchanged because the weights pool is Jetson-only.

## References

- `kernel/gpu/nvidia/ga10b_channel_handoff.h` — current v7 handoff
  schema; v8 adds the three pool fields above.
- `scripts/gpu-channel-helper.c` — Linux-side allocator + handoff
  populator.
- `runtime/src/slm/registry.rs::LoadedSlm` — the struct that gains
  `gpu_tensor_map`.
- `kernel/src/slm_ffi.c::slm_runtime_dispatch_rmsnorm_simt` (#762) —
  the FFI shim pattern that W4-W7 follow.
- #714 §B.3 — the umbrella tracking issue for the SLM-on-GPU forward.

---

*Author: GPU SLM B.3 follow-on*
*Last updated: 2026-05-10*
