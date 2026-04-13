# Session D — Rust Runtime

**Status:** ✅ Complete (all fixes landed; Pi 5 smoke test done — see hardware verification below)
**Source:** `docs/code-review-2026-04-12.md` §6
**Scope:** 12 issues (6 CRITICAL, 4 HIGH, 2 MEDIUM)
**Files touched:** `runtime/.cargo/config.toml`, `runtime/src/inference/workspace.rs`, `runtime/src/inference/engine.rs`, `runtime/src/inference/ops.rs`, `runtime/src/msg_router.rs`, `runtime/src/component/mod.rs`, `runtime/src/loader/protobuf.rs`, `runtime/src/loader/registry.rs`, `runtime/src/loader/onnx_parser.rs`, `runtime/src/mm/model_mem.rs`, `runtime/src/lib.rs`

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

## Resolution summary

| ID | Status | Verification |
|----|--------|--------------|
| RUST-C1 | ✅ Fixed | `workspace: shape overflow returns ShapeOverflow` (rust_run_tests) |
| RUST-C2 | ✅ Fixed | `workspace: alloc size overflow returns null` |
| RUST-C3 | ✅ Fixed | `msg_router: str_copy bounded`; 24 existing `test_msg_router_*` in `test_x86_boot.c`; Pi 5 subscribe/list/error-path cycles clean (see hardware verification) |
| RUST-C4 | ✅ Fixed | MNIST e2e inference (exercises im2col path) |
| RUST-C5 | ✅ Fixed | `component: find bounded deref` |
| RUST-C6 | ✅ Fixed | `protobuf: packed_varint_i64 decodes all entries` + registry.rs cap reviewed by inspection |
| RUST-H1 | ✅ Fixed | Existing `test_model_mem.c` + loader registry lifecycle tests; Pi 5 `model pools` confirms live `SpinGuard` on hardware |
| RUST-H2 | ✅ Fixed | `component: find bounded deref` exercises `c_str_to_bytes` by construction (component_register path) |
| RUST-H3 | ✅ Fixed | MNIST numerical correctness unchanged; `cargo check` both targets clean |
| RUST-H4 | ✅ Scope met | Hot path (engine/ops/workspace) already panic-free; issue #74 tracks sched/heterogeneous.rs follow-up |
| RUST-M1 | ✅ Fixed | `.cargo/config.toml` has `+neon` (aarch64) and `+sse,+sse2` (x86_64) |
| RUST-M2 | ✅ Fixed | `loader: truncated ONNX rejected` + `empty ONNX rejected` + `protobuf: length overflow detected` |

