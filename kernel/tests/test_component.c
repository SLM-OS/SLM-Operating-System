/**
 * @file test_component.c
 * @brief Component System Tests
 *
 * Validates component registration, lifecycle management, and FFI boundary.
 */

#include "unity.h"
#include "component.h"
#include "uart.h"
#include <stdint.h>

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
