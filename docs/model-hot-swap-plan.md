# Model Hot-Swap Plan (CPU-Pool Weights)

Plan for [issue #232](https://github.com/SLM-OS/SLM-Operating-System/issues/232) — atomic weight replacement for models held in CPU-visible memory pools. Hailo-backend swap is split out to [#532](https://github.com/SLM-OS/SLM-Operating-System/issues/532).

## Pre-flight finding

The inference path (`runtime/src/inference/engine.rs:184`, `runtime/src/inference/gpu.rs:229`/`:248`) currently calls `registry::get_weights(idx)`, which returns a clone of the `ModelHandle` **without bumping the refcount**. That means today an `unload` while inference is in flight would already be a use-after-free — the swap work surfaces a latent bug. Fixing the inference path to use `share_weights` / `unshare` is a prerequisite, not an add-on.

## Step 1 — Refcount the inference path (foundational)

**Files:** `runtime/src/inference/engine.rs`, `runtime/src/inference/gpu.rs`

Change every `get_weights(idx)` call site to:

```rust
let handle = registry::share_weights(idx).ok_or(...)?;
// ... do inference using mm::get_ptr(handle) ...
let _ = mm::unshare(handle);
```

Wrap with RAII (a small `WeightLease` struct that holds the handle and unshares on drop) so an early return / panic doesn't leak the refcount.

**Test:** unit test in `runtime/src/inference/` that confirms the refcount is held during inference and released after.

## Step 2 — Backend-aware swap dispatch

**File:** `runtime/src/loader/registry.rs` + new vtable hook

The registry needs to know whether a slot's backend can do a "trivial" CPU-pool swap or needs to delegate to a backend-specific path. Add to `LoadedModelEntry`:

```rust
backend_kind: BackendKind,  // CpuOnnx, Hailo, Gpu, ...
```

Set at load time based on which loader path was taken. The swap function checks this and, for CPU-only kinds, runs the trivial path; for `Hailo`, returns `LoadError::SwapNotSupported` (until [#532](https://github.com/SLM-OS/SLM-Operating-System/issues/532) lands).

## Step 3 — Implement `swap_model`

**File:** `runtime/src/loader/registry.rs`

```rust
pub fn swap_model(index: usize, new_name: &[u8], new_data: &[u8])
    -> Result<(), LoadError>
{
    // 1. Validate target slot exists, is active, backend is CpuOnnx.
    // 2. Parse new ONNX, build graph, alloc + populate new weights/workspace
    //    (do all this OUTSIDE the registry lock to avoid holding it across
    //    a multi-MB allocation).
    // 3. Acquire registry lock.
    // 4. Snapshot the OLD ModelHandle for weights+workspace.
    // 5. Replace entry's weights/workspace/graph/info with the new ones.
    // 6. Release registry lock.
    // 7. mm::free(old_weights) — if any inference holds share_weights,
    //    free decrements refcount and the actual deallocation defers
    //    until the last lease drops.
}
```

The swap is atomic from the inference path's perspective: any `share_weights(idx)` call sees either fully-old or fully-new — never half-swapped.

## Step 4 — FFI + shell command

**Files:** `runtime/src/lib.rs`, `kernel/include/slm_ffi.h`, `kernel/src/shell_sys.c`

```rust
#[no_mangle]
pub extern "C" fn rust_model_swap(
    index: u32,
    name: *const u8,
    data: *const u8,
    data_len: usize,
) -> i32
```

Add `model swap <name|idx> <path>` subcommand alongside the existing `model load/unload/info/...` in `cmd_model` (`shell_sys.c:2483`). Pattern matches `model_load`: read file from `/mnt/files/`, call `rust_model_swap`. Return error string on `SwapNotSupported` pointing at [#532](https://github.com/SLM-OS/SLM-Operating-System/issues/532).

## Step 5 — Lua binding

**File:** `kernel/src/lua_slm.c`

```c
static int l_model_swap(lua_State *L) {
    // args: index (int), path (string)
    // returns: 0 on success, error code on failure
}
```

Register in the `slm` namespace alongside existing model bindings.

## Step 6 — Tests

**File:** `kernel/tests/test_model_swap.c` (new) plus a Lua test under `scripts/`

QEMU coverage:

1. Load model A. Verify `model info 0` shows A's metadata.
2. Swap to model B. Verify `model info 0` shows B's metadata.
3. Verify A's weight block was freed (`mm::stats()` shows expected free count).
4. **Concurrent-safety test:** spawn an inference task that loops `share_weights` → `mm::get_ptr` → spin briefly → `unshare`. From the main task, swap A→B mid-loop. Verify the inference task's old-weight reads stayed valid (no torn data, pointer never went stale) and that subsequent reads see B's data.
5. Reject path: try to swap a slot that doesn't exist → `InvalidSlot`. Try to swap a Hailo-backed slot → `SwapNotSupported` with reference to [#532](https://github.com/SLM-OS/SLM-Operating-System/issues/532).

Hardware coverage (Pi 5):

- `boot_test --count 10` running a Lua script that does load+swap+infer in a loop, to validate cross-CPU refcount visibility under DSU coherency.

## Step 7 — Docs

**Files:** `docs/specs/lua-bindings.md`, shell `help` text

Document the new binding/command, the Hailo limitation pointing at [#532](https://github.com/SLM-OS/SLM-Operating-System/issues/532), and the concurrent-safety guarantee.

## Estimated touch surface

- Rust runtime: ~150 LOC + 50 LOC tests
- Kernel C: ~80 LOC shell + Lua + FFI declarations
- Tests: ~200 LOC
- Total: small, scope-contained, no new infrastructure needed

## Out of scope (for this issue)

- Hailo CCW re-upload swap path → [#532](https://github.com/SLM-OS/SLM-Operating-System/issues/532)
- Any other accelerator backend (NVDLA, GSP) — the same vtable hook will receive their swap implementations later
- Online weight quantization or format conversion during swap
