/*
 * Test Harness for SLM-OS
 *
 * Implementation of test harness - provides Unity output via UART.
 */

#include "test_harness.h"
#include "../include/uart.h"

/* ============================================================================
 * Unity Output Functions
 * ============================================================================ */

void unity_output_char(char c)
{
    uart_putc(c);
}

void unity_output_string(const char *s)
{
    uart_puts(s);
}

void unity_output_number(int64_t n)
{
    char buf[24];
    int i = 0;
    int negative = 0;

    if (n < 0) {
        negative = 1;
        n = -n;
    }

    if (n == 0) {
        uart_putc('0');
        return;
    }

    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }

    if (negative) {
        uart_putc('-');
    }

    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

void unity_output_hex(uint64_t n)
{
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int i = 0;

    if (n == 0) {
        uart_putc('0');
        return;
    }

    while (n > 0) {
        buf[i++] = hex[n & 0xf];
        n >>= 4;
    }

    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

/* ============================================================================
 * Test Harness
 * ============================================================================ */

void test_harness_init(void)
{
    uart_puts("\n");
    uart_puts("########################################\n");
    uart_puts("#    SLM-OS Test Suite (Unity)        #\n");
    uart_puts("########################################\n");
    uart_puts("\n");
}

int test_harness_run_all(void)
{
    int total_failures = 0;

    /* Run each test suite */
    int ipc_failures = test_suite_ipc();
    total_failures += ipc_failures;
    if (ipc_failures == 0) {
        uart_puts("[INFO] IPC tests passed\n");
    }

    total_failures += test_suite_model_mem();
    total_failures += test_suite_scheduler();
    total_failures += test_suite_pi_mutex();
    total_failures += test_suite_gpu();

    /* Final summary */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Test Suite Summary\n");
    uart_puts("========================================\n");

    if (total_failures == 0) {
        uart_puts("[PASS] All test suites passed!\n");
    } else {
        uart_puts("[FAIL] ");
        unity_output_number(total_failures);
        uart_puts(" total failure(s)\n");
    }

    return total_failures;
}
