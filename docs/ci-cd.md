# CI/CD Pipeline

This document describes the Continuous Integration/Continuous Deployment pipeline for SLM-OS.

---

## Overview

SLM-OS uses GitHub Actions for automated building and testing. The pipeline runs on every push and pull request, executing the full test suite in QEMU to catch regressions early.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         GitHub Actions Workflow                         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│   Push/PR ──> Install Toolchains ──> Build Runtime ──> Build Kernel    │
│                                                                         │
│                    │                                                    │
│                    ▼                                                    │
│            Run Tests in QEMU ──> Parse Results ──> Upload Artifacts     │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Workflow Configuration

The workflow is defined in `.github/workflows/ci.yml`.

### Triggers

| Trigger | Branches | Description |
|---------|----------|-------------|
| `push` | main, develop | Every push to main or develop |
| `pull_request` | main | All PRs targeting main |
| `workflow_dispatch` | any | Manual trigger from GitHub UI |

### Job: build-and-test

Runs on `ubuntu-latest` with the following steps:

#### 1. Checkout Repository
```yaml
- uses: actions/checkout@v4
```

#### 2. Install ARM GCC Toolchain
Downloads and installs the bare-metal ARM toolchain (aarch64-none-elf) from ARM's official releases:
```yaml
- name: Install ARM GCC Toolchain
  run: |
    wget -q https://developer.arm.com/-/media/Files/downloads/gnu/13.2.rel1/binrel/arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz
    sudo tar -xf arm-gnu-toolchain-13.2.rel1-x86_64-aarch64-none-elf.tar.xz -C /opt
    echo "/opt/arm-gnu-toolchain-13.2.Rel1-x86_64-aarch64-none-elf/bin" >> $GITHUB_PATH
```

#### 3. Install QEMU
```yaml
- name: Install QEMU
  run: sudo apt-get install -y qemu-system-arm
```

#### 4. Install Rust
Uses the dtolnay/rust-action for Rust toolchain setup:
```yaml
- uses: dtolnay/rust-action@stable
  with:
    targets: aarch64-unknown-none
```

#### 5. Verify Toolchain Installation
Prints versions to confirm all tools are available:
```yaml
- run: |
    aarch64-none-elf-gcc --version
    qemu-system-aarch64 --version
    rustc --version
    cargo --version
```

#### 6. Build Runtime (Rust)
```yaml
- working-directory: runtime
  run: cargo build
```

#### 7. Build Kernel (CMake)
```yaml
- run: |
    cmake -B build \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-aarch64-none-elf.cmake \
      -DCMAKE_BUILD_TYPE=Debug \
      -DPLATFORM=QEMU_VIRT
    cmake --build build
```

#### 8. Run Tests in QEMU
```yaml
- run: |
    qemu-system-aarch64 \
      -M virt \
      -cpu max \
      -smp 4 \
      -m 1G \
      -nographic \
      -semihosting \
      -kernel build/slmos.elf \
      2>&1 | tee test_output.txt
```

#### 9. Upload Artifacts
Always uploads build artifacts, even on failure:
```yaml
- uses: actions/upload-artifact@v4
  if: always()
  with:
    name: kernel-build
    path: |
      build/slmos.elf
      build/slmos.bin
      test_output.txt
    retention-days: 7
```

---

## Semihosting

The pipeline uses ARM semihosting for clean test exit. This eliminates the need for timeout-based test detection.

### How It Works

1. **QEMU Flag**: The `-semihosting` flag enables semihosting support
2. **Kernel Exit**: After tests complete, the kernel calls `semihosting_exit()`
3. **HLT Instruction**: ARM64 semihosting uses `HLT #0xF000`
4. **QEMU Intercepts**: QEMU intercepts the HLT and exits with the specified code

### Implementation

```c
// kernel/src/semihosting.c
void semihosting_exit(int exit_code)
{
    struct exit_block block = {
        .reason = (exit_code == 0) ?
            ADP_Stopped_ApplicationExit :
            ADP_Stopped_RunTimeErrorUnknown,
        .subcode = (uint64_t)exit_code
    };

    // ARM64 semihosting call
    register uint64_t op __asm__("x0") = SYS_EXIT_EXTENDED;
    register uint64_t param __asm__("x1") = (uint64_t)&block;
    __asm__ volatile("hlt #0xF000" : "+r"(op) : "r"(param) : "memory");
}
```

