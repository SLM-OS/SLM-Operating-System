/*
 * ga10b_bringup.c — GA10B (Jetson integrated Ampere) nvgpu-native bringup.
 *
 * Skeleton with phase stubs. See ga10b_bringup.h for the public API and
 * docs/archive/investigations/jetson-nvgpu-bringup-research.md for the architectural overview.
 *
 * Each phase is a separate function so the shell can invoke them
 * independently while we iterate against hardware — same pattern
 * as the discrete-Ampere bringup.c.
 */

#include "ga10b_bringup.h"
#include "ga10b_qmd.h"
#include "gsp.h"
#include "falcon.h"
#include "../../include/uart.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Per-op chatter flag. Default OFF so steady-state inference
 * doesn't drown the serial console (every dispatch produces
 * roughly 64 lines × 8 ops × 2 helpers when fully verbose).
 * Toggle from the shell with `gpu debug on|off`. Both the
 * per-op submit/poll trace AND the pipeline-mode/complete
 * summary lines are gated together — at 2 inf/s steady-state,
 * even the 2-line summary chatter is too much over telnet
 * uploads. Errors (failed dispatch, payload-mismatch, GP_GET
 * stalled) bypass the gate and always print so a wedged GPU
 * is still visible without flipping the flag first. */
static atomic_bool g_ga10b_dispatch_verbose = false;

void ga10b_dispatch_verbose_set(bool on)
{
    atomic_store_explicit(&g_ga10b_dispatch_verbose, on,
                          memory_order_relaxed);
}

bool ga10b_dispatch_verbose_get(void)
{
    return atomic_load_explicit(&g_ga10b_dispatch_verbose,
                                memory_order_relaxed);
}

#define GA10B_DBG(...)                                              \
    do {                                                            \
        if (atomic_load_explicit(&g_ga10b_dispatch_verbose,         \
                                 memory_order_relaxed))             \
            uart_printf(__VA_ARGS__);                               \
    } while (0)

/* Dispatch poll budget shared by ga10b_submit_and_poll (single-op
 * smoke-test path) and the v7 bulk dispatcher. Each iteration of
 * the outer poll loop pairs with one ga10b_dispatch_poll_delay_us
 * call below, so the wall-clock cap is approximately
 * GA10B_DISPATCH_POLL_TIMEOUT_US microseconds. The 2 s value is
 * generous — typical sema release fires in tens of microseconds,
 * but tail latencies under context-switch pressure or first-op
 * QMD fault diagnostics can stretch into the hundreds of ms. */
#define GA10B_DISPATCH_POLL_TIMEOUT_US 2000000u

/* Approximate 1 µs delay via volatile nop-spin. Calibrated for
 * Cortex-A78AE (Jetson Orin); not precise wall-clock — the actual
 * wait depends on CPU clock + memory pressure — but bounded above
 * by a small multiple of 1 µs so the overall poll budget tracks
 * GA10B_DISPATCH_POLL_TIMEOUT_US within a constant factor. The
 * `volatile` prevents the compiler from optimizing the loop body
 * away. */
static inline void ga10b_dispatch_poll_delay_us(void)
{
    for (volatile int i = 0; i < 1500; i++) { }
}

extern const struct gsp_platform_ops *gsp_platform;

/* ---- Firmware symbols from nvidia_ga10b_firmware.S ----
 *
 * Guarded by ENABLE_GA10B_FIRMWARE: the build sets this when
 * -DGA10B_FIRMWARE_DIR=<path> is provided and all required blobs
 * exist. Without it, ga10b_firmware_get() returns -1 and any
 * phase that needs firmware aborts cleanly. */
#if defined(ENABLE_GA10B_FIRMWARE)
extern const uint8_t ga10b_fw_acr_text_start[];
extern const uint8_t ga10b_fw_acr_text_end[];
extern const uint8_t ga10b_fw_acr_data_start[];
extern const uint8_t ga10b_fw_acr_data_end[];
extern const uint8_t ga10b_fw_acr_manifest_start[];
extern const uint8_t ga10b_fw_acr_manifest_end[];
extern const uint8_t ga10b_fw_fecs_start[];
extern const uint8_t ga10b_fw_fecs_end[];
extern const uint8_t ga10b_fw_fecs_sig_start[];
extern const uint8_t ga10b_fw_fecs_sig_end[];
extern const uint8_t ga10b_fw_gpccs_start[];
extern const uint8_t ga10b_fw_gpccs_end[];
extern const uint8_t ga10b_fw_gpccs_sig_start[];
extern const uint8_t ga10b_fw_gpccs_sig_end[];
extern const uint8_t ga10b_fw_pmu_image_start[];
extern const uint8_t ga10b_fw_pmu_image_end[];
extern const uint8_t ga10b_fw_pmu_desc_start[];
extern const uint8_t ga10b_fw_pmu_desc_end[];
extern const uint8_t ga10b_fw_pmu_sig_start[];
extern const uint8_t ga10b_fw_pmu_sig_end[];
extern const uint8_t ga10b_fw_net_a_start[];
extern const uint8_t ga10b_fw_net_a_end[];
extern const uint8_t ga10b_fw_net_b_start[];
extern const uint8_t ga10b_fw_net_b_end[];
extern const uint8_t ga10b_fw_net_c_start[];
extern const uint8_t ga10b_fw_net_c_end[];
extern const uint8_t ga10b_fw_net_d_start[];
extern const uint8_t ga10b_fw_net_d_end[];
extern const uint8_t ga10b_fw_safety_text_start[];
extern const uint8_t ga10b_fw_safety_text_end[];
extern const uint8_t ga10b_fw_safety_data_start[];
extern const uint8_t ga10b_fw_safety_data_end[];
extern const uint8_t ga10b_fw_safety_manifest_start[];
extern const uint8_t ga10b_fw_safety_manifest_end[];
#endif

int ga10b_firmware_get(enum ga10b_firmware_kind kind,
                       struct ga10b_firmware_blob *out)
{
    if (!out) return -1;
    out->data = NULL;
    out->size = 0;

#if !defined(ENABLE_GA10B_FIRMWARE)
    (void)kind;
    return -1;
#else
    switch (kind) {
#   define _FW(K, SYM) \
        case K: \
            out->data = SYM##_start; \
            out->size = (size_t)(SYM##_end - SYM##_start); \
            return 0;
    _FW(GA10B_FW_ACR_TEXT,         ga10b_fw_acr_text)
    _FW(GA10B_FW_ACR_DATA,         ga10b_fw_acr_data)
    _FW(GA10B_FW_ACR_MANIFEST,     ga10b_fw_acr_manifest)
    _FW(GA10B_FW_FECS,             ga10b_fw_fecs)
    _FW(GA10B_FW_FECS_SIG,         ga10b_fw_fecs_sig)
    _FW(GA10B_FW_GPCCS,            ga10b_fw_gpccs)
    _FW(GA10B_FW_GPCCS_SIG,        ga10b_fw_gpccs_sig)
    _FW(GA10B_FW_PMU_IMAGE,        ga10b_fw_pmu_image)
    _FW(GA10B_FW_PMU_DESC,         ga10b_fw_pmu_desc)
    _FW(GA10B_FW_PMU_SIG,          ga10b_fw_pmu_sig)
    _FW(GA10B_FW_NET_A,            ga10b_fw_net_a)
    _FW(GA10B_FW_NET_B,            ga10b_fw_net_b)
    _FW(GA10B_FW_NET_C,            ga10b_fw_net_c)
    _FW(GA10B_FW_NET_D,            ga10b_fw_net_d)
    _FW(GA10B_FW_SAFETY_TEXT,      ga10b_fw_safety_text)
    _FW(GA10B_FW_SAFETY_DATA,      ga10b_fw_safety_data)
    _FW(GA10B_FW_SAFETY_MANIFEST,  ga10b_fw_safety_manifest)
#   undef _FW
    default: return -1;
    }
#endif
}

/* ---- BAR0 engine bases (GA10B layout) ----
 *
 * Verified via docs/archive/investigations/jetson-nvgpu-acr-analysis.md against OE4T nvgpu
 * l4t-r36.5 sources. GSP Falcon block starts at 0x110000; the RISCV
 * subblock (used for ACR) is at 0x111000.
 *
 * FECS, GPCCS, PMU are bootstrapped by ACR itself on GA10B (since we
 * skip LSPMU) — we don't need to probe them from SLM-OS. Their bases
 * will matter for later phases (GR init, method submission) but not
 * for ACR load. */
/* NV_PGSP_BASE and NV_PGSP_RISCV_BASE come from falcon.h. Registered
 * here for grep-ability:
 *   NV_PGSP_BASE        0x00110000   (GSP Falcon block)
 *   NV_PGSP_RISCV_BASE  0x00111000   (GSP RISCV subblock)
 */

/* ---- GSP RISCV registers (offsets from NV_PGSP_RISCV_BASE) ----
 *
 * Used by phase 1 to start the HS ACR ucode in preloaded mode.
 * Register meanings per ~/slmos-ref/nvidia/nvgpu-hw-ga10b-hw_priscv_ga10b.h. */

#define RISCV_BOOT_VECTOR_LO        0x380u
#define RISCV_BOOT_VECTOR_HI        0x384u
#define RISCV_CPUCTL                0x388u
#define RISCV_CPUCTL_STARTCPU       (1u << 0)
#define RISCV_BR_RETCODE            0x65cu
#define RISCV_BCR_CTRL              0x668u
#define RISCV_BCR_CTRL_VALID        (1u << 0)
#define RISCV_BCR_CTRL_BRFETCH      (1u << 8)
#define RISCV_BCR_CTRL_CORE_RISCV   (1u << 4)   /* 1 = select RISCV core */
/* BCR_CTRL = 0x11 = CORE_SELECT=RISCV | VALID   (preloaded IMEM/DMEM mode) */
#define RISCV_BCR_CTRL_PRELOADED \
    (RISCV_BCR_CTRL_CORE_RISCV | RISCV_BCR_CTRL_VALID)

/* RISCV CPUCTL status bits — need explicit masks (the aliased Falcon
 * ones from falcon.h use HALTED=bit4 which works for both cores). */
#define RISCV_CPUCTL_HALTED         (1u << 4)
#define RISCV_CPUCTL_ACTIVE         (1u << 7)

/* ACR HS completion marker — written to MAILBOX0 by the ucode when
 * it runs. nvgpu's acr_sw_ga10b checks for ACR_OK. Exact constant
 * verified in ~/slmos-ref/nvidia/nvgpu-common-acr-acr_bootstrap.c. */
#define ACR_BOOT_OK                 0x000000ffu

/* ---- FECS / GPCCS (GR Falcon) register offsets ----
 *
 * Offsets from ~/slmos-ref/nvidia/nvgpu-hw-ga10b-hw_gr_ga10b.h (OE4T l4t-r36.5).
 * FECS is the GR front-end context-switch Falcon; GPCCS is per-GPC. On
 * GA10B cold boot (SEC_SECUREGPCCS path) ACR pre-loads both IMEM/DMEM
 * from WPR — SLM-OS only needs to issue STARTCPU and wait for the
 * ucode to advertise readiness via ctxsw_mailbox[0].
 *
 * Mailbox sentinels (gr_fecs_ctxsw_mailbox_value_pass_v / _fail_v):
 *   1 = PASS      (bootstrap complete, ready for methods)
 *   2 = FAIL      (ucode refused to come up)
 *   0x21 = checksum mismatch (secure boot verification failed)
 *
 * CPUCTL.STARTCPU is bit 1 on the GR Falcons (different from GSP
 * RISCV's bit 0 — Falcon v4 vs Falcon3 layout). */
#define GR_FECS_CPUCTL              0x00409100u
#define GR_FECS_BOOTVEC             0x00409104u
#define GR_FECS_DMACTL              0x0040910cu
#define GR_FECS_CPUCTL_ALIAS        0x00409130u
#define GR_FECS_CTXSW_MAILBOX(i)    (0x00409800u + (i) * 4u)
#define GR_FECS_CTXSW_MAILBOX_COUNT 18u

#define GR_GPCCS_CPUCTL             0x0041a100u
#define GR_GPCCS_DMACTL             0x0041a10cu
#define GR_GPC0_GPCCS_CTXSW_MAILBOX(i) (0x00502800u + (i) * 4u)

#define GR_CPUCTL_STARTCPU          (1u << 1)
#define GR_CPUCTL_HALTED            (1u << 4)

#define GR_FECS_MAILBOX_PASS        0x00000001u
#define GR_FECS_MAILBOX_FAIL        0x00000002u
#define GR_FECS_MAILBOX_CSUM_FAIL   0x00000021u

/* ---- COMPUTE_B semaphore methods (AMPERE_COMPUTE_B = 0xC7C0) ----
 *
 * These methods decode against the GR/compute engine, not PBDMA, and
 * require SET_OBJECT(0xC7C0) to have bound AMPERE_COMPUTE_B to the
 * subchannel first. Byte offsets are from clc7c0.h. OPERATION on this
 * family differs from the host family at 0x5C..0x6C: RELEASE is 0
 * here (not 1), and STRUCTURE_SIZE must be SEMAPHORE_ONE_WORD for
 * 32-bit payloads.
 *
 * Source: ~/slmos-ref/nvidia/nvgpu-include-class-clc7c0.h:109-142 */
#define NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER   0x0158u
#define NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_UPPER   0x015Cu
#define NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_LOWER   0x0160u
#define NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_UPPER   0x0164u
#define NVC7C0_REPORT_SEMAPHORE_EXECUTE             0x0168u

/* REPORT_SEMAPHORE_EXECUTE fields */
#define NVC7C0_SEM_EXECUTE_OP_RELEASE               0x0u   /* bits [1:0] */
#define NVC7C0_SEM_EXECUTE_STRUCTURE_SIZE_ONE_WORD  (1u << 3)  /* bits [4:3] */

/* ---- COMPUTE_B compute-kernel launch methods --------------------
 * Used by ga10b_bringup_launch_kernel (Phase 8). Offsets from
 * ~/slmos-ref/mesa/mesa-clc7c0.h. */
#define NVC7C0_INVALIDATE_SHADER_CACHES               0x021cu
#define NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI 0x0244u
#define NVC7C0_INVALIDATE_SKED_CACHES                 0x0298u

/* INVALIDATE_SHADER_CACHES bitfields per
 * ~/slmos-ref/mesa/mesa-clc7c0.h:299-314 (bit positions
 * decoded from "MSB:LSB" notation). All five bits set = nuke every
 * shader-side cache before the next dispatch reads input/cbuf data.
 *
 *   INSTRUCTION  bit  0   0x0001
 *   LOCKS        bit  1   0x0002
 *   FLUSH_DATA   bit  2   0x0004
 *   DATA         bit  4   0x0010
 *   CONSTANT     bit 12   0x1000
 *   --------------------------
 *   ALL                   0x1017                                        */
#define NVC7C0_INVALIDATE_SHADER_CACHES_ALL            0x1017u
#define NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A      0x02a0u
#define NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_B      0x02a4u
#define NVC7C0_SEND_PCAS_A                            0x02b4u
#define NVC7C0_SEND_SIGNALING_PCAS2_B                 0x02c0u
#define NVC7C0_SEND_SIGNALING_PCAS2_B_ACTION_INVALIDATE_COPY_SCHEDULE  0xAu
#define NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A       0x07b0u
#define NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_B       0x07b4u

/* Per-arch window base addresses for Ampere. Mesa NVK hardcodes these
 * for `VOLTA_COMPUTE_A <= cls_compute < HOPPER_COMPUTE_A` in
 * ~/slmos-ref/mesa/mesa-nvk_cmd_dispatch.c:63-79. */
#define NVC7C0_SHARED_MEMORY_WINDOW_BASE 0xfe000000u
#define NVC7C0_LOCAL_MEMORY_WINDOW_BASE  0xff000000u

/* NVC56F_SET_OBJECT is at method byte offset 0 on every channel class.
 * (GA10B_AMPERE_COMPUTE_B_CLASS_ID is declared in ga10b_bringup.h so
 * host tests can reference it.) */
#define NVC56F_SET_OBJECT            0x00u

/* ---- USERMODE doorbell (Phase 7 PBDMA kick) ----
 *
 * GA10B inherits the TU104 usermode register layout, so the doorbell
 * is at BAR0 + func_cfg0 + func_full_phys + func_doorbell
 *       = 0x17000000 + 0x30000 + 0xB80000 + 0x90 = 0x17BB0090.
 * A 32-bit write of `work_submit_token` (captured from
 * NVGPU_IOCTL_CHANNEL_SETUP_BIND) tells PBDMA to re-read GP_PUT and
 * fetch any new GPFIFO entries.
 *
 * Reference: OE4T/linux-nvgpu
 *   drivers/gpu/nvgpu/hal/fifo/usermode_tu104.c:58-75
 *   drivers/gpu/nvgpu/include/nvgpu/hw/tu104/hw_func_tu104.h:62-64 */
#define GA10B_USERMODE_DOORBELL_PHYS  0x17BB0090u

/* ---- Host-semaphore pushbuffer methods (Phase 7 SEMAPHORE_RELEASE) ----
 *
 * Method-header layout (Fermi-family, used by all later Volta+/Ampere,
 * per Nouveau include/nvhw/class/cl906f.h):
 *   [31:29] SEC_OP:         1=INC, 3=NON_INC, 4=IMMD, 5=ONE_INC
 *   [28:16] COUNT:          number of data dwords (non-IMMD)
 *   [15:13] SUBCHANNEL:     0 for host methods
 *   [12:2]  METHOD_ADDRESS: byte_offset placed directly — its low 2
 *                           bits are already 0, and bits [12:2] hold
 *                           what PBDMA decodes as "method index".
 *                           (Misreading this as "method_index at
 *                           bits [12:0]" means PBDMA sees the address
 *                           shifted right by 2 — e.g., SEM_ADDR_LO at
 *                           byte 0x5C becomes byte 0x14, which is the
 *                           legacy SEMAPHOREB slot that isn't routed.)
 *
 * GA10B (AMPERE_CHANNEL_GPFIFO_A, 0xC56F) binds the gv11b HAL's
 * "new" host-semaphore interface at method indices 0x17..0x1b
 * (byte offsets 0x5C..0x6C). The legacy NVC56F_SEMAPHOREA/B/C/D
 * at 0x04..0x07 aren't routed by the HOST on this channel class —
 * PBDMA walks past them without executing anything.
 *
 * Source:
 *   drivers/gpu/nvgpu/hal/init/hal_ga10b.c:1160 (binds gv11b_sema_add_incr_cmd)
 *   drivers/gpu/nvgpu/hal/sync/sema_cmdbuf_gv11b.c:41-101 (method encoding)
 *
 * Host methods don't need a prior SET_OBJECT / class bind — PBDMA
 * decodes them directly, so subchannel=0 is safe.
 *
 * Encoding (validated on jetson-nano-2, 2026-04-18): the hardware
 * decodes bits [12:0] of the header as method_id, where
 * method_id = byte_off / 4. nvgpu's own gv11b sema cmdbuf writes
 * 0x20010017 for SEM_ADDR_LO (byte 0x5C → method_id 0x17 placed at
 * [12:0]) and that encoding fires the sema post-doorbell. A prior
 * iteration of this macro used `byte_off & 0xFFFu` at bits [11:0]
 * — PBDMA advanced GP_GET anyway (it consumes the dword pair) but
 * silently discarded the method, so the sema never wrote. The fix
 * is to shift byte_off right by 2 before masking.
 *
 * Mask width (0x1FFFu, 13 bits): the original NV906F (Kepler+) spec
 * placed METHOD_ADDRESS at bits [12:2] — an 11-bit field, max
 * method_id 0x7FF. That would have been enough for classes with
 * byte offsets up to 0x1FFC. AMPERE_COMPUTE_B (class 0xC7C0) defines
 * methods up to byte 0x33EC (method_id 0xCFB = 12 bits), so the
 * field must have been widened on Volta+. A 13-bit mask is the
 * strictest we can apply without truncating known-valid method_ids
 * and still covers any plausible future expansion within a 3-bit
 * subchannel-boundary margin at bit 13. */
#define NVC56F_METHOD_HEADER_INC(count, subch, byte_off)                 \
    ((1u << 29) | ((uint32_t)(count) << 16) |                            \
     ((uint32_t)(subch) << 13) | (((uint32_t)(byte_off) >> 2) & 0x1FFFu))

/* New HOST semaphore interface (Volta+). Byte offsets in the method
 * space; the macro above places method_id = byte_off / 4 at bits
 * [12:0] of the header. */
#define NVC56F_SEM_ADDR_LO                0x5Cu   /* method index 0x17 */
#define NVC56F_SEM_ADDR_HI                0x60u   /* method index 0x18 */
#define NVC56F_SEM_PAYLOAD_LO             0x64u   /* method index 0x19 */
#define NVC56F_SEM_PAYLOAD_HI             0x68u   /* method index 0x1a */
#define NVC56F_SEM_EXECUTE                0x6Cu   /* method index 0x1b */

/* SEM_EXECUTE field encoding (gv11b sema_cmdbuf:83-101). OPERATION
 * RELEASE is 1 on the new interface (legacy SEMAPHORED.RELEASE=2 —
 * different register, different value). */
#define NVC56F_SEM_EXECUTE_OP_RELEASE     0x1u
/* PAYLOAD_SIZE at bit 24 and RELEASE_WFI at bit 20 are both left at
 * their default value of 0 (32-bit payload, no wait-for-idle) to
 * match nvgpu's gv11b literal `0x1` on incr_cmd without wfi. If the
 * defaults ever need to change, set bit 24 = 1 for 64-bit payload
 * and bit 20 = 1 to enable RELEASE_WFI. */

/* Known payload the GPU writes to the semaphore. Chosen to be non-zero,
 * visually distinct from stale bus values (0xbadf...), and small enough
 * to format cleanly in hex logs. */
#define GA10B_SMOKETEST_SEM_PAYLOAD       0x0000CAFEu

/* ---- FECS method gateway ----
 *
 * FECS exposes a direct host-to-ucode method interface via two push
 * registers. The host writes method data, then method address; FECS
 * processes the method and writes the result to ctxsw_mailbox[0].
 * No channel, GMMU, or page tables required.
 *
 * Reference: nvgpu gm20b_gr_falcon_submit_fecs_method_op
 *   ~/slmos-ref/nvidia/nvgpu-gr-falcon-gm20b-fusa.c
 */
#define GR_FECS_METHOD_DATA         0x00409500u
#define GR_FECS_METHOD_PUSH         0x00409504u

/* Method addresses — low 12 bits of the push register value. */
#define FECS_METHOD_HALT_PIPELINE             0x04u
#define FECS_METHOD_DISCOVER_IMAGE_SIZE       0x10u
#define FECS_METHOD_DISCOVER_ZCULL_IMAGE_SIZE 0x18u
#define FECS_METHOD_STOP_CTXSW               0x38u
#define FECS_METHOD_START_CTXSW              0x39u

/* GA10B null method data sentinel — used when the method doesn't take
 * meaningful input. The returned mailbox value is the output. */
#define FECS_METHOD_NULL_DATA       0xDEADCA11u

/* ============================================================================
 * Phase entry points
 * ============================================================================
 *
 * For now, all phases are stubs that log and return -1. Each will be
 * filled in by tasks #13 (ACR), #14 (FECS/GPCCS/PMU), #15 (channel),
 * #16 (smoke test / compute). Returning -1 here lets the runner exit
 * cleanly and lets the harness walk phase-by-phase while we iterate.
 */

int ga10b_bringup_prepare(struct ga10b_bringup *b)
{
    if (!b) return -1;
    memset(b, 0, sizeof(*b));
    b->state = GA10B_BRINGUP_INIT;
    b->last_error_phase = -1;

    if (!gsp_platform) {
        uart_puts("[GA10B] no platform ops installed\n");
        return -1;
    }

    /* Sanity: firmware must be embedded. Checking one blob is enough —
     * either the build linked all of nvidia_ga10b_firmware.S or none. */
    struct ga10b_firmware_blob tmp;
    if (ga10b_firmware_get(GA10B_FW_ACR_TEXT, &tmp) < 0 || tmp.size == 0) {
        uart_puts("[GA10B] firmware not embedded "
                  "(build with -DGA10B_FIRMWARE_DIR=<path>)\n");
        return -1;
    }
    uart_printf("[GA10B] ACR text: %lu bytes\n", (unsigned long)tmp.size);

    /* Probe the GSP Falcon — this is the one ACR runs on. FECS,
     * GPCCS, and PMU are bootstrapped by ACR itself (since we skip
     * LSPMU), so SLM-OS doesn't touch their MMIO directly. */
    if (falcon_probe(&b->gsp_flcn, NV_PGSP_BASE) < 0) {
        uart_puts("[GA10B] GSP Falcon probe failed\n");
        return -1;
    }

    uart_puts("[GA10B] prepare OK — GSP Falcon probed\n");
    return 0;
}

/* ---- Phase 1: ACR on GSP RISCV ----
 *
 * Load the ACR HS ucode into the GSP Falcon's IMEM/DMEM via PIO, then
 * kick the RISCV core in "preloaded" mode. The manifest at the tail
 * of DMEM gets consumed by the Falcon BROM to verify the signed
 * ucode; if verification passes, BROM jumps to the entry point in
 * IMEM and ACR runs.
 *
 * This matches nvgpu_acr_bootstrap_hs_ucode_riscv in
 *   ~/slmos-ref/nvidia/nvgpu-common-acr-acr_bootstrap.c:360–419
 * (preloaded BCR_CTRL=0x11 branch — we skip the 0x111 DMA path).
 *
 * On success the RISCV core runs, ACR authenticates FECS/GPCCS/(PMU),
 * and eventually halts with MAILBOX0 = ACR_BOOT_OK and BR_RETCODE =
 * success pattern. On failure BR_RETCODE reports the BROM error code
 * (signature mismatch, manifest bad, etc.).
 */

/* Thin wrappers over the platform vtable — saves repeating the
 * gsp_platform-> prefix at every register poke. */
static inline uint32_t bar0_r32(uint32_t off)
{
    return gsp_platform->read32(off);
}
static inline void bar0_w32(uint32_t off, uint32_t val)
{
    gsp_platform->write32(off, val);
}

/*
 * GA10B GSP engine reset — differs from the generic falcon_reset
 * (which only writes the self-clearing RESET bit and polls HWCFG2).
 * On GA10B the pattern is an explicit assert/deassert:
 *
 *   1. Write ENGINE.RESET = 1       (assert)
 *   2. Wait 10+ µs
 *   3. Write ENGINE.RESET = 0       (deassert — crucial!)
 *
 * Reference: ~/slmos-ref/nvidia/nvgpu-hal-gsp-gsp_ga10b.c:54 ga10b_gsp_engine_reset.
 * Without the deassert write the engine stays in reset forever, and
 * HWCFG2/CPUCTL read back as PRI poison (0xbadfXXXX) — which is
 * exactly what we see on jetson-nano-2 after Linux's nvgpu detach.
 */
static int ga10b_gsp_engine_reset(void)
{
    bar0_w32(NV_PGSP_BASE + FALCON_ENGINE, FALCON_ENGINE_RESET);
    gsp_platform->mb();

    /* 10 µs delay. gsp_platform doesn't expose a delay primitive,
     * so spin a bounded loop. On Cortex-A78AE @ ~1.5 GHz, 15k NOPs
     * is ~10 µs. Generous. */
    for (volatile int i = 0; i < 15000; i++) { }

    bar0_w32(NV_PGSP_BASE + FALCON_ENGINE, 0u);
    gsp_platform->mb();

    /* Wait for HWCFG2.MEM_SCRUBBING to clear (engine done scrubbing
     * its own memory). Up to 500 ms. Note: for the initial boot path
     * where the engine has never been reset, this typically completes
     * in < 1 ms. */
    uint32_t loops = 500u * 1000u;
    while (loops-- > 0) {
        uint32_t hwcfg2 = bar0_r32(NV_PGSP_BASE + FALCON_HWCFG2);
        if (hwcfg2 == 0xFFFFFFFFu) return -1;
        if ((hwcfg2 & 0xbadf0000u) == 0xbadf0000u) {
            /* Still priv-locked; keep waiting. */
        } else if ((hwcfg2 & FALCON_HWCFG2_MEM_SCRUBBING) == 0) {
            return 0;
        }
        for (volatile int i = 0; i < 1500; i++) { }  /* ~1 µs */
    }
    return -1;
}

static int wait_for_halt_us(uint32_t timeout_us)
{
    /* Poll RISCV CPUCTL for HALTED=1 or ACTIVE=0. 1 µs loops use a
     * cheap CNTPCT read — accurate enough for a coarse timeout. */
    uint32_t loops = timeout_us;
    while (loops-- > 0) {
        uint32_t ctl = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_CPUCTL);
        if ((ctl & RISCV_CPUCTL_HALTED) || !(ctl & RISCV_CPUCTL_ACTIVE)) {
            return 0;
        }
        /* Spin a few times to make one "loop" take ~1 µs on Cortex-A78AE
         * at 1.5 GHz. Not exact; fine for coarse ms-scale timeouts. */
        for (volatile int i = 0; i < 1500; i++) { }
    }
    return -1;
}

