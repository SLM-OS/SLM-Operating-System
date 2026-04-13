# Runtime-Specific Notes

Notes for working on the Rust runtime code.

---

## Crate Structure

```
runtime/
├── Cargo.toml           # Crate configuration (staticlib, no_std)
├── .cargo/
│   └── config.toml      # Cross-compilation settings for aarch64
└── src/
    ├── lib.rs           # Entry points (rust_init, rust_hello, etc.)
    ├── kernel_ffi.rs    # FFI declarations and safe wrappers
    ├── log.rs           # Logging infrastructure (via UART FFI)
    ├── msg_router.rs    # Topic-based pub/sub message router (FFI)
    ├── mm/
    │   ├── mod.rs       # Memory management module
    │   ├── model_mem.rs # Model memory allocator (weight/workspace pools)
    │   └── model_loader.rs  # Model loader skeleton (Phase 5)
    └── sched/
        ├── mod.rs       # Scheduler policy module
        ├── deadline.rs  # Deadline-aware scheduling
        ├── inference.rs # Inference scheduler skeleton (Phase 5)
        └── heterogeneous.rs  # big.LITTLE CPU topology awareness
```

### Module Overview

| Module | Purpose | Status |
|--------|---------|--------|
| `kernel_ffi` | FFI bindings to C kernel | Complete |
| `log` | Logging via UART (log_info, log_error, etc.) | Complete |
| `msg_router` | Topic-based pub/sub message router | Complete |
| `mm::model_mem` | 2MB-aligned model memory allocator | Complete |
| `mm::model_loader` | GGUF/ONNX model loading | Skeleton (Phase 5) |
| `sched::deadline` | Deadline-aware task scheduling hints | Complete |
| `sched::inference` | Inference request queue management | Skeleton (Phase 5) |
| `sched::heterogeneous` | CPU topology and core selection | Complete |

## `no_std` Conventions

This is a freestanding Rust crate. Key constraints:

- `#![no_std]` — no standard library
- `#![no_main]` — no Rust entry point (C kernel calls us)
- No `println!`, `format!`, or other `std` macros
- Use `core::` instead of `std::` for primitives

### Available from `core`

- `core::ptr`, `core::mem`, `core::slice`
- `core::ffi::c_char`, `core::ffi::c_void`
- `core::panic::PanicInfo`
- Primitive types: `u8`, `u32`, `u64`, `usize`, etc.

### External Dependencies

| Crate | Purpose | Notes |
|-------|---------|-------|
| `linked_list_allocator` | Global heap allocator | Requires `rust_heap_init()` call from C |
| `bitflags` | Type-safe flag enums | For `MemFlags`, `ShmFlags` |

---

## FFI Boundary Guidelines

See `docs/ffi.md` for comprehensive documentation.

### Quick Reference

**Rust → C calls:**
```rust
extern "C" {
    fn slm_print(s: *const c_char);
}
unsafe { slm_print(b"Hello\0".as_ptr() as *const c_char); }
```

**C → Rust calls:**
```rust
#[no_mangle]
pub extern "C" fn rust_init() -> i32 {
    42  // Magic number
}
```

### Safe Wrapper Pattern

Always provide safe wrappers for unsafe FFI:

```rust
// Unsafe raw function
extern "C" {
    fn slm_alloc_pages(count: usize) -> *mut u8;
}

// Safe wrapper
pub fn alloc_pages(count: usize) -> KernelResult<NonNull<u8>> {
    let ptr = unsafe { slm_alloc_pages(count) };
    NonNull::new(ptr).ok_or(KernelError::OutOfMemory)
}
```

---

## Unsafe Code Policies

### When `unsafe` is Required

1. **FFI calls** — calling any `extern "C"` function
2. **Raw pointer dereference** — when interfacing with C data
3. **Global mutable state** — the allocator initialization

### Documentation Requirements

Every `unsafe` block should have a comment explaining why it's safe:

```rust
// SAFETY: ptr was just returned by slm_alloc_pages and is non-null
// (checked above). The memory is valid for count * 4096 bytes.
unsafe {
    core::ptr::write_bytes(ptr, 0, count * 4096);
}
```

### Checked arithmetic at boundaries

Input that crosses the FFI boundary (from C or from a parsed file) can be
malicious or malformed. Use `checked_mul`, `checked_add`, `saturating_*`,
or `usize::try_from` before any buffer-size arithmetic — **never** trust
that multiplication stays in range.

```rust
// Good:
let n_elem = shape.iter().try_fold(1usize, |acc, &d| {
    acc.checked_mul(d as usize).ok_or(EngineError::ShapeOverflow)
})?;
let bytes = n_elem.checked_mul(size_of::<f32>()).ok_or(EngineError::ShapeOverflow)?;

// Bad (can wrap silently and under-allocate):
let n_elem: usize = shape.iter().map(|&d| d as usize).product();
let bytes = n_elem * size_of::<f32>();
```

