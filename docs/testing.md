# Testing Strategy for SLM-OS

This document describes the testing approach for SLM-OS, including current implementation and planned progression toward full CI/CD integration.

---

## Unity Test Framework

SLM-OS uses a bare-metal compatible subset of the [Unity Test Framework](https://github.com/ThrowTheSwitch/Unity) for C tests.

### Test Directory Structure

```
kernel/tests/
├── unity.h            # Unity API (macros, assertions)
├── unity.c            # Unity implementation
├── test_harness.h     # Test suite declarations
├── test_harness.c     # UART output, suite runner, semihosting exit
├── test_ipc.c         # IPC test suite
├── test_scheduler.c   # Scheduler tests (29 tests: priority, deadline, isolation, benchmarks)
├── test_model_mem.c   # Model memory tests (Rust allocator via FFI)
├── test_pi_mutex.c    # Priority inheritance mutex tests
├── test_gpu.c         # GPU subsystem tests (22 tests)
├── test_pmm.c         # PMM buddy allocator tests (24 tests)
├── test_vmm.c         # VMM and TLB invalidation tests (10 tests)
├── test_component.c   # Component system tests
├── test_vfs.c         # Virtual filesystem tests
├── test_shell.c       # Shell command and path resolution tests (106 tests)
├── test_littlefs.c    # LittleFS and block device tests (26 tests)
├── test_net.c         # Networking tests — IP utils + virtqueue ring
│                      #   bookkeeping under cache maintenance (QEMU only)
├── test_lua.c         # Lua scripting tests
├── test_integration.c # Multi-core integration tests (5 tests)
└── test_x86_boot.c    # x86-64 boot and platform tests (x86 only) —
                       # includes LAPIC EOI fence stress + framebuffer
                       # scroll regression coverage
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

SLM-OS uses a compile-time flag `ENABLE_BOOT_TESTS` to separate test and interactive modes:

| Build | Flag | Behavior |
|-------|------|----------|
| `make kernel-test` | `ENABLE_BOOT_TESTS=ON` | Runs all tests at boot, exits via semihosting |
| `make kernel` | `ENABLE_BOOT_TESTS=OFF` | Boots directly to interactive shell |

### `make test` Target

The primary testing mechanism is `make test`, designed for CI/CD automation:

1. Builds a separate test kernel with `ENABLE_BOOT_TESTS=ON`
2. Runs QEMU with semihosting enabled (clean exit codes)
3. Captures output to `build/test-output.log`
4. Returns QEMU exit code: 0 = all tests passed, 1 = failure

```bash
make test                    # Build and run all tests
make kernel-test-clean test  # Clean rebuild and test
```

Example output:
```
Running kernel tests...

Test Results:
=============
PASSED - All tests passed (exit code 0)
```

The test kernel executes all Unity test suites, Rust FFI tests, and multi-core integration tests, then exits via ARM semihosting.

### `make shell` Target

For interactive use, `make shell` boots directly to the shell without running tests:

```bash
make shell
```

The shell is immediately available:
```
SLM-OS Debug Shell
Type 'help' for available commands.

slm> help
slm> mem
slm> tasks
```

Exit QEMU with `Ctrl-A X`.

### Build Directories

Tests use a separate build directory to avoid conflicts:

| Target | Build Directory | Notes |
|--------|-----------------|-------|
| `make kernel` | `build/kernel` | Interactive shell kernel |
| `make kernel-test` | `build/kernel-test` | Test kernel with ENABLE_BOOT_TESTS |

### Test Result Detection

Tests use consistent markers for pass/fail detection:

| Marker | Meaning |
|--------|---------|
| `[PASS]` | Individual test passed |
| `[FAIL]` | Individual test failed |
| `[PASS] All test suites passed!` | All tests completed successfully |
| Exit code 0 | Semihosting exit success |
| Exit code 1 | Semihosting exit failure |

The Makefile primarily uses the semihosting exit code to determine pass/fail, with fallback to output parsing for crash detection.

---

## Current Test Suites

### VMM Tests (`kernel/tests/test_vmm.c`)

Validates the Virtual Memory Manager including TLB invalidation. Tests run via Unity framework (10 tests total).

#### Page Table Verification

| Test | Description |
|------|-------------|
| test_virt_to_phys_accuracy | vmm_virt_to_phys() returns correct physical address |
| test_va_pa_coherency | Write via VA is visible when read via PA |

#### TLB Invalidation Correctness (Functional Tests)

These tests actually remap pages and verify the new mapping is used after TLB invalidation:

| Test | Description |
|------|-------------|
| test_remap_requires_invalidation | Remap VA→PA2, invalidate TLB, verify PA2 data read |
| test_remap_with_full_flush | Same as above using vmm_invalidate_tlb_all() |
| test_remap_with_range_invalidation | Same as above using vmm_invalidate_tlb_range() |
| test_sequential_remaps | Remap VA to PA1→PA2→PA3→PA1, verify each |
| test_rapid_remap_stress | 50 rapid remap cycles with verification |

#### ASID and Multi-CPU Tests

| Test | Description |
|------|-------------|
| test_asid_invalidation_executes | ASID-specific TLB invalidation executes |
| test_asid_all_invalidation_executes | Full ASID flush executes |
| test_tlb_broadcast_all_cpus | TLB operations broadcast to all CPUs |

**Test Notes:**
- Uses test helper functions to manipulate page tables without auto-TLB invalidation
- Proves TLB invalidation is actually necessary and working
- Not just smoke tests — actual page remapping verified

Early VMM validation tests also run during `vmm_init()` after MMU is enabled.

### Spinlock Tests (`kernel/sched/smp.c`)

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

### SMP Tests (`kernel/sched/smp.c`)

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
| test_pi_mutex_trylock_fail | trylock returns 0 when mutex is already locked (simulates held lock) |
| test_pi_mutex_priority_preserved | Owner's original priority saved and restored on unlock |
| test_priority_inheritance_basic | LOW owner boosted to HIGH when HIGH-pri task waits |
| test_inversion_count | Verify lock/unlock cycle without waiters doesn't increment inversion count |

**Test Notes:**
- `test_priority_inheritance_basic` manually simulates PI scenario by creating owner/waiter tasks
- Verifies priority boost occurs (`effective_priority` changes from LOW to HIGH)
- Verifies priority restoration on unlock
- Logs PI events: `"PI: Boosting task 'X' (pri N->M) for waiter 'Y'"`

### IPC Tests (`kernel/tests/test_ipc.c`)

Validates message queues, shared buffers, and priority-based messaging. Tests run via Unity framework (17 tests total).

#### Basic Queue Operations

| Test | Description |
|------|-------------|
| test_queue_create_destroy | Create queue, verify ID, destroy and verify lookup returns NULL |
| test_queue_nonblocking_send_recv | Send message, receive and verify contents |
| test_queue_lookup_by_id | Find queue by ID |
| test_recv_empty_queue_nonblocking | Receive on empty queue fails |

#### Shared Buffer Tests

| Test | Description |
|------|-------------|
| test_buffer_create_destroy | Create shared buffer, destroy and verify lookup returns NULL |
| test_buffer_map_unmap | Map buffer, write/read test pattern, unmap |
| test_buffer_lookup_by_id | Find buffer by ID |

#### Timeout and Blocking Tests

| Test | Description |
|------|-------------|
| test_recv_timeout | Receive on empty queue times out correctly |
| test_send_timeout_full_queue | Send on full queue times out correctly |

#### Statistics and Stress Tests

| Test | Description |
|------|-------------|
| test_no_memory_leak | Create/destroy IPC objects, verify memory reclaimed |
| test_queue_statistics | Verify per-queue stats (sent, recv, high_water) |
| test_global_ipc_statistics | Verify global IPC stats tracking |
| test_queue_stress | High-throughput send/recv stress test |

#### Priority-Based Message Queue Tests

These tests validate the priority queue implementation with 8 priority levels (0=lowest, 7=highest) and starvation prevention.

| Test | Description |
|------|-------------|
| test_priority_fifo_within_level | Messages at same priority level are received in FIFO order |
| test_priority_interleaved_operations | High-priority messages inserted between lows are received in correct order |
| test_priority_skip_empty_levels | Higher priority levels skip empty lower levels correctly |
| test_starvation_threshold_boundary | After 16 high-priority messages, one low-priority is served to prevent starvation |

**Test Notes:**
- Priority tests use `msg_send_priority()` API with explicit priority levels
- FIFO test verifies ordering by encoding sequence numbers in messages
- Starvation test verifies the threshold (16) by counting dequeue order

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
| test_gpu_free_null_is_safe | gpu_free(NULL) doesn't crash and PMM state unchanged |
| test_gpu_free_clears_buffer | After free, buffer fields are zeroed |

#### Cache Coherency Tests

| Test | Description |
|------|-------------|
| test_cache_clean_executes | DC CVAC cache clean executes without fault |
| test_cache_invalidate_executes | DC IVAC cache invalidate executes without fault |
| test_cache_flush_executes | DC CIVAC cache flush executes without fault |
| test_gpu_sync_for_gpu_executes | gpu_sync_for_gpu() (clean) executes correctly |
| test_gpu_sync_for_cpu_executes | gpu_sync_for_cpu() (invalidate) executes and data still readable |
| test_cache_ops_null_safe | Cache ops with NULL address don't crash (verifies PMM unchanged) |
| test_cache_ops_zero_size_safe | Cache ops with zero size don't corrupt memory (writes pattern, verifies unchanged) |

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

### PMM Tests (`kernel/tests/test_pmm.c`)

Comprehensive validation of the buddy allocator physical memory manager. Tests run via Unity framework (24 tests total).

These tests verify actual buddy allocator behavior, not just page counts. Key tests prove coalescing works by allocating larger blocks after freeing smaller ones.

#### Basic Allocation

| Test | Description |
|------|-------------|
| test_single_page_alloc | Allocate/free single page, verify page count |
| test_power_of_two_alloc | Allocate exact power-of-2 pages (4 pages) |
| test_non_power_of_two_rounds_up | Verify 3 pages rounds up to 4 |

#### Alignment Verification

| Test | Description |
|------|-------------|
| test_alignment_all_orders | Orders 0-8 return properly aligned addresses |

#### Buddy Address Calculation

| Test | Description |
|------|-------------|
| test_buddy_address_calculation | Verify XOR-based buddy address math |

#### Coalescing Verification (Critical)

| Test | Description |
|------|-------------|
| test_coalesce_enables_larger_allocation | **Proves coalescing works** by allocating 4-page block after freeing 4 singles |
| test_recursive_coalescing | Free 8 pages → verify 4+ merges → allocate 8-page block |
| test_coalesce_any_free_order | Coalescing works regardless of free order (forward, reverse, interleaved) |

#### Split Verification

| Test | Description |
|------|-------------|
| test_split_tracking | Verify split_count increases during allocation |
| test_split_creates_buddies | Splitting order N requires exactly N splits |

#### Merge Statistics

| Test | Description |
|------|-------------|
| test_merge_counting | Verify merge_count tracks coalescing operations |

#### Free List Integrity

| Test | Description |
|------|-------------|
| test_free_list_integrity | After alloc/free cycles, free counts sum correctly |

#### Fragmentation and Recovery

| Test | Description |
|------|-------------|
| test_fragmentation_recovery_large | 100 single pages → free all → allocate 64-page block |
| test_checkerboard_fragmentation | Checkerboard free pattern still coalesces fully |

#### Exhaustion and Recovery

| Test | Description |
|------|-------------|
| test_exhaustion_recovery | Allocate until OOM, free all, verify full recovery |

#### Edge Cases

| Test | Description |
|------|-------------|
| test_zero_alloc | Zero pages returns NULL |
| test_double_free_detected | Double-free warns but doesn't crash |
| test_memory_is_writable | Allocated memory can be written |
| test_large_allocation_writable | 16-page block is fully writable |

#### Statistics

| Test | Description |
|------|-------------|
| test_statistics_sane | Basic PMM stats are consistent |
| test_buddy_stats_operations | alloc_count and free_count track correctly |
| test_free_counts_sum_correctly | Sum of free_counts[order] × 2^order = total free pages |

#### Stress Tests

| Test | Description |
|------|-------------|
| test_no_memory_leak | 500 alloc/free cycles don't leak memory |
| test_mixed_workload_stress | Complex allocation pattern with proper size tracking |

**Test API:**
- `pmm_get_buddy_stats()` exposes internal buddy state for testing
- Tests verify actual behavior (coalescing enables larger allocations), not just page counts

### LittleFS Tests (`kernel/tests/test_littlefs.c`)

Validates the filesystem stack including block device abstraction, RAM disk driver, LittleFS wrapper, and VFS mount point integration. Tests run via Unity framework (26 tests total).

#### RAM Disk Tests

| Test | Description |
|------|-------------|
| test_ramdisk_create | Create RAM disk with specified geometry, verify block size/count |
| test_ramdisk_read_write | Write pattern to block, read back and verify contents match |
| test_ramdisk_erase | Erase block, verify memory reset to 0xFF (flash erased state) |

#### LittleFS Mount Tests

| Test | Description |
|------|-------------|
| test_lfs_format_mount | Format device, mount LittleFS, unmount |
| test_lfs_remount | Format, mount, unmount, remount without format |
| test_lfs_stat | Get filesystem statistics (total blocks, used blocks) |

#### File Operation Tests

| Test | Description |
|------|-------------|
| test_lfs_file_create_write | Create file with LFS_O_CREAT, write content, close |
| test_lfs_file_read | Write file, reopen for read, verify contents |
| test_lfs_file_seek_size | Test file size and seek (SET, CUR) operations |
| test_lfs_file_truncate | Truncate file to smaller size, verify content preserved up to size |
| test_lfs_rename | Rename file, verify old path gone and new path exists |
| test_lfs_append_mode | Multiple opens with LFS_O_APPEND, verify content accumulates |
| test_lfs_offset_read | Seek to offsets, read partial content, verify streaming reads |

#### Directory Operation Tests

| Test | Description |
|------|-------------|
| test_lfs_mkdir | Create directory, verify via stat |
| test_lfs_dir_list | Create files/dirs, list directory, verify entries found |
| test_lfs_remove | Create file, verify exists, remove, verify gone |

#### VFS Mount Point Tests

| Test | Description |
|------|-------------|
| test_vfs_mount_lookup | Verify /mnt and /mnt/files nodes exist with correct types |
| test_vfs_mount_read_file | Read file via vfs_read_path() through mount point |
| test_vfs_mount_list_dir | List directory via vfs_list_path() through mount point |

#### VFS Mount Context Tests

| Test | Description |
|------|-------------|
| test_vfs_get_mount_ctx | Retrieve mount context and subpath from VFS path |
| test_vfs_read_with_offset | Read file at different offsets via vfs_read_path() |

#### Write Through Mount Point Tests

| Test | Description |
|------|-------------|
| test_write_through_mount | Create file via mount context, read back through VFS |
| test_mkdir_through_mount | Create directory via mount context, verify via stat |
| test_rename_through_mount | Rename file via mount context, verify old/new paths |
| test_truncate_through_mount | Truncate file via mount context, verify size changed |
| test_append_through_mount | Append to file via mount context, verify content accumulated |

**Test Notes:**
- Each test creates its own isolated RAM disk and LittleFS mount
- Tests verify actual data round-trips, not just API success
- VFS tests use the mount created during kernel boot in main.c
- RAM disk tests verify flash semantics (erase-before-write, 0xFF erased state)
- Write tests clean up created files after verification

### Shell Tests (`kernel/tests/test_shell.c`)

Validates shell command dispatch, argument parsing, working directory management, path resolution, file utility commands, and the file-driven help system. Tests run via Unity framework (106 tests total).

#### Command Dispatch Tests

| Test | Description |
|------|-------------|
| test_shell_empty_command | Empty command returns 0 |
| test_shell_whitespace_only | Whitespace-only returns 0 |
| test_shell_unknown_command | Unknown command returns -1 |

#### Basic Commands

| Test | Description |
|------|-------------|
| test_shell_cmd_clear | `clear` executes successfully |
| test_shell_cmd_uptime | `uptime` executes successfully |

#### VFS Command Error Cases

| Test | Description |
|------|-------------|
| test_shell_cmd_ls_nonexistent | `ls /nonexistent` returns -1 |
| test_shell_cmd_cat_no_args | `cat` with no args returns -1 |
| test_shell_cmd_cat_nonexistent | `cat /nonexistent` returns -1 |
| test_shell_cmd_cat_directory | `cat /sys` (directory) returns -1 |

#### VFS Command Success Cases

| Test | Description |
|------|-------------|
| test_shell_cmd_ls_root | `ls /` lists root directory |
| test_shell_cmd_ls_sys | `ls /sys` lists sys directory |
| test_shell_cmd_cat_sys_memory | `cat /sys/memory` reads file |
| test_shell_cmd_cat_sys_cpus | `cat /sys/cpus` reads file |

#### Verbose Status Commands

| Test | Description |
|------|-------------|
| test_shell_cmd_help | `help` lists commands |
| test_shell_cmd_mem | `mem` shows memory stats |
| test_shell_cmd_tasks | `tasks` shows task list |
| test_shell_cmd_cpu | `cpu` shows CPU info |
| test_shell_cmd_vmm | `vmm` shows virtual memory |
| test_shell_cmd_ipc | `ipc` shows IPC stats |
| test_shell_cmd_model | `model` shows model memory |
| test_shell_cmd_dtb | `dtb` shows device tree |

#### Component Command Tests

| Test | Description |
|------|-------------|
| test_shell_cmd_component_help | `component` shows help |
| test_shell_cmd_component_list_empty | `component list` shows empty list |
| test_shell_cmd_component_register_missing_args | Missing args returns -1 |
| test_shell_cmd_component_register_valid | Valid registration succeeds |
| test_shell_cmd_component_register_invalid_type | Invalid type returns -1 |
| test_shell_cmd_component_unregister_missing_args | Missing args returns -1 |
| test_shell_cmd_component_unregister_invalid | Invalid index returns -1 |
| test_shell_cmd_component_unregister_valid | Valid unregister succeeds |
| test_shell_cmd_component_status_missing_args | Missing args returns -1 |
| test_shell_cmd_component_status_by_index | Status by index works |
| test_shell_cmd_component_status_not_found | Nonexistent name returns -1 |
| test_shell_cmd_component_unknown_subcmd | Unknown subcommand returns -1 |

#### Run and Kill Commands

| Test | Description |
|------|-------------|
| test_shell_cmd_run_unknown_program | Unknown program returns -1 |
| test_shell_cmd_kill_no_args | No args returns -1 |
| test_shell_cmd_kill_invalid_pid | Invalid PID returns -1 |
| test_shell_cmd_kill_nonexistent_pid | Nonexistent PID returns -1 |

#### Argument Parsing Tests

| Test | Description |
|------|-------------|
| test_shell_extra_whitespace | Extra whitespace handled correctly |
| test_shell_args_with_spaces | Args with spaces between handled |
| test_shell_register_external_command | External command registration works |

#### Working Directory Tests (pwd, cd)

| Test | Description |
|------|-------------|
| test_shell_cmd_pwd | `pwd` prints current directory |
| test_shell_cmd_cd_root | `cd /` goes to root |
| test_shell_cmd_cd_no_arg | `cd` (no arg) goes to root |
| test_shell_cmd_cd_valid_dir | `cd /sys` changes to /sys |
| test_shell_cmd_cd_nonexistent | `cd /nonexistent` returns -1 |
| test_shell_cmd_cd_file | `cd /sys/memory` (file, not dir) returns -1 |
| test_shell_cmd_cd_dotdot | `cd ..` goes to parent |
| test_shell_cmd_cd_dotdot_at_root | `cd ..` at root stays at root |
| test_shell_cmd_cd_dot | `cd .` stays in current dir |

#### Relative Path Resolution Tests

| Test | Description |
|------|-------------|
| test_shell_cmd_ls_cwd | `ls` (no arg) lists cwd |
| test_shell_cmd_ls_relative | `ls sys` from root lists /sys |
| test_shell_cmd_ls_dot | `ls .` lists cwd |
| test_shell_cmd_ls_dotdot | `ls ..` lists parent |
| test_shell_cmd_cat_relative | `cat memory` from /sys reads /sys/memory |
| test_shell_path_complex | Complex path `/sys/../proc/../sys` resolves correctly |
| test_shell_path_trailing_slash | Trailing slashes handled |
| test_shell_path_double_slash | Double slashes `//` handled |

#### Mount Point Path Tests

| Test | Description |
|------|-------------|
| test_shell_cmd_cd_mount_subdir | `cd /mnt/files` works |
| test_shell_mount_relative_path | `cat hello.txt` from /mnt/files works |
| test_shell_cmd_df_cwd | `df` with cwd in mount shows filesystem stats |

#### File Utility Command Tests

Tests for the file utility commands verify actual functionality, not just return codes. For example, `cp` tests verify file content is identical after copying, and `touch` tests verify file size is 0 bytes.

| Test | Description |
|------|-------------|
| test_shell_cmd_touch | `touch` creates empty file with verified 0-byte size |
| test_shell_cmd_touch_existing | `touch` on existing file does NOT truncate content |
| test_shell_cmd_cp | `cp` copies file contents (verified via VFS read) |
| test_shell_cmd_cp_binary | `cp` preserves binary content byte-for-byte (memcmp) |
| test_shell_cmd_cp_not_found | `cp` nonexistent file returns -1 |
| test_shell_cmd_cp_missing_args | `cp` without args returns -1 |
| test_shell_cmd_stat_file | `stat` shows file info with verified size > 0 |
| test_shell_cmd_stat_dir | `stat` shows directory info |
| test_shell_cmd_stat_not_found | `stat` nonexistent path returns -1 |
| test_shell_cmd_stat_virtual | `stat` works on virtual files (/sys/memory) |
| test_shell_cmd_stat_missing_args | `stat` without args returns -1 |
| test_shell_cmd_tree | `tree` shows recursive directory listing |
| test_shell_cmd_tree_subdir | `tree` lists nested subdirectories |
| test_shell_cmd_tree_depth | `tree` respects depth limit |
| test_shell_cmd_tree_virtual | `tree /sys` lists virtual directory |
| test_shell_cmd_tree_root | `tree /` with depth limit works |
| test_shell_cmd_wc | `wc` counts lines, words, bytes |
| test_shell_cmd_wc_known_content | `wc` on known file executes correctly |
| test_shell_cmd_wc_not_found | `wc` nonexistent file returns -1 |
| test_shell_cmd_wc_missing_args | `wc` without args returns -1 |
| test_shell_cmd_hexdump | `hexdump` shows hex and ASCII output |
| test_shell_cmd_hexdump_offset | `hexdump` respects offset parameter |
| test_shell_cmd_hexdump_middle | `hexdump` with offset in middle of file |
| test_shell_cmd_hexdump_not_found | `hexdump` nonexistent file returns -1 |
| test_shell_cmd_hexdump_missing_args | `hexdump` without args returns -1 |
| test_shell_cmd_grep | `grep Hello` finds pattern at start |
| test_shell_cmd_grep_middle | `grep from` finds pattern in middle |
| test_shell_cmd_grep_case | `grep hello` (lowercase) shows 0 matches |
| test_shell_cmd_grep_no_match | `grep NOTFOUND` shows "(0 matches)" |
| test_shell_cmd_grep_not_found | `grep` on nonexistent file returns -1 |
| test_shell_cmd_grep_missing_args | `grep` without pattern/file returns -1 |
| test_shell_cmd_grep_multiline | `grep` on multiline file works |
| test_shell_cmd_find | `find hello.txt` locates exact match |
| test_shell_cmd_find_wildcard | `find *.txt` matches with trailing wildcard |
| test_shell_cmd_find_leading_wildcard | `find *lo.txt` matches with leading wildcard |
| test_shell_cmd_find_no_match | `find *.xyz` shows "(0 files found)" |
| test_shell_cmd_find_question | `find hell?.txt` matches single char wildcard |
| test_shell_cmd_find_subdir | `find` recursively searches subdirectories |
| test_shell_cmd_find_missing_args | `find` without args returns -1 |
| test_shell_cmd_find_nonmount | `find /sys` returns -1 (not a mounted FS) |

#### Help System Tests

Tests for the file-driven help system that stores help text in `/mnt/files/help/*.txt`:

| Test | Description |
|------|-------------|
| test_shell_cmd_help_list | `help` lists all commands |
| test_shell_cmd_help_valid | `help cp` shows detailed help (verified for cp, ls, grep) |
| test_shell_cmd_help_unknown | `help nonexistent_command` returns -1 |
| test_shell_help_files_exist | Help files exist at `/mnt/files/help/cp.txt`, etc. |
| test_shell_help_file_content | Help files contain substantial content (>50 bytes) |
| test_shell_help_dir_listing | `ls /mnt/files/help` shows help files |

**Test Notes:**
- Tests reset cwd to `/` after each test to avoid state leakage
- Path resolution tests verify both virtual directories and mount points
- Error cases verify proper error codes and messages
- Each test uses `shell_execute()` for programmatic command execution

### Integration Tests (`kernel/tests/test_integration.c`)

Multi-core integration tests that exercise the scheduler with actual tasks running across CPUs. Unlike unit tests, these require the full scheduler infrastructure to be running. Tests run via Unity framework (5 tests total).

| Test | Description |
|------|-------------|
| test_multicore_basic | Distribute 3 tasks to CPUs 1, 2, 3 and verify all complete |
| test_task_migration | Create task on CPU 1, migrate to CPU 3 while blocker runs, verify runs on CPU 3 |
| test_stress_multicpu | Create 6 tasks across CPUs 1, 2, 3 (2 per CPU), verify all complete |
| test_lock_contention | 3 tasks contending on same spinlock, verify no race conditions |
| test_task_lifecycle | Rapid create/destroy of 8 tasks, verify memory reclaimed |

**Test Notes:**
- Tests create real tasks on secondary CPUs (not CPU 0 where main runs)
- Uses spinlock-protected test state for cross-core coordination
- Task migration test verifies `sched_migrate_task()` moves READY tasks between run queues
- Lock contention test counts atomic increments to detect race conditions
- Lifecycle test checks PMM free pages before/after to detect memory leaks

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

- **Status**: Complete (December 2025)
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

- **Status**: Complete (December 2025)
- GitHub Actions workflow (`.github/workflows/ci.yml`)
- Runs on every push to main/develop and all PRs
- Uses ARM semihosting for clean QEMU exit
- Uploads build artifacts (kernel ELF, test output)
- See `docs/ci-cd.md` for full documentation

---

## Design Decisions

### Separate Test and Shell Builds

**Decision**: Tests are enabled via compile-time flag `ENABLE_BOOT_TESTS`, creating two distinct kernel builds.

**Rationale**:
- Interactive shell should not be delayed by test execution
- Test failures should be caught by CI, not observed during manual testing
- Semihosting exit requires knowing in advance whether tests will run
- Separate builds allow optimization of each use case

**Implementation**:
- `CMakeLists.txt` defines `option(ENABLE_BOOT_TESTS ...)` (default OFF)
- `make test` builds with `-DENABLE_BOOT_TESTS=ON`
- `make shell` builds without the flag (default OFF)
- Separate build directories prevent conflicts: `build/kernel` vs `build/kernel-test`

### QEMU-Based Testing

**Decision**: All automated tests run in QEMU, not on real hardware.

**Rationale**:
- Reproducible environment
- No hardware required for CI
- Fast iteration

**Trade-off**: May miss hardware-specific bugs. Real hardware testing should be added in Phase 4.

### Semihosting for Test Exit

**Behavior**:
- Test kernel always exits via ARM semihosting after tests complete
- Exit code 0 = all tests passed
- Exit code 1 = one or more tests failed

This enables two distinct usage modes:

| Target | Use Case | Behavior |
|--------|----------|----------|
| `make test` | CI/CD automation | QEMU exits with pass/fail exit code |
| `make shell` | Interactive use | Boots directly to shell (no tests) |

**`make test` Implementation**:
1. Builds test kernel with `ENABLE_BOOT_TESTS=ON`
2. Runs QEMU with `-semihosting` flag
3. Captures output to `build/test-output.log`
4. Uses QEMU exit code to determine pass/fail
5. Falls back to output parsing for crash detection

**`make shell` Implementation**:
1. Builds normal kernel (no `ENABLE_BOOT_TESTS`)
2. Runs QEMU without timeout
3. Shell is immediately available
4. User interacts, exits via `Ctrl-A X`

**Note**: Semihosting is only compiled for QEMU builds (`ENABLE_SEMIHOSTING=1` when `PLATFORM=QEMU_VIRT`). On real hardware (Jetson), the HLT instruction would cause a fault, so semihosting calls are compile-time disabled.

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
