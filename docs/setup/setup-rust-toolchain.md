# Installing Rust with aarch64-unknown-none Target

This guide covers installing the Rust toolchain for bare-metal ARM64 development on SLM-OS.

---

## Prerequisites

- Windows 10/11 (or Linux/macOS)
- Internet connection
- Administrator access (for initial install)
- `aarch64-none-elf-gcc` cross-compiler (see [ARM64 Toolchain Setup](setup-clion-arm64-toolchain.md))

---

## Step 1: Install Rust

### Option A: Download Installer

1. Go to https://rustup.rs
2. Download and run `rustup-init.exe`
3. Follow the prompts (default options are fine)

### Option B: Using winget (Windows)

```powershell
winget install Rustlang.Rustup
```

### Option C: Using curl (Linux/macOS)

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

---

## Step 2: Configure PATH

### Windows (PowerShell/CMD)

Close and reopen your terminal (or CLion) to pick up the new PATH entries automatically.

### Cygwin / Git Bash

Rust installs to Windows paths that Cygwin doesn't see by default. Add to `~/.bash_profile`:

```bash
export PATH="/c/Users/<username>/.cargo/bin:$PATH"
```

Then reload:

```bash
source ~/.bash_profile
```

**Note:** Replace `<username>` with the Windows username (e.g., `/c/Users/john/.cargo/bin`).

### Verify Installation

```bash
rustc --version
cargo --version
rustup --version
```

---

## Step 3: Add the Bare-Metal ARM64 Target

```bash
rustup target add aarch64-unknown-none
```

### What this target means

| Component | Meaning |
|-----------|---------|
| `aarch64` | 64-bit ARM architecture (ARMv8-A) |
| `unknown` | No specific vendor |
| `none` | No operating system (bare-metal) |

This target produces code that runs without any OS — exactly what SLM-OS needs.

---

## Step 4: Install rust-src Component

Required for building `no_std` crates:

```bash
rustup component add rust-src
```

---

## Step 5: Verify Installation

Check that the target is installed:

```bash
rustup target list --installed
```

Expected output should include:

```
aarch64-unknown-none
```

Check all components:

```bash
rustup show
```

---

## SLM-OS Runtime Configuration

The `runtime/` directory contains the Rust runtime crate. Here is the configuration used:

### runtime/Cargo.toml

```toml
[package]
name = "slm-runtime"
version = "0.1.0"
edition = "2021"
authors = ["SLM-OS Team"]
description = "Rust runtime for SLM-OS kernel"

[lib]
crate-type = ["staticlib"]  # Produces .a file for linking with C

[dependencies]
linked_list_allocator = "0.10"  # Heap allocator for no_std

[profile.dev]
panic = "abort"

[profile.release]
panic = "abort"
lto = true          # Link-time optimization
opt-level = "z"     # Optimize for size
```

### runtime/.cargo/config.toml

```toml
[build]
target = "aarch64-unknown-none"

[target.aarch64-unknown-none]
# Use the ARM cross-linker (same as C kernel)
linker = "aarch64-none-elf-gcc"

# Rust flags for bare-metal
rustflags = [
    "-C", "link-arg=-nostdlib",      # No standard library
    "-C", "link-arg=-static",         # Static linking
]
```

### runtime/src/lib.rs (minimal example)

```rust
#![no_std]
#![no_main]

use core::panic::PanicInfo;
use linked_list_allocator::LockedHeap;

// Global allocator for heap allocation
#[global_allocator]
static ALLOCATOR: LockedHeap = LockedHeap::empty();

// C kernel's panic function
extern "C" {
    fn panic(msg: *const u8) -> !;
}

#[panic_handler]
fn rust_panic(_info: &PanicInfo) -> ! {
    static MSG: &[u8] = b"Rust panic!\0";
    unsafe { panic(MSG.as_ptr()); }
}

/// Initialize heap allocator (called from C)
#[no_mangle]
pub unsafe extern "C" fn rust_heap_init(heap_start: *mut u8, heap_size: usize) {
    ALLOCATOR.lock().init(heap_start, heap_size);
}

/// Entry point for testing (called from C)
#[no_mangle]
pub extern "C" fn rust_init() -> i32 {
    42  // Magic number to verify it works
}
```

---

## FFI (Foreign Function Interface)

### Calling C from Rust

Declare external C functions and call them:

```rust
extern "C" {
    fn uart_puts(s: *const u8);
}

#[no_mangle]
pub extern "C" fn rust_hello() {
    static MSG: &[u8] = b"Hello from Rust!\n\0";
    unsafe { uart_puts(MSG.as_ptr()); }
}
```

### Calling Rust from C

Declare the Rust functions in a C header:

```c
/* slm_ffi.h */
extern void rust_heap_init(void *heap_start, size_t heap_size);
extern int rust_init(void);
extern void rust_hello(void);
```

Then call them from C:

```c
/* main.c */
#include "slm_ffi.h"

void kernel_main(void) {
    // Initialize Rust heap (4MB)
    void *heap = pmm_alloc_pages(1024);  // 1024 * 4KB = 4MB
    rust_heap_init(heap, 1024 * 4096);

    // Test Rust integration
    int magic = rust_init();
    if (magic == 42) {
        rust_hello();  // Prints "Hello from Rust!"
    }
}
```

---

## Building

### From Project Root

```bash
make runtime   # Build Rust runtime only
make kernel    # Build kernel (includes runtime)
make           # Build everything (default)
```

### Output Files

| Build Type | Location |
|------------|----------|
| Debug | `runtime/target/aarch64-unknown-none/debug/libslm_runtime.a` |
| Release | `runtime/target/aarch64-unknown-none/release/libslm_runtime.a` |

The `.a` static library is linked with the C kernel during the final link step.

---

## Troubleshooting

### "can't find crate for std"

Missing `#![no_std]` in source file.

### "error: requires panic = abort"

Add to `Cargo.toml`:

```toml
[profile.dev]
panic = "abort"

[profile.release]
panic = "abort"
```

### "linker not found"

Ensure `aarch64-none-elf-gcc` is installed and in PATH. See [ARM64 Toolchain Setup](setup-clion-arm64-toolchain.md).

### "cargo: command not found" (Cygwin)

Add to `~/.bash_profile`:

```bash
export PATH="/c/Users/<username>/.cargo/bin:$PATH"
```

Then run `source ~/.bash_profile`.

### Target not installed

```bash
rustup target add aarch64-unknown-none
```

### Undefined symbols during link

Ensure the Rust library is built before the kernel. The Makefile handles this automatically (`kernel` depends on `runtime`).

---

## Additional Resources

- [Rust Embedded Book](https://docs.rust-embedded.org/book/)
- [The Embedonomicon](https://docs.rust-embedded.org/embedonomicon/)
- [rustup documentation](https://rust-lang.github.io/rustup/)
- [linked_list_allocator crate](https://docs.rs/linked_list_allocator/)

---

*Last updated: December 2025*
