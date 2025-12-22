# Testing Strategy for SLM-OS

This document describes the testing approach for SLM-OS, including current implementation and planned progression toward full CI/CD integration.

---

## Unity Test Framework

SLM-OS uses a bare-metal compatible subset of the [Unity Test Framework](https://github.com/ThrowTheSwitch/Unity) for C tests.

### Test Directory Structure

```
kernel/tests/
├── unity.h           # Unity API (macros, assertions)
├── unity.c           # Unity implementation
├── test_harness.h    # Test suite declarations
├── test_harness.c    # UART output, suite runner
├── test_ipc.c        # IPC test suite
├── test_scheduler.c  # Scheduler tests (29 tests: priority, deadline, isolation, benchmarks)
├── test_model_mem.c  # Model memory tests (Rust allocator via FFI)
├── test_pi_mutex.c   # Priority inheritance mutex tests
└── test_gpu.c        # GPU subsystem tests (22 tests)
```

### Available Assertions

```c
TEST_ASSERT(condition)                    /* condition is true */
TEST_ASSERT_TRUE(condition)               /* same as above */
TEST_ASSERT_FALSE(condition)              /* condition is false */
TEST_ASSERT_NULL(ptr)                     /* ptr == NULL */
TEST_ASSERT_NOT_NULL(ptr)                 /* ptr != NULL */
TEST_ASSERT_EQUAL_INT(expected, actual)
TEST_ASSERT_EQUAL_UINT64(expected, actual)
TEST_ASSERT_EQUAL_PTR(expected, actual)
TEST_ASSERT_EQUAL_HEX64(expected, actual) /* hex output on failure */
TEST_ASSERT_GREATER_OR_EQUAL(threshold, value)
```

### Writing a New Test Suite

1. Create `kernel/tests/test_<feature>.c`:

```c
#include "unity.h"
#include "../include/<headers>.h"

static void test_something_works(void)
{
    int result = function_under_test();
    TEST_ASSERT_EQUAL_INT(42, result);
}

int test_suite_feature(void)
{
    UnityBegin("Feature Tests");
    RUN_TEST(test_something_works);
    return UnityEnd();
}
```

2. Add declaration to `test_harness.h`:
```c
int test_suite_feature(void);
```

3. Add call to `test_harness_run_all()` in `test_harness.c`

4. Add source to `CMakeLists.txt`:
```cmake
set(TEST_SOURCES
    ...
    kernel/tests/test_feature.c
)
```

### Test Output Format

```
[TEST] Model Memory Tests
  [PASS] test_weight_alloc_returns_valid_pointer
  [PASS] test_allocated_memory_is_writable
  [FAIL] test_something
    FAILED at kernel/tests/test_model_mem.c:42
      Expected: 100
      Actual:   99

----------------------------------------
Tests: 3  Passed: 2  Failed: 1
```

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
[INFO] IPC tests passed
[INFO] FFI tests passed
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

### Scheduler Tests (`kernel/tests/test_scheduler.c`)

Validates the deadline-aware scheduler with priority ordering, deadline boost, core isolation, and scheduling performance. Tests run via Unity framework (29 tests total).

#### Unit Tests: Deadline Boost Logic

| Test | Description |
|------|-------------|
| test_no_deadline_no_boost | Task with no deadline has effective_priority = base priority |
| test_deadline_field_set | task_set_deadline() correctly sets deadline_ns field |
| test_distant_deadline_no_boost | Deadline > 100ms gets no boost |
| test_deadline_100ms_boost_plus_one | Deadline 50-100ms gets +1 priority boost |
| test_deadline_50ms_boost_to_high | Deadline 10-50ms boosts to HIGH (6) |
| test_deadline_10ms_boost_to_critical | Deadline < 10ms boosts to CRITICAL (7) |
| test_missed_deadline_boost_to_critical | Deadline in past boosts to CRITICAL (7) |

#### Unit Tests: Priority and FFI

| Test | Description |
|------|-------------|
| test_set_priority_updates_field | task_set_priority() updates both priority fields |
| test_priority_clamped_to_max | Priority values > 7 clamped to CRITICAL (7) |
| test_ffi_task_create_returns_id | slm_task_create() returns valid task ID |
| test_ffi_set_priority | slm_task_set_priority() works via FFI |
| test_ffi_set_deadline | slm_task_set_deadline() works via FFI |
| test_ffi_invalid_task_id | FFI functions return -1 for invalid task IDs |

#### Unit Tests: Core Isolation

| Test | Description |
|------|-------------|
| test_isolate_core_marks_isolated | sched_isolate_core() sets isolation flag |
| test_cannot_isolate_cpu0 | CPU 0 (boot CPU) cannot be isolated |
| test_isolate_invalid_cpu | Invalid CPU ID returns error |
| test_affinity_any_avoids_isolated | Tasks with CPU_AFFINITY_ANY skip isolated cores |
| test_pinned_task_runs_on_isolated | Pinned tasks still run on isolated cores |
| test_set_affinity_updates_field | sched_set_task_affinity() updates task field |
| test_set_affinity_invalid_cpu | Invalid CPU returns error |
| test_multiple_cores_isolated | Multiple cores can be isolated simultaneously |

#### Integration Tests: Priority Ordering

| Test | Description |
|------|-------------|
| test_high_priority_runs_first | HIGH priority task runs before LOW (tracks execution order) |
| test_priority_ordering_multiple_levels | 5 tasks (CRITICAL/HIGH/NORMAL/LOW/IDLE) run in priority order |
| test_deadline_boost_affects_order | LOW task with urgent deadline runs before unboosted LOW |

#### Stress Tests

| Test | Description |
|------|-------------|
| test_no_starvation | HIGH and LOW priority tasks both complete all iterations; HIGH runs first |
| test_stress_mixed_priorities | 5 tasks with mixed priorities including deadline boost, verifies execution order |

#### Latency and Benchmark Tests

| Test | Description |
|------|-------------|
| test_isolated_core_latency | Compares wake-to-run latency on isolated vs non-isolated cores |
| test_benchmark_context_switch | Measures 100 context switches between two ping-pong tasks |
| test_benchmark_queue_operations | Measures add/remove latency for queue operations |

**Test Notes:**
- Priority ordering tests verify actual execution order, not just completion
- Stress tests verify both priority ordering AND starvation prevention
- Tests use CPU 0 with IRQ protection for deterministic ordering
- Benchmark tests log results with assessment (Excellent < 10µs, Good < 50µs, etc.)

### PI Mutex Tests (`kernel/tests/test_pi_mutex.c`)

Validates the priority-inheriting mutex implementation. Tests run via Unity framework.

| Test | Description |
|------|-------------|
| test_pi_mutex_init | Mutex initializes to unlocked state with NULL owner |
| test_pi_mutex_lock_unlock | Basic lock/unlock cycle updates locked flag and owner |
| test_pi_mutex_trylock_success | trylock succeeds when mutex is unlocked |
| test_pi_mutex_trylock_fail | trylock behavior when mutex is held |
| test_pi_mutex_priority_preserved | Owner's original priority saved and restored on unlock |
| test_priority_inheritance_basic | LOW owner boosted to HIGH when HIGH-pri task waits |
| test_inversion_count | pi_mutex_inversion_count() API is accessible |

**Test Notes:**
- `test_priority_inheritance_basic` manually simulates PI scenario by creating owner/waiter tasks
- Verifies priority boost occurs (`effective_priority` changes from LOW to HIGH)
- Verifies priority restoration on unlock
- Logs PI events: `"PI: Boosting task 'X' (pri N->M) for waiter 'Y'"`

### IPC Tests (`kernel/src/ipc.c`)

Validates message queues and shared buffers:

| Test | Description |
|------|-------------|
| Message queue create | Create queue with 8 slots, 64-byte messages |
| Message queue destroy | Free queue and verify cleanup |
| Non-blocking send | Send message, verify count increases |
| Non-blocking recv | Receive message, verify value matches |
| Recv on empty | Returns `IPC_ERR_EMPTY` when queue empty |
| Send on full | Returns `IPC_ERR_FULL` when queue full |
| Queue lookup | Find queue by ID, returns NULL after destroy |
| Shared buffer create | Create buffer, verify 2MB alignment and refcount |
| Shared buffer destroy | Free buffer and verify cleanup |
| Shared buffer map | Map buffer, verify refcount increases |
| Shared buffer read/write | Write value, read back and verify |
| Shared buffer unmap | Unmap buffer, verify refcount decreases |
| Buffer lookup | Find buffer by ID, returns NULL after destroy |

Tests run from `ipc_run_tests()` called at the start of the main task, before scheduler tests.

### Model Memory Tests (`kernel/tests/test_model_mem.c`)

Validates the Rust model memory allocator via FFI. These tests verify actual behavior, not just that calls succeed.

| Test | Description |
|------|-------------|
| weight_alloc_returns_valid_pointer | Allocate weight block, verify pointer is 2MB-aligned |
| allocated_memory_is_writable | Write patterns to start and end of block, read back |
| different_allocs_different_addresses | Two allocations return different, non-overlapping addresses |
| size_returns_block_size | Get size returns 2MB block size |
| pools_are_separate | Weight and workspace pools have different pool IDs and address ranges |
| refcount_prevents_premature_free | share() increments refcount, free() only releases when count reaches 0 |
| statistics_accuracy | Pool stats reflect actual allocations and frees |
| pool_exhaustion | Allocating more than pool capacity returns null handle |
| freed_memory_reused | After free, next alloc returns same block address |

Tests run via Unity framework during boot, called from `test_harness_run_all()`.

### GPU Tests (`kernel/tests/test_gpu.c`)

Validates the GPU platform abstraction layer. Since QEMU has no GPU hardware, these tests verify the stub driver implementation, cache coherency code paths, and API contracts. Tests run via Unity framework (22 tests total).

#### Initialization Tests

| Test | Description |
|------|-------------|
| test_gpu_available_after_init | GPU is available after kernel initialization |
| test_gpu_get_info_valid | gpu_get_info() returns valid driver info (stub driver) |
| test_gpu_get_info_null_returns_error | NULL parameter returns GPU_ERR_INVALID_PARAM |

#### Buffer Allocation Tests

| Test | Description |
|------|-------------|
| test_gpu_alloc_returns_valid_buffer | Allocation returns non-NULL cpu_addr and gpu_addr |
| test_gpu_alloc_memory_is_accessible | Allocated memory can be read and written |
| test_gpu_alloc_multiple_buffers_distinct | Multiple allocations return different, non-overlapping addresses |
| test_gpu_alloc_2mb_alignment | GPU_MEM_ALIGN_2MB flag returns 2MB-aligned address |
| test_gpu_alloc_zero_size_returns_error | Zero size returns GPU_ERR_INVALID_PARAM |
| test_gpu_alloc_null_buffer_returns_error | NULL buffer returns GPU_ERR_INVALID_PARAM |

#### Buffer Deallocation Tests

| Test | Description |
|------|-------------|
| test_gpu_free_releases_memory | Free returns pages to PMM (verified via pmm_get_free_pages()) |
| test_gpu_free_null_is_safe | Calling gpu_free(NULL) doesn't crash |
| test_gpu_free_clears_buffer | After free, buffer fields are zeroed |

#### Cache Coherency Tests

| Test | Description |
|------|-------------|
| test_cache_clean_executes | DC CVAC cache clean executes without fault |
| test_cache_invalidate_executes | DC IVAC cache invalidate executes without fault |
| test_cache_flush_executes | DC CIVAC cache flush executes without fault |
| test_gpu_sync_for_gpu_executes | gpu_sync_for_gpu() (clean) executes correctly |
| test_gpu_sync_for_cpu_executes | gpu_sync_for_cpu() (invalidate) executes correctly |
| test_cache_ops_null_safe | Cache ops with NULL address don't crash |
| test_cache_ops_zero_size_safe | Cache ops with zero size don't crash |

#### Integration Tests

| Test | Description |
|------|-------------|
| test_gpu_buffer_workflow | Full workflow: alloc → write → sync_for_gpu → sync_for_cpu → read → free |
| test_gpu_alloc_free_cycle | Allocate, free, reallocate — verifies memory reuse |
| test_gpu_alloc_large_buffer | Allocate and use 1MB buffer |

**Test Notes:**
- Cache coherency tests verify code paths execute without exceptions, but cannot fully validate cache behavior without real DMA hardware
- 2MB alignment test verifies the stub driver's over-allocation strategy for alignment
- Integration tests simulate typical GPU buffer lifecycle patterns

### FFI Tests (`runtime/src/lib.rs`)

Validates the Rust/C FFI boundary by exercising all FFI functions from the Rust side:

| Test | Description |
|------|-------------|
| alloc_pages/free_pages | Allocate 1 page via FFI, free it |
| alloc_pages (4 pages) | Allocate multiple pages, free them |
| slm_print | Print via FFI (verified by output appearing) |
| get_time_ns | Call time function without crash |
| slm_task_create | Create task with Rust entry function |
| msg_send | Send message to test queue |
| msg_recv | Receive message, verify content matches |

**Test Implementation Details:**

- Tests run from `rust_run_tests()` called from C after IPC tests
- Uses `slm_ffi_get_test_queue()` to get a message queue for IPC testing
- Task creation test spawns a real task with a Rust `extern "C"` entry point
- FFI type validation runs during `rust_init()` before these tests

**Additional Validation:**

The Rust runtime also validates FFI types at two levels:

1. **Compile-time**: `const _: () = { assert!(...) }` blocks verify type sizes
2. **Runtime**: `rust_ffi_validate()` verifies error codes and flag values match C

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

### Creating New Test Suites (C)

1. Create a test function: `static int <subsystem>_run_tests(void)`
2. Use `[PASS]`/`[FAIL]` markers for individual tests
3. Print `[INFO] <name> tests passed` on success
4. Call from the subsystem's init function or main
5. Update this document

### Adding FFI Tests (Rust)

Add tests to `rust_run_tests()` in `runtime/src/lib.rs`:

```rust
// Test N: Description
{
    let result = /* call FFI function */;
    let passed = /* verify result */;
    print_test_result(b"test name\0", passed);
    if !passed { failures += 1; }
}
```

Use the `print_test_result()` helper to maintain consistent output format.

---

## Testing Progression Plan

### Phase 1: Inline Tests (Manual + `make test`)

- **Status**: Complete
- Tests ran inline during subsystem initialization
- `make test` provides automated pass/fail detection
- Manual verification via `make run` for debugging

### Phase 2: Unity Test Framework

- **Status**: Implemented (December 2025)
- Unity test framework added to `kernel/tests/`
- Test suites separated into individual files
- Centralized test harness runs all suites
- Test result summary (X passed, Y failed) per suite

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
