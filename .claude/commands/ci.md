Run the SLM-OS CI pipeline locally: build all targets, run the QEMU test suite and runtime host tests, check C braces on changed lines, and check the Jetson boot layout.

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

### 3. Build — Jetson with real AI weights
```
make kernel-clean && make kernel PLATFORM=JETSON_ORIN_NANO AI_WEIGHTS=real
make kernel-test-clean && make kernel-test PLATFORM=JETSON_ORIN_NANO AI_WEIGHTS=real
```
Code under `SLMOS_AI_WEIGHTS_REAL`, in both the main and test kernels, isn't compiled
by any other step, and an unused variable there broke this build unnoticed (#1110).
Report: pass/fail, any warnings.

### 4. Run test suite (ARM64 QEMU)
```
make test
```
The Makefile's `test` target runs QEMU under `systemd-run` with semihosting
and reports pass/fail via the QEMU exit code only — per-test `[PASS]`/`[FAIL]`
lines are not printed to stdout. Treat "PASSED - All tests passed (exit code 0)"
as success. On failure, also grep the build/run output for `PAGE FAULT` and
`KERNEL PANIC` to characterize the crash.

### 5. Runtime host unit tests
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

### 6. Brace check
```
make test-check-braces && make check-braces
```
Runs `clang-tidy` on C lines changed since `origin/main`, as the main, test
and check-only wide kernel of all four platforms (`scripts/check-c-braces.py`). Fix any it reports.
A `could not parse` line means a file was not checked, so don't treat that as a
pass. `make` exits 2 for any failure, so read the script's output. Its last line
gives parses per configuration, and all twelve should appear when headers or
shared files change.

A `not checked` line exits 0, because those files are outside the configured
builds (`kernel/net/ssh/`, host tools). Name them in the Notes column so someone
reads them for braces by hand.

### 7. Jetson boot layout check
```
make test-jetson-uefi-layout
```
Does its own clean Jetson build, then checks it for invariants the QEMU test
suite doesn't cover: the Jetson-only early vector table, the UEFI hand-off as
built for Jetson, and the boot-time register setup. `/ci` doesn't cover the
hand-off at runtime, so when changing the EFI entry or stub, also run the
command below. `test-qemu-uefi` boots whatever image is in `build/kernel`, and
this step leaves a Jetson one there, so rebuild first:
```
make kernel-clean && make kernel PLATFORM=QEMU_VIRT && make test-qemu-uefi
```

Treat "All checks passed." as success; failures are the lines marked `✗`. A `!`
line means a check was skipped and the script still says "All checks passed.",
so name it in the Notes column and don't treat that check as a pass.

The build log is at `build/jetson-layout-build.log`. The step leaves a Jetson
build in `build/kernel`.

### 8. Diff check
Run `git diff origin/main...HEAD --stat` and confirm no untracked build artifacts
are being committed (`.o`, `.obj`, `.bin`, `.elf`, `.iso`, `.a` files).

## Output

Summarize results as a table:

| Step | Status | Notes |
|------|--------|-------|
| ARM64 build | ✅/❌ | warnings if any |
| x86-64 build | ✅/❌ | warnings if any |
| Jetson real-weights build | ✅/❌ | warnings if any |
| Test suite | ✅/❌ | N/M passed |
| Runtime host tests | ✅/❌ | N passed per feature set |
| Brace check | ✅/❌ | violations if any |
| Jetson layout check | ✅/❌ | failed or skipped checks if any |
| Artifact check | ✅/❌ | stray files if any |

If everything passes, say "Ready for PR."
If anything fails, list what needs fixing before a PR can be opened.
