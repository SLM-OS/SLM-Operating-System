# Session D — Rust Runtime

**Source:** `docs/code-review-2026-04-12.md` §6
**Scope:** 12 issues (6 CRITICAL, 4 HIGH, 2 MEDIUM)
**Files touched:** `runtime/src/inference/workspace.rs`, `runtime/src/inference/ops.rs`, `runtime/src/msg_router.rs`, `runtime/src/component/mod.rs`, `runtime/src/loader/registry.rs`, `runtime/src/mm/model_mem.rs`, `runtime/src/lib.rs`, `runtime/Cargo.toml`

## Mission

Harden the Rust runtime against integer overflow, `static mut` data races,
and unsafe-block invariant violations. The runtime is `no_std` kernel code,
so a panic halts the machine — paths that can fail must return `Result`,
not panic.

## Working rules

- Read `CLAUDE.md` and `runtime/CLAUDE.md`. The `no_std` constraints in
  `runtime/CLAUDE.md` override general Rust practice.
- **Every `unsafe` block** should have a `// SAFETY:` comment explaining the
  invariant. Add one where missing.
- Pi 5 testing matters for inference paths (`bench model`); x86 for base
  runtime.
- Commit `runtime/Cargo.lock` if dependency resolution changes (it shouldn't
  for these fixes).
- Work in a worktree: `git worktree add ../slm-os-session-d -b fix/session-d-rust`.
- Ask John before committing.

## Issues

### CRITICAL

#### RUST-C1 — `alloc_tensor` unchecked multiplication
- **File:** `runtime/src/inference/workspace.rs:57-68`
- **Problem:** Shape multiplication wraps `usize`, allocating less than
  needed; subsequent ops write OOB.
- **Fix:**
  ```rust
  let mut n_elem: usize = 1;
  for &d in shape {
      let dn = d as usize;
      n_elem = n_elem.checked_mul(dn).ok_or(TensorError::SizeOverflow)?;
  }
  let size = n_elem.checked_mul(core::mem::size_of::<f32>())
                   .ok_or(TensorError::SizeOverflow)?;
  ```
  Add a `TensorError::SizeOverflow` variant if needed.
- **Verification:** Add a unit test in `#[cfg(test)]` section passing a
  shape that would wrap. Runs locally with `cargo test --target x86_64-unknown-linux-gnu`
  if the runtime can be compiled for host (otherwise add a kernel-side test).

#### RUST-C2 — `BumpAllocator::alloc` alignment overflow
- **File:** `runtime/src/inference/workspace.rs:44-50`
- **Problem:** `(self.offset + align - 1) & !(align - 1)` can wrap `usize`;
  subsequent capacity check passes with a bogus pointer.
- **Fix:**
  ```rust
  let aligned = self.offset.checked_add(align - 1).ok_or(())? & !(align - 1);
  if aligned < self.offset { return core::ptr::null_mut(); }  // wrap guard
  if self.capacity.checked_sub(aligned).map_or(true, |rem| rem < size) {
      return core::ptr::null_mut();
  }
  let ptr = unsafe { self.base.add(aligned) };
  self.offset = aligned + size;
  ptr
  ```
- **Verification:** Unit test on a near-`usize::MAX` offset edge case.

#### RUST-C3 — `msg_router.rs` `static mut` globals race
- **File:** `runtime/src/msg_router.rs:153-157, 186-191, 212`
- **Problem:** `TOPICS`, `TOPIC_COUNT`, `WILDCARD_SUBS`, `LAST_RECEIVED`
  accessed from multi-CPU paths without held lock in all branches. `str_copy`
  reads unbounded C strings.
- **Fix:**
  - Mirror the OPS_LOCK pattern from `ops.rs`: add a `static MSG_ROUTER_LOCK:
    Spinlock = Spinlock::new();` and require lock acquisition before every
    access to any of the `static mut` globals.
  - Make `str_copy(dst: &mut [u8], src: *const u8, max_src_len: usize)` and
    bound the source read with a `max_src_len` parameter supplied by the
    caller.
  - Every `unsafe` block that touches the globals gets a `// SAFETY: MSG_ROUTER_LOCK held`
    comment.
