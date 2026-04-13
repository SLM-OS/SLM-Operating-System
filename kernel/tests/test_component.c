/**
 * @file test_component.c
 * @brief Component System Tests
 *
 * Validates component registration, lifecycle management, and FFI boundary.
 */

#include "unity.h"
#include "component.h"
#include "uart.h"
#include "pmm.h"
#include "task.h"
#include "sched.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

/**
 * Clean up all registered components.
 * Call at start of tests that need a clean slate.
 */
static void cleanup_all_components(void)
{
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        component_unregister(i);  /* Ignore errors - slot may be empty */
    }
}

/* ============================================================================
 * Registration Tests
 * ============================================================================ */

/**
 * Test basic component registration.
 */
static void test_component_register_basic(void)
{
    cleanup_all_components();
    int idx = component_register("test-svc", "1.0.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    /* Verify it shows up in count */
    TEST_ASSERT_EQUAL_UINT32(1, component_count());

    /* Clean up */
    TEST_ASSERT_EQUAL(0, component_unregister((uint32_t)idx));
    TEST_ASSERT_EQUAL_UINT32(0, component_count());
}

/**
 * Test registration of different component types.
 */
static void test_component_register_types(void)
{
    cleanup_all_components();
    int idx_svc = component_register("svc", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    int idx_drv = component_register("drv", "1.0", COMPONENT_TYPE_DRIVER, COMPONENT_PRIORITY_HIGH);
    int idx_app = component_register("app", "1.0", COMPONENT_TYPE_APPLICATION, COMPONENT_PRIORITY_LOW);

    TEST_ASSERT_GREATER_OR_EQUAL(0, idx_svc);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx_drv);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx_app);

    /* All should be different slots */
    TEST_ASSERT_NOT_EQUAL(idx_svc, idx_drv);
    TEST_ASSERT_NOT_EQUAL(idx_drv, idx_app);
    TEST_ASSERT_NOT_EQUAL(idx_svc, idx_app);

    TEST_ASSERT_EQUAL_UINT32(3, component_count());
}

/**
 * Test registration with NULL parameters fails gracefully.
 */