### Test Harness Integration

```c
// kernel/tests/test_harness.c
int test_harness_run_all(void)
{
    int total_failures = 0;

    // Run all test suites...
    total_failures += test_suite_ipc();
    total_failures += test_suite_vmm();
    // ... etc

    // Exit via semihosting when available
    if (semihosting_available()) {
        uart_puts("[INFO] Exiting via semihosting...\n");
        semihosting_exit(total_failures == 0 ? 0 : 1);
    }

    return total_failures;
}
```

### Conditional Compilation

Semihosting is only enabled for QEMU builds:
```cmake
# CMakeLists.txt
if(PLATFORM STREQUAL "QEMU_VIRT")
    add_compile_definitions(ENABLE_SEMIHOSTING=1)
endif()
```

On real hardware (Jetson), the HLT instruction would cause a fault, so semihosting is disabled at compile time.

---

## Test Result Parsing

The workflow parses QEMU output to determine test status:

```bash
if grep -q "\[PASS\] All test suites passed" test_output.txt; then
    echo "All tests passed!"
    exit 0
elif grep -q "PAGE FAULT" test_output.txt; then
    echo "ERROR: Kernel crashed with page fault"
    exit 1
elif grep -q "\[FAIL\]" test_output.txt; then
    echo "ERROR: Some tests failed"
    exit 1
else
    echo "WARNING: Could not determine test status"
    exit 1
fi
```

### Expected Output Markers

| Marker | Meaning |
|--------|---------|
| `[PASS] All test suites passed` | All tests passed |
| `[FAIL]` | At least one test failed |
| `PAGE FAULT` | Kernel crashed |
| `KERNEL PANIC` | Kernel panic occurred |

---

## Local Testing

The Makefile mirrors the CI behavior for local testing:

```bash
# Run tests locally (uses semihosting)
make test

# Or with explicit build directory
make BUILD_DIR=/c/temp/slmos-build kernel test
```

The local Makefile also uses `-semihosting` and parses output for `[PASS]`/`[FAIL]`.

---

## Build Artifacts

Each CI run produces the following artifacts (retained 7 days):

| Artifact | Description |
|----------|-------------|
| `slmos.elf` | Kernel ELF binary (for debugging) |
| `slmos.bin` | Raw binary image (for flashing) |
| `test_output.txt` | Complete QEMU console output |

---

## Troubleshooting CI Failures

### Build Failures

1. **Toolchain not found**: Check ARM GCC installation step
2. **Rust build errors**: Check `runtime/` for compilation issues
3. **CMake errors**: Verify toolchain file exists

### Test Failures

1. **Timeout**: Semihosting should prevent this; check if kernel hangs before tests
2. **PAGE FAULT**: Review crash location in test output
3. **[FAIL] markers**: Check which specific test failed

### Debugging Locally

```bash
# Reproduce CI environment locally
make kernel-clean kernel test

# View full test output
cat /c/temp/slmos-build/test-output.log

# Run QEMU manually with more verbose output
qemu-system-aarch64 -M virt -cpu max -smp 4 -m 1G -nographic \
    -semihosting -kernel build/slmos.elf
```

---

## Future Enhancements

The following CI features are planned but not yet implemented:

- **Coverage tracking**: Measure test coverage percentage
- **Performance regression detection**: Track context switch time, etc.
- **Multi-platform matrix**: Test on multiple QEMU configurations
- **Hardware test farm**: Run tests on real Jetson hardware
- **Artifact caching**: Speed up builds by caching toolchains

---

## Files

| File | Purpose |
|------|---------|
| `.github/workflows/ci.yml` | GitHub Actions workflow definition |
| `kernel/src/semihosting.c` | Semihosting implementation |
| `kernel/include/semihosting.h` | Semihosting API declarations |
| `kernel/tests/test_harness.c` | Test harness with semihosting exit |
| `CMakeLists.txt` | Build configuration (ENABLE_SEMIHOSTING) |
| `Makefile` | Local build/test with semihosting |

---

*Created: December 2025*
