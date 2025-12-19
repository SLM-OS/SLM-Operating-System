# Installing Rust with aarch64-unknown-none Target

This guide covers installing the Rust toolchain for bare-metal ARM64 development on SLM-OS.

---

## Prerequisites

- Windows 10/11 (or Linux/macOS)
- Internet connection
- Administrator access (for initial install)

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

## Step 2: Restart Terminal

Close and reopen your terminal (or CLion) to pick up the new PATH entries.

Verify Rust is installed:

```powershell
rustc --version
cargo --version
rustup --version
```

---

## Step 3: Add the Bare-Metal ARM64 Target

```powershell
rustup target add aarch64-unknown-none
```

### What this target means

| Component | Meaning |
|-----------|---------|
| `aarch64` | 64-bit ARM architecture (ARMv8-A) |
| `unknown` | No specific vendor |
| `none` | No operating system (bare-metal) |

This target produces code that runs without any OS — exactly what we need for SLM-OS.

---

## Step 4: Install rust-src Component

Required for building `no_std` crates:

```powershell
rustup component add rust-src
```

---

## Step 5: Verify Installation

Check that the target is installed:

```powershell
rustup target list --installed
```

Expected output should include:

```
aarch64-unknown-none
```

Check all components:

```powershell
rustup show
```

---

## Project Configuration

When creating Rust code for SLM-OS, use these configurations:

### Cargo.toml

```toml
[package]
name = "slm-runtime"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib"]

[profile.dev]
panic = "abort"

[profile.release]
panic = "abort"
lto = true
opt-level = "z"  # Optimize for size
```

### .cargo/config.toml

Create this file in your Rust project to set defaults:

```toml
[build]
target = "aarch64-unknown-none"

[target.aarch64-unknown-none]
rustflags = ["-C", "link-arg=-nostartfiles"]
```

### Source Files

All Rust source files for bare-metal must include:

```rust
#![no_std]
#![no_main]

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
```

- `#![no_std]` — Disables the standard library (requires OS)
- `#![no_main]` — Disables the default entry point
- `#[panic_handler]` — Required custom panic handler since there's no OS to handle panics

---

## Building

To build for the target:

```powershell
cargo build --target aarch64-unknown-none
```

Or if you configured `.cargo/config.toml`:

```powershell
cargo build
```

Output will be in `target/aarch64-unknown-none/debug/` or `release/`.

---

## Troubleshooting

### "can't find crate for std"

You forgot `#![no_std]` in your source file.

### "error: requires panic = abort"

Add to your `Cargo.toml`:

```toml
[profile.dev]
panic = "abort"

[profile.release]
panic = "abort"
```

### "linker not found"

Ensure `aarch64-none-elf-gcc` is installed and in PATH. Rust uses the system linker for bare-metal targets.

### Target not installed

```powershell
rustup target add aarch64-unknown-none
```

---

## Additional Resources

- [Rust Embedded Book](https://docs.rust-embedded.org/book/)
- [The Embedonomicon](https://docs.rust-embedded.org/embedonomicon/)
- [rustup documentation](https://rust-lang.github.io/rustup/)

---

*Last updated: December 2025*
