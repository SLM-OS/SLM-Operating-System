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
#include "../include/timer.h"
#include "../include/shell.h"
#if !defined(PLATFORM_X86_64)
#include "../include/dtb.h"
#endif
#include "test_filter.h"
#include <stdbool.h>

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
    /* Production builds wire `pci`, `gpu` and the Lua commands from
     * shell_init() inside shell_task_entry. The test build does spawn
     * the shell task, but at TASK_PRIORITY_IDLE on CPU 0 it never gets
     * scheduled while the harness runs from main_task (no `slmos>`
     * prompt ever appears in the x86 test log), so shell_init() never
     * runs. Any test that asserts on these commands' presence (e.g.
     * test_nvidia_gpu_shell_command_registered,
     * test_x86_gpu_cmd_in_externals,
     * test_lua_shell_commands_registered_with_expected_mutation_modes,
     * the test_shell_session `lua -e` tests) needs the registrations
     * to fire here. Replicating the shell_init calls is the smallest
     * change that lets both code paths share the same registration
     * functions. */
    {
        extern void pci_register_shell_commands(void);
        extern void nvidia_gpu_register_shell_commands(void);
        extern void lua_shell_init(void);
        pci_register_shell_commands();
        nvidia_gpu_register_shell_commands();
        lua_shell_init();
    }
#endif
}

static int test_harness_finish(int total_failures);

/* Enter a suite. Unfiltered (the default), every suite runs, exactly as
 * before the filter existed. Filtered, only the suites that the generated
 * index (scripts/gen-test-index.py) lists for a named test run, unless a
 * name is missing from the index, in which case every suite is entered
 * and RUN_TEST skips the unnamed tests. */
