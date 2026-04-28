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

/* Cross-platform smoke test: asserts model_mem_init produced a usable
 * allocator on the active target. Runs on every PLATFORM. */
int test_suite_model_mem_smoke(void);

/* Eviction policy tests (Phase AI-Eviction M1/M2 — calls into Rust) */
int test_suite_eviction(void);

/* SLM loader / GGUF FFI tests (Phase SLM, M1.5 — calls into Rust) */
int test_suite_slm_load(void);

/* SLM shell verb dispatch tests (Phase SLM, M7.1 — calls into the
 * `slm` command family registered via builtin_commands[]) */
int test_suite_slm_shell(void);

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

/* Shell session pool / per-task binding tests */
int test_suite_shell_session(void);

/* Per-session command-history ring + ESC[A/B parser tests (#434) */
int test_suite_shell_history(void);

/* Runtime blob autoload config + boot replay tests */
int test_suite_blob_autoload(void);

/* Boot-media gate + keep-alive ref tests (#414) */
int test_suite_boot_media(void);

/* Telnet IAC state machine tests */
int test_suite_telnet(void);

/* /etc/telnetd.conf parser tests */
int test_suite_telnetd_config(void);

/* `telnetd` shell command + shell_io_tcp enumeration / kick tests */
int test_suite_telnetd_cmd(void);

/* TCP telemetry-feed server: filter / fanout / drop-oldest / pool */
int test_suite_tcp_telemetry_server(void);

/* VMM/TLB tests */
int test_suite_vmm(void);

/* PMM buddy allocator tests */
int test_suite_pmm(void);

/* PCIe host-controller tests (ARM64 only — QEMU virt GPEX for now) */
int test_suite_pcie(void);

/* BPMP IPC tests (ARM64 only — stub behaviour on non-Jetson, live
 * hardware exercise via shell commands on Jetson). Related docs:
 * docs/jetson-pcie-investigation.md. */
int test_suite_bpmp(void);

/* Inference-device abstraction tests (registry + fake backend;
 * cpu-mlp-specific cases gated on CONFIG_AI_SCHEDULER) */
int test_suite_inference_device(void);

/* Hailo-8 driver core tests (mocked platform ops) */
int test_suite_hailo(void);

/* HEF outer-header validator + nanopb freestanding smoke test */
int test_suite_hef(void);

/* HEF protobuf body parser (nanopb-driven, synthetic wire-format blobs) */
int test_suite_hef_parser(void);

/* LittleFS integration tests */
int test_suite_littlefs(void);

/* Networking tests (QEMU only) */
int test_suite_net(void);

/* HTTP URL parsing + shell dispatch tests */
int test_suite_net_http(void);

/* USB core tests (platform-neutral; Phase 1 of #266) */
int test_suite_usb_core(void);

/* CDC-ECM class driver tests (ENABLE_NETWORKING; Phase 2 of #266) */
int test_suite_cdc_ecm(void);

/* XHCI ring primitive tests (platform-neutral; Phase 3A of #266) */
int test_suite_xhci_ring(void);

/* Tegra234 XHCI wrapper offset + CSB-paging tests (Phase 3A.2 of #266) */
int test_suite_xhci_tegra(void);

/* PORTSC decode + context layout (Phase 3A Step 5) */
int test_suite_xhci_device(void);

/* TRB builder tests (Phase 3A Step 6 + 7b) */
int test_suite_xhci_xfer(void);

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

/* Model hot-swap tests (registry::swap_model + rust_model_swap FFI) */
int test_suite_model_swap(void);

/* GPU compute integration tests (capability detection, fallback - calls into Rust) */
int test_suite_gpu_compute(void);

/* Syscall infrastructure tests (dispatch, trap frame, user task creation) */
int test_suite_syscall(void);

/* Component integration tests (infer_classify, example components) */
int test_suite_components_m5(void);

/* DTB parser tests (BOOT-H1 bounds checks) */
int test_suite_dtb(void);

/* BCM mailbox property-tag buffer tests (#367, dynamic-kernel-replace Stage 1) */
int test_suite_bcm_mailbox(void);

/* Camera C-API + Tegra234 clock-id static_assert pins (#396) */
int test_suite_camera(void);

/* FatFs / FAT32 integration tests (#368, dynamic-kernel-replace Stage 2) */
int test_suite_fat32(void);

/* SDHCI block driver integration tests (#369, dynamic-kernel-replace Stage 3) */
int test_suite_sdhci(void);

/* `kernel` admin command surface tests (#370, dynamic-kernel-replace Stage 4) */
int test_suite_kernel_cmd(void);

/* SHA-256 vendored-library vector tests (#370, dynamic-kernel-replace Stage 4) */
int test_suite_sha256(void);

/* General-purpose FDT reader tests (kernel/lib/fdt) */
int test_suite_fdt(void);

/* ELF loader tests (BOOT-H2 bounds checks) */
int test_suite_elf(void);

/* Message router tests (Rust FFI, ARM64 only) */
int test_suite_msg_router(void);

/* Steal-deque unit tests (#59 Phase A) */
int test_suite_steal_deque(void);

/* Cooperative-preemption tests (issue #99 resolution) */
int test_suite_coop_preempt(void);

/* Scheduler trace buffer tests (#195) */
int test_suite_sched_trace(void);

/* Latency histogram + EWMA rate tests (admin & telemetry suite, M1) */
int test_suite_latency_hist(void);

/* Per-consumer GPU toggle tests (admin & telemetry suite, M2) */
int test_suite_gpu_consumer(void);

/* GPU dispatch circuit-breaker tests (#552 mitigation, PR #555). */
int test_suite_gpu_dispatch_breaker(void);

/* Eviction + inference latency/rate tests (admin & telemetry suite, M3) */
int test_suite_admin_telemetry(void);

/* Telemetry feed pub/sub event counter tests (admin & telemetry, M4) */
int test_suite_telemetry_feed(void);

/* Model engine registry + .meta parser tests (admin & telemetry, M5) */
int test_suite_model_engine(void);

#endif /* TEST_HARNESS_H */