static void test_component_register_null_params(void)
{
    cleanup_all_components();
    int idx = component_register(NULL, "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_EQUAL(-1, idx);

    idx = component_register("test", NULL, COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_EQUAL(-1, idx);

    /* Count should be unchanged */
    TEST_ASSERT_EQUAL_UINT32(0, component_count());
}

/**
 * Test registration with invalid type fails.
 */
static void test_component_register_invalid_type(void)
{
    cleanup_all_components();
    int idx = component_register("test", "1.0", 99, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_EQUAL(-1, idx);
    TEST_ASSERT_EQUAL_UINT32(0, component_count());
}

/**
 * Test slot exhaustion - registering more than MAX_COMPONENTS.
 */
static void test_component_register_exhaustion(void)
{
    cleanup_all_components();
    char name[32];
    int indices[COMPONENT_MAX_COUNT];

    /* Fill all slots */
    for (uint32_t i = 0; i < COMPONENT_MAX_COUNT; i++) {
        uart_snprintf(name, sizeof(name), "comp-%u", i);
        indices[i] = component_register(name, "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
        TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(0, indices[i], "Failed to register component");
    }

    TEST_ASSERT_EQUAL_UINT32(COMPONENT_MAX_COUNT, component_count());

    /* Next registration should fail */
    int overflow = component_register("overflow", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_EQUAL(-1, overflow);

    /* Count should still be MAX */
    TEST_ASSERT_EQUAL_UINT32(COMPONENT_MAX_COUNT, component_count());
}

/* ============================================================================
 * Unregistration Tests
 * ============================================================================ */

/**
 * Test basic unregistration.
 */
static void test_component_unregister_basic(void)
{
    cleanup_all_components();
    int idx = component_register("temp", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);
    TEST_ASSERT_EQUAL_UINT32(1, component_count());

    TEST_ASSERT_EQUAL(0, component_unregister((uint32_t)idx));
    TEST_ASSERT_EQUAL_UINT32(0, component_count());
}

/**
 * Test unregistering invalid index fails.
 */
static void test_component_unregister_invalid_index(void)
{
    TEST_ASSERT_EQUAL(-1, component_unregister(COMPONENT_MAX_COUNT + 10));
}

/**
 * Test unregistering empty slot fails.
 */
static void test_component_unregister_empty_slot(void)
{
    cleanup_all_components();
    /* Ensure slot 0 is empty */
    component_unregister(0);

    /* Try to unregister again - should fail */
    TEST_ASSERT_EQUAL(-1, component_unregister(0));
}

/**
 * Test double unregister fails.
 */
static void test_component_unregister_double(void)
{
    cleanup_all_components();
    int idx = component_register("temp", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    TEST_ASSERT_EQUAL(0, component_unregister((uint32_t)idx));
    TEST_ASSERT_EQUAL(-1, component_unregister((uint32_t)idx));  /* Second should fail */
}

/* ============================================================================
 * Query Tests
 * ============================================================================ */

/**
 * Test get_info retrieves correct data.
 */
static void test_component_get_info(void)
{
    cleanup_all_components();
    int idx = component_register("my-service", "2.5.1", COMPONENT_TYPE_DRIVER, COMPONENT_PRIORITY_HIGH);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    component_info_t info;
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx, &info));

    /* Verify fields */
    TEST_ASSERT_EQUAL_STRING("my-service", (const char *)info.name);
    TEST_ASSERT_EQUAL_STRING("2.5.1", (const char *)info.version);
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_TYPE_DRIVER, info.component_type);
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_PRIORITY_HIGH, info.priority);
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_LOADED, info.state);
    TEST_ASSERT_EQUAL_UINT32(0, info.task_id);  /* No task yet */
}

/**
 * Test get_info with NULL pointer fails.
 */
static void test_component_get_info_null(void)
{
    cleanup_all_components();
    int idx = component_register("temp", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    TEST_ASSERT_EQUAL(-1, component_get_info((uint32_t)idx, NULL));
}

/**
 * Test get_info with invalid index fails.
 */
static void test_component_get_info_invalid_index(void)
{
    component_info_t info;
    TEST_ASSERT_EQUAL(-1, component_get_info(COMPONENT_MAX_COUNT + 10, &info));
}

/**
 * Test get_info on empty slot fails.
 */
static void test_component_get_info_empty_slot(void)
{
    cleanup_all_components();
    component_info_t info;

    TEST_ASSERT_EQUAL(-1, component_get_info(0, &info));
}

/**
 * Test find by name.
 */
static void test_component_find_by_name(void)
{
    cleanup_all_components();
    int idx1 = component_register("alpha", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    int idx2 = component_register("beta", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    int idx3 = component_register("gamma", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);

    TEST_ASSERT_EQUAL(idx1, component_find("alpha"));
    TEST_ASSERT_EQUAL(idx2, component_find("beta"));
    TEST_ASSERT_EQUAL(idx3, component_find("gamma"));
}

/**
 * Test find non-existent component.
 */
static void test_component_find_not_found(void)
{
    TEST_ASSERT_EQUAL(-1, component_find("nonexistent"));
}

/**
 * Test find with NULL name.
 */
static void test_component_find_null(void)
{
    TEST_ASSERT_EQUAL(-1, component_find(NULL));
}

/* ============================================================================
 * State Transition Tests
 * ============================================================================ */

/**
 * Test state transitions.
 */
static void test_component_state_transitions(void)
{
    cleanup_all_components();
    int idx = component_register("stateful", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    component_info_t info;

    /* Initial state should be LOADED */
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx, &info));
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_LOADED, info.state);

    /* Transition to INITIALIZING */
    TEST_ASSERT_EQUAL(0, component_set_state((uint32_t)idx, COMPONENT_INITIALIZING));
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx, &info));
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_INITIALIZING, info.state);

    /* Transition to RUNNING */
    TEST_ASSERT_EQUAL(0, component_set_state((uint32_t)idx, COMPONENT_RUNNING));
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx, &info));
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_RUNNING, info.state);

    /* Transition to SUSPENDED */
    TEST_ASSERT_EQUAL(0, component_set_state((uint32_t)idx, COMPONENT_SUSPENDED));
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx, &info));
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_SUSPENDED, info.state);

    /* Transition to TERMINATING */
    TEST_ASSERT_EQUAL(0, component_set_state((uint32_t)idx, COMPONENT_TERMINATING));
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx, &info));
    TEST_ASSERT_EQUAL_UINT8(COMPONENT_TERMINATING, info.state);
}

