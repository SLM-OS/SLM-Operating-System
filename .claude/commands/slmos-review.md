Review the current branch's changes against main for SLM-OS code quality.

## Instructions

1. Run `git diff main...HEAD` to get the full diff for this branch.
2. Run `git diff main...HEAD --stat` to get an overview of changed files.
3. If the diff is very large, review file-by-file using `git diff main...HEAD -- <file>`.

## Review Criteria

You are reviewing a bare-metal operating system. There is no libc, no Linux,
no POSIX. All code runs in kernel space (EL1/EL2 on ARM64, Ring 0 on x86-64).

### Critical (must fix before merge)

**Memory Safety:**
- Buffer overflows (kernel stacks are 16KB fixed)
- Use-after-free in task cleanup or IPC teardown
- Integer overflow in size calculations (`size_t` arithmetic)
- Missing bounds checks on array/buffer access

**Concurrency:**
- Shared data without locks or atomics
- Missing memory barriers after page table writes (`dsb ishst` + `isb`), MMIO config, or cross-CPU signaling
- Spinlock held across allocation or I/O
- `yield()` or `sleep()` called with interrupts disabled

**Interrupt Safety:**
- ISR handlers that allocate, acquire sleeping locks, or call `yield()`/`schedule()`

**Hardware Access:**
- MMIO without volatile (`readl`/`writel`)
- Wrong register width for hardware spec
- Missing cache maintenance after DMA or page table changes

### Warnings (should fix)

- Platform-specific code without `#ifdef PLATFORM_*` guard
- New public API without test in `kernel/tests/`
- Cross-platform build breakage risk (check all targets: QEMU ARM64, Pi 5, Jetson, x86-64)
- Rust FFI `extern "C"` without documented safety invariants
- Lock ordering inconsistency
- **Unbraced control flow bodies in C** — flag `if`/`else`/`while`/`for`/`do` without `{ ... }` UNLESS the construct is a stand-alone single-line guard.
  - Exempt: stand-alone `if (cond) stmt;` on a single source line — e.g. `if (err) return rc;`. Only the bare `if` form is exempt; the `if` must not have an `else`/`else if` arm.
  - **NOT exempt:** chained `if/else if/.../else`, even when every branch's body is a single line. The chain must be fully braced. Reason: clang-tidy's `readability-braces-around-statements` flags these (it measures body span on the outer `if`, which always spans multiple lines once an `else` is attached), and that's the convention the kernel C tree was normalised to in PR #627.
  - Canonical check: clang-tidy's `readability-braces-around-statements` with `ShortStatementLines: 1`.
  - Exempt files: anything under `kernel/lib/` (vendored: lwip, lua, littlefs, fatfs, fdt, nanopb, plus `md5.c` and `sha256.c`) and any `**/*.pb.c` (nanopb-generated outputs, regardless of location).

### Suggestions (nice to have)

- Functions > 80 lines that could be split
- Magic numbers that should be named constants
- Missing `static` on file-scoped functions
- Comments explaining "why" rather than "what"

### Do NOT flag

- `unsafe` in Rust FFI — inherent to bare-metal
- `goto` for error cleanup in C — idiomatic kernel style
- Global mutable state — unavoidable in kernel; check synchronization instead
- Assembly formatting preferences

## Output Format

Organize findings by file, then by severity (Critical → Warning → Suggestion).
For each finding, include:
- The file and line range
- The severity tag
- What the issue is
- A concrete fix or recommendation

End with a summary: total findings by severity, overall assessment of merge readiness.