int ga10b_bringup_acr(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_INIT) return -1;

    struct ga10b_firmware_blob text, data, manifest;
    if (ga10b_firmware_get(GA10B_FW_ACR_TEXT, &text) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_DATA, &data) < 0 ||
        ga10b_firmware_get(GA10B_FW_ACR_MANIFEST, &manifest) < 0) {
        uart_puts("[GA10B-ACR] firmware blobs missing\n");
        b->last_error_phase = 1;
        return -1;
    }
    uart_printf("[GA10B-ACR] text=%lu data=%lu manifest=%lu bytes\n",
                (unsigned long)text.size,
                (unsigned long)data.size,
                (unsigned long)manifest.size);

    /* Sanity: code must fit in IMEM, data+manifest must fit in DMEM. */
    if (text.size > b->gsp_flcn.imem_size) {
        uart_printf("[GA10B-ACR] text %lu > IMEM %u\n",
                    (unsigned long)text.size, b->gsp_flcn.imem_size);
        b->last_error_phase = 1;
        return -1;
    }
    if (data.size + manifest.size > b->gsp_flcn.dmem_size) {
        uart_printf("[GA10B-ACR] data+manifest %lu > DMEM %u\n",
                    (unsigned long)(data.size + manifest.size),
                    b->gsp_flcn.dmem_size);
        b->last_error_phase = 1;
        return -1;
    }

    /* Reset GSP Falcon via the GA10B-specific assert/deassert sequence
     * (generic falcon_reset only asserts — sufficient for discrete
     * Ampere where the engine comes out of BIOS in a usable state,
     * but not for Jetson where Linux nvgpu leaves it in reset). */
    if (ga10b_gsp_engine_reset() < 0) {
        uart_puts("[GA10B-ACR] GSP engine reset failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    uart_puts("[GA10B-ACR] GSP engine reset complete\n");

    /* PIO-upload the three pieces:
     *   - text  → IMEM offset 0    (secure=true so IMEMC.SECURE is set
     *                                — matches nvgpu copy_to_imem path
     *                                for HS ucodes)
     *   - data  → DMEM offset 0
     *   - manifest → DMEM at offset (dmem_size - manifest_size). The
     *     Falcon BROM reads PKC parameters from the tail of DMEM.
     */
    if (falcon_pio_upload_imem(&b->gsp_flcn, text.data, (uint32_t)text.size,
                               0u, true) < 0) {
        uart_puts("[GA10B-ACR] IMEM upload failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    if (falcon_pio_upload_dmem(&b->gsp_flcn, data.data, (uint32_t)data.size,
                               0u) < 0) {
        uart_puts("[GA10B-ACR] DMEM data upload failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    uint32_t manifest_off = b->gsp_flcn.dmem_size - (uint32_t)manifest.size;
    if (falcon_pio_upload_dmem(&b->gsp_flcn, manifest.data,
                               (uint32_t)manifest.size, manifest_off) < 0) {
        uart_puts("[GA10B-ACR] DMEM manifest upload failed\n");
        b->last_error_phase = 1;
        return -1;
    }
    uart_printf("[GA10B-ACR] ucode loaded "
                "(IMEM@0+%lu, DMEM@0+%lu, manifest@%u+%lu)\n",
                (unsigned long)text.size,
                (unsigned long)data.size,
                manifest_off,
                (unsigned long)manifest.size);

    /* Read back the first 16 bytes of each DMEM region to verify
     * PIO upload integrity. If these don't match the source bytes,
     * something is wrong with our PIO path and the BROM rejection
     * is moot until that's fixed.
     * DMEMC bits: bit 24=AINCW (auto-inc on write), bit 25=AINCR
     *             (auto-inc on read). Low 24 bits = byte offset. */
    {
#       define DMEMC_AINCR_BIT (1u << 25)
        /* Read data@0 */
        bar0_w32(NV_PGSP_BASE + FALCON_DMEMC(0), DMEMC_AINCR_BIT | 0u);
        gsp_platform->mb();
        uint32_t d0 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t d1 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t src_d0 = (uint32_t)data.data[0] |
                          ((uint32_t)data.data[1] << 8) |
                          ((uint32_t)data.data[2] << 16) |
                          ((uint32_t)data.data[3] << 24);
        uint32_t src_d1 = (uint32_t)data.data[4] |
                          ((uint32_t)data.data[5] << 8) |
                          ((uint32_t)data.data[6] << 16) |
                          ((uint32_t)data.data[7] << 24);
        uart_printf("[GA10B-ACR] dmem@0 readback: 0x%08lx 0x%08lx "
                    "(src 0x%08lx 0x%08lx) %s\n",
                    (unsigned long)d0, (unsigned long)d1,
                    (unsigned long)src_d0, (unsigned long)src_d1,
                    (d0 == src_d0 && d1 == src_d1) ? "OK" : "MISMATCH");

        /* Read manifest@manifest_off */
        bar0_w32(NV_PGSP_BASE + FALCON_DMEMC(0),
                 DMEMC_AINCR_BIT | manifest_off);
        gsp_platform->mb();
        uint32_t m0 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t m1 = bar0_r32(NV_PGSP_BASE + FALCON_DMEMD(0));
        uint32_t src_m0 = (uint32_t)manifest.data[0] |
                          ((uint32_t)manifest.data[1] << 8) |
                          ((uint32_t)manifest.data[2] << 16) |
                          ((uint32_t)manifest.data[3] << 24);
        uint32_t src_m1 = (uint32_t)manifest.data[4] |
                          ((uint32_t)manifest.data[5] << 8) |
                          ((uint32_t)manifest.data[6] << 16) |
                          ((uint32_t)manifest.data[7] << 24);
        uart_printf("[GA10B-ACR] dmem@manifest readback: 0x%08lx 0x%08lx "
                    "(src 0x%08lx 0x%08lx) %s\n",
                    (unsigned long)m0, (unsigned long)m1,
                    (unsigned long)src_m0, (unsigned long)src_m1,
                    (m0 == src_m0 && m1 == src_m1) ? "OK" : "MISMATCH");
    }

    /* Program RISCV boot: vector = 0, BCR_CTRL = CORE_RISCV|VALID. */
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_BOOT_VECTOR_LO, 0u);
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_BOOT_VECTOR_HI, 0u);
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_BCR_CTRL, RISCV_BCR_CTRL_PRELOADED);

    /* Clear MAILBOX0 so we can detect the ucode writing to it. */
    gsp_platform->write32(NV_PGSP_BASE + 0x040u, 0u);
    gsp_platform->mb();

    /* STARTCPU — RISCV begins executing from BOOT_VECTOR (= 0, which
     * after BROM verification means entry point in IMEM). */
    bar0_w32(NV_PGSP_RISCV_BASE + RISCV_CPUCTL, RISCV_CPUCTL_STARTCPU);
    gsp_platform->mb();
    uart_puts("[GA10B-ACR] STARTCPU kicked — polling for halt\n");

    /* Wait up to 2 s for ACR to either halt or report status. */
    int wait_rc = wait_for_halt_us(2u * 1000u * 1000u);

    uint32_t mbox0    = bar0_r32(NV_PGSP_BASE + 0x040u);
    uint32_t mbox1    = bar0_r32(NV_PGSP_BASE + 0x044u);
    uint32_t cpuctl   = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_CPUCTL);
    uint32_t retcode  = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_BR_RETCODE);
    uint32_t hwcfg2   = bar0_r32(NV_PGSP_BASE + FALCON_HWCFG2);
    uint32_t bcr_ctrl = bar0_r32(NV_PGSP_RISCV_BASE + RISCV_BCR_CTRL);

    uart_printf("[GA10B-ACR] post-kick: CPUCTL=0x%08lx MBOX0=0x%08lx "
                "MBOX1=0x%08lx BR_RETCODE=0x%08lx wait_rc=%d\n",
                (unsigned long)cpuctl,
                (unsigned long)mbox0,
                (unsigned long)mbox1,
                (unsigned long)retcode,
                wait_rc);
    uart_printf("[GA10B-ACR] hwcfg2=0x%08lx bcr_ctrl=0x%08lx "
                "br_retcode.result=%lu (0=run 1=dunno 2=FAIL 3=PASS)\n",
                (unsigned long)hwcfg2,
                (unsigned long)bcr_ctrl,
                (unsigned long)(retcode & 0x3u));

    if (wait_rc < 0) {
        uart_puts("[GA10B-ACR] timeout — RISCV neither halted nor stopped\n");
        b->last_error_phase = 1;
        return -1;
    }
    /* BR_RETCODE != 0 typically means BROM rejected the ucode.
     * MAILBOX0 = ACR_BOOT_OK (0xff) means ACR ran and succeeded.
     * Either absence of "OK" or presence of a BROM error = fail. */
    if (mbox0 != ACR_BOOT_OK) {
        uart_printf("[GA10B-ACR] ACR did NOT report BOOT_OK "
                    "(expected 0x%x, got 0x%lx)\n",
                    ACR_BOOT_OK, (unsigned long)mbox0);
        b->last_error_phase = 1;
        return -1;
    }

    uart_puts("[GA10B-ACR] ACR running — BOOT_OK received\n");
    b->state = GA10B_BRINGUP_ACR_RUNNING;
    return 0;
}