#define RUN_SUITE(fn) \
    do { \
        if (harness_suite_selected(#fn)) { \
            total_failures += fn(); \
        } \
    } while (0)

/* Section banners printed between suites; omitted from filtered runs. */
static void harness_banner(const char *title)
{
    if (test_filter_active()) {
        return;
    }
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts(title);
    uart_puts("\n");
    uart_puts("========================================\n");
}

static bool harness_walk_all;   /* a filtered name is not in the index */

static bool streq(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static bool harness_suite_selected(const char *suite)
{
    if (!test_filter_active() || harness_walk_all) {
        return true;
    }
    for (uint32_t i = 0; i < g_test_filter.count; i++) {
        uint32_t cursor = 0;
        const char *s;
        while ((s = test_index_next_suite(g_test_filter.names[i], &cursor)) != NULL) {
            if (streq(s, suite)) {
                return true;
            }
        }
    }
    return false;
}

static int harness_run_suites(void)
{
    int total_failures = 0;

#if defined(BOOT_TESTS_HW_SET) && !defined(PLATFORM_X86_64)
    /* Hardware measurement set (#1193): only the coherency litmus suite
     * and the multi-core integration tests, so a board can run them
     * without the rest of the suite (whose known hardware failures,
     * #1109 / #1199, can halt the run before it gets here). */
    uart_puts("[INFO] BOOT_TESTS_HW_SET: coherency + integration suites only\n");
    RUN_SUITE(test_suite_coherency);
    harness_banner("Integration Tests (Multi-Core)");
    RUN_SUITE(test_suite_integration);
    return total_failures;
#endif

    /* Run model_mem smoke test FIRST so an init regression (e.g.
     * silent PMM oversubscribe on a tight-RAM platform) surfaces
     * before downstream tests have a chance to silently no-op or
     * panic on the uninitialized allocator. Fast, side-effect free. */
    RUN_SUITE(test_suite_model_mem_smoke);

    /* Run each test suite */
    if (harness_suite_selected("test_suite_ipc")) {
        int ipc_failures = test_suite_ipc();
        total_failures += ipc_failures;
        if (ipc_failures == 0 && !test_filter_active()) {
            uart_puts("[INFO] IPC tests passed\n");
        }
    }

    RUN_SUITE(test_suite_pi_mutex);
    RUN_SUITE(test_suite_steal_deque);
    RUN_SUITE(test_suite_steal_shares);
    RUN_SUITE(test_suite_test_filter);
    RUN_SUITE(test_suite_fault_dump);
    RUN_SUITE(test_suite_sched_trace);
    RUN_SUITE(test_suite_latency_hist);
    RUN_SUITE(test_suite_gpu_consumer);
    RUN_SUITE(test_suite_gpu_tier);
    RUN_SUITE(test_suite_operator_library);
    RUN_SUITE(test_suite_oplib_pool);
    RUN_SUITE(test_suite_oplib_weights_pool);
    RUN_SUITE(test_suite_operator_dispatch);
    RUN_SUITE(test_suite_oplib_probe);
    RUN_SUITE(test_suite_gpu_dispatch_breaker);
    RUN_SUITE(test_suite_admin_telemetry);
    RUN_SUITE(test_suite_telemetry_feed);
    RUN_SUITE(test_suite_model_engine);
#if !defined(PLATFORM_X86_64)
    RUN_SUITE(test_suite_msg_router);
    RUN_SUITE(test_suite_gic);
    RUN_SUITE(test_suite_coop_preempt);
    RUN_SUITE(test_suite_cpu_supervisor);
    RUN_SUITE(test_suite_mpidr_lookup);
    RUN_SUITE(test_suite_pmu);
#endif
#if !defined(PLATFORM_X86_64) && !defined(PLATFORM_RASPI5)
    /* UEFI-direct relocation decision (#26); efi_stub.c is built on
     * every ARM64 platform with a PE header (not Pi 5). */
    RUN_SUITE(test_suite_efi_reloc);
    RUN_SUITE(test_suite_boot_models);
#endif
    RUN_SUITE(test_suite_tegra_cpufreq);
    RUN_SUITE(test_suite_tegra_fan);
    RUN_SUITE(test_suite_emc_hold);
    /* GA10B ACR LS blob builders (#1088): pure, every platform. */
    RUN_SUITE(test_suite_ga10b_netlist);
    RUN_SUITE(test_suite_ga10b_ctxsw_ucode);
    RUN_SUITE(test_suite_ga10b_acr_blob);
    RUN_SUITE(test_suite_ga10b_pmu);
    RUN_SUITE(test_suite_ga10b_chan);
    RUN_SUITE(test_suite_gpu_chan_state);
    RUN_SUITE(test_suite_ga10b_grctx);
    RUN_SUITE(test_suite_ga10b_cold);
    RUN_SUITE(test_suite_ga10b_mnist_pipeline);
    RUN_SUITE(test_suite_ga10b_cold_pipelines); /* #1092 sched/eviction */
    RUN_SUITE(test_suite_oplib_gpu_as);   /* #1092 cold-channel oplib */
    RUN_SUITE(test_suite_gpu);
    RUN_SUITE(test_suite_vfs);
    RUN_SUITE(test_suite_shell);
    RUN_SUITE(test_suite_shell_session);
    RUN_SUITE(test_suite_shell_history);
    RUN_SUITE(test_suite_blob_autoload);
    RUN_SUITE(test_suite_boot_media);
#if defined(ENABLE_NETWORKING)
    RUN_SUITE(test_suite_telnet);
    RUN_SUITE(test_suite_telnetd_config);
    RUN_SUITE(test_suite_telnetd_cmd);
    RUN_SUITE(test_suite_tcp_telemetry_server);
#endif
    RUN_SUITE(test_suite_pmm);
#if !defined(PLATFORM_X86_64)
    {
        extern int test_suite_kbuf(void);
        RUN_SUITE(test_suite_kbuf);
    }
    RUN_SUITE(test_suite_pcie);
    RUN_SUITE(test_suite_bpmp);
    RUN_SUITE(test_suite_inference_device);
    RUN_SUITE(test_suite_hailo);
    RUN_SUITE(test_suite_hailo_trace);
    RUN_SUITE(test_suite_hailo_replay);
    RUN_SUITE(test_suite_hef);
    RUN_SUITE(test_suite_hef_parser);
#endif
    RUN_SUITE(test_suite_littlefs);
    RUN_SUITE(test_suite_x86_boot);
    RUN_SUITE(test_suite_elf);
    /* Platform-neutral protocol-layout tests (no MMIO). */
    RUN_SUITE(test_suite_bcm_mailbox);
    /* Camera C-API + Tegra234 clock-id static_assert pins (#396). */
    RUN_SUITE(test_suite_camera);
    /* FatFs / FAT32 integration (uses ramdisk + PMM). */
    RUN_SUITE(test_suite_fat32);
#if !defined(PLATFORM_X86_64)
    /* SDHCI driver against QEMU sdhci-pci (skips cleanly if absent). */
    RUN_SUITE(test_suite_sdhci);
    /* SHA-256 vector tests (vendored library, no platform deps). */
    RUN_SUITE(test_suite_sha256);
    /* Crypto-quality RNG (jitter pool + injection coverage). #199a / #893. */
    RUN_SUITE(test_suite_rng);
#if defined(NET_SSHD)
    /* SSH daemon — public API + per-conn ring (#199a / #893). */
    RUN_SUITE(test_suite_sshd);
    /* SSH host-key persistence (#199b / #892). The TU only compiles
     * when NET_SSHD is on, so it stays inside the same gate. */
    RUN_SUITE(test_suite_host_key);
    /* Password DB + scrypt verify (#199d / #896). */
    RUN_SUITE(test_suite_passwd);
    /* Dedicated wolfssl heap (wolf_heap.c) — alloc/free/realloc paths. */
    RUN_SUITE(test_suite_wolf_heap);
    /* /etc/sshd.conf parser (sshd_autostart.c). */
    RUN_SUITE(test_suite_sshd_autostart);
#endif
    /* `kernel` admin command surface (also relies on sdhci-pci). */
    RUN_SUITE(test_suite_kernel_cmd);
    RUN_SUITE(test_suite_dtb);
    RUN_SUITE(test_suite_fdt);
    RUN_SUITE(test_suite_model_mem);
    RUN_SUITE(test_suite_eviction);
    RUN_SUITE(test_suite_slm_load);
    RUN_SUITE(test_suite_slm_shell);
    RUN_SUITE(test_suite_slm_runner);
    /* Dynamic-batching shell verbs (#55 / PR #917): `infer batch ...`
     * + `bench infer-stress` argument validation. Rust side is
     * covered by rust_batch_inference_test. */
    RUN_SUITE(test_suite_infer_shell);
    RUN_SUITE(test_suite_scheduler);
    RUN_SUITE(test_suite_component);
    RUN_SUITE(test_suite_vmm);
    /* Coherency litmus + negative controls (#1193): runs pinned tasks on
     * every CPU, so it sits with the other scheduler-backed suites. */
    RUN_SUITE(test_suite_coherency);
    /* Run queues / task table / current_task stay cacheable (#1193). */
    RUN_SUITE(test_suite_nc_axis);
    RUN_SUITE(test_suite_lua);
    /* libc-stub regression: strtol/strtod chain rewrite in lua_stubs.c. */
    RUN_SUITE(test_suite_lua_stubs);
#endif
    /* SLM GPU dispatch counter FFI tests — outside the !X86_64 gate
     * because the suite calls only `slm_runtime_dispatch_stats` /
     * `_reset`, which are defined on every platform (Jetson real
     * impl + non-Jetson stub in slm_ffi.c). No Rust deps. */
    RUN_SUITE(test_suite_slm_dispatch_stats);
#if defined(ENABLE_BOOT_TESTS)
    /* Boot-test kernels only: the suite embeds about 1.1 MB of fixtures. */
    RUN_SUITE(test_suite_slm_golden);
    RUN_SUITE(test_suite_slm_prog);
#endif
    /* Networking tests run on all platforms with ENABLE_NETWORKING */
#if defined(ENABLE_NETWORKING)
    RUN_SUITE(test_suite_net);
    RUN_SUITE(test_suite_net_http);
#endif

    /* USB core tests (platform-neutral; mock HCD only). */
    RUN_SUITE(test_suite_usb_core);

    /* CDC-ECM class driver tests (depend on net_driver, so ENABLE_NETWORKING gated). */
#if defined(ENABLE_NETWORKING)
    RUN_SUITE(test_suite_cdc_ecm);
#endif

    /* XHCI ring primitive tests (platform-neutral; mothballed Phase 3A code). */
    RUN_SUITE(test_suite_xhci_ring);

    /* Tegra234 XHCI wrapper offset + CSB-paging tests (Phase 3A.2). */
    RUN_SUITE(test_suite_xhci_tegra);

    /* PORTSC decode + context layout (Phase 3A Step 5). */
    RUN_SUITE(test_suite_xhci_device);

    /* TRB builders for control / bulk (Phase 3A Step 6 + 7b). */
    RUN_SUITE(test_suite_xhci_xfer);

#if !defined(PLATFORM_X86_64)
    /* Phase 5 test suites (ARM64 only — use Rust runtime + ARM assembly) */

    /* Model loader tests (Rust ONNX parser + registry) */
    harness_banner("Model Loader Tests");
    RUN_SUITE(test_suite_model_loader);

    /* Inference engine tests (tensor ops + end-to-end MNIST) */
    harness_banner("Inference Engine Tests");
    RUN_SUITE(test_suite_inference);

    /* Model hot-swap tests (registry::swap_model + rust_model_swap) */
    harness_banner("Model Hot-Swap Tests (#232)");
    RUN_SUITE(test_suite_model_swap);

    /* GPU compute integration tests */
    harness_banner("GPU Compute Tests");
    RUN_SUITE(test_suite_gpu_compute);

    /* Component integration tests (M5) */
    harness_banner("Component Integration Tests");
    RUN_SUITE(test_suite_components_m5);

    /* Syscall infrastructure tests */
    harness_banner("Syscall Infrastructure Tests");
    RUN_SUITE(test_suite_syscall);
#endif /* !PLATFORM_X86_64 */

    /* Rust FFI tests */
    harness_banner("Rust FFI Tests");
    if (!test_filter_active()) {
        total_failures += rust_run_tests();
    }

    /*
     * Integration tests run actual tasks across CPUs.
     * These must run after all unit tests because they exercise
     * the full scheduler infrastructure.
     */
    harness_banner("Integration Tests (Multi-Core)");
#if !defined(PLATFORM_X86_64)
    RUN_SUITE(test_suite_integration);
#endif

    return total_failures;
}

/* ============================================================================
 * Filtered runs (docs/testing.md "Running selected tests")
 * ============================================================================ */

/* Build-time default: make kernel-test TEST_FILTER=a,b TEST_REPEAT=N. */
#if defined(SLMOS_TEST_FILTER_DEFAULT)
static const char harness_build_filter[] = SLMOS_TEST_FILTER_DEFAULT;
#else
static const char harness_build_filter[] = "";
#endif
#if defined(SLMOS_TEST_REPEAT_DEFAULT)
static const char harness_build_repeat[] = SLMOS_TEST_REPEAT_DEFAULT;
#else
static const char harness_build_repeat[] = "";
#endif

static size_t cstrlen(const char *s)
{
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static void harness_print_filter_error(const char *where, int rc)
{
    uart_printf("[FILTER] [FAIL] %s: unparseable test filter (error %d); "
                "running nothing\n", where, rc);
}

/* Set g_test_filter from (in order) the kernel command line and the
 * build-time default. Returns 1 when a filter is active, 0 when none
 * was given, or a negative error (the caller fails the run). */
static int harness_filter_from_boot(void)
{
#if !defined(PLATFORM_X86_64)
    const dtb_chosen_t *chosen = dtb_get_chosen();
    if (chosen != NULL && chosen->bootargs[0] != '\0') {
        int rc = test_filter_from_cmdline(&g_test_filter, chosen->bootargs);
        if (rc < 0) {
            harness_print_filter_error("kernel command line", rc);
            return rc;
        }
        if (rc == 1) {
            return 1;
        }
    }
#endif
    size_t nlen = cstrlen(harness_build_filter);
    if (nlen == 0) {
        return 0;
    }
    test_filter_t f = { 0 };
    int rc = test_filter_parse_names(&f, harness_build_filter, nlen);
    if (rc == TEST_FILTER_OK) {
        f.repeat = 1;
        size_t rlen = cstrlen(harness_build_repeat);
        if (rlen != 0) {
            rc = test_filter_parse_repeat(harness_build_repeat, rlen, &f.repeat);
        }
    }
    if (rc != TEST_FILTER_OK) {
        harness_print_filter_error("build-time TEST_FILTER/TEST_REPEAT", rc);
        return rc;
    }
    f.active = true;
    f.source = "build";
    g_test_filter = f;
    return 1;
}

/* Run the active filter: g_test_filter.repeat passes over the named
 * tests, each pass entering the selected suites in the usual order, so
 * "a,b" with repeat 3 runs a b a b a b. Returns the failure count:
 * failed iterations plus one for each name that never ran. */
static int harness_run_filtered(void)
{
    test_filter_t *f = &g_test_filter;
    int total_failures = 0;

    for (uint32_t i = 0; i < TEST_FILTER_MAX_NAMES; i++) {
        g_test_filter_stats[i] = (test_filter_stats_t){ 0 };
    }

    harness_walk_all = false;
    uart_puts("[FILTER] tests=");
    for (uint32_t i = 0; i < f->count; i++) {
        uart_printf("%s%s", i ? "," : "", f->names[i]);
    }
    uart_printf(" repeat=%u source=%s index=%u\n", f->repeat,
                f->source ? f->source : "?", test_index_size());
    for (uint32_t i = 0; i < f->count; i++) {
        uint32_t cursor = 0;
        if (test_index_next_suite(f->names[i], &cursor) == NULL) {
            uart_printf("[FILTER] %s is not in the test index; entering every suite\n",
                        f->names[i]);
            harness_walk_all = true;
        }
    }

    for (uint32_t pass = 1; pass <= f->repeat; pass++) {
        g_test_filter_pass = pass;
        total_failures += harness_run_suites();
    }

    uint64_t freq = timer_get_frequency();
    uart_puts("\n[FILTER] Summary\n");
    for (uint32_t i = 0; i < f->count; i++) {
        const test_filter_stats_t *st = &g_test_filter_stats[i];
        if (st->runs == 0) {
            uart_printf("[FILTER] [FAIL] %s: not found (0 runs)\n", f->names[i]);
            total_failures++;
            continue;
        }
        uint64_t avg = st->total_ticks / st->runs;
        uart_printf("[FILTER] %s %s: runs=%u passed=%u failed=%u ignored=%u "
                    "us min=%lu avg=%lu max=%lu\n",
                    st->failed ? "[FAIL]" : "[PASS]", f->names[i],
                    st->runs, st->passed, st->failed, st->ignored,
                    (unsigned long)(freq ? st->min_ticks * 1000000u / freq : 0),
                    (unsigned long)(freq ? avg * 1000000u / freq : 0),
                    (unsigned long)(freq ? st->max_ticks * 1000000u / freq : 0));
    }
    return total_failures;
}

int test_harness_run_all(void)
{
    int rc = harness_filter_from_boot();
#if defined(FAULT_DUMP_SELFTEST) && !defined(PLATFORM_X86_64)
    /* FAULT_DUMP_SELFTEST=ON: the forced faults leave CPUs 1, 2 and 3
     * halted, so run those two tests alone unless a filter was given. */
    if (rc == 0) {
        static const char selftest[] =
            "test_fault_dump_two_cpus_concurrent,"
            "test_fault_dump_holder_stops_console_continues";
        test_filter_t f = { 0 };
        (void)test_filter_parse_names(&f, selftest, sizeof(selftest) - 1);
        f.repeat = 1;
        f.active = true;
        f.source = "FAULT_DUMP_SELFTEST";
        g_test_filter = f;
        rc = 1;
    }
#endif
    if (rc < 0) {
        return test_harness_finish(1);
    }
    if (rc == 1) {
        return test_harness_finish(harness_run_filtered());
    }
    return test_harness_finish(harness_run_suites());
}

/* `ktest <name[,name...]> [repeat]`: the same filtered run from the
 * shell of a boot-test kernel, after the boot run has finished. */
static int cmd_ktest(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        uart_puts("usage: ktest <test[,test...]> [repeat]\n");
        return 1;
    }
    test_filter_t f = { 0 };
    int rc = test_filter_parse_names(&f, argv[1], cstrlen(argv[1]));
    f.repeat = 1;
    if (rc == TEST_FILTER_OK && argc == 3) {
        rc = test_filter_parse_repeat(argv[2], cstrlen(argv[2]), &f.repeat);
    }
    if (rc != TEST_FILTER_OK) {
        harness_print_filter_error("ktest", rc);
        return 1;
    }
    f.active = true;
    f.source = "shell";
    g_test_filter = f;
    int failures = harness_run_filtered();
    g_test_filter.active = false;
    uart_puts("\n[FILTER] ");
    if (failures == 0) {
        uart_puts("[PASS] ktest\n");
    } else {
        uart_printf("[FAIL] ktest: %d failure(s)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}

static const shell_cmd_t ktest_cmd = {
    .name = "ktest",
    .handler = cmd_ktest,
    .help = "Run named boot tests: ktest <test[,test...]> [repeat]",
    .mutates = true,
    .category = SHELL_CAT_PROCESS,
};

/* Called from shell_init() in boot-test images. */
void test_harness_register_shell(void)
{
    (void)shell_register_command(&ktest_cmd);
}

static int test_harness_finish(int total_failures)
{
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
