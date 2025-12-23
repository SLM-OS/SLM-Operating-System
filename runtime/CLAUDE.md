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

*Last updated: December 2025*