/* ---- Phase 2/3: FECS and GPCCS bootstrap ----
 *
 * ACR (phase 1) eagerly loads FECS and GPCCS ucode into their IMEM/DMEM
 * on GA10B — is_lazy_bootstrap = false for both in acr_sw_ga10b.c. After
 * ACR reports BOOT_OK, the GR Falcons are pre-loaded but halted. SLM-OS
 * issues STARTCPU on each and polls ctxsw_mailbox[0] for the PASS (=1)
 * sentinel that the ucode writes as its first readiness signal.
 *
 * Reference:
 *   ~/slmos-ref/nvidia/nvgpu-common-gr-gr_falcon.c:737-738  — start_gpccs/start_fecs
 *   ~/slmos-ref/nvidia/nvgpu-hal-gr-falcon-gr_falcon_ga10b_fusa.c — mailbox plumbing
 *
 * We split FECS and GPCCS into separate phases so the shell can invoke
 * them independently while iterating. The nvgpu driver issues them
 * back-to-back (start_gpccs, start_fecs, wait_ctxsw_ready); order isn't
 * load-bearing since each engine boots independently, but for phase
 * sequencing we do FECS first because it's the GR front-end. */

/* Poll a GR Falcon ctxsw mailbox[0] for any non-zero value, up to
 * `timeout_us`. Returns the mailbox value observed, or 0xFFFFFFFF on
 * timeout. The caller interprets the value (PASS=1, FAIL=2,
 * CSUM_FAIL=0x21, or any unexpected non-zero). The 1-µs loop body is
 * approximate (same as wait_for_halt_us — 1500 NOPs at ~1.5 GHz),
 * which is fine for coarse 2 s timeouts. */
static uint32_t wait_ctxsw_mailbox0_us(uint32_t mailbox0_reg,
                                       uint32_t timeout_us)
{
    uint32_t loops = timeout_us;
    while (loops-- > 0) {
        uint32_t v = bar0_r32(mailbox0_reg);
        if (v != 0u) return v;
        for (volatile int i = 0; i < 1500; i++) { }
    }
    return 0xFFFFFFFFu;
}

/* Dump the first 8 ctxsw mailboxes of a GR Falcon for diagnosis on
 * failure. Most debug info the ucode reports lives in mailboxes 1–7
 * (error code, line number, stall point). */
static void dump_gr_mailboxes(const char *tag, uint32_t base)
{
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t v = bar0_r32(base + i * 4u);
        uart_printf("[%s]   mailbox[%u]=0x%08lx\n",
                    tag, (unsigned)i, (unsigned long)v);
    }
}

int ga10b_bringup_fecs(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_ACR_RUNNING) return -1;

    /* Clear mailbox[0] so the PASS/FAIL poll sees the ucode's first
     * write rather than whatever stale value lingered across resets. */
    bar0_w32(GR_FECS_CTXSW_MAILBOX(0), 0u);
    gsp_platform->mb();

    /* Kick the FECS Falcon. CPUCTL.STARTCPU = bit 1 on GR Falcons. */
    bar0_w32(GR_FECS_CPUCTL, GR_CPUCTL_STARTCPU);
    gsp_platform->mb();
    uart_puts("[GA10B-FECS] STARTCPU kicked — polling ctxsw mailbox[0]\n");

    uint32_t result = wait_ctxsw_mailbox0_us(GR_FECS_CTXSW_MAILBOX(0),
                                             2u * 1000u * 1000u);
    if (result != GR_FECS_MAILBOX_PASS) {
        uart_printf("[GA10B-FECS] bootstrap did NOT reach PASS "
                    "(mailbox[0]=0x%08lx)\n", (unsigned long)result);
        dump_gr_mailboxes("GA10B-FECS", GR_FECS_CTXSW_MAILBOX(0));
        b->last_error_phase = 2;
        return -1;
    }

    uart_puts("[GA10B-FECS] bootstrap PASS\n");
    b->state = GA10B_BRINGUP_FECS_UP;
    return 0;
}

int ga10b_bringup_gpccs(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_FECS_UP) return -1;

    bar0_w32(GR_GPC0_GPCCS_CTXSW_MAILBOX(0), 0u);
    gsp_platform->mb();

    bar0_w32(GR_GPCCS_CPUCTL, GR_CPUCTL_STARTCPU);
    gsp_platform->mb();
    uart_puts("[GA10B-GPCCS] STARTCPU kicked — polling ctxsw mailbox[0]\n");

    uint32_t result = wait_ctxsw_mailbox0_us(GR_GPC0_GPCCS_CTXSW_MAILBOX(0),
                                             2u * 1000u * 1000u);
    if (result != GR_FECS_MAILBOX_PASS) {
        uart_printf("[GA10B-GPCCS] bootstrap did NOT reach PASS "
                    "(mailbox[0]=0x%08lx)\n", (unsigned long)result);
        dump_gr_mailboxes("GA10B-GPCCS", GR_GPC0_GPCCS_CTXSW_MAILBOX(0));
        b->last_error_phase = 3;
        return -1;
    }

    uart_puts("[GA10B-GPCCS] bootstrap PASS\n");
    b->state = GA10B_BRINGUP_GPCCS_UP;
    return 0;
}

/* ---- Phase 4: PMU bootstrap ----
 *
 * GA10B's PMU is optional for compute. nvgpu gates eager load on
 * `support_ls_pmu` — acr_sw_ga10b.c sets lazy_bootstrap=true when
 * support_ls_pmu is on, meaning ACR does NOT load PMU; something
 * else (lsfm) triggers it lazily. When support_ls_pmu is off
 * (GA10B default for Jetson L4T-r36.5), PMU is not loaded at all
 * and this phase is a no-op that just advances the state machine.
 *
 * If/when we wire PMU loading on (power management, thermal, PG),
 * this function grows to mirror the FECS pattern: STARTCPU on
 * PMU CPUCTL at NV_PPMU_BASE + 0x100, poll a ready sentinel. For
 * now we don't need PMU to submit a single NOP+SEMAPHORE method,
 * so skipping here is the pragmatic path.
 *
 * Reference: ~/slmos-ref/nvidia/nvgpu-common-acr-acr_sw_ga10b.c:417
 *   (lsf->is_lazy_bootstrap = g->support_ls_pmu ? true : false) */
int ga10b_bringup_pmu(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_GPCCS_UP) return -1;

    uart_puts("[GA10B-PMU] skipped — support_ls_pmu=false on GA10B; "
              "PMU not required for compute submission\n");
    b->state = GA10B_BRINGUP_PMU_UP;
    return 0;
}

/* ---- Inherit: detect Linux's bootstrapped state ----
 *
 * Path 3 of #190: after a no-suspend kexec from Linux, the GPU stays
 * powered and the Falcon security state is preserved. Linux's nvgpu
 * driver has already run ACR, FECS, and GPCCS to completion. Instead
 * of resetting the engines (which re-asserts HWCFG2 bit 13 priv-
 * lockdown), SLM-OS reads the Falcon state registers and verifies
 * that all three engines are halted-with-PASS:
 *
 *   - HWCFG2 bit 13 = 0  (PRI aperture unlocked)
 *   - FECS ctxsw_mailbox[0] = 1 (PASS)
 *   - GPCCS ctxsw_mailbox[0] = 1 (PASS)
 *
 * On success, the state machine jumps directly to PMU_UP, skipping
 * phases 1–4. This is the fast path after `slmos-kexec --no-gpu-suspend`.
 *
 * Requires: no runtime-PM suspend in the kexec helper (GPU must NOT
 * have been power-gated). If HWCFG2 shows lockdown, returns -1 and
 * the caller should fall back to the from-scratch ACR path (which
 * will also fail on locked hardware, but with better diagnostics).
 */

/* GA10B UFLUSH register block (BAR0+0x70000-0x70010), per
 * ~/slmos-ref/nvidia/nvgpu-hw-ga10b-hw_flush_ga10b.h.
 * All four registers share the layout: bit 0 = PENDING_BUSY (write 1
 * to start the op), bit 1 = OUTSTANDING_TRUE. Op completes when both
 * bits read back 0. */
#define GA10B_UFLUSH_FB_FLUSH              0x00070000u
#define GA10B_UFLUSH_L2_SYSMEM_INVALIDATE  0x00070004u
#define GA10B_UFLUSH_L2_FLUSH_DIRTY        0x00070010u
#define GA10B_UFLUSH_PENDING_BUSY          (1u << 0)
#define GA10B_UFLUSH_OUTSTANDING_TRUE      (1u << 1)
#define GA10B_UFLUSH_DONE_MASK \
    (GA10B_UFLUSH_PENDING_BUSY | GA10B_UFLUSH_OUTSTANDING_TRUE)

/* Issue one UFLUSH op and poll until PENDING_BUSY and OUTSTANDING_TRUE
 * both clear. Mirrors gk20a_mm_fb_flush / gk20a_mm_l2_invalidate /
 * gk20a_mm_l2_flush in nvgpu-hal-mm-cache-flush_gk20a_fusa.c — same
 * write-then-poll-both-bits pattern, same retry budget shape.
 *
 * Timeout: 100 retries × ~5 µs busy-wait = ~500 µs per op ceiling.
 * nvgpu uses 100 retries for FB_FLUSH, 200 for L2_INV, 2000 for
 * L2_FLUSH; 100 is sufficient for our use because we run this only
 * at inherit (no concurrent GPU traffic) so the op should complete
 * in <10 µs. Calibration of the inner busy-wait constant (7500
 * volatile-loop iterations ≈ 5 µs on Cortex-A78AE @ ~1.5 GHz) follows
 * the same scheme as `ga10b_gsp_engine_reset` (15 000 ≈ 10 µs).
 *
 * Returns 0 on success, -1 on timeout. */
static int ga10b_uflush_op(uint32_t reg, const char *name)
{
    bar0_w32(reg, GA10B_UFLUSH_PENDING_BUSY);
    gsp_platform->mb();

    for (int retries = 0; retries < 100; retries++) {
        uint32_t v = bar0_r32(reg);
        if ((v & GA10B_UFLUSH_DONE_MASK) == 0) {
            return 0;
        }
        for (volatile int i = 0; i < 7500; i++) { }  /* ~5 µs */
    }
    uart_printf("[GA10B-INHERIT] uflush %s timed out (reg=0x%lx, last=0x%lx)\n",
                name, (unsigned long)reg, (unsigned long)bar0_r32(reg));
    return -1;
}

/* Evict the GPU L2 of any clean lines inherited from Linux's nvgpu.
 *
 * After kexec, GA10B's L2 cache may still hold lines tagged for
 * physical addresses that nvgpu's MNIST helper wrote pre-handoff.
 * The first GPU dispatch in SLM-OS can read those stale lines
 * instead of fetching fresh data we wrote at the same GPU VA, which
 * surfaces as the iter-1 "got nvgpu's helper output" race (#596).
 *
 * Also called between dispatches via the 3-point coherency sites
 * added in #722 (set_input + pre-launch + post-launch). Same UFLUSH
 * sequence; same preconditions.
 *
 * Sequence per gv11b_mm_l2_flush in
 * ~/slmos-ref/nvidia/nvgpu-hal-mm-cache-flush_gv11b_fusa.c
 * (and kmemsysCacheOp_GM200 in OGKM):
 *
 *   1. fb_flush   — drain pending sysmem writes into L2
 *   2. l2_flush_dirty       — write any dirty L2 lines back to DRAM
 *   3. l2_sysmem_invalidate — invalidate clean L2 lines (this is the
 *                             one that reaches the stale nvgpu data)
 *   4. fb_flush   — second sysmembar so DRAM is the source of truth
 *
 * "For GK20A, an explicit sysmembar flush is needed before L2 cache
 * flush operation. Refer GK20A LTC IAS (section 5.5)" — kmemsysCacheOp_GM200
 * comment. GK20A is the predecessor Tegra GPU; the same ordering applies
 * to GA10B.
 *
 * Preconditions (all callers must hold):
 *   - `g_gpu_dispatch_lock` IRQ-off (UFLUSH ops are not re-entrant
 *     against concurrent dispatch from another context)
 *   - GPU is quiescent on the active channel — either pre-handoff
 *     (inherit), or after a prior dispatch's sema fired and before
 *     the next submit's USERD GP_PUT write. The 100-retry budget in
 *     `ga10b_uflush_op` was sized for this; if the GPU has in-flight
 *     work targeting the same memory addresses, the L2_FLUSH_DIRTY
 *     op may need the larger 2000-retry budget nvgpu uses.
 *
 * Caller logging policy: callers use `(void)ga10b_l2_evict_sysmem()`
 * when a failure produces a visible-stale next-call result that the
 * operator can detect (set_input, pre-launch). The post-launch site
 * surfaces failures explicitly because a timeout there silently
 * returns stale logits to the FFI caller — there's no follow-up
 * call to make the staleness visible.
 *
 * Lock-hold cost: 4 ops × ~500 µs/op ≈ 2 ms IRQ-off worst case if
 * every op hits its retry limit. Typical run is ~40 µs total (each
 * op completes in <10 µs per `ga10b_uflush_op`'s comment). The
 * 3-point-evict pattern in #722 multiplies this by 3 per dispatch:
 * ~120 µs typical / ~6 ms worst case. Bounded and acceptable for
 * MNIST workloads; not the right shape for SLM forward-path latency
 * (see docs/gpu-qmd-per-dispatch-plan.md §Status). */
int ga10b_l2_evict_sysmem(void)
{
    if (!gsp_platform) return -1;

    int rc;
    rc = ga10b_uflush_op(GA10B_UFLUSH_FB_FLUSH, "fb_flush[1]");
    if (rc < 0) return rc;
    rc = ga10b_uflush_op(GA10B_UFLUSH_L2_FLUSH_DIRTY, "l2_flush_dirty");
    if (rc < 0) return rc;
    rc = ga10b_uflush_op(GA10B_UFLUSH_L2_SYSMEM_INVALIDATE, "l2_sysmem_invalidate");
    if (rc < 0) return rc;
    rc = ga10b_uflush_op(GA10B_UFLUSH_FB_FLUSH, "fb_flush[2]");
    return rc;
}

int ga10b_bringup_inherit(struct ga10b_bringup *b)
{
    if (!b) return -1;
    memset(b, 0, sizeof(*b));
    b->state = GA10B_BRINGUP_INIT;
    b->last_error_phase = -1;

    if (!gsp_platform) {
        uart_puts("[GA10B-INHERIT] no platform ops installed\n");
        return -1;
    }

    /* Check HWCFG2 bit 13 — the priv-lockdown gate. */
    uint32_t hwcfg2 = bar0_r32(NV_PGSP_BASE + FALCON_HWCFG2);
    uint32_t bit13 = (hwcfg2 >> 13) & 1u;
    uart_printf("[GA10B-INHERIT] HWCFG2=0x%08lx bit13=%lu\n",
                (unsigned long)hwcfg2, (unsigned long)bit13);

    if (bit13 != 0) {
        uart_puts("[GA10B-INHERIT] FAIL — priv-lockdown is asserted. "
                  "Was --no-gpu-suspend used?\n");
        return -1;
    }

    /* Read FECS and GPCCS ctxsw mailboxes. */
    uint32_t fecs_mbox0  = bar0_r32(GR_FECS_CTXSW_MAILBOX(0));
    uint32_t gpccs_mbox0 = bar0_r32(GR_GPC0_GPCCS_CTXSW_MAILBOX(0));
    uint32_t fecs_cpuctl  = bar0_r32(GR_FECS_CPUCTL);
    uint32_t gpccs_cpuctl = bar0_r32(GR_GPCCS_CPUCTL);
    uint32_t gsp_cpuctl   = bar0_r32(NV_PGSP_BASE + 0x100u);

    uart_printf("[GA10B-INHERIT] GSP  CPUCTL=0x%08lx\n",
                (unsigned long)gsp_cpuctl);
    uart_printf("[GA10B-INHERIT] FECS CPUCTL=0x%08lx mailbox[0]=0x%08lx\n",
                (unsigned long)fecs_cpuctl, (unsigned long)fecs_mbox0);
    uart_printf("[GA10B-INHERIT] GPCCS CPUCTL=0x%08lx mailbox[0]=0x%08lx\n",
                (unsigned long)gpccs_cpuctl, (unsigned long)gpccs_mbox0);

    if (fecs_mbox0 != GR_FECS_MAILBOX_PASS) {
        uart_printf("[GA10B-INHERIT] FECS not ready (expected 1, got 0x%lx)\n",
                    (unsigned long)fecs_mbox0);
        return -1;
    }
    if (gpccs_mbox0 != GR_FECS_MAILBOX_PASS) {
        uart_printf("[GA10B-INHERIT] GPCCS not ready (expected 1, got 0x%lx)\n",
                    (unsigned long)gpccs_mbox0);
        return -1;
    }

    uart_puts("[GA10B-INHERIT] Linux left ACR/FECS/GPCCS in PASS state — "
              "skipping phases 1-4\n");

    /* Evict L2 lines inherited from Linux's nvgpu before any SLM-OS
     * dispatch reads through them. See ga10b_l2_evict_sysmem header
     * for the sequence (#596 iter-1 stale-read race). Non-fatal: a
     * timeout here would still let the rest of bringup proceed —
     * caller sees the iter-1 race come back instead of a hard fail. */
    if (ga10b_l2_evict_sysmem() < 0) {
        uart_puts("[GA10B-INHERIT] L2 evict timed out — first dispatch may "
                  "still see stale nvgpu data\n");
    } else {
        uart_puts("[GA10B-INHERIT] L2 evict done (FB_FLUSH + L2_FLUSH_DIRTY + "
                  "L2_SYSMEM_INVALIDATE + FB_FLUSH)\n");
    }

    b->state = GA10B_BRINGUP_PMU_UP;
    return 0;
}

