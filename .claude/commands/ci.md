Run the SLM-OS CI pipeline locally: build all targets and run the QEMU test suite.

## Instructions

Run the following steps in order. Stop and report on first failure.

### 1. Build — ARM64 QEMU target
```
make kernel-clean && make kernel PLATFORM=QEMU_VIRT
```
Report: pass/fail, any warnings.

### 2. Build — x86-64 QEMU target
```
make kernel-clean && make kernel PLATFORM=X86_64
```
Report: pass/fail, any warnings.

### 3. Run test suite (ARM64 QEMU)
```
make test
```
The Makefile's `test` target runs QEMU under `systemd-run` with semihosting
and reports pass/fail via the QEMU exit code only — per-test `[PASS]`/`[FAIL]`
lines are not printed to stdout. Treat "PASSED - All tests passed (exit code 0)"
as success. On failure, also grep the build/run output for `PAGE FAULT` and
`KERNEL PANIC` to characterize the crash.

### 4. Runtime host unit tests
```
make runtime-test
```
Runs the Rust runtime's `#[cfg(test)]` modules on the host via
`cargo test`, once with the default features and once with
`slm,ai_eviction_models`. Treat two `test result: ok.` lines as success.
This step catches compile or link rot in test-only code, which
`make test` never builds: it went unnoticed for months before #1034.
A link error naming a kernel symbol means `test_ffi_stubs` in
`runtime/src/lib.rs` needs a stub for it.

### 5. Diff check
Run `git diff main...HEAD --stat` and confirm no untracked build artifacts
are being committed (`.o`, `.obj`, `.bin`, `.elf`, `.iso`, `.a` files).

## Output

Summarize results as a table:

| Step | Status | Notes |
|------|--------|-------|
| ARM64 build | ✅/❌ | warnings if any |
| x86-64 build | ✅/❌ | warnings if any |
| Test suite | ✅/❌ | N/M passed |
| Runtime host tests | ✅/❌ | N passed per feature set |
| Artifact check | ✅/❌ | stray files if any |

If everything passes, say "Ready for PR."
If anything fails, list what needs fixing before a PR can be opened.
