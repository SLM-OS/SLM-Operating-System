# Testing Strategy for SLM-OS

This document describes the testing approach for SLM-OS, including current implementation and planned progression toward full CI/CD integration.

---

## Current Testing Infrastructure

### `make test` Target

The primary testing mechanism is `make test`, which:

1. Builds the kernel
2. Runs QEMU with a 60-second timeout
3. Captures output to `build/test-output.log`
4. Parses output for test results
5. Returns exit code 0 on success, 1 on failure

```bash
make test
```

Example output:
```
Running kernel tests (timeout: 60s)...

Test Results:
=============
PASSED - All tests passed
[INFO] VMM tests passed
[INFO] Spinlock tests passed
[INFO] SMP tests passed
[INFO] Scheduler tests passed
```

### Test Result Detection

Tests use consistent markers for pass/fail detection:

| Marker | Meaning |
|--------|---------|
| `[PASS]` | Individual test passed |
| `[FAIL]` | Individual test failed |
| `[INFO] <name> tests passed` | Test suite completed successfully |

The Makefile checks for `[FAIL]` first (any failure = overall failure), then checks for `tests passed` to confirm success.

---

## Current Test Suites

### VMM Tests (`kernel/src/vmm.c`)

Validates the Virtual Memory Manager after MMU is enabled:

| Test | Description |
|------|-------------|
| virt_to_phys (identity) | Translate identity-mapped RAM address |
| virt_to_phys (TTBR1) | Translate kernel high address |
| is_mapped (true) | Verify mapped address returns true |
| is_mapped (false) | Verify unmapped address returns false |
| TTBR1 read/write | Write via high VA, read via identity VA |
| Dynamic map/unmap | Map new block, read/write, unmap |

Tests run automatically during `vmm_init()` after MMU is enabled.

### Spinlock Tests (`kernel/src/smp.c`)

Validates synchronization primitives on a single core:

| Test | Description |
|------|-------------|
| Spinlock initialized | Verify lock starts unlocked |
| spin_lock acquires | Lock state changes after acquire |
| spin_trylock (held) | Returns 0 when lock is held |
| spin_unlock releases | Lock state changes after release |
| spin_trylock (free) | Returns 1 when lock is free |
| Ticket lock | Basic acquire/release cycle works |
| IRQ save/restore | DAIF manipulation works correctly |
| IRQ-safe spinlock | Combined lock+IRQ disable works |
| Memory barriers | dmb/dsb/isb execute without fault |

Tests run automatically at start of `smp_init()` before secondary cores boot.

### SMP Tests (`kernel/src/smp.c`)

Validates multi-core boot via PSCI:

| Test | Description |
|------|-------------|
| All CPUs online | Verify `cpus_online` equals `cpu_count` |
| CPU online flags | Each CPU's `online` flag is set |
| CPU logical map | Logical ID lookup returns correct value |
| Boot CPU MPIDR | CPU 0's MPIDR matches actual register |

Tests run automatically at the end of `smp_init()` after all cores boot.

### Scheduler Tests (`kernel/src/main.c`)

Validates the multi-core scheduler, task distribution, migration, and concurrency:

| Test | Description |
|------|-------------|
| Basic Multi-Core Execution | 3 tasks run concurrently on CPUs 1, 2, 3 |
| Cross-Core Task Migration | Task queued on CPU 1, migrated to CPU 3, verified running on new CPU |
| Stress Test | 6 tasks (2 per CPU) complete correctly across CPUs 1, 2, 3 |
| Lock Contention | 3 tasks × 50 increments with spinlock, counter equals expected (no race conditions) |

**Test Notes:**
- Tests run on CPUs 1-3 to avoid interfering with the main task on CPU 0
- Migration test uses a "blocker" task to keep CPU 1 busy while migrating a READY task
- Lock contention test verifies that spinlocks correctly protect shared data across cores

Tests run from the main task function after boot completes. The kernel triggers an intentional undefined instruction fault after tests complete to terminate QEMU.

---

## Writing New Tests

### Adding Tests to Existing Suites

Follow the pattern in `vmm_run_tests()`:

```c
{
    /* Test description */
    bool condition = /* test logic */;

    if (condition) {
        uart_printf("  [PASS] Test description\n");
    } else {
        uart_printf("  [FAIL] Test description - details\n");
        errors++;
    }
}
```

### Creating New Test Suites

1. Create a test function: `static int <subsystem>_run_tests(void)`
2. Use `[PASS]`/`[FAIL]` markers for individual tests
3. Print `[INFO] <name> tests passed` on success
4. Call from the subsystem's init function or main
5. Update this document

---

## Testing Progression Plan

### Phase 1: Current State (Manual + `make test`)

- **Status**: Implemented
- Tests run as part of kernel boot
- `make test` provides automated pass/fail detection
- Manual verification via `make run` for debugging

### Phase 2: Multiple Test Suites

- **Trigger**: When 2-3 distinct test suites exist (VMM, SMP, IPC, etc.)
- Add test suite selection (run all vs. specific suite)
- Consider separating test code from production code
- Add test result summary (X passed, Y failed)

### Phase 3: Test Runner Script

- **Trigger**: When test configuration becomes complex
- Create `scripts/run-tests.sh` or `scripts/run-tests.py`
- Support test filtering, verbose output, timing
- Generate machine-readable results (JSON/JUnit XML)

### Phase 4: CI/CD Integration

- **Trigger**: When project is shared or needs automated regression testing
- GitHub Actions or similar CI platform
- Run tests on every commit/PR
- Block merges on test failure
- Add hardware-specific test configurations (QEMU variants, real hardware)

---

## Design Decisions

### Tests Run During Boot

**Decision**: Tests run as part of kernel initialization, not as separate binaries.

**Rationale**:
- Kernel is in early development; no loader infrastructure yet
- Tests need full kernel context (MMU, interrupts, scheduler)
- Simpler than building separate test harnesses

**Trade-off**: Tests add to boot time (~30 seconds for full suite); will need to be conditional or removable for production builds.

### QEMU-Based Testing

**Decision**: All automated tests run in QEMU, not on real hardware.

**Rationale**:
- Reproducible environment
- No hardware required for CI
- Fast iteration

**Trade-off**: May miss hardware-specific bugs. Real hardware testing should be added in Phase 4.

### Timeout-Based Test Completion

**Decision**: `make test` uses a fixed timeout to terminate QEMU.

**Rationale**:
- Kernel doesn't have a "shutdown" mechanism yet
- Simple and reliable
- 60 seconds allows full scheduler test suite to complete

**Future**: Add QEMU semihosting exit or serial command to cleanly terminate tests.

---

## Test Output Location

- **Log file**: `build/test-output.log`
- Contains full kernel output from test run
- Useful for debugging test failures

---

## Troubleshooting

### Tests timeout but don't complete

- Increase `TEST_TIMEOUT` in Makefile
- Check if kernel is hanging before tests run
- Run `make run` to see full output interactively

### "UNKNOWN - Could not determine test status"

- Tests may not have run (crash before reaching them)
- Check `build/test-output.log` for panic or exception output
- Ensure test suite prints `[INFO] <name> tests passed`

### Tests pass locally but fail in CI

- Check for timing-sensitive tests
- Ensure CI environment matches local (QEMU version, CPU count)
- Add debug output to identify differences

---

*Created: December 2025*
*Last updated: December 2025*