/* ---- Phase 5: FECS method gateway smoke test ----
 *
 * Submit DISCOVER_IMAGE_SIZE to FECS via the method push registers.
 * No channel, GMMU, or page tables needed. This is the minimal proof
 * that the GPU's GR engine is alive and responsive to SLM-OS commands
 * after the inherit path.
 *
 * Protocol (per nvgpu gm20b_gr_falcon_submit_fecs_method_op):
 *   1. Clear ctxsw_mailbox[0] (GA10B: read-modify-write to zero)
 *   2. Write method data to GR_FECS_METHOD_DATA (0x409500)
 *   3. Write method address to GR_FECS_METHOD_PUSH (0x409504)
 *   4. Poll ctxsw_mailbox[0] for non-zero response
 *
 * DISCOVER_IMAGE_SIZE returns the GR context image size in bytes.
 * Any non-zero value proves FECS processed the method. A zero after
 * timeout means FECS is unresponsive.
 */
static int fecs_submit_method(uint32_t method_addr, uint32_t method_data,
                              uint32_t *out_result, uint32_t timeout_us)
{
    /* Step 1: Clear ctxsw_mailbox[0] so the poll sees FECS's first
     * write rather than a stale value. GA10B's mailbox is a plain
     * read/write register (no write-to-clear hardware), so writing 0
     * directly is sufficient. */
    bar0_w32(GR_FECS_CTXSW_MAILBOX(0), 0u);
    gsp_platform->mb();

    /* Step 2: Write method data. */
    bar0_w32(GR_FECS_METHOD_DATA, method_data);

    /* Step 3: Write method address — triggers FECS processing. */
    bar0_w32(GR_FECS_METHOD_PUSH, method_addr);
    gsp_platform->mb();

    /* Step 4: Poll mailbox[0] for non-zero response. */
    uint32_t loops = timeout_us;
    while (loops-- > 0) {
        uint32_t v = bar0_r32(GR_FECS_CTXSW_MAILBOX(0));
        if (v != 0u) {
            if (out_result) *out_result = v;
            return 0;
        }
        for (volatile int i = 0; i < 1500; i++) { }
    }
    return -1;  /* timeout */
}

int ga10b_bringup_address_space(struct ga10b_bringup *b)
{
    if (!b || b->state != GA10B_BRINGUP_PMU_UP) return -1;

    uart_puts("[GA10B-P5] FECS method gateway smoke test\n");

    /* Submit DISCOVER_IMAGE_SIZE — the simplest no-channel method.
     * Returns the GR context image size in mailbox[0]. */
    uint32_t image_size = 0;
    int rc = fecs_submit_method(FECS_METHOD_DISCOVER_IMAGE_SIZE,
                                FECS_METHOD_NULL_DATA,
                                &image_size, 2u * 1000u * 1000u);

    if (rc < 0) {
        uart_puts("[GA10B-P5] FECS method TIMEOUT — ucode unresponsive\n");
        /* Dump some diagnostic state. */
        uint32_t mbox0 = bar0_r32(GR_FECS_CTXSW_MAILBOX(0));
        uint32_t cpuctl = bar0_r32(GR_FECS_CPUCTL);
        uart_printf("[GA10B-P5]   FECS CPUCTL=0x%08lx mailbox[0]=0x%08lx\n",
                    (unsigned long)cpuctl, (unsigned long)mbox0);
        b->last_error_phase = 5;
        return -1;
    }

    uart_printf("[GA10B-P5] FECS DISCOVER_IMAGE_SIZE = %lu bytes (0x%lx)\n",
                (unsigned long)image_size, (unsigned long)image_size);

    if (image_size == 0 || image_size == 0xDEADCA11u) {
        uart_puts("[GA10B-P5] suspicious result — FECS may have echoed "
                  "the null sentinel\n");
        b->last_error_phase = 5;
        return -1;
    }

    uart_puts("[GA10B-P5] GPU GR engine is alive — FECS responded to "
              "SLM-OS method\n");
    b->state = GA10B_BRINGUP_ENGINES_READY;
    return 0;
}

/* ---- Phase 6: Inherit channel from Linux ----
 *
 * Channel creation from scratch at EL2 would require reimplementing
 * the nvgpu kernel driver (TSG open, channel bind, ALLOC_AS,
 * SETUP_BIND, nvmap, runlist programming). Rather than port that,
 * a Linux-side helper creates the channel via nvgpu ioctls and
 * writes the channel metadata (USERD, GPFIFO, pushbuffer, semaphore,
 * doorbell token) to a dmabuf in DRAM. SLM-OS scans DRAM for the
 * handoff magic after a --no-gpu-suspend kexec and uses the values
 * verbatim. Note: BAR0 itself is accessible at EL2 — the blocker is
 * the kernel-side ioctl surface, not a hardware firewall. An early
 * read of the commit 70d2a94 firewall map reported NV_USERMODE
 * blocked; that was a misinterpretation (wrong offset + misread of
 * the GPU's "no register here" 0xbadf response).
 *
 * This function validates the handoff block and stores the channel
 * addresses in the bringup struct for Phase 7 (method submission).
 */

#include "ga10b_channel_handoff.h"

/* Cached handoff data for use by phase 7.
 *
 * Single-channel only: each invocation of `nvgpu channel` overwrites
 * this. The shell-driven flow is inherently sequential (prepare →
 * inherit → channel → submit), so this is fine. If a future caller
 * needs multiple inherited channels, promote this to a per-channel
 * struct passed through b->.
 *
 * Visibility: `static` in production, file-scope under
 * SLM_HOST_HARNESS so the test harness can drive
 * input_buf_phys/size synthetically without simulating the full
 * Phase-6 inherit path. Production code does not declare an extern
 * for this symbol. */
#ifdef SLM_HOST_HARNESS
struct ga10b_channel_handoff g_handoff;
#else
static struct ga10b_channel_handoff g_handoff;
#endif

/* Per-channel QMD-pool round-robin counter. Used by the v7 dispatch
 * path in `ga10b_bringup_launch_kernel` to pick a fresh slot for
 * each launch. Persists across launches (modulo pool_n_slots) so
 * consecutive dispatches use distinct pool offsets — that's what
 * forces SKED to redecode each time and makes the per-QMD
 * INVALIDATE_*_CACHE bits actually fire (issue #558).
 *
 * Single-channel design today; if SLM-OS ever supports multiple
 * concurrent channels, this becomes a per-channel field. The slot
 * counter is local to the dispatch-loop body — host tests cover
 * the counter advancement directly via `ga10b_qmd_pool_prepare`
 * (which takes its own slot_inout pointer), so this static does
 * not need extern visibility for tests. */
static uint32_t g_qmd_pool_next_slot;

/* Scan a physical-memory range for the handoff magic, at the given
 * stride. Returns the address of the first match, or 0 if not found.
 * The stride and range are parameters so the host tests can drive this
 * against a mocked buffer; production callers use
 * GA10B_HANDOFF_SCAN_START/END from the handoff header. */
uint64_t ga10b_find_handoff_in_range(uint64_t start, uint64_t end,
                                     uint64_t stride)
{
    for (uint64_t p = start; p < end; p += stride) {
        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)p;
        if (*w == GA10B_CHANNEL_HANDOFF_MAGIC) {
            return p;
        }
    }
    return 0;
}

/* Validate a candidate handoff block. Returns 0 on success, -1 if
 * the magic, version, addresses, or GPFIFO entry count are invalid.
 * Pure-logic function — no MMIO, no globals. Host-testable. */
int ga10b_validate_handoff(const struct ga10b_channel_handoff *h)
{
    if (!h) return -1;
    if (h->magic != GA10B_CHANNEL_HANDOFF_MAGIC) return -1;
    /* v2: channel-only layout consumed by Phase 6/7.
     * v3: + compute-kernel launch state (shader/cbuf/qmd/output).
     * v4: + expected_payload so launch_kernel can target any kernel,
     *      not just one that writes 0xCAFE.
     * v5: + multi-op pipeline pointer for chaining N kernel dispatches
     *      (model inference path).
     * v6: + input_buf_phys/size so SLM-OS can swap the model's input
     *      tensor at runtime (per-image MNIST classification).
     * v7: + per-dispatch QMD pool (qmd_pool_phys/gpu_va/size_bytes/
     *      n_slots). Prefix layout is identical to v6, so the
     *      address/size/payload checks below all apply unchanged.
     *      The v7-specific tail fields are validated later at the
     *      dispatch path via `ga10b_v7_validate_handoff`. Without
     *      v7 in this list the scanner rejects v7 handoffs and
     *      reports "Handoff not found" even though the magic+kind
     *      match — the bug that blocked end-to-end v7 inherit on
     *      hardware until 2026-05-07.
     * Phase 6 inherit accepts all six; launch_kernel version-gates
     * at dispatch time (v3 minimum for single-shot, v5 for pipelines,
     * v7 for the QMD-pool path). */
    if (h->version != 2 && h->version != 3 &&
        h->version != 4 && h->version != 5 && h->version != 6 &&
        h->version != 7) return -1;
    if (h->userd_phys == 0 || h->gpfifo_phys == 0 ||
        h->pushbuf_phys == 0 || h->semaphore_phys == 0) return -1;
    if (h->work_submit_token == 0) return -1;
    /* gpfifo_entries must be a non-zero power of two. */
    if (h->gpfifo_entries == 0 ||
        (h->gpfifo_entries & (h->gpfifo_entries - 1)) != 0) return -1;
    /* GPFIFO and pushbuffer-builder helpers encode the upper VA byte
     * as `(va >> 32) & 0xFF`, giving 40 bits of address. Reject any
     * handoff whose VAs spill past that — silent truncation produces
     * a "PBDMA advanced but nothing executed" failure mode that is
     * very hard to diagnose. v3+ adds qmd/sem; check whichever fields
     * the version provides. */
    if (h->pushbuf_gpu_va >= (1ULL << 40)) return -1;
    if (h->semaphore_gpu_va >= (1ULL << 40)) return -1;
    if (h->version >= 3) {
        if (h->qmd_gpu_va >= (1ULL << 40)) return -1;
    }
    return 0;
}

/* Production scan — covers the IOVMM heap range where nvmap allocates
 * on GA10B. Originally 0x100000000..0x180000000 based on early helper
 * placements; kernel-launch helper (gpu-kernel-launch.c) allocates
 * more buffers and nvmap has been observed to place the handoff
 * dmabuf above 0x180000000 (e.g. 0x191523000 on 2026-04-21 with the
 * v3 helper). Widened to 0x200000000 (8 GB cap = physical RAM
 * ceiling on Orin Nano) to cover the full nvmap pool. ~300 ms scan
 * on hardware.
 *
 * PRECONDITION: the Jetson VMM must identity-map DRAM through at
 * least 0x200000000 as cacheable Normal memory. Today this is true
 * (Jetson's PMM/VMM maps the full 6.7 GB of non-ECC DRAM). If a
 * future VMM change skips any 4 KB page in the scan range, this
 * function will take a synchronous data abort with no recovery. */
/* Public, host-testable kind-aware scanner. Walks the same DRAM
 * range as `ga10b_find_handoff_in_range` but skips matches whose
 * pipeline_kind doesn't equal `wanted_kind`. Each magic-matching
 * candidate is also passed through `ga10b_validate_handoff` so a
 * pseudorandom 32-bit collision with the magic doesn't masquerade
 * as a real handoff. Returns 0 if no kind-matching handoff exists.
 * PR-3 of gpu-policy-models.md.
 *
 * Pure-logic — no MMIO, no globals. Pinned by host tests in
 * host-tools/gsp-harness/test_ga10b_bringup.c. */
uint64_t ga10b_find_handoff_of_kind_in_range(uint64_t start, uint64_t end,
                                             uint64_t stride,
                                             uint32_t wanted_kind)
{
    uint64_t cursor = start;
    while (cursor < end) {
        uint64_t found = ga10b_find_handoff_in_range(cursor, end, stride);
        if (found == 0) return 0;
        const struct ga10b_channel_handoff *h =
            (const struct ga10b_channel_handoff *)(uintptr_t)found;
        if (ga10b_validate_handoff(h) == 0 &&
            h->pipeline_kind == wanted_kind) {
            return found;
        }
        /* Magic matched but validation failed or kind didn't match;
         * advance past this match and keep scanning. */
        cursor = found + stride;
    }
    return 0;
}

static uint64_t find_handoff_scan_kind(uint32_t wanted_kind)
{
    return ga10b_find_handoff_of_kind_in_range(0x100000000ULL,
                                                0x200000000ULL,
                                                4096, wanted_kind);
}

int ga10b_bringup_channel(struct ga10b_bringup *b)
{
    /* Default: MNIST kind. Preserves the pre-PR-3 contract for
     * existing callers (slm_gpu_run_mnist, the bringup-test shell
     * cmd, host harness tests). */
    return ga10b_bringup_channel_kind(b, GA10B_PIPELINE_KIND_MNIST);
}

int ga10b_bringup_channel_kind(struct ga10b_bringup *b, uint32_t wanted_kind)
{
    if (!b) return -1;
    /* Accept either PMU_UP (after inherit, skip Phase 5) or
     * ENGINES_READY (after Phase 5 FECS method test). */
    if (b->state != GA10B_BRINGUP_ENGINES_READY &&
        b->state != GA10B_BRINGUP_PMU_UP) return -1;

    uart_printf("[GA10B-P6] Scanning DRAM for handoff magic (kind=%u)...\n",
                (unsigned)wanted_kind);
    uint64_t handoff_phys = find_handoff_scan_kind(wanted_kind);
    if (handoff_phys == 0) {
        uart_printf("[GA10B-P6] Handoff (kind=%u) not found. Was the Linux "
                    "helper run?\n", (unsigned)wanted_kind);
        b->last_error_phase = 6;
        return -1;
    }
    uart_printf("[GA10B-P6] Found handoff at phys 0x%lx (kind=%u)\n",
                (unsigned long)handoff_phys, (unsigned)wanted_kind);

    /* Read the handoff structure from the discovered location. */
    volatile struct ga10b_channel_handoff *hoff =
        (volatile struct ga10b_channel_handoff *)(uintptr_t)handoff_phys;

    /* Copy to a local (non-volatile) struct for easier access. */
    g_handoff.magic              = hoff->magic;
    g_handoff.version            = hoff->version;
    g_handoff.channel_id         = hoff->channel_id;
    g_handoff.tsg_id             = hoff->tsg_id;
    g_handoff.userd_phys         = hoff->userd_phys;
    g_handoff.userd_gp_put_offset = hoff->userd_gp_put_offset;
    g_handoff.userd_gp_get_offset = hoff->userd_gp_get_offset;
    g_handoff.gpfifo_phys        = hoff->gpfifo_phys;
    g_handoff.gpfifo_gpu_va      = hoff->gpfifo_gpu_va;
    g_handoff.gpfifo_entries     = hoff->gpfifo_entries;
    g_handoff.gpfifo_entry_size  = hoff->gpfifo_entry_size;
    g_handoff.pushbuf_phys       = hoff->pushbuf_phys;
    g_handoff.pushbuf_gpu_va     = hoff->pushbuf_gpu_va;
    g_handoff.pushbuf_size       = hoff->pushbuf_size;
    g_handoff.semaphore_phys     = hoff->semaphore_phys;
    g_handoff.semaphore_gpu_va   = hoff->semaphore_gpu_va;
    g_handoff.inst_block_phys    = hoff->inst_block_phys;
    g_handoff.initial_gp_put     = hoff->initial_gp_put;
    g_handoff.initial_gp_get     = hoff->initial_gp_get;
    g_handoff.work_submit_token  = hoff->work_submit_token;

    /* v3 extension: kernel launch state. Safe to read unconditionally
     * because the on-disk struct is always 192 bytes (static_assert
     * in the header), but only meaningful when version==3 — v2
     * helpers leave these fields zero, which both Phase 8's version
     * gate and the explicit non-zero checks in launch_kernel reject. */
    g_handoff.shader_phys        = hoff->shader_phys;
    g_handoff.shader_gpu_va      = hoff->shader_gpu_va;
    g_handoff.cbuf_phys          = hoff->cbuf_phys;
    g_handoff.cbuf_gpu_va        = hoff->cbuf_gpu_va;
    g_handoff.qmd_phys           = hoff->qmd_phys;
    g_handoff.qmd_gpu_va         = hoff->qmd_gpu_va;
    g_handoff.output_phys        = hoff->output_phys;
    g_handoff.output_gpu_va      = hoff->output_gpu_va;
    g_handoff.shader_size        = hoff->shader_size;
    g_handoff.cbuf_size          = hoff->cbuf_size;

    /* v4 extension. Safe to read unconditionally — on v2/v3 the
     * helper leaves the field zero. launch_kernel falls back to
     * GA10B_SMOKETEST_SEM_PAYLOAD when expected_payload is zero. */
    g_handoff.expected_payload   = hoff->expected_payload;

    /* v5 extension: pipeline pointer + count. Zero on v2/v3/v4. */
    g_handoff.pipeline_n_ops     = hoff->pipeline_n_ops;
    g_handoff.pipeline_ops_phys  = hoff->pipeline_ops_phys;

    /* v6 extension: runtime input buffer. Zero on v2..v5. SLM-OS's
     * slm_gpu_set_mnist_input writes user-supplied bytes to
     * input_buf_phys (with cache_clean) before the next dispatch. */
    g_handoff.input_buf_phys     = hoff->input_buf_phys;
    g_handoff.input_buf_size     = hoff->input_buf_size;

    /* PR-3 extension: pipeline-kind discriminator. Validated above
     * (find_handoff_scan_kind only returns matches), but we copy it
     * into g_handoff so launch_kernel can re-check at dispatch time
     * for defense-in-depth. */
    g_handoff.pipeline_kind      = hoff->pipeline_kind;

    /* v7 extension: per-dispatch QMD pool descriptor. Zero on v2..v6.
     * Without these copies `ga10b_handoff_is_v7` returns false even
     * when the helper published v7, so `ga10b_bringup_launch_kernel`
     * falls through to the v5/v6 path and submits dispatches against
     * the v7 ops' empty `qmd_gpu_va` fields — observed on hardware
     * (issue #710) as "GP_GET advanced but payload didn't land" at
     * the first op. The receiver is unconditionally version-7-shaped
     * (the `_Static_assert sizeof(...) == 256` covers it), so the
     * unconditional read is safe; older helpers leave these zero
     * and `is_v7` correctly returns false. */
    g_handoff.qmd_pool_phys      = hoff->qmd_pool_phys;
    g_handoff.qmd_pool_gpu_va    = hoff->qmd_pool_gpu_va;
    g_handoff.qmd_pool_size_bytes = hoff->qmd_pool_size_bytes;
    g_handoff.qmd_pool_n_slots   = hoff->qmd_pool_n_slots;

    /* Validate the handoff block (pure-logic, host-testable). */
    if (ga10b_validate_handoff((const struct ga10b_channel_handoff *)
                                &g_handoff) < 0) {
        uart_printf("[GA10B-P6] handoff validation FAILED: "
                    "magic=0x%08lx version=%lu entries=%lu "
                    "userd=0x%lx gpfifo=0x%lx pb=0x%lx sem=0x%lx\n",
                    (unsigned long)g_handoff.magic,
                    (unsigned long)g_handoff.version,
                    (unsigned long)g_handoff.gpfifo_entries,
                    (unsigned long)g_handoff.userd_phys,
                    (unsigned long)g_handoff.gpfifo_phys,
                    (unsigned long)g_handoff.pushbuf_phys,
                    (unsigned long)g_handoff.semaphore_phys);
        uart_puts("[GA10B-P6] Did the Linux helper run before kexec?\n");
        b->last_error_phase = 6;
        return -1;
    }

    uart_printf("[GA10B-P6] channel=%lu tsg=%lu\n",
                (unsigned long)g_handoff.channel_id,
                (unsigned long)g_handoff.tsg_id);
    uart_printf("[GA10B-P6] userd_phys=0x%lx gp_put_off=%lu gp_get_off=%lu\n",
                (unsigned long)g_handoff.userd_phys,
                (unsigned long)g_handoff.userd_gp_put_offset,
                (unsigned long)g_handoff.userd_gp_get_offset);
    uart_printf("[GA10B-P6] gpfifo_phys=0x%lx gpu_va=0x%lx entries=%lu\n",
                (unsigned long)g_handoff.gpfifo_phys,
                (unsigned long)g_handoff.gpfifo_gpu_va,
                (unsigned long)g_handoff.gpfifo_entries);
    uart_printf("[GA10B-P6] pushbuf_phys=0x%lx gpu_va=0x%lx size=%lu\n",
                (unsigned long)g_handoff.pushbuf_phys,
                (unsigned long)g_handoff.pushbuf_gpu_va,
                (unsigned long)g_handoff.pushbuf_size);
    uart_printf("[GA10B-P6] semaphore_phys=0x%lx gpu_va=0x%lx\n",
                (unsigned long)g_handoff.semaphore_phys,
                (unsigned long)g_handoff.semaphore_gpu_va);
    uart_printf("[GA10B-P6] initial gp_put=%lu gp_get=%lu\n",
                (unsigned long)g_handoff.initial_gp_put,
                (unsigned long)g_handoff.initial_gp_get);

    uart_puts("[GA10B-P6] channel handoff valid — inherited from Linux\n");
    b->state = GA10B_BRINGUP_CHANNEL_OPEN;
    return 0;
}