### C-string bounds-before-deref

When converting a `*const c_char` to a slice, bound-check the index
**before** the dereference. A pointer that is not NUL-terminated must not
be read past `max_len`.

```rust
// Good:
while len < MAX_NAME_LEN {
    let c = unsafe { *p };  // SAFETY: len < MAX_NAME_LEN
    if c == 0 { break; }
    len += 1;
    p = unsafe { p.add(1) };
}

// Bad (reads *p before bound check):
while unsafe { *p } != 0 {
    len += 1;
    p = unsafe { p.add(1) };
    if len >= MAX_NAME_LEN { break; }
}
```

`msg_router::str_copy` takes an explicit `max_src_len` for the same
reason.

---

## Locking

### SpinGuard RAII pattern

Spinlocks wrapping static mutable state (pool allocators, registries,
the message router) use an RAII guard so early returns via `?` cannot
leak the lock:

```rust
static LOCK: AtomicBool = AtomicBool::new(false);

struct SpinGuard;
impl SpinGuard {
    fn new() -> Self {
        while LOCK.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
            core::hint::spin_loop();
        }
        SpinGuard
    }
}
impl Drop for SpinGuard {
    fn drop(&mut self) { LOCK.store(false, Ordering::Release); }
}

// Usage:
pub fn some_op() -> Result<T, E> {
    let _g = SpinGuard::new();
    // SAFETY: _g held — exclusive access to the protected statics.
    unsafe { /* ... */ }
    // _g dropped here, lock released on every exit path
}
```

Panics compile to `abort` (`Cargo.toml [profile.*] panic = "abort"`), so
the RAII guard does not protect against unwinding — but `?` early
returns and intentional control flow still benefit.

Every place that touches `static mut` must take the corresponding lock.
Mutable statics without a held lock are UB under the Rust memory model;
the compiler warns `static_mut_refs` on any unguarded reference.

### Publish/ack deadlock avoidance

`msg_router::publish_internal` holds `MSG_ROUTER_LOCK` only while it
scans the topic/wildcard arrays and gathers target mailbox pointers.
The lock is released *before* the ack-wait loop — otherwise a subscriber
calling `msg_router_ack()` (which also acquires the lock) would
deadlock. Mailbox `ready`/`ack` atomics handle cross-CPU sync on the
individual slot; the lock is only needed for array-level consistency.

---

## SIMD

### `#[target_feature]` on unsafe NEON helpers

`unsafe` functions that use NEON intrinsics are annotated with:

```rust
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn foo_simd(ptr: *mut f32, n: usize) {
    #[cfg(target_arch = "aarch64")]
    { use core::arch::aarch64::*; /* ... */ }
    #[cfg(not(target_arch = "aarch64"))]
    { /* scalar fallback */ }
}
```

`cfg_attr` applies `target_feature` only on aarch64, leaving the
function callable (via the scalar branch) on x86_64 test builds. NEON
is mandated by ARMv8-A, so the attribute mostly documents intent and
matches the explicit `+neon` in `.cargo/config.toml`.

---

## Memory Management

### Allocator Initialization

The Rust heap is initialized by C during boot:

```c
// In kernel/src/main.c
void *rust_heap = pmm_alloc_pages(256);  // 1MB
rust_heap_init(rust_heap, 256 * 4096);
```

After this, Rust can use `Box`, `Vec`, etc. (with `alloc` crate if added).

### Ownership Rules

- Memory allocated by C → freed by C (`slm_free_pages`)
- Memory allocated by Rust → freed by Rust (drop)
- Never mix allocators!

---

## Panic Handling

Rust panics are routed to C:

```rust
#[panic_handler]
fn rust_panic(_info: &PanicInfo) -> ! {
    static MSG: &[u8] = b"Rust panic!\0";
    unsafe { kernel_ffi::panic(MSG.as_ptr()); }
}
```

To test: `rust_test_panic()` (does not return)

---

## Building

The runtime is built automatically as part of `make kernel`:

```bash
make kernel          # Builds runtime then kernel
make runtime         # Builds runtime only
make runtime-clean   # Cleans runtime build
```

Debug vs Release:
```bash
make BUILD_TYPE=Debug kernel    # Default
make BUILD_TYPE=Release kernel  # Optimized, LTO enabled
```

---

## Adding New FFI Functions

1. **Add C declaration** in `kernel/include/slm_ffi.h`
2. **Add C implementation** in `kernel/src/slm_ffi.c`
3. **Add Rust extern** in `runtime/src/kernel_ffi.rs`
4. **Add safe wrapper** in `runtime/src/kernel_ffi.rs`
5. **Update docs** in `docs/ffi.md`

---

*Last updated: 12 April 2026*
