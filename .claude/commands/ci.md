Run the SLM-OS CI pipeline locally: build all targets, run the QEMU test suites (ARM64 and x86-64) and runtime host tests, check C braces on changed lines, check the configure order under `make -j`, and check the Jetson boot layout.

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

### 4b. Run test suite (x86-64 QEMU)
```
make test PLATFORM=X86_64
```
Same pass line as step 4. Runs on its own because the x86-64 kernel takes the
scalar paths where ARM64 takes NEON, and the SLM golden suite below checks both.

### 5. Run test suite with the AI scheduler (ARM64 QEMU)
```
make test AI_SCHED=ON
```
`kernel/sched/ai/` and its tests build only with this flag, and it's off by default on
QEMU, so step 4 doesn't compile them (#1113). Treat the same "PASSED" line as success.

### 6. Runtime host unit tests
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

**SLM goldens (DoD item 7).** Steps 4, 4b and 6 include the suites that compare
the SLM stack with upstream llama.cpp (`runtime/src/slm/golden.rs`,
`kernel/tests/test_slm_golden.c`, fixtures and reference commit in
`scripts/fixtures/slm/`): tokenizer against the real Qwen vocabulary (host),
single ops against ggml (host and both QEMU kernels), the Q/K/V projections with
their bias adds against ggml (`golden_qkv_bias.rec`, host `golden_qkv_bias` and
the kernels' `test_golden_qkv_bias`), and greedy tokens plus
logits on a small Qwen2 GGUF through the production registry, forward path and
FFI (host and both QEMU kernels). A mismatch means the model computes something
other than llama.cpp does: report it, and never widen a tolerance or regenerate
a golden to make it pass.

The real 1.1 GB model's golden is not a `/ci` step: it is a hardware target on
jetson-nano-2 (`make jetson-real-golden`, then `slm golden` after a UEFI-direct
boot, see "Real-model golden" in `docs/deploy/jetson-uefi-direct.md`). Its harness
runs here on the tiny model (`golden_real_harness_on_tiny_passes_and_catches_dropped_bias`,
step 6), and `make slm-real-golden-host` runs it on the real model on the host
when the GGUF is there.

### 7. Brace check
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

### 8. Build order check
```
make test-build-order
```
Checks that each kernel configures after the Rust build under `make -j`, again after
a flag change wipes its build directory, and that the SD test image and GRUB ISO wait
for the kernel (#1144). The steps above build serially,
so they don't cover this. It compiles no kernel code. It deletes each platform's
`libslm_runtime.a`, and the next build recreates it, so don't run it beside another
build in the same checkout.
Treat "All build-order tests passed" as success.

### 9. Jetson boot layout check
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

### 10. Diff check
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
| x86-64 test suite | ✅/❌ | N/M passed |
| AI scheduler test suite | ✅/❌ | N/M passed |
| Runtime host tests | ✅/❌ | N passed per feature set |
| Brace check | ✅/❌ | violations if any |
| Build order check | ✅/❌ | failed checks if any |
| Jetson layout check | ✅/❌ | failed or skipped checks if any |
| Artifact check | ✅/❌ | stray files if any |

If everything passes, say "Ready for PR."
If anything fails, list what needs fixing before a PR can be opened.