uint32_t ga10b_bringup_active_pipeline_kind(void)
{
    /* Reads the global g_handoff. Safe — single-threaded shell
     * task is the only caller path today (slm_ffi.c's
     * ensure_*_bringup helpers). */
    return g_handoff.pipeline_kind;
}

const struct ga10b_channel_handoff *ga10b_bringup_handoff(void)
{
    /* g_handoff is zero-initialised; magic stays 0 until the
     * channel-inherit phase copies a validated block in. Use that
     * as the "loaded" signal so callers can detect the no-handoff
     * case without inspecting individual fields. */
    if (g_handoff.magic != GA10B_CHANNEL_HANDOFF_MAGIC) {
        return NULL;
    }
    return &g_handoff;
}

/* ---- Phase 7: Pushbuffer submission (NOP + SEMAPHORE_RELEASE) ----
 *
 * Write a minimal pushbuffer (NOP method + SEMAPHORE_RELEASE), add
 * a GPFIFO entry pointing to it, advance GP_PUT in USERD, and poll
 * the semaphore for completion. This is the end-to-end proof that
 * the inherited channel is live and PBDMA is consuming our work.
 *
 * GPFIFO entry format (8 bytes, from hw_pbdma_ga10b.h):
 *   word 0: [31:2] = gpu_va >> 2, [1] = priv, [0] = entry_type (PB=0)
 *   word 1: [30:10] = length (dwords), [7:0] = va[39:32],
 *           [31] = sync (1 = wait for idle)
 *
 * Pushbuffer uses host-semaphore methods (see NVC56F_* macros near
 * the top of this file) — decoded directly by PBDMA, so no engine
 * class binding / SET_OBJECT is required for the inherit path.
 *
 * Success criterion: GP_GET advances (PBDMA consumed our GPFIFO
 * entry). The semaphore payload is logged as a diagnostic — it
 * fires only when the compute context is actually executing, which
 * depends on channel state beyond our current ioctl-layer match
 * (tracked in issue #273). PR #269 shipped with the same criterion.
 */
uint32_t ga10b_build_sema_release_pushbuffer(uint32_t *pb,
                                             uint64_t sem_gpu_va,
                                             uint32_t payload)
{
    /* Five INC+count=1 method headers, each followed by one data
     * dword. The layout matches nvgpu's gv11b_sema_add_incr_cmd
     * (drivers/gpu/nvgpu/hal/sync/sema_cmdbuf_gv11b.c:41-101) exactly.
     * SEM_ADDR_LO goes first despite convention because that's the
     * order the gv11b helper emits. */
    pb[0] = NVC56F_METHOD_HEADER_INC(1, 0, NVC56F_SEM_ADDR_LO);
    pb[1] = (uint32_t)(sem_gpu_va & 0xFFFFFFFFu);
    pb[2] = NVC56F_METHOD_HEADER_INC(1, 0, NVC56F_SEM_ADDR_HI);
    pb[3] = (uint32_t)((sem_gpu_va >> 32) & 0xFFu);
    pb[4] = NVC56F_METHOD_HEADER_INC(1, 0, NVC56F_SEM_PAYLOAD_LO);
    pb[5] = payload;
    pb[6] = NVC56F_METHOD_HEADER_INC(1, 0, NVC56F_SEM_PAYLOAD_HI);
    pb[7] = 0u;                  /* 32-bit release: hi word ignored */
    pb[8] = NVC56F_METHOD_HEADER_INC(1, 0, NVC56F_SEM_EXECUTE);
    /* OPERATION=RELEASE; PAYLOAD_SIZE (bit 24) and RELEASE_WFI (bit
     * 20) left at default 0 — matches nvgpu's literal `0x1` on a
     * non-wfi incr_cmd. See NVC56F_SEM_EXECUTE_* defines above. */
    pb[9] = NVC56F_SEM_EXECUTE_OP_RELEASE;
    return GA10B_SEMA_RELEASE_PB_DWORDS;
}

uint32_t ga10b_build_compute_sema_release_pushbuffer(uint32_t *pb,
                                                     uint64_t sem_gpu_va,
                                                     uint32_t payload)
{
    /* First pair: SET_OBJECT binding AMPERE_COMPUTE_B to subch 1.
     *
     * NVK pins per-class subchannel assignments in nv_push.h: graphics
     * classes (0x9097 … 0xC797) bind to subch 0; compute classes
     * (0x90C0 … 0xC7C0 AMPERE_COMPUTE_B) bind to subch 1. Issue #273
     * root-caused to binding 0xC7C0 on subch 0 here: nvgpu's
     * validate_class_veid_pbdma saw an invalid (class=COMPUTE_B,
     * subch=0, veid>=1 from ASYNC subcontext) tuple and raised GR FE
     * CLASS_SUBCH_MISMATCH before any semaphore method reached the
     * engine. Subch 1 matches what NVK / CUDA emit. */
    pb[0]  = NVC56F_METHOD_HEADER_INC(1, 1, NVC56F_SET_OBJECT);
    pb[1]  = GA10B_AMPERE_COMPUTE_B_CLASS_ID;

    /* Payload first (lower then upper), then address, then execute —
     * matches the order the REPORT_SEMAPHORE_* methods are offset in
     * clc7c0.h. Structure-size ONE_WORD means the GPU writes only the
     * 32-bit payload at the target address (no timestamp/16-byte
     * struct). OP=RELEASE fires the write once execute is dispatched.
     * All methods on subch 1 to match the SET_OBJECT binding above. */
    pb[2]  = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER);
    pb[3]  = payload;
    pb[4]  = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_UPPER);
    pb[5]  = 0u;     /* 32-bit release: hi payload ignored */
    pb[6]  = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_LOWER);
    pb[7]  = (uint32_t)(sem_gpu_va & 0xFFFFFFFFu);
    pb[8]  = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_UPPER);
    pb[9]  = (uint32_t)((sem_gpu_va >> 32) & 0xFFu);
    pb[10] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_REPORT_SEMAPHORE_EXECUTE);
    pb[11] = NVC7C0_SEM_EXECUTE_OP_RELEASE |
             NVC7C0_SEM_EXECUTE_STRUCTURE_SIZE_ONE_WORD;
    return GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS;
}

/* Helper for IMMD-opcode method headers (SEC_OP = 4, 13-bit data
 * field, subch at [15:13], method at [12:0]). The host-family builder
 * above uses NVC56F_METHOD_HEADER_INC for increment-op methods; this
 * is the complementary constructor for inline-immediate values.
 * Kept static — the only caller is the kernel-launch builder below. */
static inline uint32_t ga10b_hdr_immd(uint32_t subch, uint32_t byte_off,
                                      uint32_t data)
{
    return (4u << 29) | ((data & 0x1FFFu) << 16) |
           ((subch & 0x7u) << 13) | (((byte_off) >> 2) & 0x1FFFu);
}

uint32_t ga10b_build_launch_kernel_pushbuffer(uint32_t *pb,
                                              uint64_t qmd_gpu_va)
{
    /* Matches the Linux helper scripts/gpu-kernel-launch.c's dispatch
     * pushbuffer exactly. Subch 1 = compute per NVK's nv_push.h and
     * the #273 fix. The 4 prelude methods (SET_OBJECT + two windows +
     * two cache invalidates) reproduce nvk_push_dispatch_state_init +
     * nvk_cmd_buffer_begin_compute. They're emitted every launch
     * because channel state resets across kexec cycles are
     * conservative; steady-state these writes are cheap and harmless. */
    pb[0]  = NVC56F_METHOD_HEADER_INC(1, 1, NVC56F_SET_OBJECT);
    pb[1]  = GA10B_AMPERE_COMPUTE_B_CLASS_ID;

    pb[2]  = NVC56F_METHOD_HEADER_INC(2, 1,
                                      NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A);
    pb[3]  = 0u;                                /* upper 17 bits */
    pb[4]  = NVC7C0_SHARED_MEMORY_WINDOW_BASE;  /* lower 32 */

    pb[5]  = NVC56F_METHOD_HEADER_INC(2, 1,
                                      NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A);
    pb[6]  = 0u;
    pb[7]  = NVC7C0_LOCAL_MEMORY_WINDOW_BASE;

    pb[8]  = ga10b_hdr_immd(1, NVC7C0_INVALIDATE_SKED_CACHES, 0);
    pb[9]  = ga10b_hdr_immd(1, NVC7C0_INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI, 0);

    /* INVALIDATE_SHADER_CACHES — Why: Phase 6 hardware verification
     * showed that even with per-dispatch QMD rotation, the GPU's
     * shader-side caches (instruction, data, constant, locks) still
     * carried stale entries from the prior launch on the same channel.
     * The deterministic off-by-one was eliminated by per-dispatch QMD
     * (PR #574/#577); the residual intermittent staleness was
     * coherency at the shader-cache level, not the SKED level. With
     * `gpu debug on` (uart_printf between launches) staleness vanished
     * because the printf path inserted enough delay for the caches to
     * naturally drain. This explicit invalidate makes the dispatch
     * pushbuffer self-sufficient. */
    pb[10] = ga10b_hdr_immd(1, NVC7C0_INVALIDATE_SHADER_CACHES,
                            NVC7C0_INVALIDATE_SHADER_CACHES_ALL);

    pb[11] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SEND_PCAS_A);
    pb[12] = (uint32_t)(qmd_gpu_va >> 8);

    /* PCAS2_B on Ampere with INVALIDATE_COPY_SCHEDULE. The Turing-era
     * SEND_SIGNALING_PCAS_B (at 0x02bc) silently no-ops dispatch on
     * GA10B — see pb builder docstring in the header. */
    pb[13] = ga10b_hdr_immd(1, NVC7C0_SEND_SIGNALING_PCAS2_B,
                            NVC7C0_SEND_SIGNALING_PCAS2_B_ACTION_INVALIDATE_COPY_SCHEDULE);

    return GA10B_LAUNCH_KERNEL_PB_DWORDS;
}

uint32_t ga10b_build_launch_kernel_with_sema_pushbuffer(uint32_t *pb,
                                                         uint64_t qmd_gpu_va,
                                                         uint64_t sem_gpu_va,
                                                         uint32_t payload)
{
    /* First 14 dwords: same as the plain launch_kernel pushbuffer. */
    (void)ga10b_build_launch_kernel_pushbuffer(pb, qmd_gpu_va);

    /* Trailing 10 dwords: REPORT_SEMAPHORE_PAYLOAD/ADDRESS/EXECUTE
     * on subch 1 (already bound to AMPERE_COMPUTE_B by the prelude).
     * REPORT_SEMAPHORE_EXECUTE with OP=RELEASE waits for the prior
     * compute work on this channel to drain, flushes L2 → DRAM, then
     * stores `payload` at sem_gpu_va. Same encoding as
     * ga10b_build_compute_sema_release_pushbuffer's tail (which we
     * use for the smoke test). */
    pb[14] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_LOWER);
    pb[15] = payload;
    pb[16] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_PAYLOAD_UPPER);
    pb[17] = 0u;     /* 32-bit release; high payload ignored */
    pb[18] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_LOWER);
    pb[19] = (uint32_t)(sem_gpu_va & 0xFFFFFFFFu);
    pb[20] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_SET_REPORT_SEMAPHORE_ADDRESS_UPPER);
    pb[21] = (uint32_t)((sem_gpu_va >> 32) & 0xFFu);
    pb[22] = NVC56F_METHOD_HEADER_INC(1, 1, NVC7C0_REPORT_SEMAPHORE_EXECUTE);
    pb[23] = NVC7C0_SEM_EXECUTE_OP_RELEASE |
             NVC7C0_SEM_EXECUTE_STRUCTURE_SIZE_ONE_WORD;
    return GA10B_LAUNCH_KERNEL_SEMA_PB_DWORDS;
}

/* Copy a freshly-built pushbuffer into the inherited pb_phys region,
 * post a GPFIFO entry pointing at it, advance GP_PUT, ring the USERMODE
 * doorbell, and poll `poll_phys` for `expected_payload`.
 *
 * `poll_phys` is the CPU-physical target the GPU writes via the
 * submitted pushbuffer — `g_handoff.semaphore_phys` for the Phase 7
 * host-family and COMPUTE_B SEMAPHORE_RELEASE builders (both of
 * which encode the sema VA into the pushbuffer), or
 * `g_handoff.output_phys` for the Phase 8 compute-kernel launch
 * (where the shader itself stores the payload via STG.E to a VA
 * that maps to output_phys).
 *
 * `expected_payload` is what the GPU is supposed to write. For
 * Phase 7 sema release this is always GA10B_SMOKETEST_SEM_PAYLOAD
 * (0xCAFE). For Phase 8 launch_kernel it's whatever the v4 handoff
 * says the kernel produces, or GA10B_SMOKETEST_SEM_PAYLOAD as a
 * fallback for v3 handoffs. Making it a parameter rather than a
 * hard-coded constant is what v4 unlocks — SLM-OS can now
 * dispatch any kernel, not just write_cafe.
 *
 * `error_phase` is written into `b->last_error_phase` on failure so
 * the caller's phase-number logging stays accurate (Phase 7 for
 * sema, Phase 8 for launch).
 *
 * `tag` is the logging prefix (e.g. "GA10B-P7", "GA10B-P7C",
 * "GA10B-P8").
 *
 * Success criterion is strict: the payload must land at `poll_phys`
 * within the 2 s timeout. `gp_advanced && !payload_matched` returns
 * -1 — PBDMA consumed the pushbuffer but the GPU did not produce
 * the expected side effect. That's the "silent no-op" failure mode
 * (e.g. PCAS_B on Ampere silently dropping dispatch) and must not
 * be reported as success; an earlier loose criterion was a
 * now-resolved #273 workaround and has been dropped.
 *
 * GPFIFO bookkeeping: ALWAYS advance `g_handoff.initial_gp_put` /
 * `initial_gp_get` on return, regardless of `gp_advanced` or
 * `payload_matched`. Once the GPFIFO entry has been written and
 * USERD GP_PUT has been advanced, the slot is consumed from the
 * channel's hardware perspective — a subsequent submit MUST target
 * the next slot, or it would re-write the same USERD value (a
 * no-op for PBDMA) and overwrite the in-flight entry. The earlier
 * `if (gp_advanced)` guard caused a permanent wedge in #596 when
 * the 2 s poll fired before PBDMA's GP_GET caught up. See the
 * in-function comment for the full reasoning.
 *
 * Future architectural follow-up: refactor the v7 pipeline
 * dispatcher to nvgpu's bulk pattern (queue all op entries, write
 * USERD GP_PUT once with the cumulative new value, ring doorbell
 * once, poll only the final semaphore). Eliminates per-op PBDMA
 * pressure and matches the path NVIDIA tests at scale. The
 * bookkeeping fix here remains load-bearing for any single-op
 * caller (smoke tests, debug paths). */