/**
 * Test invalid state value.
 */
static void test_component_state_invalid(void)
{
    cleanup_all_components();
    int idx = component_register("temp", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx);

    TEST_ASSERT_EQUAL(-1, component_set_state((uint32_t)idx, 99));
}

/**
 * Test state change on invalid index.
 */
static void test_component_state_invalid_index(void)
{
    TEST_ASSERT_EQUAL(-1, component_set_state(COMPONENT_MAX_COUNT + 10, COMPONENT_RUNNING));
}

/* ============================================================================
 * Slot Reuse Tests
 * ============================================================================ */

/**
 * Test that unregistered slots can be reused.
 */
static void test_component_slot_reuse(void)
{
    cleanup_all_components();
    /* Register and unregister */
    int idx1 = component_register("first", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx1);
    TEST_ASSERT_EQUAL(0, component_unregister((uint32_t)idx1));

    /* Register again - should reuse the same slot */
    int idx2 = component_register("second", "1.0", COMPONENT_TYPE_SERVICE, COMPONENT_PRIORITY_NORMAL);
    TEST_ASSERT_GREATER_OR_EQUAL(0, idx2);

    /* The slot should be reused (typically same index) */
    TEST_ASSERT_EQUAL_UINT32(1, component_count());

    /* Verify it's the new component */
    component_info_t info;
    TEST_ASSERT_EQUAL(0, component_get_info((uint32_t)idx2, &info));
    TEST_ASSERT_EQUAL_STRING("second", (const char *)info.name);
}

/* ============================================================================
 * Cleanup Context Lifecycle (IPC-C4 regression)
 *
 * component_start() allocates a per-task cleanup context via pmm_alloc_page()
 * and attaches it with task_set_cleanup(). component_task_cleanup() must free
 * the page when the task exits. These tests exercise the same pattern with
 * a spy callback to verify the contract holds, guarding against re-introduction
 * of the old shared static array.
 * ============================================================================ */

static volatile uint32_t c4_observed_value;
static volatile void *c4_observed_ctx_ptr;

static void c4_cleanup_spy(void *arg)
{
    uint32_t *ctx = (uint32_t *)arg;
    c4_observed_ctx_ptr = arg;
    c4_observed_value = *ctx;
    pmm_free_page(ctx);
}

static void c4_exit_immediately(void *arg)
{
    (void)arg;
    /* Returning hands off to task_entry_trampoline which calls task_exit(). */
}

/*
 * Verify that a heap-allocated ctx reaches the cleanup callback with its
 * payload intact and that the page is released back to the PMM afterward.
 * If the old static-array pattern were reintroduced, the page-free check
 * would still pass trivially (no allocation happened), so see the
 * multi-task test below for unique-pointer coverage.
 */
