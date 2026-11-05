---
name: slmos-review
description: Review SLM-OS code changes against the project's review criteria (C, assembly, Rust; memory safety, concurrency, IRQ safety, hardware access, boot paths, test evidence). Reviews the current branch against origin/main by default, or a PR number or paths. Use before opening or merging a PR, after review fixes, or when asked to review a branch or PR.
argument-hint: "[PR-number | path ...] [--since <sha>]"
context: fork
agent: general-purpose
background: false
effort: high
allowed-tools: Bash(git fetch *) Bash(git branch --show-current) Bash(git log *) Bash(git status *) Bash(git diff *) Bash(git show *) Bash(echo *) Bash(gh pr diff *) Bash(gh pr view *) Bash(gh issue list *) Bash(make check-braces*)
---

# SLM-OS review

Arguments: $ARGUMENTS

Snapshot taken when this review started:

- Fetch: !`git fetch origin main --quiet 2>&1 && echo ok || echo "FAILED: origin/main may be stale"`
- Branch: !`git branch --show-current || true`
- Commits on the branch: !`git log --oneline origin/main..HEAD || true`
- Uncommitted: !`git status --short || true`
- Size: !`git diff origin/main...HEAD --shortstat || true`

Review only. Don't edit files, commit, build, run tests or touch hardware; `/ci` and `/deploy-and-test` do that. The one exception is `make check-braces`, which configures but doesn't compile.

## 1. Set the scope

Always diff against `origin/main`, never local `main`, which is stale in most worktrees and would pull other people's merged work into the review.

| Arguments | Review |
|---|---|
| none | `git diff origin/main...HEAD`, plus uncommitted work (`git diff HEAD`, untracked files) |
| PR number | `gh pr diff N` and `gh pr view N --json title,body,closingIssuesReferences`; read code from `git fetch origin pull/N/head` with `git show FETCH_HEAD:<path>`, since this checkout may be on another branch |
| paths | the same as none, limited to those paths |
| `--since <sha>` | a re-review, see step 5: `git diff <sha>` plus untracked files, or `git diff <sha> FETCH_HEAD` with a PR number. If `git log --oneline HEAD..<sha>` (or `FETCH_HEAD..<sha>`) prints anything, a rebase has moved the branch, so say so and review the full diff |

Run git from the working directory, without `-C`, so the pre-approved commands match. If the diff is empty, say so and stop. For a large diff, go file by file, riskiest first (assembly, locking, DMA, parsers, then the rest), and list anything left unread in the report.

## 2. Read for context

- Read each changed function in full and its callers, not just the hunks. Most real findings here depend on who calls what with which locks and IRQ state.
- Read the commit messages and PR body for intent. Treat their claims as things to check, including "tested on hardware".
- Find the sections for the touched subsystems with `grep -n '^#' kernel/CLAUDE.md runtime/CLAUDE.md`, and read those. Don't read both files whole. Root `CLAUDE.md` has the Jetson boot-path rules. Those notes record rules learned from real failures, so a violation is a finding.
- Run `gh issue list --search "<keywords>"` before reporting something that looks long-standing. If it's already tracked, cite the issue instead of raising it as new.
- Ignore `docs/archive/`, which includes old review snapshots.

## 3. Look for these

SLM-OS is a bare-metal OS with no libc, Linux or POSIX. The kernel runs at EL1/EL2 on ARM64 and Ring 0 on x86-64; user tasks run at EL0 over `kernel/src/syscall.c`. A mistake usually means a crash, a silent board or corruption. Targets are QEMU ARM64, Pi 5, Jetson Orin Nano and x86-64.

**Memory safety**
- Overflows and missing bounds checks. Stack size comes from `STACK_SIZE` in `kernel/include/config.h`; check it there, not from memory.
- Size arithmetic that can wrap (`size_t`, `base + len`). Use the subtraction form of the check.
- Use-after-free in task cleanup, IPC teardown and heap edge cases (split underflow, coalescing).
- **Untrusted input:** model files (ONNX, GGUF, HEF), ELF, filesystems, network and SSH data, syscall arguments, and sizes reported by firmware or devices. Every length and index must be validated before use.
- Secrets on the stack (keys, plaintext passwords, KDF working buffers, DER private keys, DRBG output) are wiped with `secure_zero` (`kernel/include/string.h`), not `memset` or a hand-written loop, which the optimizer can remove.