static int ga10b_submit_and_poll(struct ga10b_bringup *b,
                                 const uint32_t *pb_buf,
                                 uint32_t pb_dwords,
                                 uint64_t poll_phys,
                                 uint32_t expected_payload,
                                 int error_phase,
                                 const char *tag)
{
    /* Bound pb_dwords against the inherited pushbuffer size before any
     * write. Today's callers cap at GA10B_LAUNCH_KERNEL_SEMA_PB_DWORDS
     * (= 24), but a future caller passing a larger value would
     * overflow g_handoff.pushbuf_phys. Cheap up-front check.
     *
     * Note: this and the 40-bit-VA check below intentionally use
     * unconditional `uart_printf`, not GA10B_DBG. Dispatch refusals
     * are operator-actionable failures that must be visible without
     * the verbose flag — the same pattern used by the
     * `pipeline op N malformed` and `pipeline op N failed` paths
     * later in this file. */
    if ((uint64_t)pb_dwords * 4u > g_handoff.pushbuf_size) {
        uart_printf("[%s] submit refused: pb_dwords=%lu exceeds "
                    "pushbuf size=%lu bytes\n",
                    tag,
                    (unsigned long)pb_dwords,
                    (unsigned long)g_handoff.pushbuf_size);
        return error_phase;
    }
    uint32_t pb_bytes = pb_dwords * 4u;

    /* GPFIFO encoding (below) packs the GPU VA into 32 + 8 bits = 40
     * bits of address. Reject any handoff whose pushbuf VA spills past
     * that without complaining loudly — silent truncation produces
     * "PBDMA advanced but nothing executed" mysteries. */
    if (g_handoff.pushbuf_gpu_va >= (1ULL << 40)) {
        uart_printf("[%s] submit refused: pushbuf_gpu_va=0x%llx "
                    "exceeds 40-bit GPFIFO encoding\n",
                    tag,
                    (unsigned long long)g_handoff.pushbuf_gpu_va);
        return error_phase;
    }

    /* Zero the poll target so a post-submit non-zero read is
     * unambiguous proof the GPU wrote the payload (not residue
     * from a prior run or from the Linux helper's pre-kexec
     * validation). */
    volatile uint32_t *poll = (volatile uint32_t *)(uintptr_t)poll_phys;
    *poll = 0;
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean((const void *)poll, sizeof(uint32_t));
    }
    gsp_platform->mb();

    GA10B_DBG("[%s] poll target at phys 0x%lx cleared to 0 "
              "(expected payload=0x%lx)\n",
              tag,
              (unsigned long)poll_phys,
              (unsigned long)expected_payload);

    volatile uint32_t *pb = (volatile uint32_t *)(uintptr_t)
        g_handoff.pushbuf_phys;
    for (uint32_t i = 0; i < pb_dwords; i++) pb[i] = pb_buf[i];

    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean((const void *)pb, pb_bytes);
    }
    gsp_platform->mb();

    /* GPFIFO entry (Ampere format, 8 bytes): see ga10b_bringup.h. */
    uint64_t pb_gpu_va = g_handoff.pushbuf_gpu_va;
    uint32_t gp_entry0 = (uint32_t)(pb_gpu_va & 0xFFFFFFFCu);
    uint32_t gp_entry1 = (uint32_t)((pb_gpu_va >> 32) & 0xFFu) |
                         (pb_dwords << 10);

    uint32_t gp_put = g_handoff.initial_gp_put;
    uint32_t gp_idx = gp_put & (g_handoff.gpfifo_entries - 1);
    volatile uint64_t *gpfifo = (volatile uint64_t *)(uintptr_t)
        g_handoff.gpfifo_phys;
    uint64_t entry = ((uint64_t)gp_entry1 << 32) | gp_entry0;
    gpfifo[gp_idx] = entry;
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean((const void *)&gpfifo[gp_idx],
                                  sizeof(uint64_t));
    }
    gsp_platform->mb();

    GA10B_DBG("[%s] GPFIFO[%lu] = 0x%08lx_%08lx (pb_va=0x%lx, %lu bytes)\n",
              tag,
              (unsigned long)gp_idx,
              (unsigned long)gp_entry1,
              (unsigned long)gp_entry0,
              (unsigned long)pb_gpu_va,
              (unsigned long)pb_bytes);

    /* Write the MASKED gp_put to USERD — see ga10b_next_gp_put header
     * doc in ga10b_channel_handoff.h for the why (nvgpu compatibility,
     * #601 wedge). Pinned in
     * test_ga10b_bringup.c:test_next_gp_put_wraps_at_ring_boundary. */
    uint32_t new_gp_put = ga10b_next_gp_put(gp_put, g_handoff.gpfifo_entries);
    volatile uint32_t *userd = (volatile uint32_t *)(uintptr_t)
        g_handoff.userd_phys;
    uint32_t gp_put_word = g_handoff.userd_gp_put_offset / 4;
    userd[gp_put_word] = new_gp_put;
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean((const void *)&userd[gp_put_word],
                                  sizeof(uint32_t));
    }
    gsp_platform->mb();

    GA10B_DBG("[%s] GP_PUT advanced: %lu → %lu (USERD word %lu)\n",
              tag,
              (unsigned long)gp_put,
              (unsigned long)new_gp_put,
              (unsigned long)gp_put_word);

    volatile uint32_t *doorbell =
        (volatile uint32_t *)(uintptr_t)GA10B_USERMODE_DOORBELL_PHYS;
    *doorbell = g_handoff.work_submit_token;
    gsp_platform->mb();
    GA10B_DBG("[%s] doorbell 0x%lx <- 0x%08lx\n",
              tag,
              (unsigned long)GA10B_USERMODE_DOORBELL_PHYS,
              (unsigned long)g_handoff.work_submit_token);

    GA10B_DBG("[%s] polling (2s timeout)...\n", tag);
    uint32_t poll_val = 0;
    for (uint32_t us = 0; us < GA10B_DISPATCH_POLL_TIMEOUT_US; us++) {
        if (gsp_platform->cache_invalidate) {
            gsp_platform->cache_invalidate((void *)poll, sizeof(uint32_t));
        }
        poll_val = *poll;
        /* Mode selector lives in ga10b_channel_handoff.h so host
         * tests can pin the exact predicate without re-encoding it
         * (and the Linux launcher uses the same helper). */
        if (ga10b_poll_match(poll_val, expected_payload)) break;
        ga10b_dispatch_poll_delay_us();
    }

    uint32_t gp_get_word = g_handoff.userd_gp_get_offset / 4;
    if (gsp_platform->cache_invalidate) {
        gsp_platform->cache_invalidate((void *)&userd[gp_get_word],
                                       sizeof(uint32_t));
    }
    uint32_t final_gp_get = userd[gp_get_word];

    GA10B_DBG("[%s] result: poll=0x%08lx GP_GET=%lu (was %lu)\n",
              tag,
              (unsigned long)poll_val,
              (unsigned long)final_gp_get,
              (unsigned long)g_handoff.initial_gp_get);

    /* gp_advanced is diagnostic-only after the #596 fix — used to
     * discriminate the two failure modes in the error log below
     * ("PBDMA didn't see our submit" vs "PBDMA consumed it but no
     * payload"). The bookkeeping below no longer depends on it. */
    bool gp_advanced     = (final_gp_get != g_handoff.initial_gp_get);
    bool payload_matched = ga10b_poll_match(poll_val, expected_payload);

    /* Bookkeeping: ALWAYS advance the cached put pointer, even when
     * `gp_advanced` is false. Reasoning:
     *
     *   - The GPFIFO entry has already been written to slot `gp_idx`
     *     in the gpfifo[gp_idx] = entry write earlier in this
     *     function. Reusing that slot on the next call would
     *     overwrite an in-flight entry.
     *   - The USERD GP_PUT register has already been written to
     *     `new_gp_put`. PBDMA's view of "next slot to fetch" is
     *     advanced regardless of whether it has YET acted on it.
     *     A subsequent submit that re-writes the SAME USERD GP_PUT
     *     value (because the cached put didn't advance) is a no-op
     *     from PBDMA's perspective — the wedge mode that surfaced as
     *     "iter 1 op 8 fails, iter 2+ op 1 fails forever" in #596.
     *   - For `gp_advanced=false`, `final_gp_get == initial_gp_get`,
     *     so the assignment to `initial_gp_get` is a no-op there;
     *     keep both lines together for symmetry. */
    g_handoff.initial_gp_put = new_gp_put;
    g_handoff.initial_gp_get = final_gp_get;

    if (payload_matched) {
        GA10B_DBG("[%s] payload observed — GPU executed submit.\n", tag);
        b->state = GA10B_BRINGUP_METHOD_ACCEPTED;
        return 0;
    }

    if (gp_advanced) {
        /* Generic diagnostic for the silent-no-op class: PBDMA
         * consumed the pushbuffer but the expected side effect
         * (semaphore release / kernel store) never landed. Caller-
         * specific hypotheses (host-family sema encoding,
         * COMPUTE_B class/subchannel binding, Ampere
         * PCAS2_B/PCAS_B dispatch-kick selection, QMD shader
         * address, etc.) are logged by the caller if it has more
         * context than this helper does. */
        uart_printf("[%s] GP_GET advanced but payload didn't land "
                    "(got 0x%lx, want 0x%lx) — PBDMA consumed our "
                    "submit but the GPU didn't produce the expected "
                    "side effect. Inspect the caller's pushbuffer "
                    "encoding.\n",
                    tag,
                    (unsigned long)poll_val,
                    (unsigned long)expected_payload);
    } else {
        uart_printf("[%s] GP_GET did not advance — PBDMA didn't see "
                    "our submit\n", tag);
    }
    b->last_error_phase = error_phase;
    return -1;
}

int ga10b_bringup_smoke_test(struct ga10b_bringup *b)
{
    /* Accept CHANNEL_OPEN (first submit) or METHOD_ACCEPTED (after a
     * prior submit) — symmetric with smoke_test_compute. Re-submitting
     * on the same channel is valid because submit_and_poll bumps
     * g_handoff.initial_gp_put to the next GPFIFO slot on PBDMA
     * progress, so back-to-back submits target distinct slots. */
    if (!b || (b->state != GA10B_BRINGUP_CHANNEL_OPEN &&
               b->state != GA10B_BRINGUP_METHOD_ACCEPTED)) return -1;

    uart_puts("[GA10B-P7] pushbuffer smoke test — writing GPFIFO entry\n");

    /* Host-family SEMAPHORE_RELEASE (Volta+ method indices 0x17..0x1b).
     * PBDMA-decoded, bypasses GR entirely — doesn't require any class
     * binding. See ga10b_build_sema_release_pushbuffer() for encoding.
     * The GPU writes the payload to semaphore_gpu_va, which maps to
     * semaphore_phys on the CPU side — that's our poll target. */
    uint32_t pb_buf[GA10B_SEMA_RELEASE_PB_DWORDS];
    uint32_t pb_dwords = ga10b_build_sema_release_pushbuffer(
        pb_buf, g_handoff.semaphore_gpu_va, GA10B_SMOKETEST_SEM_PAYLOAD);

    /* Phase 7 host-family always writes GA10B_SMOKETEST_SEM_PAYLOAD
     * — the encoding is hard-coded in the SEMAPHORE_RELEASE
     * pushbuffer. */
    return ga10b_submit_and_poll(b, pb_buf, pb_dwords,
                                 g_handoff.semaphore_phys,
                                 GA10B_SMOKETEST_SEM_PAYLOAD,
                                 7, "GA10B-P7");
}

int ga10b_bringup_smoke_test_compute(struct ga10b_bringup *b)
{
    /* Accept CHANNEL_OPEN (first submit) or METHOD_ACCEPTED (after a
     * prior host-family submit) — a compute smoke test is valid on a
     * channel that's either fresh-inherited or has already passed the
     * host-family check. */
    if (!b || (b->state != GA10B_BRINGUP_CHANNEL_OPEN &&
               b->state != GA10B_BRINGUP_METHOD_ACCEPTED)) return -1;

    uart_puts("[GA10B-P7C] compute pushbuffer smoke test — "
              "AMPERE_COMPUTE_B SEMAPHORE_RELEASE\n");

    /* Compute-class pushbuffer: SET_OBJECT(COMPUTE_B on subch 1) +
     * REPORT_SEMAPHORE methods at clc7c0.h byte offsets. See
     * ga10b_build_compute_sema_release_pushbuffer() for encoding.
     *
     * Issue #291 filed suspecting MME_FE1 was a missing-MME-init
     * blocker; subsequent investigation (Linux gpu-compute-smoke
     * 2026-04-21) showed the exception was a symptom of the
     * pre-PR#295 header-encoding bug where byte offsets were
     * placed at bits [11:0] instead of method_id at [12:0]. Under
     * the broken encoding, COMPUTE_B methods decoded as method_ids
     * beyond the class's defined range, and MME's method-track
     * front-end faulted on the unknown method. With the corrected
     * encoding the compute SEMAPHORE_RELEASE fires without any MME
     * IRAM upload. No NVK-style MME init needed. */
    uint32_t pb_buf[GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS];
    uint32_t pb_dwords = ga10b_build_compute_sema_release_pushbuffer(
        pb_buf, g_handoff.semaphore_gpu_va, GA10B_SMOKETEST_SEM_PAYLOAD);

    /* Phase 7 COMPUTE_B also uses the fixed payload — the shader
     * is the SEMAPHORE_RELEASE method itself, not arbitrary code. */
    return ga10b_submit_and_poll(b, pb_buf, pb_dwords,
                                 g_handoff.semaphore_phys,
                                 GA10B_SMOKETEST_SEM_PAYLOAD,
                                 7, "GA10B-P7C");
}

/* v7 per-dispatch QMD pipeline — BULK SUBMIT pattern.
 *
 * Called from `ga10b_bringup_launch_kernel` after
 * `ga10b_handoff_is_v7` returns true. Validates the v7 fields,
 * authors fresh QMDs for all ops, queues their pushbuffers + a
 * trailing semaphore release into the GPFIFO, and kicks PBDMA
 * with a single GP_PUT write + a single doorbell ring. Returns 0
 * on chain success, -1 on validation / dispatch failure (with
 * b->last_error_phase = 8).
 *
 * Why bulk: the previous per-op dispatcher called
 * `ga10b_submit_and_poll` once per op — N pushbuffer writes, N
 * GPFIFO entries, N USERD GP_PUT writes, N doorbell rings, N
 * 2-second polls. That stressed PBDMA's USERD/doorbell path with
 * N updates per inference (8 per MNIST chain) and was the failure
 * mode behind the wedge in #596. The bulk pattern matches what
 * nvgpu's `nvgpu_submit_prepare_gpfifo_track` /
 * `gv11b_userd_gp_put` do at scale — append all GPFIFO entries
 * for a logical submit, then advance USERD GP_PUT and ring the
 * doorbell exactly once. PBDMA stays in its known-good operating
 * regime; no per-op pressure.
 *
 * Layout in the pushbuffer area (`g_handoff.pushbuf_phys`,
 * size = pushbuf_size, default 65 536 B on the helper):
 *
 *     +---------------------------------------------------+
 *     | op[0] launch_kernel pb (14 dwords = 56 B)         |
 *     | op[1] launch_kernel pb (14 dwords = 56 B)         |
 *     | ...                                               |
 *     | op[N-1] launch_kernel pb (14 dwords = 56 B)       |
 *     | trailing COMPUTE_B sema-release pb (12 dw = 48 B) |
 *     +---------------------------------------------------+
 *
 * Each op's pushbuffer is a SEND_PCAS2_B-kicked dispatch only
 * (no per-op semaphore). The GPU's compute engine processes them
 * in order on subch 1. The trailing entry uses
 * NVC7C0_REPORT_SEMAPHORE with STRUCTURE_SIZE_ONE_WORD and the
 * default FLUSH_DISABLE=0, which (per Mesa NVK
 * `nvk_dispatch_signal_semaphore`) waits for the compute pipeline
 * to drain and flushes L2 → DRAM before the release write
 * becomes visible. So when the poll matches, every preceding
 * kernel has retired AND its outputs are in coherent DRAM —
 * exactly the synchronization Bug C / #596 was missing.
 *
 * Failure mode: a poll timeout means somewhere in the chain
 * stalled — but unlike the per-op path, we lose per-op error
 * localization. If a regression needs that, re-run with
 * `gpu debug on` to see which slot's submit didn't fire its
 * barrier. Diagnostic granularity is the cost; structural
 * robustness on PBDMA is the win.
 *
 * Reference: `nvgpu-common-fifo-submit.c::nvgpu_submit_prepare_gpfifo_track`
 * (lines 392-413, 572) and `nvgpu-hal-fifo-userd_gv11b.c::gv11b_userd_gp_put`
 * (lines 67-77) in the reference cache.
 *
 * Host testability: the bounds-check paths have dedicated
 * coverage via `ga10b_v7_validate_handoff`. The dispatch-loop
 * body is exercised indirectly through the `ga10b_qmd_pool_prepare`
 * + `ga10b_build_*_pushbuffer` tests. Making the bulk dispatch
 * directly host-testable is still tracked under #580 (refactor to
 * take handoff + slot counter as parameters and a stub-submit
 * function pointer).
 */