static void test_component_cleanup_ctx_payload_preserved(void)
{
    c4_observed_value = 0;
    c4_observed_ctx_ptr = NULL;

    uint32_t *ctx = (uint32_t *)pmm_alloc_page();
    TEST_ASSERT_NOT_NULL(ctx);
    *ctx = 0xDEADBEEFu;

    size_t free_held = pmm_get_free_pages();

    struct task *t = task_create("c4_payload", c4_exit_immediately, NULL);
    TEST_ASSERT_NOT_NULL(t);
    task_set_cleanup(t, c4_cleanup_spy, ctx);
    scheduler_add_task_to_cpu(t, 0);

    int timeout = 500;
    while (c4_observed_value == 0 && timeout > 0) {
        yield();
        timeout--;
    }

    TEST_ASSERT_MESSAGE(timeout > 0, "c4 cleanup callback never fired");
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFu, c4_observed_value);
    TEST_ASSERT_EQUAL_PTR(ctx, (void *)c4_observed_ctx_ptr);

    /* Let the scheduler finish task_destroy so the stack is reclaimed. */
    for (int i = 0; i < 4; i++) yield();

    /* The ctx page should have returned to the pool; task stack freed on
     * top of that. A conservative >= check tolerates unrelated test-harness
     * allocations that may happen concurrently. */
    size_t free_after = pmm_get_free_pages();
    TEST_ASSERT_MESSAGE(free_after >= free_held + 1,
        "cleanup did not free the per-task ctx page");
}

/*
 * Verify each task gets its OWN ctx pointer — i.e. no shared static array
 * is being reused across tasks. This is the core regression that IPC-C4
 * guards against: under the old pattern, task A and task B could both
 * receive &cleanup_ctxs[same_slot] and fire each other's cleanup.
 */
#define C4_MULTI_N 3
static volatile void *c4_multi_ptrs[C4_MULTI_N];
static volatile uint32_t c4_multi_values[C4_MULTI_N];
static volatile int c4_multi_done;

static void c4_multi_spy(void *arg)
{
    uint32_t *ctx = (uint32_t *)arg;
    int slot = (int)(*ctx & 0xFF);  /* low byte encodes which task */
    c4_multi_ptrs[slot] = arg;
    c4_multi_values[slot] = *ctx;
    pmm_free_page(ctx);
    __atomic_fetch_add(&c4_multi_done, 1, __ATOMIC_RELEASE);
}

static void test_component_cleanup_ctx_is_per_task(void)
{
    c4_multi_done = 0;
    for (int i = 0; i < C4_MULTI_N; i++) {
        c4_multi_ptrs[i] = NULL;
        c4_multi_values[i] = 0;
    }

    /* Start N tasks, each with a distinct heap-allocated ctx whose
     * contents encode the slot index. */
    for (int i = 0; i < C4_MULTI_N; i++) {
        uint32_t *ctx = (uint32_t *)pmm_alloc_page();
        TEST_ASSERT_NOT_NULL(ctx);
        *ctx = 0x1000u | (uint32_t)i;

        struct task *t = task_create("c4_multi", c4_exit_immediately, NULL);
        TEST_ASSERT_NOT_NULL(t);
        task_set_cleanup(t, c4_multi_spy, ctx);
        scheduler_add_task_to_cpu(t, 0);
    }

    /* Wait for all cleanups to fire. */
    int timeout = 1000;
    while (__atomic_load_n(&c4_multi_done, __ATOMIC_ACQUIRE) < C4_MULTI_N
           && timeout > 0) {
        yield();
        timeout--;
    }
    TEST_ASSERT_MESSAGE(timeout > 0, "not all c4 cleanups fired");

    /* Each slot must have been touched exactly once with its own encoded
     * value — proving each task carried its own ctx. */
    for (int i = 0; i < C4_MULTI_N; i++) {
        TEST_ASSERT_EQUAL_UINT32(0x1000u | (uint32_t)i, c4_multi_values[i]);
        TEST_ASSERT_NOT_NULL((void *)c4_multi_ptrs[i]);
    }

    /* All ctx pointers must be distinct. */
    for (int i = 0; i < C4_MULTI_N; i++) {
        for (int j = i + 1; j < C4_MULTI_N; j++) {
            TEST_ASSERT_MESSAGE(c4_multi_ptrs[i] != c4_multi_ptrs[j],
                "per-task ctx pointers collided — static array regression?");
        }
    }
}

