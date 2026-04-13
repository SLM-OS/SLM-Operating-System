/*
 * Test Harness for SLM-OS
 *
 * Provides Unity output functions and test suite management.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "unity.h"

/*
 * Initialize the test harness.
 * Must be called before running any tests.
 */
void test_harness_init(void);

/*
 * Run all test suites.
 * Returns total number of failures across all suites.
 */
int test_harness_run_all(void);

/* ============================================================================
 * Test Suite Declarations
 * ============================================================================ */

/* IPC tests (existing) */
int test_suite_ipc(void);

/* Scheduler tests (existing) */
int test_suite_scheduler(void);

/* Model memory tests (new - calls into Rust) */
int test_suite_model_mem(void);

/* Eviction policy tests (Phase AI-Eviction M1/M2 — calls into Rust) */
int test_suite_eviction(void);

/* Priority inheritance mutex tests */
int test_suite_pi_mutex(void);

/* GPU subsystem tests */
int test_suite_gpu(void);

/* Component system tests */
int test_suite_component(void);

/* Virtual filesystem tests */
int test_suite_vfs(void);

/* Shell command tests */
int test_suite_shell(void);

/* VMM/TLB tests */
int test_suite_vmm(void);

/* PMM buddy allocator tests */
int test_suite_pmm(void);

/* LittleFS integration tests */
int test_suite_littlefs(void);

/* Networking tests (QEMU only) */
int test_suite_net(void);

/* Lua scripting tests */
int test_suite_lua(void);

/* Multi-core integration tests (actual tasks across CPUs) */
int test_suite_integration(void);

/* x86-64 boot and platform tests (x86 only) */
int test_suite_x86_boot(void);

/* Model loader tests (ONNX parsing, registry - calls into Rust) */
int test_suite_model_loader(void);

/* Inference engine tests (tensor ops, end-to-end MNIST - calls into Rust) */
int test_suite_inference(void);

/* GPU compute integration tests (capability detection, fallback - calls into Rust) */
int test_suite_gpu_compute(void);

/* Syscall infrastructure tests (dispatch, trap frame, user task creation) */
int test_suite_syscall(void);

/* Component integration tests (infer_classify, example components) */
int test_suite_components_m5(void);

/* DTB parser tests (BOOT-H1 bounds checks) */
int test_suite_dtb(void);

/* ELF loader tests (BOOT-H2 bounds checks) */
int test_suite_elf(void);

/* Message router tests (Rust FFI, ARM64 only) */
int test_suite_msg_router(void);

/* Steal-deque unit tests (#59 Phase A) */
int test_suite_steal_deque(void);

#endif /* TEST_HARNESS_H */
