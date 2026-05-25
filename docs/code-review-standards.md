# REVIEW.md — SLM-OS Code Review Standards

This file guides automated code review for SLM-OS. Claude reads this
alongside CLAUDE.md when reviewing pull requests.

## Project Context

SLM-OS is a bare-metal operating system. There is no libc, no Linux kernel,
no POSIX layer. All code runs in kernel space (EL1/EL2 on ARM64, Ring 0 on
x86-64). Mistakes that would be bugs in userspace applications are crashes
or security holes here.

### Languages & Platforms
- **C** (kernel core, drivers, shell, tests)
- **ARM64 assembly** (boot, context switch, exception vectors)
- **x86-64 assembly** (boot, context switch, IDT)
- **Rust** (runtime: model loader, inference scheduler, component system)

### Target hardware
- QEMU virt (primary development/CI)
- Raspberry Pi 5 (Cortex-A76, GIC-400, BCM2712)
- NVIDIA Jetson Orin Nano (Cortex-A78AE, GIC-600, T234)
- x86-64 with LAPIC/IOAPIC (QEMU and real hardware with RTX 3050)

## Critical Review Checks (always flag these)

### Memory Safety
- Stack buffer overflows (kernel stacks are 16KB, fixed)
- Missing bounds checks on array/buffer access
- Use-after-free in task cleanup or IPC teardown paths
- Integer overflow in size calculations (especially `size_t` arithmetic)
- Heap allocator edge cases (split underflow, coalesce errors)

### Concurrency
- Shared data accessed without locks or atomics
- Missing `dmb sy` / `dsb sy` / `isb` barriers on ARM64 after:
  - Page table writes (need `dsb ishst` + `isb`)
  - MMIO device configuration
  - Cross-CPU signaling via shared memory
- Missing `mfence` / `sfence` / `lfence` on x86 equivalents
- Spinlock hold time (flag if holding across allocation or I/O)
- Lock ordering: always acquire in consistent order to prevent deadlock
- Interrupt-disabled sections: keep short, never call `yield()` or `sleep()`

### Interrupt Context
- Code called from IRQ handlers must NOT:
  - Call `kmalloc()` or any allocator
  - Acquire sleeping locks (mutexes)
  - Call `yield()` or `schedule()`
  - Perform unbounded loops
- Timer ISR and IPC signal handlers are common violation sites

### Hardware Register Access
- All MMIO must use volatile (`readl`/`writel` or equivalent)
- Register width must match hardware spec (8/16/32/64-bit)
- Read-modify-write on shared registers needs synchronization
- Device-specific initialization order matters — flag reordering

## Architecture Checks (flag violations)

### Platform Abstraction
- New hardware access should go through `platform.h` or HAL layer
- `#ifdef PLATFORM_*` guards for platform-specific code paths
- Hardcoded base addresses should use defines from platform headers

### Modularity
- Components should compile independently
- No circular dependencies between kernel subsystems
- Rust FFI boundaries: `extern "C"` functions must be `unsafe`
  with documented safety invariants

### Cross-Platform Build
- Changes must not break any of: QEMU ARM64, Pi 5, Jetson, x86-64
- Assembly files need platform-appropriate guards or separate implementations
- `Makefile` / CMake changes should be validated against all targets

## Testing Standards

- New public APIs require at least one test in `kernel/tests/`
- Bug fixes should include a regression test
- Tests should verify behavior, not just "doesn't crash"
  (e.g., check actual values after page table manipulation)
- Hardware-dependent tests should be gated with `#ifdef` and
  documented as requiring specific platform

### Subsystem-specific checklists

Some subsystems have an additional checklist that reviewers walk
through for every relevant PR. Pattern-match the diff against these
when applicable:

- **NIC drivers** — any new file under `kernel/drivers/*net*.c`, any
  change to `kernel/net/lwip_slm.c`, or a new `struct net_driver`
  registration in `kernel/src/main.c`: use
  [`docs/net-driver-checklist.md`](net-driver-checklist.md). Covers
  driver ops, build wiring, the 8 mandatory live integration tests,
  platform-specific tests, docs, and the manual smoke test.

- **Crypto state on the stack** — any new code that holds a key,
  password, scrypt working buffer, or DER-encoded private key in a
  stack-local: wipe via `secure_zero(buf, sizeof(buf))` from
  [`kernel/include/string.h`](../kernel/include/string.h), NOT a
  hand-rolled `for ... = 0u` loop. The optimiser dead-store-
  eliminates the hand loop because the storage goes out of scope
  unread; `secure_zero` writes through a `volatile` pointer that
  defeats the elision. See [`docs/security.md`](security.md) and the
  precedent set in PRs #902 / #992 / #915 / #916 / #918.

## Style (suggestions, not blockers)

- Functions > 80 lines: suggest splitting
- Magic numbers: suggest named constants
- Missing `static` on file-scoped functions
- Rust: prefer `Result<T, E>` over panicking
- Comments explaining "why" are more valuable than "what"

## Do NOT Flag

- Use of `unsafe` in Rust FFI — it's inherent to bare-metal OS work
- `goto` for error cleanup in C — this is idiomatic kernel style
- Global mutable state — unavoidable in a kernel; check synchronization instead
- Assembly style preferences — focus on correctness, not formatting
