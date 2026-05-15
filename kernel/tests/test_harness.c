/*
 * Test Harness for SLM-OS
 *
 * Implementation of test harness - provides Unity output via UART.
 */

#include "test_harness.h"
#include "../include/platform.h"
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

#if defined(PLATFORM_X86_64)
    /* Production builds wire `pci` and `gpu` shell commands from
     * shell_init() inside shell_task_entry. The test build never
     * spawns the shell task — it runs the harness from main_task and
     * exits via semihosting — so any test that asserts on these
     * commands' presence (e.g. test_nvidia_gpu_shell_command_registered,
     * test_x86_gpu_cmd_in_externals) needs the registrations to fire
     * here. Replicating the shell_init platform block is the smallest
     * change that lets both code paths share the same registration
     * function. */
    {
        extern void pci_register_shell_commands(void);
        extern void nvidia_gpu_register_shell_commands(void);
        pci_register_shell_commands();
        nvidia_gpu_register_shell_commands();
    }
#endif
}

int test_harness_run_all(void)
{
    int total_failures = 0;

    /* Run model_mem smoke test FIRST so an init regression (e.g.
     * silent PMM oversubscribe on a tight-RAM platform) surfaces
     * before downstream tests have a chance to silently no-op or
     * panic on the uninitialized allocator. Fast, side-effect free. */
    total_failures += test_suite_model_mem_smoke();

    /* Run each test suite */
    int ipc_failures = test_suite_ipc();
    total_failures += ipc_failures;
    if (ipc_failures == 0) {
        uart_puts("[INFO] IPC tests passed\n");
    }

    total_failures += test_suite_pi_mutex();
    total_failures += test_suite_steal_deque();
    total_failures += test_suite_sched_trace();
    total_failures += test_suite_latency_hist();
    total_failures += test_suite_gpu_consumer();
    total_failures += test_suite_gpu_tier();
    total_failures += test_suite_operator_library();
    total_failures += test_suite_oplib_pool();
    total_failures += test_suite_oplib_weights_pool();
    total_failures += test_suite_operator_dispatch();
    total_failures += test_suite_oplib_probe();
    total_failures += test_suite_gpu_dispatch_breaker();
    total_failures += test_suite_admin_telemetry();
    total_failures += test_suite_telemetry_feed();
    total_failures += test_suite_model_engine();
#if !defined(PLATFORM_X86_64)
    total_failures += test_suite_msg_router();
    total_failures += test_suite_coop_preempt();
    total_failures += test_suite_cpu_supervisor();
    total_failures += test_suite_mpidr_lookup();
#endif
    total_failures += test_suite_gpu();
    total_failures += test_suite_vfs();
    total_failures += test_suite_shell();
    total_failures += test_suite_shell_session();
    total_failures += test_suite_shell_history();
    total_failures += test_suite_blob_autoload();
    total_failures += test_suite_boot_media();
#if defined(ENABLE_NETWORKING)
    total_failures += test_suite_telnet();
    total_failures += test_suite_telnetd_config();
    total_failures += test_suite_telnetd_cmd();
    total_failures += test_suite_tcp_telemetry_server();
#endif
    total_failures += test_suite_pmm();
#if !defined(PLATFORM_X86_64)
    {
        extern int test_suite_kbuf(void);
        total_failures += test_suite_kbuf();
    }
    total_failures += test_suite_pcie();
    total_failures += test_suite_bpmp();
    total_failures += test_suite_inference_device();
    total_failures += test_suite_hailo();
    total_failures += test_suite_hailo_trace();
    total_failures += test_suite_hailo_replay();
    total_failures += test_suite_hef();
    total_failures += test_suite_hef_parser();
#endif
    total_failures += test_suite_littlefs();
    total_failures += test_suite_x86_boot();
    total_failures += test_suite_elf();
    /* Platform-neutral protocol-layout tests (no MMIO). */
    total_failures += test_suite_bcm_mailbox();
    /* Camera C-API + Tegra234 clock-id static_assert pins (#396). */
    total_failures += test_suite_camera();
    /* FatFs / FAT32 integration (uses ramdisk + PMM). */
    total_failures += test_suite_fat32();
#if !defined(PLATFORM_X86_64)
    /* SDHCI driver against QEMU sdhci-pci (skips cleanly if absent). */
    total_failures += test_suite_sdhci();
    /* SHA-256 vector tests (vendored library, no platform deps). */
    total_failures += test_suite_sha256();
    /* `kernel` admin command surface (also relies on sdhci-pci). */
    total_failures += test_suite_kernel_cmd();
    total_failures += test_suite_dtb();
    total_failures += test_suite_fdt();
    total_failures += test_suite_model_mem();
    total_failures += test_suite_eviction();
    total_failures += test_suite_slm_load();
    total_failures += test_suite_slm_shell();
    total_failures += test_suite_slm_runner();
    total_failures += test_suite_slm_dispatch_stats();
    total_failures += test_suite_scheduler();
    total_failures += test_suite_component();
    total_failures += test_suite_vmm();
    total_failures += test_suite_lua();
    /* libc-stub regression: strtol/strtod chain rewrite in lua_stubs.c. */
    total_failures += test_suite_lua_stubs();
#endif
    /* Networking tests run on all platforms with ENABLE_NETWORKING */
#if defined(ENABLE_NETWORKING)
    total_failures += test_suite_net();
    total_failures += test_suite_net_http();
#endif

    /* USB core tests (platform-neutral; mock HCD only). */
    total_failures += test_suite_usb_core();

    /* CDC-ECM class driver tests (depend on net_driver, so ENABLE_NETWORKING gated). */
#if defined(ENABLE_NETWORKING)
    total_failures += test_suite_cdc_ecm();
#endif

    /* XHCI ring primitive tests (platform-neutral; mothballed Phase 3A code). */
    total_failures += test_suite_xhci_ring();

    /* Tegra234 XHCI wrapper offset + CSB-paging tests (Phase 3A.2). */
    total_failures += test_suite_xhci_tegra();

    /* PORTSC decode + context layout (Phase 3A Step 5). */
    total_failures += test_suite_xhci_device();

    /* TRB builders for control / bulk (Phase 3A Step 6 + 7b). */
    total_failures += test_suite_xhci_xfer();

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

    /* Model hot-swap tests (registry::swap_model + rust_model_swap) */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("Model Hot-Swap Tests (#232)\n");
    uart_puts("========================================\n");
    total_failures += test_suite_model_swap();

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