Follow-up issues:
- [#74](https://github.com/johnjezl/CS-496-Capstone-SLM-Operating-System/issues/74) — defensive `.unwrap()` calls in `sched/heterogeneous.rs` (RUST-H4 scope overflow).
- [#80](https://github.com/johnjezl/CS-496-Capstone-SLM-Operating-System/issues/80) — pre-existing Pi 5 publish hang (`pit_ticks` doesn't advance when shell task spins with DAIF.I=1). Reproduced against the pre-Session-D baseline; Session D's lock release semantics are correct.

## Pi 5 hardware verification — April 12, 2026

Built with Session D changes and deployed to `pi-5-1` via `labctl sdwire_update`.

**Boot reliability** — `labctl boot_test --runs 10 --expect-pattern "Type 'help' for available commands"`: 9/10 successful boots (one Kasa power-plug auth flake, unrelated to kernel code).

**msg_router lock paths (RUST-C3 + RUST-H1)** — all exercised cleanly post-fix:
- `msg subscribe /sensors/temp 0` → OK
- `msg subscribe /sensors/humidity 1` → stored as `/sensors/humidi` (bounded `str_copy` truncates at `TOPIC_NAME_LEN - 1 = 15`, then NUL)
- `msg subscribe /sensors/* 2` → wildcard registered
- `msg list` → returns all topics, lock correctly released on read path
- Fill all 8 topic slots, then 9th subscribe → `[msg] No free topic slots` error, **lock released** (next subscribe after this succeeds → SpinGuard RAII drop verified on error return)
- Fill topic `/a` to `MAX_SUBSCRIBERS = 4`, then 5th subscribe → `[msg] Topic '/a' full (4 subscribers)` via `uart_printf`, **lock released**, next command still works

**Model memory pool (RUST-H1 `model_mem` SpinGuard)** — `model pools` prints weight and workspace stats cleanly; lock flows through `weight_pool_stats()` → `SpinGuard::new()` → `(*addr_of_mut!(WEIGHT_POOL)).stats()` and is released on drop.

**SMP cross-CPU dispatch** — `bench smp` dispatched tasks to CPUs 1/2/3; all three `COMPLETED` (output interleaved across CPUs, known Pi 5 UART artifact unrelated to msg_router).

**Not run on Pi 5:** `msg send` to a subscriber with no ack-task hangs (pre-existing issue #80). Real cross-CPU publish stress with ack-capable components needs a component-IPC fixture that isn't wired for Pi 5 shell invocation today.

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
- [x] `cargo check` for aarch64-unknown-none + x86_64-unknown-none
- [x] `make test` (QEMU ARM64)
- [ ] `make test PLATFORM=X86_64` — pre-existing GRUB multiboot issue, unrelated
- [x] `make test AI_SCHED=ON`
- [x] `bench model` numerical results match pre-change reference (MNIST e2e `rust_run_tests` case 13)
- [x] Truncated ONNX rejected without panic (`rust_run_tests` case 12d)
- [x] Pi 5 boot reliability (9/10, 1 unrelated Kasa auth flake)
- [x] Pi 5 msg_router subscribe/list/error-path cycles — lock released correctly on every exit
- [x] Pi 5 `bench smp` cross-CPU dispatch
- [x] Pi 5 `model pools` — SpinGuard RAII on hardware
- Pi 5 publish-with-ack stress deferred: blocked by pre-existing issue #80 (`pit_ticks` on Pi 5), not a Session D regression
```

## Verification — test coverage

Tests added to `runtime/src/lib.rs::rust_run_tests` (exercised by `make test`):

| Test name (UART output) | Verifies |
|-------------------------|----------|
| `workspace: tensor alloc` | Baseline, updated to new `Result` return |
| `workspace: exhaustion returns WorkspaceExhausted` | RUST-C1 error variant distinct from OOM |
| `workspace: shape overflow returns ShapeOverflow` | RUST-C1 checked_mul path |
| `workspace: alloc size overflow returns null` | RUST-C2 checked_add / checked_sub path |
| `loader: truncated ONNX rejected without panic` | RUST-M2 protobuf bounds |
| `loader: empty ONNX rejected without panic` | RUST-M2 empty-buffer path |
| `msg_router: str_copy bounded (no over-read)` | RUST-C3 `str_copy(max_src_len)` |
| `component: find bounded deref (no over-read)` | RUST-C5, RUST-H2 bound-before-deref |
| `protobuf: packed_varint_i64 decodes all entries` | RUST-C6 iterator correctness |
| `protobuf: length overflow detected` | RUST-M2 `LengthOverflow` wire path |

Not covered by an added Rust-level test (rationale listed):

- **RUST-C4 (im2col type)** — exercised end-to-end by the MNIST `run_inference` test in case 13, which goes through the `conv2d` im2col path.
- **RUST-H1 (SpinGuard RAII)** — every existing call to `alloc_weights`, `alloc_workspace`, `free`, `share`, `get_ptr`, `get_size`, `weight_pool_stats`, `workspace_pool_stats`, plus every `loader::registry::*` op, now flows through `SpinGuard::new()`. If the guard failed to release on drop, the second call in any test pair would deadlock. `test_model_mem.c` and the loader lifecycle tests in `rust_run_tests` exercise this.
- **RUST-H3 (`target_feature(neon)`)** — numerical correctness of MNIST inference (case 13) is the strongest signal; mis-gated NEON would either fail to compile (hard) or produce incorrect results (soft).
- **RUST-H4 (hot-path panics)** — hot-path files were already clean; `rg '\.unwrap\(\)|\.expect\(|panic!' runtime/src/inference runtime/src/loader` returns zero hits. Tracked for scheduler in issue #74.
- **RUST-C3 SMP stress** — pub/sub on a single CPU is covered by the 24 msg_router tests in `test_x86_boot.c`. Multi-CPU publish/ack contention needs Pi 5 hardware to observe deadlock/over-read regressions under real SMP; deferred.