**Concurrency**
- Shared data without locks or atomics, inconsistent lock ordering.
- **Preemption is real on Pi 5 and Jetson:** a task can be preempted holding a plain lock. If code with IRQs masked on that CPU then spins on the same lock, the CPU hangs, and on CPU 0 the network and serial shell die with it.
- **Shell commands race the system:** a shell command can re-run a bring-up or install step while another CPU is dispatching. Shared state must be replaced under the lock its readers use.
- **Publish order:** write the fields, do any invalidate or barrier, then set the flag or pointer last with release semantics. Readers test the flag first. Check this for handoffs, TLB invalidates and pool swaps.
- ARM64 barriers: `dsb ishst` + `isb` after page table writes; barriers around MMIO configuration and cross-CPU signaling. x86-64: `mfence`/`sfence`/`lfence` equivalents.
- Cross-CPU data on Pi 5 and Jetson: see "Cache Maintenance" and "Non-Cacheable Shared Memory" in `kernel/CLAUDE.md`. QEMU won't show these bugs.

**IRQ safety**
- ISRs and IRQs-off paths (`spin_lock_irqsave` sections, the GPU dispatch lock, the lwIP core lock) must not allocate, sleep, `yield()`, `schedule()` or loop without a bound.
- Flag a spinlock newly held across allocation or slow device I/O, unless that is its documented design (the UART lock, `g_gpu_dispatch_lock`, the runtime's long-work locks).
- **No unconditional UART output on these paths.** UART writes are slow and IRQs stay masked throughout, so per-dispatch prints cost milliseconds. Log before taking the lock, print once, or put it behind a debug switch.
- **Rust reached from IRQs-off paths:** check every lock taken below the call. Short sections use IRQ-saving guards; long-held locks use a one-shot try-lock and skip on busy; nothing allocates. Also check nothing allocates or takes a plain lock under an IRQ-saving guard. See "Locks reached from IRQs-off context" in `runtime/CLAUDE.md`.
- Secondary CPUs don't print after boot on platforms with `PLATFORM_HAS_NC_MEMORY`: the UART lock there has no cross-CPU exclusion. The Jetson `thermal log` and fan tasks are the known exceptions.

**Hardware access**
- MMIO goes through `volatile` access at the width the spec gives. Read-modify-write on a shared register is synchronized and leaves write-1-to-clear or write-1-to-disable bits alone.
- Device init order matches the reference driver in `~/slmos-ref/` when it's available; flag reordering.
- Cache maintenance after DMA and page table changes.
- **Boot path:** kexec inherits Linux-initialized hardware and UEFI-direct boot doesn't. Code touching such a block on Jetson must check `boot_source_is_uefi()`, because an unpowered block stops the board in EL3. Any new MMIO address must be mapped in every boot path that reaches it.
- Assembly and C agree: `struct task` context offset, `CTX_*` defines, and anything else hardcoded in `.S` files.

**Failure and retry paths**
- Trace each error return. Undo what was done: clocks, locks, half-installed state.
- Don't free memory that a GPU, firmware or device may still reference; invalidating and leaking is correct there.
- A retry must work after a partial failure, not return early as done or fail until reboot.

**ML and inference correctness**
- Model attributes are honored or rejected, not silently ignored. Shape, broadcast and index logic is checked against the operator's definition.
- GPU pipelines give the same result as the CPU reference path.

**Rust**
- `runtime/CLAUDE.md` rules: `SAFETY` comments on `unsafe` blocks, checked arithmetic at FFI boundaries, C-string bounds checked before deref, no `static mut` access without its lock, `Result` rather than panics.
- `extern "C"` functions document their safety invariants. A new kernel symbol used by Rust needs a stub in `test_ffi_stubs` (`runtime/src/lib.rs`), or `make runtime-test` stops linking.

**Builds and configuration**
- Platform code is guarded by `#ifdef PLATFORM_*`, and values live in `kernel/include/platform.h` instead of hardcoded addresses.
- Check optional features too, not just the four platforms: `AI_SCHED`, `WORK_STEALING`, `EVICTION_MODELS`, `JETSON_HW_TICK`, `SECONDARY_PREEMPT`, `NET_SSHD`. Look for code nested inside an unrelated `#if`, and for code that breaks when a feature is off.
- A new build option joins the shared per-variant CMake argument lists in the `Makefile`, so toggling it forces a reconfigure (`make check-build-signatures`). A new default-off option that gates C code also joins the wide lists for `make check-braces`, unless it would hide a branch no other configuration parses.
- Kernel C follows the C23 rules in `kernel/CLAUDE.md`. No circular dependencies between subsystems.
- NIC driver changes (`kernel/drivers/*net*.c`, `kernel/net/lwip_slm.c`, a new `struct net_driver`) follow `docs/net-driver-checklist.md`.

**Tests and evidence**
- New public APIs and bug fixes have a test that would fail without the change and checks values rather than "doesn't crash".
- The test is actually built: registered in the suite and not hidden behind a flag the default run leaves off. Tests include the real header instead of re-declaring prototypes. Host-only logic may belong in a `make test-*` target or `make runtime-test`.
- QEMU TCG hides cache incoherence, PMU and timer behavior. A change in those areas needs hardware evidence in the PR, and "hardware-verified" on shared infrastructure means the branch was rebased first. Ticking counters don't prove preemption.

**Comments, docs and PR text**
- Comments and docs that the change made false. These are common here, so grep for the old names and behavior.
- Docs follow `CLAUDE.md`: no second person, TODO status markers, no third-party sources added to the repo.
- A closing keyword ("fix", "close", "resolve" + `#N`) in a commit message or PR body closes that issue on merge, even in a negated sentence. Flag any that shouldn't, and check `closingIssuesReferences` on a PR.

**C style: braces**
- When reviewing the checked-out branch, run `make check-braces` and report each violation as a Warning. Don't look for these by hand. It checks changed lines as the main, test and check-only wide kernel of all four platforms, and its result is authoritative for code those configurations compile.
- If it can't run (no `clang-tidy` or cross compiler on PATH), or for a PR on another branch, say so in the report and read changed lines for the rule: every `if`/`else`/`while`/`for`/`do` body is braced, except a stand-alone single-line statement like `if (err) return rc;`. An `if`/`else` across several lines is fully braced even with one-line bodies.
- Also read by hand any file it lists as not checked, macro bodies (the rule skips macro expansions), changes inside `#if` blocks that are off even in the wide configuration (`NET_SSHD_DEMO_ALLOW_ALL`, `PI5_FIQ_TIMER`, `SLM_HOST_HARNESS` or the `ENABLE_*_EMBED` blocks, for example), and code that needs both a wide-only option and `ENABLE_BOOT_TESTS`.

**Suggestions only**
- Functions over 80 lines worth splitting, magic numbers, missing `static`, comments that say what instead of why.

**Do not flag**
- `unsafe` in Rust FFI, `goto` for C error cleanup, global mutable state as such (check its synchronization instead), assembly formatting.
- Vendored or generated code unless the change itself is wrong: `kernel/lib/`, `**/*.pb.c`, `runtime/src/mm/eviction/generated/`.
- A scheduler or eviction policy for not beating the default. Pluggable policies are options, and being weak on some workloads is a result, not a defect.

## 4. Verify, then sweep

Do both before writing the report.

**Verify each candidate.** Trace the path that triggers it: callers, locks held, IRQ state, platform and flags. Look inside callees for the barrier or check that seems missing. Read nearby comments; if one documents the decision, report only if the comment is wrong. Check constants in the source. Drop any Critical or Warning that doesn't survive with a concrete failure scenario.

**Sweep the class.** For each confirmed finding, search the rest of the diff and the touched subsystem for the same mistake, such as other writers of that state or other prints under that lock. Report every instance in one finding. Don't leave siblings for a later round.

## 5. Re-reviews

With `--since <sha>`, or when the branch log shows earlier review-fix commits:

- Check that each earlier fix is correct and complete and didn't introduce a new problem.
- Concentrate on what changed since then, using the full diff as context. Report older code only for a Critical or Warning that was missed.
- Don't repeat findings the commits or PR already considered and rejected, unless the reasoning is wrong.

## 6. Report

The report must stand alone, since it's handed back to the session that asked for it. Order findings by severity, then file.

- **Critical:** must fix. A reachable crash, hang, corruption, security hole or silently wrong result.
- **Warning:** should fix. A real defect with limited impact, or a missing test or stale doc.
- **Suggestion:** optional.

For each Critical and Warning give:

- `path:line`, plus any other locations from the sweep
- the defect in one or two sentences
- **Failure scenario:** the concrete state or input and what goes wrong
- the fix

Suggestions need one line each. End with:

- counts by severity
- what wasn't reviewed or couldn't be verified statically, such as behavior needing hardware
- the verdict: **Ready** when there are no Critical or Warning findings, otherwise **Not ready**. Suggestions alone never call for another review round.