- **Verification:** QEMU run with `bench smp` component; Pi 5 cross-CPU
  publish smoke test.

#### RUST-C4 — `im2col` buffer type confusion
- **File:** `runtime/src/inference/ops.rs:222, 238-241`
- **Problem:** `static mut IM2COL_BUF: [u32; IM2COL_MAX]` cast to `*mut f32`.
  Type is wrong and size accounting is fragile.
- **Fix:** Redefine as `static mut IM2COL_BUF: [f32; IM2COL_MAX] = [0.0; IM2COL_MAX];`.
  If capacity must still be expressed in bytes, use a dedicated
  `#[repr(C, align(16))] struct Im2ColBuf([f32; IM2COL_MAX])` so alignment
  and element type are clear.
- **Verification:** Run inference with a conv layer — results must match
  pre-change reference (add a regression value in the test if absent).

#### RUST-C5 — Component C-string deref before bounds check
- **File:** `runtime/src/component/mod.rs:83-93`
- **Problem:** Loop body dereferences `*p` before checking `len < MAX_NAME_LEN`.
  A non-NUL-terminated input reads OOB before the bounds check fires.
- **Fix:**
  ```rust
  if name.is_null() { return -1; }
  let mut len = 0;
  let mut p = name;
  while len < MAX_NAME_LEN {
      let c = unsafe { *p };  // SAFETY: len < MAX_NAME_LEN and caller promises valid memory
      if c == 0 { break; }
      len += 1;
      p = unsafe { p.add(1) };
  }
  if len == MAX_NAME_LEN { return -1; }  // no NUL found
  ```

#### RUST-C6 — `registry.rs` i64 decode assumes fixed size
- **File:** `runtime/src/loader/registry.rs:254-267`
- **Problem:** Assumes each packed varint produces exactly 8 bytes; no item
  count tracking.
- **Fix:**
  ```rust
  let max_items = I64_DECODE_BUF.len() / 8;
  let mut item_count = 0;
  for val in super::protobuf::packed_varint_i64(i64_packed) {
      if item_count >= max_items { break; }
      // write val to I64_DECODE_BUF at offset item_count * 8
      item_count += 1;
  }
  ```

### HIGH

#### RUST-H1 — Spinlock leak on panic / early return
- **File:** `runtime/src/mm/model_mem.rs:438-442`
- **Fix:** Convert manual `lock_acquire` / `lock_release` to RAII:
  ```rust
  struct SpinGuard;
  impl SpinGuard {
      fn new() -> Self { lock_acquire(); SpinGuard }
  }
  impl Drop for SpinGuard {
      fn drop(&mut self) { lock_release(); }
  }
  // Usage:
  let _g = SpinGuard::new();
  let result = unsafe { (*addr_of_mut!(WEIGHT_POOL)).alloc(POOL_WEIGHT) };
  /* _g dropped here, lock released */
  ```
  Apply pattern to all sites in `model_mem.rs`.
- **Verification:** Existing `test_model_mem.c` and runtime tests.