static int ga10b_dispatch_v7_pipeline(struct ga10b_bringup *b)
{
    enum ga10b_v7_validation_error verr =
        ga10b_v7_validate_handoff(&g_handoff);
    if (verr != GA10B_V7_OK) {
        switch (verr) {
        case GA10B_V7_ERR_OPS_PHYS_ZERO:
            uart_puts("[GA10B-P8-v7] pipeline_n_ops > 0 but "
                      "pipeline_ops_phys is zero\n");
            break;
        case GA10B_V7_ERR_OPS_EXCEED_CAP:
            uart_printf("[GA10B-P8-v7] pipeline_n_ops=%lu exceeds "
                        "GA10B_PIPELINE_V7_MAX_OPS=%u — refusing "
                        "to dispatch (handoff likely corrupt)\n",
                        (unsigned long)g_handoff.pipeline_n_ops,
                        (unsigned)GA10B_PIPELINE_V7_MAX_OPS);
            break;
        case GA10B_V7_ERR_POOL_SIZE_INSUFFICIENT:
            uart_printf("[GA10B-P8-v7] qmd_pool_size_bytes=%lu < "
                        "qmd_pool_n_slots=%lu × %u — handoff "
                        "inconsistent\n",
                        (unsigned long)g_handoff.qmd_pool_size_bytes,
                        (unsigned long)g_handoff.qmd_pool_n_slots,
                        (unsigned)GA10B_QMD_SIZE_BYTES);
            break;
        case GA10B_V7_ERR_NULL_HANDOFF:
            /* Unreachable in production — `ga10b_handoff_is_v7`
             * already null-checks and we only enter this branch
             * when it returned true. Defensive log so a future
             * caller that skips `is_v7` still surfaces the bug
             * instead of dispatching with garbage. */
            uart_puts("[GA10B-P8-v7] handoff pointer is NULL — "
                      "validate-after-is_v7 contract violated\n");
            break;
        case GA10B_V7_OK:
            /* Unreachable — the outer `if (verr != OK)` rejects
             * this case. Keeping the case label so a future
             * addition to the enum prompts a -Wswitch warning. */
            break;
        }
        b->last_error_phase = 8;
        return -1;
    }
    /* Capacity check: N kernel-dispatch entries + 1 trailing sema
     * entry must fit comfortably in the GPFIFO ring AND the
     * pushbuffer area. nvgpu reserves 2 extra entries per submit
     * (pre/post fence); we cap at half-ring as a safety margin
     * mirroring `check_gpfifo_capacity` (`EXTRA_GPFIFO_ENTRIES`
     * + headroom). With the post-#601 ring of 512, that allows up
     * to 255 ops per chain — vastly more than the ~8 a typical
     * MNIST or sched chain requires. */
    uint32_t n = g_handoff.pipeline_n_ops;
    uint32_t total_entries = n + 1u;
    if (total_entries > (g_handoff.gpfifo_entries / 2u)) {
        uart_printf("[GA10B-P8-v7] %lu entries (%lu ops + 1 sema) "
                    "exceeds half-ring budget %lu (gpfifo_entries=%lu)\n",
                    (unsigned long)total_entries,
                    (unsigned long)n,
                    (unsigned long)(g_handoff.gpfifo_entries / 2u),
                    (unsigned long)g_handoff.gpfifo_entries);
        b->last_error_phase = 8;
        return -1;
    }

    uint32_t pb_kernel_bytes =
        GA10B_LAUNCH_KERNEL_PB_DWORDS * 4u;          /* 56 */
    uint32_t pb_sema_bytes =
        GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS * 4u;   /* 48 */
    uint64_t total_pb_bytes =
        (uint64_t)n * pb_kernel_bytes + pb_sema_bytes;
    if (total_pb_bytes > g_handoff.pushbuf_size) {
        uart_printf("[GA10B-P8-v7] %llu pushbuf bytes "
                    "(%lu ops × %u + %u sema) exceeds "
                    "pushbuf_size=%lu\n",
                    (unsigned long long)total_pb_bytes,
                    (unsigned long)n,
                    (unsigned)pb_kernel_bytes,
                    (unsigned)pb_sema_bytes,
                    (unsigned long)g_handoff.pushbuf_size);
        b->last_error_phase = 8;
        return -1;
    }

    /* CPU-physical → identity-mapped CPU VA on Jetson. Same
     * contract as the v3 dispatch fields and the v5/v6 ops array:
     * the helper allocates these via nvmap into the IOVMM heap,
     * where SMMU passthrough makes the CPU's view of the page
     * numerically equal to the physical address. Holds at EL2
     * with the unified DRAM map; would need an explicit
     * phys-to-virt translation on any platform that doesn't
     * identity-map. */
    const struct ga10b_pipeline_op_v7 *ops_v7 =
        (const struct ga10b_pipeline_op_v7 *)
            (uintptr_t)g_handoff.pipeline_ops_phys;
    uint8_t *pool_va =
        (uint8_t *)(uintptr_t)g_handoff.qmd_pool_phys;
    if (gsp_platform->cache_invalidate) {
        gsp_platform->cache_invalidate(
            (void *)ops_v7,
            (size_t)n * sizeof(*ops_v7));
    }

    /* Pre-dispatch L2 evict. set_input only evicts lines for the
     * input buffer; the dispatch path also reads cbuf, shader pages,
     * and the QMD pool slot — any of which may carry L2 lines from a
     * prior dispatch that were re-cached after set_input ran. Pre-
     * launch evict ensures SKED, the SASS instruction fetch, and the
     * SASS LDG path all see a clean L2 view of the channel-mapped
     * buffers on this dispatch's first read. ~40 µs (#715).
     *
     * Caller is `ga10b_bringup_launch_kernel`, which holds
     * `g_gpu_dispatch_lock` IRQ-off and is invoked between dispatches
     * (after the prior sema fired, before this submit's USERD GP_PUT
     * write) — so the GPU is quiescent on this channel and the
     * 100-retry UFLUSH budget in `ga10b_uflush_op` is sufficient. */
    (void)ga10b_l2_evict_sysmem();

    /* Pre-clear the poll target ONCE — the trailing sema entry is
     * the only writer. Pre-zero so a non-zero match below is
     * unambiguous proof the GPU wrote the payload. */
    uint64_t sema_gpu_va =
        g_handoff.semaphore_gpu_va + GA10B_SEMA_PAGE_OFFSET;
    uint64_t poll_phys =
        g_handoff.semaphore_phys + GA10B_SEMA_PAGE_OFFSET;
    volatile uint32_t *poll = (volatile uint32_t *)(uintptr_t)poll_phys;
    *poll = 0;
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean((const void *)poll, sizeof(uint32_t));
    }
    gsp_platform->mb();

    uint32_t gp_put_start = g_handoff.initial_gp_put;
    uint32_t ring_mask = g_handoff.gpfifo_entries - 1u;
    uint64_t pushbuf_phys = g_handoff.pushbuf_phys;
    uint64_t pushbuf_gpu_va = g_handoff.pushbuf_gpu_va;
    volatile uint64_t *gpfifo =
        (volatile uint64_t *)(uintptr_t)g_handoff.gpfifo_phys;

    GA10B_DBG("[GA10B-P8-v7] BULK %lu ops + 1 sema — gp_put=%lu, "
              "pushbuf_phys=0x%lx pool_phys=0x%lx\n",
              (unsigned long)n,
              (unsigned long)gp_put_start,
              (unsigned long)pushbuf_phys,
              (unsigned long)g_handoff.qmd_pool_phys);

    /* Phase 1: queue all N kernel-dispatch entries. */
    for (uint32_t i = 0; i < n; i++) {
        const struct ga10b_pipeline_op_v7 *op = &ops_v7[i];
        if (!ga10b_pipeline_op_is_valid(
                (const struct ga10b_pipeline_op *)op)) {
            uart_printf("[GA10B-P8-v7] op %lu malformed "
                        "(qmd=0x%lx out=0x%lx) — aborting\n",
                        (unsigned long)(i + 1),
                        (unsigned long)op->qmd_gpu_va,
                        (unsigned long)op->output_phys);
            b->last_error_phase = 8;
            return -1;
        }

        /* Author a fresh QMD into the next pool slot. */
        struct ga10b_qmd_pool_slot slot = ga10b_qmd_pool_prepare(
            pool_va,
            g_handoff.qmd_pool_gpu_va,
            g_handoff.qmd_pool_n_slots,
            &g_qmd_pool_next_slot,
            op);
        if (gsp_platform->cache_clean) {
            gsp_platform->cache_clean((const void *)slot.cpu_va,
                                      GA10B_QMD_SIZE_BYTES);
        }

        /* Build the dispatch-only pushbuffer at this op's slot in
         * the packed pushbuffer area. Each op gets pb_kernel_bytes
         * of space starting at offset i * pb_kernel_bytes. */
        uint64_t pb_op_phys =
            pushbuf_phys + (uint64_t)i * pb_kernel_bytes;
        uint64_t pb_op_gpu_va =
            pushbuf_gpu_va + (uint64_t)i * pb_kernel_bytes;
        uint32_t *pb_op_cpu = (uint32_t *)(uintptr_t)pb_op_phys;
        uint32_t pb_dwords = ga10b_build_launch_kernel_pushbuffer(
            pb_op_cpu, slot.gpu_va);

        /* GPFIFO entry: 8-byte Ampere format. gp_e0 is the lower
         * 32 bits of the pushbuffer GPU VA (PCB-aligned, so bits
         * [1:0] are zero); gp_e1 holds the upper 8 bits of the VA
         * + the pushbuffer length in dwords. */
        uint32_t gp_e0 = (uint32_t)(pb_op_gpu_va & 0xFFFFFFFCu);
        uint32_t gp_e1 = (uint32_t)((pb_op_gpu_va >> 32) & 0xFFu) |
                         (pb_dwords << 10);
        uint64_t entry = ((uint64_t)gp_e1 << 32) | gp_e0;
        uint32_t gp_idx = (gp_put_start + i) & ring_mask;
        gpfifo[gp_idx] = entry;

        GA10B_DBG("[GA10B-P8-v7]   queue op[%lu/%lu] slot=%lu "
                  "qmd_gpu_va=0x%lx pb_phys=0x%lx pb_gpu_va=0x%lx "
                  "gp_idx=%lu\n",
                  (unsigned long)(i + 1),
                  (unsigned long)n,
                  (unsigned long)slot.index,
                  (unsigned long)slot.gpu_va,
                  (unsigned long)pb_op_phys,
                  (unsigned long)pb_op_gpu_va,
                  (unsigned long)gp_idx);
    }

    /* Phase 2: queue the trailing COMPUTE_B REPORT_SEMAPHORE
     * entry. This fires AFTER all N preceding kernel dispatches
     * drain (FLUSH_DISABLE=0 + STRUCTURE_SIZE_ONE_WORD on the
     * REPORT_SEMAPHORE_EXECUTE method) so its release write is
     * the unambiguous "all kernels done, all outputs in DRAM"
     * signal. */
    uint64_t pb_sema_phys =
        pushbuf_phys + (uint64_t)n * pb_kernel_bytes;
    uint64_t pb_sema_gpu_va =
        pushbuf_gpu_va + (uint64_t)n * pb_kernel_bytes;
    uint32_t *pb_sema_cpu = (uint32_t *)(uintptr_t)pb_sema_phys;
    uint32_t sema_pb_dwords =
        ga10b_build_compute_sema_release_pushbuffer(
            pb_sema_cpu, sema_gpu_va, GA10B_SEMA_RELEASE_PAYLOAD);

    uint32_t s_e0 = (uint32_t)(pb_sema_gpu_va & 0xFFFFFFFCu);
    uint32_t s_e1 = (uint32_t)((pb_sema_gpu_va >> 32) & 0xFFu) |
                    (sema_pb_dwords << 10);
    uint64_t sema_entry = ((uint64_t)s_e1 << 32) | s_e0;
    uint32_t sema_gp_idx = (gp_put_start + n) & ring_mask;
    gpfifo[sema_gp_idx] = sema_entry;

    GA10B_DBG("[GA10B-P8-v7]   queue sema_release pb_phys=0x%lx "
              "pb_gpu_va=0x%lx gp_idx=%lu sema_gpu_va=0x%lx "
              "(payload=0x%x)\n",
              (unsigned long)pb_sema_phys,
              (unsigned long)pb_sema_gpu_va,
              (unsigned long)sema_gp_idx,
              (unsigned long)sema_gpu_va,
              (unsigned)GA10B_SEMA_RELEASE_PAYLOAD);

    /* Phase 3: cache-clean the entire pushbuffer area + the
     * GPFIFO entries we just wrote, then memory-barrier so PBDMA
     * sees the writes when we kick. The GPFIFO entries can span a
     * ring-wrap — handle the split case by cleaning two contiguous
     * runs. */
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean(
            (const void *)(uintptr_t)pushbuf_phys,
            (size_t)total_pb_bytes);

        uint32_t start_idx = gp_put_start & ring_mask;
        if ((uint64_t)start_idx + total_entries <=
                g_handoff.gpfifo_entries) {
            gsp_platform->cache_clean(
                (const void *)&gpfifo[start_idx],
                (size_t)total_entries * sizeof(uint64_t));
        } else {
            uint32_t first = g_handoff.gpfifo_entries - start_idx;
            uint32_t second = total_entries - first;
            gsp_platform->cache_clean(
                (const void *)&gpfifo[start_idx],
                (size_t)first * sizeof(uint64_t));
            gsp_platform->cache_clean(
                (const void *)&gpfifo[0],
                (size_t)second * sizeof(uint64_t));
        }
    }
    gsp_platform->mb();

    /* Phase 4: write USERD GP_PUT ONCE with the cumulative new
     * value, then ring the doorbell ONCE. This is the bulk-submit
     * win — PBDMA only sees one GP_PUT advance and one doorbell
     * for the entire chain. */
    uint32_t new_gp_put = (gp_put_start + total_entries) & ring_mask;
    volatile uint32_t *userd =
        (volatile uint32_t *)(uintptr_t)g_handoff.userd_phys;
    uint32_t gp_put_word = g_handoff.userd_gp_put_offset / 4u;
    userd[gp_put_word] = new_gp_put;
    if (gsp_platform->cache_clean) {
        gsp_platform->cache_clean(
            (const void *)&userd[gp_put_word], sizeof(uint32_t));
    }
    gsp_platform->mb();

    GA10B_DBG("[GA10B-P8-v7] GP_PUT advanced: %lu → %lu "
              "(total_entries=%lu)\n",
              (unsigned long)gp_put_start,
              (unsigned long)new_gp_put,
              (unsigned long)total_entries);

    volatile uint32_t *doorbell =
        (volatile uint32_t *)(uintptr_t)GA10B_USERMODE_DOORBELL_PHYS;
    *doorbell = g_handoff.work_submit_token;
    gsp_platform->mb();

    GA10B_DBG("[GA10B-P8-v7] doorbell rung — polling for sema "
              "(2s timeout)\n");

    /* Phase 5: poll the channel sema for the trailing release
     * payload. Same budget the per-op path used; see
     * GA10B_DISPATCH_POLL_TIMEOUT_US near the top of the file. */
    uint32_t poll_val = 0;
    for (uint32_t us = 0; us < GA10B_DISPATCH_POLL_TIMEOUT_US; us++) {
        if (gsp_platform->cache_invalidate) {
            gsp_platform->cache_invalidate(
                (void *)poll, sizeof(uint32_t));
        }
        poll_val = *poll;
        if (ga10b_poll_match(poll_val, GA10B_SEMA_RELEASE_PAYLOAD)) {
            break;
        }
        ga10b_dispatch_poll_delay_us();
    }

    /* Phase 6: bookkeeping. Same always-advance contract as
     * `ga10b_submit_and_poll` (#596 / PR #610): the GPFIFO entries
     * have been written and USERD GP_PUT has been advanced, so
     * the cached counters MUST move forward regardless of whether
     * the poll matched. Otherwise a subsequent call would re-write
     * the same USERD value (no-op for PBDMA) and overwrite the
     * in-flight entries. */
    uint32_t gp_get_word = g_handoff.userd_gp_get_offset / 4u;
    if (gsp_platform->cache_invalidate) {
        gsp_platform->cache_invalidate(
            (void *)&userd[gp_get_word], sizeof(uint32_t));
    }
    uint32_t final_gp_get = userd[gp_get_word];
    uint32_t prev_gp_get = g_handoff.initial_gp_get;
    g_handoff.initial_gp_put = new_gp_put;
    g_handoff.initial_gp_get = final_gp_get;

    bool payload_matched =
        ga10b_poll_match(poll_val, GA10B_SEMA_RELEASE_PAYLOAD);
    if (payload_matched) {
        GA10B_DBG("[GA10B-P8-v7] BULK pipeline complete — %lu ops "
                  "fired, GP_GET=%lu\n",
                  (unsigned long)n,
                  (unsigned long)final_gp_get);
        b->state = GA10B_BRINGUP_METHOD_ACCEPTED;
        return 0;
    }

    /* Diagnostic on timeout: discriminate "PBDMA didn't see our
     * submit at all" (gp_get unchanged) from "PBDMA consumed
     * entries but the trailing sema didn't fire" (gp_get advanced
     * but poll_val never landed). The second case usually means
     * one of the kernel dispatches faulted internally; re-run with
     * `gpu debug on` to see per-op QMD/pb authoring traces. */
    if (final_gp_get != prev_gp_get) {
        uart_printf("[GA10B-P8-v7] BULK poll timeout — PBDMA "
                    "advanced (GP_GET %lu → %lu) but sema didn't "
                    "fire (poll=0x%lx, want 0x%x)\n",
                    (unsigned long)prev_gp_get,
                    (unsigned long)final_gp_get,
                    (unsigned long)poll_val,
                    (unsigned)GA10B_SEMA_RELEASE_PAYLOAD);
    } else {
        uart_printf("[GA10B-P8-v7] BULK poll timeout — PBDMA "
                    "didn't see our submits (GP_GET still %lu)\n",
                    (unsigned long)final_gp_get);
    }
    b->last_error_phase = 8;
    return -1;
}

