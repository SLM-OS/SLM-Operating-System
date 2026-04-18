/*
 * Test Harness for SLM-OS
 *
 * Implementation of test harness - provides Unity output via UART.
 */

#include "test_harness.h"
#include "../include/uart.h"
#include "../include/semihosting.h"
#include "../include/slm_ffi.h"

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

    total_failures += test_suite_pi_mutex();
    total_failures += test_suite_steal_deque();
    total_failures += test_suite_sched_trace();
#if !defined(PLATFORM_X86_64)
    total_failures += test_suite_msg_router();
    total_failures += test_suite_coop_preempt();
#endif
    total_failures += test_suite_gpu();
    total_failures += test_suite_vfs();
    total_failures += test_suite_shell();
    total_failures += test_suite_shell_session();
#if defined(ENABLE_NETWORKING)
    total_failures += test_suite_telnet();
    total_failures += test_suite_telnetd_config();
#endif
    total_failures += test_suite_pmm();
#if !defined(PLATFORM_X86_64)
    total_failures += test_suite_pcie();
    total_failures += test_suite_inference_device();
    total_failures += test_suite_hailo();
    total_failures += test_suite_hef();
#endif
    total_failures += test_suite_littlefs();
    total_failures += test_suite_x86_boot();
    total_failures += test_suite_elf();
#if !defined(PLATFORM_X86_64)
    total_failures += test_suite_dtb();
    total_failures += test_suite_fdt();
    total_failures += test_suite_model_mem();
    total_failures += test_suite_eviction();
    total_failures += test_suite_scheduler();
    total_failures += test_suite_component();
    total_failures += test_suite_vmm();
    total_failures += test_suite_lua();
#endif
    /* Networking tests run on all platforms with ENABLE_NETWORKING */
#if defined(ENABLE_NETWORKING)
    total_failures += test_suite_net();
#endif

#if !defined(PLATFORM_X86_64)
    /* Phase 5 test suites (ARM64 only — use Rust runtime + ARM assembly) */

    /* Model loader tests (Rust ONNX parser + registry) */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Model Loader Tests\n");
    uart_puts("========================================\n");
    total_failures += test_suite_model_loader();

    /* Inference engine tests (tensor ops + end-to-end MNIST) */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Inference Engine Tests\n");
    uart_puts("========================================\n");
    total_failures += test_suite_inference();

    /* GPU compute integration tests */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("GPU Compute Tests\n");
    uart_puts("========================================\n");
    total_failures += test_suite_gpu_compute();

    /* Component integration tests (M5) */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Component Integration Tests\n");
    uart_puts("========================================\n");
    total_failures += test_suite_components_m5();

    /* Syscall infrastructure tests */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Syscall Infrastructure Tests\n");
    uart_puts("========================================\n");
    total_failures += test_suite_syscall();
#endif /* !PLATFORM_X86_64 */

    /* Rust FFI tests */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Rust FFI Tests\n");
    uart_puts("========================================\n");
    total_failures += rust_run_tests();

    /*
     * Integration tests run actual tasks across CPUs.
     * These must run after all unit tests because they exercise
     * the full scheduler infrastructure.
     */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Integration Tests (Multi-Core)\n");
    uart_puts("========================================\n");
#if !defined(PLATFORM_X86_64)
    total_failures += test_suite_integration();
#endif

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

    /*
     * When running under QEMU with semihosting, always exit after tests.
     * This ensures proper exit codes for CI regardless of pass/fail.
     */
    if (semihosting_available()) {
        int exit_code = (total_failures > 0) ? 1 : 0;
        uart_puts("[INFO] Exiting via semihosting...\n");
        semihosting_exit(exit_code);
    }

    return total_failures;
}