#### RUST-H2 — `c_str_to_bytes` must bounds-check every deref
- **File:** `runtime/src/component/mod.rs:200-206` (and the helper's definition)
- **Fix:** Find the `c_str_to_bytes` implementation (grep `fn c_str_to_bytes`),
  ensure the loop uses the `max` bound in the iteration condition, not in the
  body. Mirror the RUST-C5 fix pattern.

#### RUST-H3 — NEON intrinsics without explicit target feature
- **File:** `runtime/src/inference/ops.rs:130-150`
- **Fix:**
  ```rust
  #[cfg(target_arch = "aarch64")]
  #[target_feature(enable = "neon")]
  unsafe fn matmul_neon(...) { /* intrinsics here */ }
  ```
  Also verify `Cargo.toml`:
  ```toml
  [target.'cfg(target_arch = "aarch64")']
  rustflags = ["-C", "target-feature=+neon"]
  ```
  (or equivalent `.cargo/config.toml`).

#### RUST-H4 — Panics in inference hot path
- **File:** `runtime/src/lib.rs:128` and throughout `runtime/src/inference/`
- **Fix:** Grep for `panic!`, `unwrap()`, `expect()` in `runtime/src/inference/`
  and `runtime/src/loader/`. Replace with `.ok_or(InferenceError::...)?` or
  `.unwrap_or(default)`. Reserve `panic!` for truly unreachable states in
  `#[cfg(debug_assertions)]`-gated checks.
- **Note:** This is broad. Prioritize hot-path files (`engine.rs`, `ops.rs`,
  `workspace.rs`). If scope grows, split the rest into a post-capstone issue.

### MEDIUM

#### RUST-M1 — SIMD feature gates in `Cargo.toml`
- **File:** `runtime/Cargo.toml`, `runtime/.cargo/config.toml`
- **Fix:** Confirm aarch64 has `+neon` and x86_64 has `+sse,+sse2` (or
  stronger for the actual ops used). Combine with RUST-H3 work.

#### RUST-M2 — ONNX parser length trust
- **File:** `runtime/src/loader/onnx_parser.rs` (687 lines — audit
  wholesale)
- **Fix:** For every `read_bytes(n)` or equivalent slice read, verify
  `n <= remaining()`. Add a test with a truncated ONNX payload.

## Suggested work order

1. **RUST-C1, C2** (30 min) — workspace allocator hardening (pair; tests together)
2. **RUST-H1** (30 min) — SpinGuard RAII in model_mem
3. **RUST-C5** (15 min) — component name parse bounds
4. **RUST-H2** (20 min) — c_str_to_bytes audit
5. **RUST-C6** (15 min) — registry decode item count
6. **RUST-C4** (15 min) — im2col type fix
7. **RUST-C3** (1-2 hr) — msg_router spinlock + str_copy bounds
8. **RUST-H3 + RUST-M1** (30 min) — NEON target_feature + Cargo config
9. **RUST-M2** (1 hr) — ONNX length audit
10. **RUST-H4** (variable) — panic removal in hot path; defer rest

## Testing requirements

- **Every fix:** `cd runtime && cargo check --target aarch64-unknown-none`
  (and `x86_64-unknown-none`). Full kernel build: `make test`.
- **RUST-C1, C2, C5, C6:** Add unit tests in the runtime (`#[cfg(test)]`).
- **RUST-C3:** Pi 5 cross-CPU publish stress.
- **RUST-C4, H3:** Run `bench model` and verify numerical results unchanged.
- **RUST-M2:** Feed a truncated `.onnx` to the loader; expect error, no
  panic.
- **Jetson:** `make kernel PLATFORM=JETSON_ORIN_NANO AI_SCHED=ON` compiles.

## Deliverable — PR template

```
## Summary
Session D fixes from code-review-2026-04-12: Rust runtime hardening.

## Issues fixed
- RUST-C1..C6 (CRITICAL: overflow, races, type confusion)
- RUST-H1..H4 (HIGH: RAII, bounds, SIMD gate, panic→Result)
- RUST-M1, RUST-M2 (MEDIUM: Cargo config, ONNX bounds)

## Test plan
- [ ] `cargo check` for aarch64-unknown-none + x86_64-unknown-none
- [ ] `make test` (QEMU ARM64)
- [ ] `make test PLATFORM=X86_64`
- [ ] `make test AI_SCHED=ON`
- [ ] `bench model` numerical results match pre-change reference
- [ ] Truncated ONNX rejected without panic
- [ ] Pi 5 cross-CPU publish stress test
```
