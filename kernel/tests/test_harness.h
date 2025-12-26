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

/* Priority inheritance mutex tests */
int test_suite_pi_mutex(void);

/* GPU subsystem tests */
int test_suite_gpu(void);

/* Component system tests */
int test_suite_component(void);

/* Virtual filesystem tests */
int test_suite_vfs(void);

#endif /* TEST_HARNESS_H */