int ga10b_bringup_launch_kernel(struct ga10b_bringup *b)
{
    if (!b || (b->state != GA10B_BRINGUP_CHANNEL_OPEN &&
               b->state != GA10B_BRINGUP_METHOD_ACCEPTED)) return -1;

    if (g_handoff.version < 3) {
        uart_printf("[GA10B-P8] handoff version %lu — compute-kernel "
                    "launch requires v3 (helper must be run with "
                    "--preserve-for-kexec)\n",
                    (unsigned long)g_handoff.version);
        b->last_error_phase = 8;
        return -1;
    }

    /* v7 per-dispatch QMD path. When the helper has populated the v7
     * pool descriptor (version >= 7 + qmd_pool_n_slots > 0), each
     * op's QMD bytes are authored *here* per-launch into a slot of
     * the GMMU-mapped QMD pool, and the slot's GPU VA is what gets
     * fed into SEND_PCAS_A. Forces SKED to redecode each launch —
     * workaround for the off-by-one staleness that comes from
     * replaying byte-identical helper-baked QMDs. See
     * docs/gpu-qmd-per-dispatch-plan.md and #558.
     *
     * Falls through to the v5/v6 path on any v7 input that's
     * missing or out of range. The v6 path remains the default until
     * Phase 3 (helper-side pool allocation) lands. */
    if (ga10b_handoff_is_v7(&g_handoff)) {
        return ga10b_dispatch_v7_pipeline(b);
    }

    /* v5/v6 multi-op pipeline path (helper-baked QMDs). When
     * pipeline_n_ops > 0, the handoff carries an array of
     * struct ga10b_pipeline_op describing N dispatches to run in
     * sequence. Used by model-inference helpers (MNIST and beyond)
     * where each op's output is the next op's input. */
    if (g_handoff.version >= 5 && g_handoff.pipeline_n_ops > 0) {
        if (g_handoff.pipeline_ops_phys == 0) {
            uart_puts("[GA10B-P8] pipeline_n_ops > 0 but pipeline_ops_phys "
                      "is zero\n");
            b->last_error_phase = 8;
            return -1;
        }
        if (g_handoff.pipeline_n_ops > GA10B_PIPELINE_MAX_OPS) {
            uart_printf("[GA10B-P8] pipeline_n_ops=%lu exceeds "
                        "GA10B_PIPELINE_MAX_OPS=%u — refusing to "
                        "dispatch (handoff likely corrupt)\n",
                        (unsigned long)g_handoff.pipeline_n_ops,
                        (unsigned)GA10B_PIPELINE_MAX_OPS);
            b->last_error_phase = 8;
            return -1;
        }
        /* Cast assumes Jetson's identity DRAM mapping at EL2: the
         * physical address read from the handoff is also a valid
         * virtual address SLM-OS can dereference. Same assumption
         * the v3 path makes for shader_phys/qmd_phys. */
        const struct ga10b_pipeline_op *ops =
            (const struct ga10b_pipeline_op *)
                (uintptr_t)g_handoff.pipeline_ops_phys;
        /* Defensive: invalidate the array's cache range before the
         * first read. In practice this is a no-op (Linux's pre-
         * kexec msync cleaned the page; SLM-OS hasn't touched it
         * yet) but avoids depending on that timing for correctness
         * if a future change re-reads the array. */
        if (gsp_platform->cache_invalidate) {
            gsp_platform->cache_invalidate(
                (void *)ops,
                (size_t)g_handoff.pipeline_n_ops * sizeof(*ops));
        }
        GA10B_DBG("[GA10B-P8] pipeline mode — %lu ops, "
                  "ops_phys=0x%lx\n",
                  (unsigned long)g_handoff.pipeline_n_ops,
                  (unsigned long)g_handoff.pipeline_ops_phys);
        for (uint32_t i = 0; i < g_handoff.pipeline_n_ops; i++) {
            const struct ga10b_pipeline_op *op = &ops[i];
            if (!ga10b_pipeline_op_is_valid(op)) {
                uart_printf("[GA10B-P8] pipeline op %lu malformed "
                            "(qmd=0x%lx out=0x%lx) — aborting\n",
                            (unsigned long)(i + 1),
                            (unsigned long)op->qmd_gpu_va,
                            (unsigned long)op->output_phys);
                b->last_error_phase = 8;
                return -1;
            }
            /* Use the channel's semaphore as the per-op completion
             * signal instead of polling a value cell of the kernel's
             * output. AMPERE_COMPUTE_B's REPORT_SEMAPHORE_EXECUTE
             * with OP=RELEASE waits for the kernel to drain and
             * flushes L2 → DRAM before writing the semaphore — so
             * polling it gives a real "all output is in DRAM"
             * signal. Fixes both #372 (sentinel-zero deadlock) and
             * #390 (Conv1 truncation past 4 KB).
             *
             * GA10B_SEMA_PAGE_OFFSET (header) documents why the
             * per-op sema sits 2 KB into the channel-semaphore page:
             * the launcher's gpu_write_handoff_v6 aliases that page
             * with op 7's logits buffer, so we need a non-overlapping
             * offset within the same 4 KB allocation. */
            uint64_t sema_gpu_va =
                g_handoff.semaphore_gpu_va + GA10B_SEMA_PAGE_OFFSET;
            uint64_t sema_phys =
                g_handoff.semaphore_phys + GA10B_SEMA_PAGE_OFFSET;
            GA10B_DBG("[GA10B-P8]   op[%lu/%lu] qmd=0x%lx out=0x%lx "
                      "sema_phys=0x%lx (payload=0x%x)\n",
                      (unsigned long)(i + 1),
                      (unsigned long)g_handoff.pipeline_n_ops,
                      (unsigned long)op->qmd_gpu_va,
                      (unsigned long)op->output_phys,
                      (unsigned long)sema_phys,
                      (unsigned)GA10B_SEMA_RELEASE_PAYLOAD);
            uint32_t pb_buf[GA10B_LAUNCH_KERNEL_SEMA_PB_DWORDS];
            uint32_t pb_dwords =
                ga10b_build_launch_kernel_with_sema_pushbuffer(
                    pb_buf, op->qmd_gpu_va,
                    sema_gpu_va,
                    GA10B_SEMA_RELEASE_PAYLOAD);
            int rc = ga10b_submit_and_poll(b, pb_buf, pb_dwords,
                                           sema_phys,
                                           GA10B_SEMA_RELEASE_PAYLOAD,
                                           8, "GA10B-P8");
            if (rc < 0) {
                uart_printf("[GA10B-P8] pipeline op %lu failed (rc=%d) "
                            "— aborting chain\n",
                            (unsigned long)(i + 1), rc);
                return rc;
            }
        }
        GA10B_DBG("[GA10B-P8] pipeline complete — %lu ops fired\n",
                  (unsigned long)g_handoff.pipeline_n_ops);
        return 0;
    }

    /* Single-shot path (v3/v4). */
    if (g_handoff.qmd_gpu_va == 0 || g_handoff.output_phys == 0) {
        uart_puts("[GA10B-P8] handoff missing qmd_gpu_va/output_phys\n");
        b->last_error_phase = 8;
        return -1;
    }

    uart_puts("[GA10B-P8] compute-kernel launch — dispatching pre-"
              "uploaded QMD\n");
    uart_printf("[GA10B-P8]   qmd_gpu_va=0x%lx  output_phys=0x%lx\n",
                (unsigned long)g_handoff.qmd_gpu_va,
                (unsigned long)g_handoff.output_phys);

    /* Build the launch pushbuffer. The builder is pure logic so host
     * tests pin its bit layout; see host-tools/gsp-harness tests.
     *
     * The shader (uploaded pre-kexec by the Linux helper) stores the
     * payload via STG.E to a VA that maps to output_phys on the CPU
     * side — that's the poll target. Unlike the host-family and
     * COMPUTE_B SEMAPHORE_RELEASE paths (which point PBDMA at
     * semaphore_phys), the QMD path drives the SMs through the
     * shader code; the "side effect" is whatever the shader
     * writes. */
    uint32_t pb_buf[GA10B_LAUNCH_KERNEL_PB_DWORDS];
    uint32_t pb_dwords = ga10b_build_launch_kernel_pushbuffer(
        pb_buf, g_handoff.qmd_gpu_va);

    /* v4 handoffs carry a per-kernel expected payload; v3 handoffs
     * (and v4 handoffs with expected_payload==0) fall back to the
     * write_cafe constant so existing flows keep working. Predicate
     * lives in the shared header so host-test can pin the exact
     * selection rule without re-encoding it. */
    uint32_t expected = ga10b_pick_launch_payload(&g_handoff,
                                                  GA10B_SMOKETEST_SEM_PAYLOAD);
    uart_printf("[GA10B-P8]   expected_payload=0x%lx (handoff v%lu)\n",
                (unsigned long)expected,
                (unsigned long)g_handoff.version);

    return ga10b_submit_and_poll(b, pb_buf, pb_dwords,
                                 g_handoff.output_phys, expected,
                                 8, "GA10B-P8");
}

int ga10b_bringup_run(struct ga10b_bringup *b)
{
    int rc;

    rc = ga10b_bringup_prepare(b);
    if (rc < 0) return rc;

    rc = ga10b_bringup_acr(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_fecs(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_gpccs(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_pmu(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_address_space(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_channel(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    rc = ga10b_bringup_smoke_test(b);
    if (rc < 0) { b->state = GA10B_BRINGUP_FAILED; return rc; }

    uart_puts("[GA10B] bringup complete — channel open, method accepted\n");
    return 0;
}

/* Sanity ceiling on the v6 input buffer. The launcher allocates a
 * page (4 KB) for MNIST today; cap at 1 MB so a corrupted handoff
 * with bogus input_buf_size can't authorize a multi-megabyte memcpy
 * into wherever input_buf_phys points. Real models will need this
 * raised, but a hard ceiling beats trusting an arbitrary 32-bit
 * field. */
#define GA10B_INPUT_BUF_MAX_BYTES (1u * 1024u * 1024u)

int ga10b_bringup_set_input(struct ga10b_bringup *b,
                             const void *bytes, size_t cap)
{
    if (!b || !bytes) return -3;
    if (g_handoff.input_buf_phys == 0u) return -1;
    if (g_handoff.input_buf_size > GA10B_INPUT_BUF_MAX_BYTES) return -1;
    if (cap > (size_t)g_handoff.input_buf_size) return -2;

    /* Same identity-DRAM-mapping assumption as the pipeline reader:
     * the physical address from the handoff is also a valid VA at EL2.
     * Safe to memcpy through it. */
    void *dst = (void *)(uintptr_t)g_handoff.input_buf_phys;
    memcpy(dst, bytes, cap);

    /* Clean the cache so the GPU sees the fresh input on the next
     * dispatch. The mb() after cache_clean is the DSB that completes
     * the cache maintenance — DC CVAC alone is not synchronizing on
     * AArch64. Pattern matches `ga10b_submit_and_poll`'s pre-DMA
     * sync for QMD/pushbuffer writes. */
    if (gsp_platform && gsp_platform->cache_clean) {
        gsp_platform->cache_clean(dst, cap);
    }
    if (gsp_platform && gsp_platform->mb) {
        gsp_platform->mb();
    }

    /* GPU L2 invalidate. CPU has just written new bytes to DRAM at
     * input_buf_phys, but the GPU's chip-wide L2 may still hold lines
     * cached from a previous dispatch's read of the same physical
     * address. Without this invalidate, the next dispatch's SASS LDG
     * gets stale-from-L2 data even though DRAM is fresh — the off-by-
     * one staleness pattern observed in #715 ("each call returns the
     * previous call's result"). The QMD's per-launch
     * INVALIDATE_SHADER_CACHES + CWD_MEMBAR_TYPE_L1_SYSMEMBAR cover
     * SM-side caches but do not reach the LTC.
     *
     * Companion evicts run in `ga10b_dispatch_v7_pipeline` (pre-launch,
     * to drop any lines re-cached between set_input and the actual
     * dispatch) and `ga10b_bringup_read_pipeline_output` (post-launch,
     * to flush GPU dirty L2 output lines back to DRAM before the CPU
     * read). All three were empirically required on jetson-nano-2 to
     * pass an ABBA + AAAA test matrix (digit_0/3/7 mixed sequences)
     * — removing the post-launch evict alone reproduces the
     * staleness; removing the pre-launch evict shifts the failure
     * pattern from "off-by-one" to "calls 2+4 wrong with values
     * swapped". The architectural understanding of which is strictly
     * necessary on which path is incomplete; the narrowing experiment
     * is tracked in #723 (gate each site behind a kernel cmdline flag
     * and run the full subset matrix).
     *
     * The 4-step UFLUSH sequence (FB_FLUSH + L2_FLUSH_DIRTY +
     * L2_SYSMEM_INVALIDATE + FB_FLUSH) is the same one
     * `ga10b_bringup_inherit` runs once at startup. ~40 µs typical,
     * 2 ms worst-case under IRQ-off (caller holds
     * g_gpu_dispatch_lock). */
    (void)ga10b_l2_evict_sysmem();
    return (int)cap;
}

int ga10b_bringup_set_input_fill(struct ga10b_bringup *b,
                                  uint32_t value_bits, uint32_t n_floats)
{
    if (!b) return -3;
    if (g_handoff.input_buf_phys == 0u) return -1;
    if (g_handoff.input_buf_size > GA10B_INPUT_BUF_MAX_BYTES) return -1;
    /* Wrap-safe size check: division side, not multiplication side
     * (input_buf_size / 4 is at most ~256 K, can't overflow). */
    if (n_floats > (g_handoff.input_buf_size / sizeof(uint32_t))) return -2;

    /* Use byte-granular memcpy rather than uint32_t* stores: avoids
     * an unaligned-pointer assumption on `input_buf_phys` (the
     * launcher aligns to 4096, but a malformed handoff could break
     * it). On AArch64 with SCTLR.A=1 a misaligned uint32_t store
     * would fault — memcpy is alignment-safe. */
    uint8_t *dst = (uint8_t *)(uintptr_t)g_handoff.input_buf_phys;
    for (uint32_t i = 0u; i < n_floats; i++) {
        memcpy(dst + i * sizeof(uint32_t), &value_bits, sizeof(uint32_t));
    }

    size_t bytes_written = (size_t)n_floats * sizeof(uint32_t);
    if (gsp_platform && gsp_platform->cache_clean) {
        gsp_platform->cache_clean(dst, bytes_written);
    }
    if (gsp_platform && gsp_platform->mb) {
        gsp_platform->mb();
    }
    /* GPU L2 invalidate after CPU input write — see set_input header
     * for the rationale. Same staleness fix applies to the fill path
     * (sched-MLP runs through here on every assign_cpu tick).
     *
     * Sched-MLP cost note: ~40 µs IRQ-off per call. The hot path is
     * already throttled by #651's RATE_LIMIT_NS + try-lock combo, so
     * adding this evict doesn't change the per-tick budget shape. If
     * `bench smp` or `bench stealing` regress under sched_mlp=ON,
     * suspect the evict's worst-case (2 ms) tail rather than the
     * typical case. */
    (void)ga10b_l2_evict_sysmem();
    return (int)bytes_written;
}

int ga10b_bringup_read_pipeline_output(struct ga10b_bringup *b,
                                        void *out, size_t cap)
{
    if (!b || !out) return -1;
    if (g_handoff.version < 5u || g_handoff.pipeline_n_ops == 0u) {
        return -1;
    }
    if (g_handoff.pipeline_ops_phys == 0u ||
        g_handoff.pipeline_n_ops > GA10B_PIPELINE_MAX_OPS) {
        return -1;
    }

    /* Resolve the LAST op's output_phys. Same identity-DRAM-mapping
     * assumption as the pipeline runner — physical address read from
     * DRAM is also a valid VA SLM-OS can dereference at EL2.
     *
     * Stride must match the on-disk op layout: 24 bytes on v5/v6
     * (struct ga10b_pipeline_op), 80 bytes on v7 (struct
     * ga10b_pipeline_op_v7). Both share `output_phys` at byte offset
     * 8 in their prefix, but indexing as v6 across a v7 array reads
     * from inside an earlier op's tail (op[N-1] under v6 stride lands
     * inside op[(N-1)*3/10] under v7 stride for the MNIST 8-op
     * shape). The helper sets v7 `expected_payload = 0` and `flags
     * = 0` per op, so the bogus v6-cast read of `ops[7].output_phys`
     * returns 0 and this function returned -1 even when v7 dispatch
     * succeeded. Discovered via #710. */
    uint64_t last_output_phys;
    if (ga10b_handoff_is_v7(&g_handoff)) {
        const struct ga10b_pipeline_op_v7 *ops =
            (const struct ga10b_pipeline_op_v7 *)
                (uintptr_t)g_handoff.pipeline_ops_phys;
        last_output_phys = ops[g_handoff.pipeline_n_ops - 1u].output_phys;
    } else {
        const struct ga10b_pipeline_op *ops =
            (const struct ga10b_pipeline_op *)
                (uintptr_t)g_handoff.pipeline_ops_phys;
        last_output_phys = ops[g_handoff.pipeline_n_ops - 1u].output_phys;
    }
    if (last_output_phys == 0u) return -1;

    /* Post-dispatch L2 evict — load-bearing for #715. The trailing
     * COMPUTE_SEMA_RELEASE entry in the dispatch fires with
     * FLUSH_DISABLE=0, which is supposed to flush GPU writes through to
     * sysmem before the sema release — but on GA10B silicon, hardware
     * verification showed output reads still return stale data without
     * an explicit L2 evict here. Drops any GPU L2 lines that hold this
     * dispatch's output (writeback dirty lines + invalidate clean), so
     * the CPU's subsequent cache_invalidate + memcpy reads fresh
     * DRAM. Without this, two consecutive dispatches with different
     * inputs return identical (stale) logits.
     *
     * This is the third leg of the cross-dispatch coherency tripod;
     * see the companion comments in `ga10b_bringup_set_input` and the
     * pre-dispatch site in `ga10b_dispatch_v7_pipeline`.
     *
     * Surface failures here. A timed-out evict on this site means the
     * read below will return whatever was previously in DRAM at
     * `last_output_phys` — exactly the off-by-one symptom #715 closed.
     * Logging makes the silent-staleness regression visible without
     * having to re-run the ABBA hardware test. */
    if (ga10b_l2_evict_sysmem() < 0) {
        uart_puts("[GA10B] post-dispatch L2 evict timed out — "
                  "output read may return stale logits\n");
    }

    /* Invalidate the buffer's cache range before the read. The GPU
     * wrote the data via its own (uncached-from-CPU's-perspective)
     * write path; without this, a stale cache line could mask the
     * fresh data. Same pattern as ga10b_submit_and_poll's poll
     * invalidate. */
    const void *src = (const void *)(uintptr_t)last_output_phys;
    if (gsp_platform && gsp_platform->cache_invalidate) {
        gsp_platform->cache_invalidate((void *)src, cap);
    }
    memcpy(out, src, cap);
    return (int)cap;
}
