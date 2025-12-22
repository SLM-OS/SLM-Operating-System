/*
 * IPC Tests
 *
 * TODO: Migrate existing IPC tests from main.c to use Unity framework.
 * For now, this is a stub that calls the existing ipc_run_tests().
 */

#include "unity.h"
#include "../include/ipc.h"

/* Existing test function in ipc.c */
extern int ipc_run_tests(void);

int test_suite_ipc(void)
{
    UnityBegin("IPC Tests (legacy)");

    /* Call existing tests - they print their own output */
    int failures = ipc_run_tests();

    /* Report summary */
    Unity.test_count = 1;  /* Treat as single test for now */
    Unity.test_failures = failures > 0 ? 1 : 0;

    return UnityEnd();
}