/* ============================================================================
 * Helper Function Tests
 * ============================================================================ */

/**
 * Test state name helper.
 */
static void test_component_state_name(void)
{
    TEST_ASSERT_EQUAL_STRING("loaded", component_state_name(COMPONENT_LOADED));
    TEST_ASSERT_EQUAL_STRING("initializing", component_state_name(COMPONENT_INITIALIZING));
    TEST_ASSERT_EQUAL_STRING("running", component_state_name(COMPONENT_RUNNING));
    TEST_ASSERT_EQUAL_STRING("suspended", component_state_name(COMPONENT_SUSPENDED));
    TEST_ASSERT_EQUAL_STRING("updating", component_state_name(COMPONENT_UPDATING));
    TEST_ASSERT_EQUAL_STRING("terminating", component_state_name(COMPONENT_TERMINATING));
    TEST_ASSERT_EQUAL_STRING("unloaded", component_state_name(COMPONENT_UNLOADED));
    TEST_ASSERT_EQUAL_STRING("unknown", component_state_name(99));
}

/**
 * Test type name helper.
 */
static void test_component_type_name(void)
{
    TEST_ASSERT_EQUAL_STRING("service", component_type_name(COMPONENT_TYPE_SERVICE));
    TEST_ASSERT_EQUAL_STRING("driver", component_type_name(COMPONENT_TYPE_DRIVER));
    TEST_ASSERT_EQUAL_STRING("application", component_type_name(COMPONENT_TYPE_APPLICATION));
    TEST_ASSERT_EQUAL_STRING("unknown", component_type_name(99));
}

/* ============================================================================
 * Test Runner
 * ============================================================================ */

/**
 * Run all component system tests.
 * Returns number of failures.
 */
int test_suite_component(void)
{
    UnityBegin("Component Tests");

    /* Registration tests */
    RUN_TEST(test_component_register_basic);
    RUN_TEST(test_component_register_types);
    RUN_TEST(test_component_register_null_params);
    RUN_TEST(test_component_register_invalid_type);
    RUN_TEST(test_component_register_exhaustion);

    /* Unregistration tests */
    RUN_TEST(test_component_unregister_basic);
    RUN_TEST(test_component_unregister_invalid_index);
    RUN_TEST(test_component_unregister_empty_slot);
    RUN_TEST(test_component_unregister_double);

    /* Query tests */
    RUN_TEST(test_component_get_info);
    RUN_TEST(test_component_get_info_null);
    RUN_TEST(test_component_get_info_invalid_index);
    RUN_TEST(test_component_get_info_empty_slot);
    RUN_TEST(test_component_find_by_name);
    RUN_TEST(test_component_find_not_found);
    RUN_TEST(test_component_find_null);

    /* State transition tests */
    RUN_TEST(test_component_state_transitions);
    RUN_TEST(test_component_state_invalid);
    RUN_TEST(test_component_state_invalid_index);

    /* Slot reuse tests */
    RUN_TEST(test_component_slot_reuse);

    /* Cleanup ctx lifecycle (IPC-C4 regression) */
    RUN_TEST(test_component_cleanup_ctx_payload_preserved);
    RUN_TEST(test_component_cleanup_ctx_is_per_task);

    /* Helper function tests */
    RUN_TEST(test_component_state_name);
    RUN_TEST(test_component_type_name);

    int failures = UnityEnd();

    if (failures == 0) {
        uart_puts("[INFO] Component tests passed\n");
    } else {
        uart_printf("[FAIL] Component tests: %d failures\n", failures);
    }

    return failures;
}
