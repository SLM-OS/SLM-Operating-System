/*
 * test_ga10b_bringup.c — regression tests for the GA10B nvgpu-style
 * bringup (kernel/gpu/nvidia/ga10b_bringup.c).
 *
 * Four surfaces exercised without real hardware:
 *
 *   1. Firmware accessor — `ga10b_firmware_get` returns 0/size for
 *      every enum kind when the firmware is "embedded" (we stand in
 *      small synthetic blobs via matching `ga10b_fw_*_start/_end`
 *      symbols) and -1 for out-of-range kinds.
 *
 *   2. Prepare guards — prepare() bails cleanly when gsp_platform is
 *      NULL, and when the firmware blob size is zero. Neither should
 *      crash or leave b in a partially-initialized state.
 *
 *   3. Phase-ordering state machine — phases 2–7 each refuse to run
 *      from the wrong state and set last_error_phase. Exercises the
 *      top-level ga10b_bringup_run too: in the host test we don't
 *      have Falcon hardware so we expect Phase 1 (ACR) to fail; the
 *      test verifies the state-machine behavior around that failure
 *      (state = FAILED, last_error_phase = 1, subsequent phase calls
 *      reject cleanly).
 *
 *   4. ACR sequence plumbing (mock-vtable) — walks the ACR loader
 *      through a happy-path mock. Asserts the driver issues the
 *      nvgpu-native sequence: engine reset assert→delay→deassert,
 *      IMEM PIO upload of the full ACR text at offset 0, DMEM PIO
 *      of data at offset 0 and manifest at (dmem_size - manifest),
 *      BCR_CTRL = 0x11, BOOT_VECTOR_{LO,HI} = 0, CPUCTL.STARTCPU,
 *      then halts when the mock flips MBOX0 = ACR_BOOT_OK + CPUCTL
 *      to HALTED.
 *
 * Wired into `make test-ga10b-bringup` for CI.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/falcon.h"
#include "../../kernel/gpu/nvidia/ga10b_bringup.h"
#include "../../kernel/gpu/nvidia/ga10b_channel_handoff.h"
#include "../../kernel/gpu/nvidia/gsp.h"
#include "../../scripts/gpu-qmd-bits.h"

/* nvidia_vbios_platform_load is referenced by the shared gsp bringup
 * link set, but not by ga10b_bringup.c. Provide a stub so the test
 * binary links even if it pulls in sibling TUs. */
int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    (void)out_data; (void)out_size;
    return -1;
}

/* uart_puts / uart_printf are called by ga10b_bringup.c for diagnostic
 * output. In SLM-OS these route to the UART; for the host test we
 * forward to stderr so test logs include what the driver logged.
 * (`host-tools/gsp-harness/uart.h` would give us this for free if
 * ga10b_bringup.c included `uart.h` relative — it includes the kernel
 * path explicitly, so we provide real functions here instead.) */
#include <stdarg.h>
void uart_puts(const char *s) { fputs(s, stderr); }
void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

#define REQUIRE_EQ(a, b) do { \
    unsigned long _a = (unsigned long)(a); \
    unsigned long _b = (unsigned long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "FAIL %s:%d: %s (=%lu) != %s (=%lu)\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        failures++; \
    } \
} while (0)

/* ----------------------------------------------------------------------
 * Synthetic firmware blobs
 *
 * ga10b_bringup.c expects pairs of labels `X_start` and `X_end` such
 * that `(X_end - X_start)` yields the blob size. The real build
 * achieves this via `.incbin` between two `.global` labels in
 * `nvidia_ga10b_firmware.S`. Tests replicate that shape with a file-
 * scope inline-asm block that reserves N bytes of .rodata between
 * two globals — the same mechanism, just using `.space`.
 *
 * We only need small blobs (the logic doesn't care about real sizes),
 * but the ACR phase test compares bytes post-upload, so the first few
 * blobs get full `.space` pre-initialized with 0x00.
 * ---------------------------------------------------------------------- */

#define TEST_BLOB_SIZE_ACR_TEXT        1024
#define TEST_BLOB_SIZE_ACR_DATA         512
#define TEST_BLOB_SIZE_ACR_MANIFEST    2048
#define TEST_BLOB_SIZE_FECS             272
#define TEST_BLOB_SIZE_FECS_SIG        2248
#define TEST_BLOB_SIZE_GPCCS            272
#define TEST_BLOB_SIZE_GPCCS_SIG       2248
#define TEST_BLOB_SIZE_PMU_IMAGE       4096
#define TEST_BLOB_SIZE_PMU_DESC          76
#define TEST_BLOB_SIZE_PMU_SIG         2248
#define TEST_BLOB_SIZE_NET_A            256
#define TEST_BLOB_SIZE_NET_B            256
#define TEST_BLOB_SIZE_NET_C            256
#define TEST_BLOB_SIZE_NET_D            256
#define TEST_BLOB_SIZE_SAFETY_TEXT      256
#define TEST_BLOB_SIZE_SAFETY_DATA      256
#define TEST_BLOB_SIZE_SAFETY_MANIFEST  256

/* Declare `_start[]` and `_end[]` in one asm stanza per blob so the
 * assembler emits them contiguously in .data. Use `.data` instead
 * of `.rodata` so the test can memcpy patterns into each blob at
 * runtime — .rodata would be mapped read-only. `.previous` restores
 * the section GCC was using (.text) so compile-generated debug info
 * labels land where they're expected. */
#define DECLARE_BLOB(sym, size)                                             \
    __asm__(                                                                 \
        ".pushsection .data\n"                                               \
        ".globl " #sym "_start\n"                                            \
        ".balign 4\n"                                                        \
        #sym "_start:\n"                                                     \
        ".space " #size ", 0\n"                                              \
        ".globl " #sym "_end\n"                                              \
        #sym "_end:\n"                                                       \
        ".popsection\n"                                                      \
    );                                                                       \
    extern uint8_t sym##_start[];                                            \
    extern uint8_t sym##_end[]

DECLARE_BLOB(ga10b_fw_acr_text,        1024);
DECLARE_BLOB(ga10b_fw_acr_data,        512);
DECLARE_BLOB(ga10b_fw_acr_manifest,    2048);
DECLARE_BLOB(ga10b_fw_fecs,            272);
DECLARE_BLOB(ga10b_fw_fecs_sig,        2248);
DECLARE_BLOB(ga10b_fw_gpccs,           272);
DECLARE_BLOB(ga10b_fw_gpccs_sig,       2248);
DECLARE_BLOB(ga10b_fw_pmu_image,       4096);
DECLARE_BLOB(ga10b_fw_pmu_desc,        76);
DECLARE_BLOB(ga10b_fw_pmu_sig,         2248);
DECLARE_BLOB(ga10b_fw_net_a,           256);
DECLARE_BLOB(ga10b_fw_net_b,           256);
DECLARE_BLOB(ga10b_fw_net_c,           256);
DECLARE_BLOB(ga10b_fw_net_d,           256);
DECLARE_BLOB(ga10b_fw_safety_text,     256);
DECLARE_BLOB(ga10b_fw_safety_data,     256);
DECLARE_BLOB(ga10b_fw_safety_manifest, 256);

/* Expected blob sizes for the accessor test. Must match the
 * DECLARE_BLOB counts above. */
static const size_t EXPECTED_SIZES[GA10B_FW_KIND_COUNT] = {
    [GA10B_FW_ACR_TEXT]        = TEST_BLOB_SIZE_ACR_TEXT,
    [GA10B_FW_ACR_DATA]        = TEST_BLOB_SIZE_ACR_DATA,
    [GA10B_FW_ACR_MANIFEST]    = TEST_BLOB_SIZE_ACR_MANIFEST,
    [GA10B_FW_FECS]            = TEST_BLOB_SIZE_FECS,
    [GA10B_FW_FECS_SIG]        = TEST_BLOB_SIZE_FECS_SIG,
    [GA10B_FW_GPCCS]           = TEST_BLOB_SIZE_GPCCS,
    [GA10B_FW_GPCCS_SIG]       = TEST_BLOB_SIZE_GPCCS_SIG,
    [GA10B_FW_PMU_IMAGE]       = TEST_BLOB_SIZE_PMU_IMAGE,
    [GA10B_FW_PMU_DESC]        = TEST_BLOB_SIZE_PMU_DESC,
    [GA10B_FW_PMU_SIG]         = TEST_BLOB_SIZE_PMU_SIG,
    [GA10B_FW_NET_A]           = TEST_BLOB_SIZE_NET_A,
    [GA10B_FW_NET_B]           = TEST_BLOB_SIZE_NET_B,
    [GA10B_FW_NET_C]           = TEST_BLOB_SIZE_NET_C,
    [GA10B_FW_NET_D]           = TEST_BLOB_SIZE_NET_D,
    [GA10B_FW_SAFETY_TEXT]     = TEST_BLOB_SIZE_SAFETY_TEXT,
    [GA10B_FW_SAFETY_DATA]     = TEST_BLOB_SIZE_SAFETY_DATA,
    [GA10B_FW_SAFETY_MANIFEST] = TEST_BLOB_SIZE_SAFETY_MANIFEST,
};

/* Host buffers used to verify byte-exact transport of the upload. */
static uint8_t g_acr_text_bytes[TEST_BLOB_SIZE_ACR_TEXT];
static uint8_t g_acr_data_bytes[TEST_BLOB_SIZE_ACR_DATA];
static uint8_t g_acr_manifest_bytes[TEST_BLOB_SIZE_ACR_MANIFEST];

/* ----------------------------------------------------------------------
 * Mock BAR0 + gsp_platform
 * ---------------------------------------------------------------------- */

#define MOCK_BAR0_SIZE (16u * 1024u * 1024u)
static uint32_t g_bar0[MOCK_BAR0_SIZE / 4];

/* Shadow GSP Falcon state — the mock treats these offsets specially. */
static struct {
    bool     reset_asserted;
    uint32_t reset_asserts;
    uint32_t reset_deasserts;
    uint32_t hwcfg2;            /* returned on read */
    uint32_t cpuctl;
    uint32_t startcpu_writes;
    uint32_t mbox0, mbox1;
    uint32_t br_retcode;
    uint32_t bcr_ctrl;
    /* Auto-increment PIO capture. */
    uint32_t imemc, imem_offs, imem_writes;
    uint32_t dmemc, dmem_offs, dmem_writes;
    /* Record the exact bytes uploaded so we can verify. */
    uint8_t  imem_buf[128 * 1024];
    uint8_t  dmem_buf[128 * 1024];
    /* Test-controlled behavior. */
    bool     startcpu_halts;    /* flip to true to simulate ACR BOOT_OK */
    uint32_t ack_mbox0_on_halt; /* value MBOX0 takes when CPU halts */
    uint32_t ack_retcode_on_halt;
} g_gsp;

/* GSP BAR0 base — absolute offset 0x110000. We pretend BAR0 starts at 0. */
#define GSP_BASE        0x00110000u
#define GSP_RISCV_BASE  (GSP_BASE + 0x1000u)

/* Register offsets (absolute). */
#define R_FLCN_MBOX0        (GSP_BASE + 0x040u)
#define R_FLCN_MBOX1        (GSP_BASE + 0x044u)
#define R_FLCN_CPUCTL       (GSP_BASE + 0x100u)
#define R_FLCN_HWCFG        (GSP_BASE + 0x108u)
#define R_FLCN_HWCFG2       (GSP_BASE + 0x0F4u)
#define R_FLCN_DMACTL       (GSP_BASE + 0x10cu)
#define R_FLCN_ENGINE       (GSP_BASE + 0x3c0u)
#define R_FLCN_IMEMC0       (GSP_BASE + 0x180u)
#define R_FLCN_IMEMT0       (GSP_BASE + 0x188u)
#define R_FLCN_IMEMD0       (GSP_BASE + 0x184u)
#define R_FLCN_DMEMC0       (GSP_BASE + 0x1c0u)
#define R_FLCN_DMEMD0       (GSP_BASE + 0x1c4u)

#define R_RISCV_BOOTVEC_LO  (GSP_RISCV_BASE + 0x380u)
#define R_RISCV_BOOTVEC_HI  (GSP_RISCV_BASE + 0x384u)
#define R_RISCV_CPUCTL      (GSP_RISCV_BASE + 0x388u)
#define R_RISCV_BR_RETCODE  (GSP_RISCV_BASE + 0x65cu)
#define R_RISCV_BCR_CTRL    (GSP_RISCV_BASE + 0x668u)

/* GR Falcon registers (absolute). These match the constants in
 * kernel/gpu/nvidia/ga10b_bringup.c; if those change, update here. */
#define R_FECS_CPUCTL           0x00409100u
#define R_FECS_CTXSW_MBOX(i)    (0x00409800u + (i) * 4u)
#define R_FECS_METHOD_DATA      0x00409500u
#define R_FECS_METHOD_PUSH      0x00409504u
#define R_GPCCS_CPUCTL          0x0041a100u
#define R_GPC0_GPCCS_MBOX(i)    (0x00502800u + (i) * 4u)
#define GR_STARTCPU             (1u << 1)

/* Shadow state for FECS/GPCCS plumbing tests. */
static struct {
    uint32_t fecs_cpuctl_writes;
    uint32_t gpccs_cpuctl_writes;
    uint32_t fecs_mbox0;
    uint32_t gpccs_mbox0;
    /* Test-controlled: what value mailbox[0] takes after STARTCPU. */
    uint32_t fecs_mbox0_after_start;
    uint32_t gpccs_mbox0_after_start;
    bool     fecs_auto_pass;
    bool     gpccs_auto_pass;
    /* FECS method gateway shadow state. */
    uint32_t method_data_writes;
    uint32_t method_push_writes;
    uint32_t last_method_data;
    uint32_t last_method_addr;
    /* Test-controlled: what mailbox[0] gets set to on method push. */
    uint32_t method_result;
    bool     method_auto_respond;
} g_gr;

/* Fake HWCFG value: IMEM=64KB (256 blocks × 256B), DMEM=64KB. */
#define FAKE_HWCFG          (256u | (256u << 9))
#define FAKE_IMEM           (64u * 1024u)
#define FAKE_DMEM           (64u * 1024u)

/* Fake HWCFG2: RISCV_ENABLE bit set, MEM_SCRUBBING cleared. */
#define FAKE_HWCFG2_IDLE    ((1u << 10))

static uint32_t mock_read32(uint32_t off)
{
    if (off == R_FLCN_HWCFG)    return FAKE_HWCFG;
    if (off == R_FLCN_HWCFG2)   return g_gsp.hwcfg2;
    if (off == R_FLCN_CPUCTL)   return g_gsp.cpuctl;
    if (off == R_FLCN_MBOX0)    return g_gsp.mbox0;
    if (off == R_FLCN_MBOX1)    return g_gsp.mbox1;
    if (off == R_FECS_CTXSW_MBOX(0))     return g_gr.fecs_mbox0;
    if (off == R_GPC0_GPCCS_MBOX(0))     return g_gr.gpccs_mbox0;
    if (off == R_RISCV_CPUCTL) {
        /* If the test wants the CPU to auto-halt, flip HALTED on
         * first poll after STARTCPU. */
        if (g_gsp.startcpu_halts && g_gsp.startcpu_writes > 0 &&
            (g_gsp.cpuctl & (1u << 4)) == 0) {
            g_gsp.cpuctl |= (1u << 4);  /* HALTED */
            g_gsp.cpuctl &= ~(1u << 7); /* clear ACTIVE */
            g_gsp.mbox0 = g_gsp.ack_mbox0_on_halt;
            g_gsp.br_retcode = g_gsp.ack_retcode_on_halt;
        }
        return g_gsp.cpuctl;
    }
    if (off == R_RISCV_BR_RETCODE) return g_gsp.br_retcode;
    if (off == R_RISCV_BCR_CTRL)   return g_gsp.bcr_ctrl;
    if (off < MOCK_BAR0_SIZE) return g_bar0[off / 4];
    return 0xFFFFFFFFu;
}

static void mock_write32(uint32_t off, uint32_t val)
{
    if (off == R_FLCN_ENGINE) {
        if ((val & 1u) != 0) {
            g_gsp.reset_asserted = true;
            g_gsp.reset_asserts++;
            /* When asserted, HWCFG2 becomes scrubbing. */
            g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE | (1u << 12);  /* SCRUBBING */
        } else {
            g_gsp.reset_asserted = false;
            g_gsp.reset_deasserts++;
            /* When deasserted, scrubbing clears quickly. */
            g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE;
        }
        return;
    }
    if (off == R_FLCN_IMEMC0) {
        g_gsp.imemc = val;
        /* Low 24 bits are offset (bytes). */
        g_gsp.imem_offs = val & 0x00FFFFFFu;
        return;
    }
    if (off == R_FLCN_IMEMT0) {
        /* tag — ignored in this mock */
        return;
    }
    if (off == R_FLCN_IMEMD0) {
        if (g_gsp.imem_offs + 4 <= sizeof(g_gsp.imem_buf)) {
            g_gsp.imem_buf[g_gsp.imem_offs + 0] = val & 0xff;
            g_gsp.imem_buf[g_gsp.imem_offs + 1] = (val >> 8) & 0xff;
            g_gsp.imem_buf[g_gsp.imem_offs + 2] = (val >> 16) & 0xff;
            g_gsp.imem_buf[g_gsp.imem_offs + 3] = (val >> 24) & 0xff;
        }
        g_gsp.imem_offs += 4;
        g_gsp.imem_writes++;
        return;
    }
    if (off == R_FLCN_DMEMC0) {
        g_gsp.dmemc = val;
        g_gsp.dmem_offs = val & 0x00FFFFFFu;
        return;
    }
    if (off == R_FLCN_DMEMD0) {
        if (g_gsp.dmem_offs + 4 <= sizeof(g_gsp.dmem_buf)) {
            g_gsp.dmem_buf[g_gsp.dmem_offs + 0] = val & 0xff;
            g_gsp.dmem_buf[g_gsp.dmem_offs + 1] = (val >> 8) & 0xff;
            g_gsp.dmem_buf[g_gsp.dmem_offs + 2] = (val >> 16) & 0xff;
            g_gsp.dmem_buf[g_gsp.dmem_offs + 3] = (val >> 24) & 0xff;
        }
        g_gsp.dmem_offs += 4;
        g_gsp.dmem_writes++;
        return;
    }
    if (off == R_FLCN_MBOX0) g_gsp.mbox0 = val;
    if (off == R_FLCN_MBOX1) g_gsp.mbox1 = val;
    if (off == R_FLCN_DMACTL) { /* ignored */ }
    if (off == R_RISCV_BCR_CTRL) { g_gsp.bcr_ctrl = val; }
    if (off == R_RISCV_CPUCTL) {
        if ((val & 1u) != 0) {  /* STARTCPU */
            g_gsp.startcpu_writes++;
            g_gsp.cpuctl |= (1u << 7);  /* ACTIVE */
        }
        return;
    }
    if (off == R_FECS_CTXSW_MBOX(0)) {
        g_gr.fecs_mbox0 = val;
        return;
    }
    if (off == R_GPC0_GPCCS_MBOX(0)) {
        g_gr.gpccs_mbox0 = val;
        return;
    }
    if (off == R_FECS_CPUCTL) {
        if ((val & GR_STARTCPU) != 0) {
            g_gr.fecs_cpuctl_writes++;
            if (g_gr.fecs_auto_pass) {
                g_gr.fecs_mbox0 = g_gr.fecs_mbox0_after_start;
            }
        }
        return;
    }
    if (off == R_GPCCS_CPUCTL) {
        if ((val & GR_STARTCPU) != 0) {
            g_gr.gpccs_cpuctl_writes++;
            if (g_gr.gpccs_auto_pass) {
                g_gr.gpccs_mbox0 = g_gr.gpccs_mbox0_after_start;
            }
        }
        return;
    }
    if (off == R_FECS_METHOD_DATA) {
        g_gr.last_method_data = val;
        g_gr.method_data_writes++;
        return;
    }
    if (off == R_FECS_METHOD_PUSH) {
        g_gr.last_method_addr = val;
        g_gr.method_push_writes++;
        /* Simulate FECS processing the method: write result to mbox[0]. */
        if (g_gr.method_auto_respond) {
            g_gr.fecs_mbox0 = g_gr.method_result;
        }
        return;
    }
    if (off < MOCK_BAR0_SIZE) g_bar0[off / 4] = val;
}

static void mock_mb(void) { __asm__ volatile("" ::: "memory"); }

/* No-ops for the surfaces the bringup code doesn't exercise here. */
static void mock_bar1_read(uint32_t o, void *d, size_t n)        { (void)o;(void)d;(void)n; }
static void mock_bar1_write(uint32_t o, const void *s, size_t n) { (void)o;(void)s;(void)n; }
static void *mock_dma_alloc(size_t s, size_t a, uint64_t *p)     { (void)s;(void)a; if (p) *p = 0; return NULL; }
static void mock_dma_free(void *p, size_t s)                     { (void)p;(void)s; }
static void mock_cache_clean(const void *a, size_t s)            { (void)a;(void)s; }
static void mock_cache_invalidate(void *a, size_t s)             { (void)a;(void)s; }
static void mock_fw_get(enum gsp_firmware_kind k, struct gsp_firmware_blob *o) { (void)k; o->data=NULL; o->size=0; o->version=NULL; }
static int  mock_vbios(const void **d, size_t *s)                { *d=NULL; *s=0; return 0; }

static const struct gsp_platform_ops mock_ops = {
    .read32           = mock_read32,
    .write32          = mock_write32,
    .bar1_read        = mock_bar1_read,
    .bar1_write       = mock_bar1_write,
    .dma_alloc        = mock_dma_alloc,
    .dma_free         = mock_dma_free,
    .cache_clean      = mock_cache_clean,
    .cache_invalidate = mock_cache_invalidate,
    .mb               = mock_mb,
    .firmware_get     = mock_fw_get,
    .vbios_get_fwsec  = mock_vbios,
};

/* The shared code's platform pointer. */
extern const struct gsp_platform_ops *gsp_platform;

static void mock_reset(void)
{
    memset(&g_gsp, 0, sizeof(g_gsp));
    memset(&g_gr,  0, sizeof(g_gr));
    memset(g_bar0, 0, sizeof(g_bar0));
    g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE;
    g_gsp.ack_mbox0_on_halt = 0xFFu;           /* ACR_BOOT_OK */
    g_gsp.ack_retcode_on_halt = 0x3u;          /* PASS */
    g_gsp.startcpu_halts = true;
    /* Default: FECS/GPCCS auto-PASS on first STARTCPU so the happy
     * path walks straight through. Tests that need FAIL behaviour
     * override these knobs before invoking the phase. */
    g_gr.fecs_auto_pass = true;
    g_gr.gpccs_auto_pass = true;
    g_gr.fecs_mbox0_after_start  = 0x1u;       /* PASS */
    g_gr.gpccs_mbox0_after_start = 0x1u;       /* PASS */
    g_gr.method_auto_respond = true;
    g_gr.method_result = 0x7d500u;  /* 513280 — realistic context image size */
    gsp_platform = &mock_ops;
}

/* Fill each firmware blob with a distinctive pattern so IMEM/DMEM
 * readback is meaningful. Writes into the linker-placed symbols
 * directly, and keeps host-side copies for verification. */
static void firmware_fill_patterns(void)
{
    for (size_t i = 0; i < sizeof(g_acr_text_bytes); i++)
        g_acr_text_bytes[i] = (uint8_t)(i & 0xff);
    for (size_t i = 0; i < sizeof(g_acr_data_bytes); i++)
        g_acr_data_bytes[i] = (uint8_t)((i + 0x11) & 0xff);
    for (size_t i = 0; i < sizeof(g_acr_manifest_bytes); i++)
        g_acr_manifest_bytes[i] = (uint8_t)((i + 0x22) & 0xff);

    memcpy(ga10b_fw_acr_text_start,     g_acr_text_bytes,     sizeof(g_acr_text_bytes));
    memcpy(ga10b_fw_acr_data_start,     g_acr_data_bytes,     sizeof(g_acr_data_bytes));
    memcpy(ga10b_fw_acr_manifest_start, g_acr_manifest_bytes, sizeof(g_acr_manifest_bytes));
}

/* ======================================================================
 * Test 1: firmware accessor
 * ====================================================================== */

static void test_firmware_accessor_sizes(void)
{
    printf("== test_firmware_accessor_sizes ==\n");
    firmware_fill_patterns();

    for (int k = 0; k < GA10B_FW_KIND_COUNT; k++) {
        struct ga10b_firmware_blob blob;
        int rc = ga10b_firmware_get((enum ga10b_firmware_kind)k, &blob);
        REQUIRE_EQ(rc, 0);
        REQUIRE(blob.data != NULL);
        REQUIRE_EQ(blob.size, EXPECTED_SIZES[k]);
    }

    /* First bytes of ACR text match the pattern (i & 0xff). */
    struct ga10b_firmware_blob text;
    REQUIRE_EQ(ga10b_firmware_get(GA10B_FW_ACR_TEXT, &text), 0);
    REQUIRE_EQ(text.data[0], 0x00);
    REQUIRE_EQ(text.data[1], 0x01);
    REQUIRE_EQ(text.data[7], 0x07);
    REQUIRE_EQ(text.data[0xff], 0xff);
}

static void test_firmware_accessor_rejects_out_of_range(void)
{
    printf("== test_firmware_accessor_rejects_out_of_range ==\n");
    struct ga10b_firmware_blob blob = { (const uint8_t *)0xdeadbeef, 0x1234 };
    int rc = ga10b_firmware_get((enum ga10b_firmware_kind)GA10B_FW_KIND_COUNT, &blob);
    REQUIRE_EQ(rc, -1);
    /* On failure the accessor must zero the outputs — callers rely
     * on this so they can detect "no firmware" without checking rc. */
    REQUIRE(blob.data == NULL);
    REQUIRE_EQ(blob.size, 0);
}

static void test_firmware_accessor_null_out(void)
{
    printf("== test_firmware_accessor_null_out ==\n");
    REQUIRE_EQ(ga10b_firmware_get(GA10B_FW_ACR_TEXT, NULL), -1);
}

/* ======================================================================
 * Test 2: prepare guards
 * ====================================================================== */

static void test_prepare_rejects_null_platform(void)
{
    printf("== test_prepare_rejects_null_platform ==\n");
    struct ga10b_bringup b;
    gsp_platform = NULL;
    int rc = ga10b_bringup_prepare(&b);
    REQUIRE_EQ(rc, -1);
}

static void test_prepare_rejects_null_bringup(void)
{
    printf("== test_prepare_rejects_null_bringup ==\n");
    mock_reset();
    int rc = ga10b_bringup_prepare(NULL);
    REQUIRE_EQ(rc, -1);
}

static void test_prepare_populates_state(void)
{
    printf("== test_prepare_populates_state ==\n");
    mock_reset();
    firmware_fill_patterns();
    struct ga10b_bringup b;
    memset(&b, 0xaa, sizeof(b));  /* dirty caller buffer */
    int rc = ga10b_bringup_prepare(&b);
    REQUIRE_EQ(rc, 0);
    /* prepare() should zero b and then probe the GSP Falcon. After
     * a successful run the state is INIT and imem_size/dmem_size
     * come from HWCFG. */
    REQUIRE_EQ(b.state, GA10B_BRINGUP_INIT);
    REQUIRE_EQ(b.last_error_phase, -1);
    REQUIRE_EQ(b.gsp_flcn.imem_size, FAKE_IMEM);
    REQUIRE_EQ(b.gsp_flcn.dmem_size, FAKE_DMEM);
    REQUIRE(b.gsp_flcn.has_riscv);
}

/* ======================================================================
 * Test 3: phase-ordering state machine
 * ====================================================================== */

static void test_phases_reject_wrong_state(void)
{
    printf("== test_phases_reject_wrong_state ==\n");
    mock_reset();
    firmware_fill_patterns();
    struct ga10b_bringup b;
    REQUIRE_EQ(ga10b_bringup_prepare(&b), 0);

    /* From INIT, everything except ACR must refuse. */
    REQUIRE_EQ(ga10b_bringup_fecs(&b),  -1);
    REQUIRE_EQ(ga10b_bringup_gpccs(&b), -1);
    REQUIRE_EQ(ga10b_bringup_pmu(&b),   -1);
    REQUIRE_EQ(ga10b_bringup_address_space(&b), -1);
    REQUIRE_EQ(ga10b_bringup_channel(&b), -1);
    REQUIRE_EQ(ga10b_bringup_smoke_test(&b), -1);
    /* smoke_test_compute + launch_kernel also reject the pre-channel
     * state — both need CHANNEL_OPEN or METHOD_ACCEPTED before
     * a pushbuffer submit is meaningful. */
    REQUIRE_EQ(ga10b_bringup_smoke_test_compute(&b), -1);
    REQUIRE_EQ(ga10b_bringup_launch_kernel(&b),      -1);

    /* State should not have changed. */
    REQUIRE_EQ(b.state, GA10B_BRINGUP_INIT);
}

static void test_phases_reject_null_bringup(void)
{
    printf("== test_phases_reject_null_bringup ==\n");
    REQUIRE_EQ(ga10b_bringup_acr(NULL),                 -1);
    REQUIRE_EQ(ga10b_bringup_fecs(NULL),                -1);
    REQUIRE_EQ(ga10b_bringup_gpccs(NULL),               -1);
    REQUIRE_EQ(ga10b_bringup_pmu(NULL),                 -1);
    REQUIRE_EQ(ga10b_bringup_address_space(NULL),       -1);
    REQUIRE_EQ(ga10b_bringup_channel(NULL),             -1);
    REQUIRE_EQ(ga10b_bringup_smoke_test(NULL),          -1);
    REQUIRE_EQ(ga10b_bringup_smoke_test_compute(NULL),  -1);
    REQUIRE_EQ(ga10b_bringup_launch_kernel(NULL),       -1);
}

/* ======================================================================
 * Test 4: ACR sequence plumbing (happy path against mock)
 * ====================================================================== */

static void test_acr_sequence_happy_path(void)
{
    printf("== test_acr_sequence_happy_path ==\n");
    mock_reset();
    firmware_fill_patterns();
    struct ga10b_bringup b;
    REQUIRE_EQ(ga10b_bringup_prepare(&b), 0);
    REQUIRE_EQ(ga10b_bringup_acr(&b), 0);

    /* Engine reset pattern: assert exactly once, deassert exactly once. */
    REQUIRE_EQ(g_gsp.reset_asserts,   1);
    REQUIRE_EQ(g_gsp.reset_deasserts, 1);

    /* IMEM upload: full ACR text bytes (1024 bytes = 256 u32 words). */
    REQUIRE_EQ(g_gsp.imem_writes, sizeof(g_acr_text_bytes) / 4);

    /* DMEM: data (512 B = 128 u32) + manifest (2048 B = 512 u32) writes. */
    REQUIRE_EQ(g_gsp.dmem_writes,
               (sizeof(g_acr_data_bytes) + sizeof(g_acr_manifest_bytes)) / 4);

    /* BCR_CTRL must be 0x11 (CORE_RISCV | VALID), BOOTVEC must be 0. */
    REQUIRE_EQ(g_gsp.bcr_ctrl, 0x11);
    /* STARTCPU exactly once. */
    REQUIRE_EQ(g_gsp.startcpu_writes, 1);

    /* Post-halt state. */
    REQUIRE_EQ(b.state, GA10B_BRINGUP_ACR_RUNNING);
    REQUIRE_EQ(b.last_error_phase, -1);

    /* Verify the bytes the mock received match the source firmware
     * (i.e. PIO upload transported bytes correctly). */
    REQUIRE(memcmp(g_gsp.imem_buf, g_acr_text_bytes,
                   sizeof(g_acr_text_bytes)) == 0);
    REQUIRE(memcmp(g_gsp.dmem_buf, g_acr_data_bytes,
                   sizeof(g_acr_data_bytes)) == 0);
    REQUIRE(memcmp(&g_gsp.dmem_buf[FAKE_DMEM - sizeof(g_acr_manifest_bytes)],
                   g_acr_manifest_bytes,
                   sizeof(g_acr_manifest_bytes)) == 0);
}

static void test_acr_returns_error_on_brom_fail(void)
{
    printf("== test_acr_returns_error_on_brom_fail ==\n");
    mock_reset();
    firmware_fill_patterns();
    g_gsp.ack_mbox0_on_halt = 0;             /* not ACR_BOOT_OK */
    g_gsp.ack_retcode_on_halt = 0x2;         /* FAIL */

    struct ga10b_bringup b;
    REQUIRE_EQ(ga10b_bringup_prepare(&b), 0);
    REQUIRE_EQ(ga10b_bringup_acr(&b), -1);
    REQUIRE_EQ(b.last_error_phase, 1);
    REQUIRE(b.state != GA10B_BRINGUP_ACR_RUNNING);
}

/* ======================================================================
 * Test 5: FECS / GPCCS / PMU phases (plumbing against mock)
 *
 * FECS and GPCCS boot post-ACR by issuing STARTCPU on their CPUCTL
 * and polling ctxsw_mailbox[0] for PASS (=1). The mock auto-writes
 * the configured mailbox value when STARTCPU fires, so the happy
 * path completes in one poll iteration. Failure variants flip the
 * mock's ack value to exercise the error paths.
 *
 * PMU phase is a no-op on GA10B default (support_ls_pmu=false) —
 * just verify it advances state without touching any MMIO.
 * ====================================================================== */

/* Drive a bringup to ACR_RUNNING so phase 2+ preconditions hold.
 * Isolated helper because multiple tests need the same setup. */
static void drive_to_acr_running(struct ga10b_bringup *b)
{
    mock_reset();
    firmware_fill_patterns();
    REQUIRE_EQ(ga10b_bringup_prepare(b), 0);
    REQUIRE_EQ(ga10b_bringup_acr(b), 0);
    REQUIRE_EQ(b->state, GA10B_BRINGUP_ACR_RUNNING);
}

static void test_fecs_happy_path(void)
{
    printf("== test_fecs_happy_path ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);

    REQUIRE_EQ(ga10b_bringup_fecs(&b), 0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_FECS_UP);
    REQUIRE_EQ(b.last_error_phase, -1);
    /* Driver must have kicked STARTCPU exactly once. */
    REQUIRE_EQ(g_gr.fecs_cpuctl_writes, 1);
    /* Mailbox[0] must land on PASS (=1). */
    REQUIRE_EQ(g_gr.fecs_mbox0, 0x1u);
}

static void test_fecs_fails_on_fail_sentinel(void)
{
    printf("== test_fecs_fails_on_fail_sentinel ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);

    /* Override the mock: first STARTCPU causes FAIL (=2) instead of PASS. */
    g_gr.fecs_mbox0_after_start = 0x2u;
    REQUIRE_EQ(ga10b_bringup_fecs(&b), -1);
    REQUIRE_EQ(b.last_error_phase, 2);
    REQUIRE(b.state != GA10B_BRINGUP_FECS_UP);
}

static void test_fecs_fails_on_checksum_sentinel(void)
{
    printf("== test_fecs_fails_on_checksum_sentinel ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);

    /* Checksum-mismatch sentinel 0x21 — secure verification failed. */
    g_gr.fecs_mbox0_after_start = 0x21u;
    REQUIRE_EQ(ga10b_bringup_fecs(&b), -1);
    REQUIRE_EQ(b.last_error_phase, 2);
}

static void test_gpccs_happy_path(void)
{
    printf("== test_gpccs_happy_path ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);

    REQUIRE_EQ(ga10b_bringup_fecs(&b), 0);
    REQUIRE_EQ(ga10b_bringup_gpccs(&b), 0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_GPCCS_UP);
    REQUIRE_EQ(g_gr.gpccs_cpuctl_writes, 1);
    REQUIRE_EQ(g_gr.gpccs_mbox0, 0x1u);
}

static void test_gpccs_rejects_wrong_state(void)
{
    printf("== test_gpccs_rejects_wrong_state ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);
    /* Skip FECS — GPCCS must refuse to run from ACR_RUNNING. */
    REQUIRE_EQ(ga10b_bringup_gpccs(&b), -1);
    REQUIRE_EQ(g_gr.gpccs_cpuctl_writes, 0);
}

static void test_gpccs_fails_on_fail_sentinel(void)
{
    printf("== test_gpccs_fails_on_fail_sentinel ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);
    REQUIRE_EQ(ga10b_bringup_fecs(&b), 0);

    g_gr.gpccs_mbox0_after_start = 0x2u;
    REQUIRE_EQ(ga10b_bringup_gpccs(&b), -1);
    REQUIRE_EQ(b.last_error_phase, 3);
    REQUIRE(b.state != GA10B_BRINGUP_GPCCS_UP);
}

static void test_pmu_is_noop(void)
{
    printf("== test_pmu_is_noop ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);
    REQUIRE_EQ(ga10b_bringup_fecs(&b),  0);
    REQUIRE_EQ(ga10b_bringup_gpccs(&b), 0);

    /* PMU is a no-op on GA10B default; it should advance state and
     * touch no additional MMIO state. */
    uint32_t fecs_writes_before  = g_gr.fecs_cpuctl_writes;
    uint32_t gpccs_writes_before = g_gr.gpccs_cpuctl_writes;

    REQUIRE_EQ(ga10b_bringup_pmu(&b), 0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_PMU_UP);
    REQUIRE_EQ(b.last_error_phase, -1);

    /* No additional STARTCPU writes — PMU phase must be inert. */
    REQUIRE_EQ(g_gr.fecs_cpuctl_writes,  fecs_writes_before);
    REQUIRE_EQ(g_gr.gpccs_cpuctl_writes, gpccs_writes_before);
}

/* ======================================================================
 * Test 6: inherit (Path 3 — detect Linux's bootstrapped state)
 *
 * The inherit function reads HWCFG2 (bit 13), FECS mailbox[0], and
 * GPCCS mailbox[0]. If all show the "already booted" pattern, it
 * jumps the state machine directly to PMU_UP.
 * ====================================================================== */

static void test_inherit_happy_path(void)
{
    printf("== test_inherit_happy_path ==\n");
    mock_reset();

    /* Pre-set the mock BAR0 to simulate Linux's post-boot state:
     *   HWCFG2 bit 13 = 0 (unlocked)
     *   FECS mailbox[0] = 1 (PASS)
     *   GPCCS mailbox[0] = 1 (PASS) */
    g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE;  /* bit 13 = 0 */
    g_gr.fecs_mbox0  = 0x1u;           /* PASS */
    g_gr.gpccs_mbox0 = 0x1u;           /* PASS */

    struct ga10b_bringup b;
    int rc = ga10b_bringup_inherit(&b);
    REQUIRE_EQ(rc, 0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_PMU_UP);
    REQUIRE_EQ(b.last_error_phase, -1);
}

static void test_inherit_rejects_priv_lockdown(void)
{
    printf("== test_inherit_rejects_priv_lockdown ==\n");
    mock_reset();

    /* Set bit 13 (priv-lockdown) in HWCFG2 */
    g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE | (1u << 13);
    g_gr.fecs_mbox0  = 0x1u;
    g_gr.gpccs_mbox0 = 0x1u;

    struct ga10b_bringup b;
    int rc = ga10b_bringup_inherit(&b);
    REQUIRE_EQ(rc, -1);
}

static void test_inherit_rejects_fecs_not_ready(void)
{
    printf("== test_inherit_rejects_fecs_not_ready ==\n");
    mock_reset();

    g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE;
    g_gr.fecs_mbox0  = 0x0u;  /* NOT ready */
    g_gr.gpccs_mbox0 = 0x1u;

    struct ga10b_bringup b;
    int rc = ga10b_bringup_inherit(&b);
    REQUIRE_EQ(rc, -1);
}

static void test_inherit_rejects_gpccs_not_ready(void)
{
    printf("== test_inherit_rejects_gpccs_not_ready ==\n");
    mock_reset();

    g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE;
    g_gr.fecs_mbox0  = 0x1u;
    g_gr.gpccs_mbox0 = 0x2u;  /* FAIL sentinel */

    struct ga10b_bringup b;
    int rc = ga10b_bringup_inherit(&b);
    REQUIRE_EQ(rc, -1);
}

/* ======================================================================
 * Test 7: FECS method gateway (Phase 5 smoke test)
 *
 * Phase 5 submits DISCOVER_IMAGE_SIZE via the FECS method push
 * registers. The mock writes method_result to mailbox[0] when the
 * method addr register is written. Tests cover: happy path (method
 * returns a realistic context size), timeout (mock doesn't respond),
 * null-sentinel rejection (FECS echoed 0xDEADCA11 back), and
 * wrong-state rejection.
 * ====================================================================== */

/* Drive to PMU_UP via inherit so phase 5 precondition holds. */
static void drive_to_pmu_up_via_inherit(struct ga10b_bringup *b)
{
    mock_reset();
    /* Simulate Linux's post-boot state for inherit. */
    g_gsp.hwcfg2 = FAKE_HWCFG2_IDLE;  /* bit 13 = 0 */
    g_gr.fecs_mbox0  = 0x1u;
    g_gr.gpccs_mbox0 = 0x1u;
    REQUIRE_EQ(ga10b_bringup_inherit(b), 0);
    REQUIRE_EQ(b->state, GA10B_BRINGUP_PMU_UP);
}

static void test_fecs_method_gateway_happy_path(void)
{
    printf("== test_fecs_method_gateway_happy_path ==\n");
    struct ga10b_bringup b;
    drive_to_pmu_up_via_inherit(&b);

    /* method_auto_respond=true, method_result=0x7d500 from mock_reset */
    int rc = ga10b_bringup_address_space(&b);
    REQUIRE_EQ(rc, 0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_ENGINES_READY);
    REQUIRE_EQ(b.last_error_phase, -1);
    /* Verify the mock saw the method submission. */
    REQUIRE_EQ(g_gr.method_data_writes, 1);
    REQUIRE_EQ(g_gr.method_push_writes, 1);
    REQUIRE_EQ(g_gr.last_method_addr, 0x10u);  /* DISCOVER_IMAGE_SIZE */
    REQUIRE_EQ(g_gr.last_method_data, 0xDEADCA11u);
}

static void test_fecs_method_gateway_timeout(void)
{
    printf("== test_fecs_method_gateway_timeout ==\n");
    struct ga10b_bringup b;
    drive_to_pmu_up_via_inherit(&b);

    /* Disable auto-respond so mailbox stays 0 → timeout. */
    g_gr.method_auto_respond = false;
    int rc = ga10b_bringup_address_space(&b);
    REQUIRE_EQ(rc, -1);
    REQUIRE_EQ(b.last_error_phase, 5);
}

static void test_fecs_method_gateway_rejects_null_sentinel(void)
{
    printf("== test_fecs_method_gateway_rejects_null_sentinel ==\n");
    struct ga10b_bringup b;
    drive_to_pmu_up_via_inherit(&b);

    /* FECS echoes back the null sentinel instead of a real size. */
    g_gr.method_result = 0xDEADCA11u;
    int rc = ga10b_bringup_address_space(&b);
    REQUIRE_EQ(rc, -1);
    REQUIRE_EQ(b.last_error_phase, 5);
}

static void test_phase5_rejects_wrong_state(void)
{
    printf("== test_phase5_rejects_wrong_state ==\n");
    mock_reset();
    struct ga10b_bringup b;
    memset(&b, 0, sizeof(b));
    b.state = GA10B_BRINGUP_INIT;  /* wrong state — needs PMU_UP */
    int rc = ga10b_bringup_address_space(&b);
    REQUIRE_EQ(rc, -1);
}

/* Walk phases 1→4 in one go and verify the state machine advances
 * cleanly. Later phases (address_space, channel, smoke_test) still
 * return -1 since they're unimplemented — so we don't run them here. */
static void test_phases_1_through_4_chain(void)
{
    printf("== test_phases_1_through_4_chain ==\n");
    struct ga10b_bringup b;
    drive_to_acr_running(&b);

    REQUIRE_EQ(ga10b_bringup_fecs(&b),  0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_FECS_UP);

    REQUIRE_EQ(ga10b_bringup_gpccs(&b), 0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_GPCCS_UP);

    REQUIRE_EQ(ga10b_bringup_pmu(&b),   0);
    REQUIRE_EQ(b.state, GA10B_BRINGUP_PMU_UP);

    /* last_error_phase must remain -1 across all four phases. */
    REQUIRE_EQ(b.last_error_phase, -1);
}

/* ======================================================================
 * Test 8: channel handoff validation (pure-logic, no MMIO)
 *
 * Phase 6 parses a channel handoff block written by the Linux-side
 * helper. Validation is a pure function of the struct's contents —
 * magic, version, non-null addresses, power-of-2 GPFIFO entries.
 * These tests don't need any hardware or mock BAR0.
 * ====================================================================== */

/* Build a "valid" reference handoff struct that passes all checks.
 * Individual tests mutate one field to exercise each rejection. */
static void fill_valid_handoff(struct ga10b_channel_handoff *h)
{
    memset(h, 0, sizeof(*h));
    h->magic          = GA10B_CHANNEL_HANDOFF_MAGIC;
    h->version        = 2;
    h->channel_id     = 0;
    h->tsg_id         = 0;
    h->userd_phys     = 0x140000000ULL;
    h->userd_gp_put_offset = 35 * 4;
    h->userd_gp_get_offset = 34 * 4;
    h->gpfifo_phys    = 0x140001000ULL;
    h->gpfifo_gpu_va  = 0x1ffc000000ULL;
    h->gpfifo_entries = 1024;         /* power of 2 */
    h->gpfifo_entry_size = 8;
    h->pushbuf_phys   = 0x140010000ULL;
    h->pushbuf_gpu_va = 0x1ffc100000ULL;
    h->pushbuf_size   = 65536;
    h->semaphore_phys = 0x140020000ULL;
    h->semaphore_gpu_va = 0x1ffc200000ULL;
    h->inst_block_phys = 0x140030000ULL;
    h->initial_gp_put = 0;
    h->initial_gp_get = 0;
    h->work_submit_token = 0x1fc;     /* real GA10B token observed in bringup */
}

static void test_handoff_validate_happy_path(void)
{
    printf("== test_handoff_validate_happy_path ==\n");
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
}

static void test_handoff_validate_null_rejected(void)
{
    printf("== test_handoff_validate_null_rejected ==\n");
    REQUIRE_EQ(ga10b_validate_handoff(NULL), -1);
}

static void test_handoff_validate_bad_magic(void)
{
    printf("== test_handoff_validate_bad_magic ==\n");
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.magic = 0xDEADBEEF;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    h.magic = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
}

static void test_handoff_validate_bad_version(void)
{
    printf("== test_handoff_validate_bad_version ==\n");
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.version = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    h.version = 1;                  /* v1 lacked work_submit_token */
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    /* v2 (channel-only), v3 (+ kernel-launch state), v4 (+
     * expected_payload), v5 (+ pipeline), and v6 (+ input_buf) all
     * pass — Phase 6/7 reads only v2 fields, Phase 8 checks the
     * version at dispatch time before reading v3..v6 fields. */
    h.version = 2;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.version = 3;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.version = 4;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.version = 5;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.version = 6;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.version = 7;                  /* future, not yet defined */
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    h.version = 0xFFFFFFFF;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
}

static void test_handoff_validate_missing_doorbell_token(void)
{
    printf("== test_handoff_validate_missing_doorbell_token ==\n");
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.work_submit_token = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
}

/* ======================================================================
 * Phase 7 SEMAPHORE_RELEASE pushbuffer builder
 *
 * Verifies ga10b_build_sema_release_pushbuffer() emits the exact dword
 * stream PBDMA expects. Guards against three specific regressions:
 *
 *   1. Method-address encoding. The hardware decodes bits [12:0] of
 *      the header as method_id, where method_id = byte_off / 4. A
 *      prior version of this code placed byte_off directly at bits
 *      [11:0] (`byte_off & 0xFFFu`), producing 0x2001005C for SEM_ADDR_LO
 *      instead of the correct 0x20010017. PBDMA advanced GP_GET anyway
 *      (it consumes the dword pair), but silently discarded the method
 *      because 0x5C placed at [11:0] decodes to a different method_id
 *      than intended. The sema never actually fired post-doorbell.
 *      Tests assert literal dword values (0x20010017 .. 0x2001001B)
 *      matching nvgpu's own gv11b sema cmdbuf.
 *
 *   2. Method family. GA10B's AMPERE_CHANNEL_GPFIFO_A (0xC56F) routes
 *      the new host-semaphore methods at byte offsets 0x5C-0x6C, NOT
 *      the legacy SEMAPHOREA/B/C/D at 0x10-0x1C. The test expects the
 *      new offsets; any revert to legacy would be caught here.
 *
 *   3. Subchannel assignment. AMPERE_COMPUTE_B (class 0xC7C0) must bind
 *      to subchannel 1, not 0; nvgpu's validate_class_veid_pbdma
 *      rejects the (COMPUTE_B, subch 0, veid>=1) tuple as
 *      CLASS_SUBCH_MISMATCH. Host-family methods at 0x5C-0x6C stay on
 *      subch 0 since they are PBDMA-decoded, not GR-decoded.
 * ====================================================================== */

/* Expected method-header builder for test-side clarity. Mirrors the
 * kernel's NVC56F_METHOD_HEADER_INC macro: INC opcode (001<<29), count
 * [28:16], subch [15:13], method_id = byte_off / 4 at [12:0]. The
 * byte_off-is-shifted encoding was validated live on jetson-nano-2
 * (2026-04-18) — nvgpu's own gv11b sema cmdbuf writes 0x20010017 for
 * SEM_ADDR_LO (byte 0x5C → method_id 0x17 at [12:0]), and changing
 * the helper to match fired the sema post-doorbell. A prior iteration
 * placed byte_off at [11:0] instead; PBDMA advanced GP_GET anyway but
 * silently discarded the method. Tests below also pin several headers
 * to their literal dword value as a second line of defense — if
 * this macro and the kernel macro ever drift together, the literal
 * asserts still catch it. */
#define EXPECT_INC_HDR(count, subch, byte_off)                         \
    ((1u << 29) | ((uint32_t)(count) << 16) |                          \
     ((uint32_t)(subch) << 13) | (((uint32_t)(byte_off) >> 2) & 0x1FFFu))

/* Direct encoding test — covers both EXPECT_INC_HDR (test side) and
 * NVC56F_METHOD_HEADER_INC (kernel side) against nvgpu's reference
 * bit patterns. The EXPECT_INC_HDR asserts exercise test-side
 * encoding at byte offsets across the full method space, including
 * values that would have been truncated by the prior `byte_off &
 * 0xFFFu` encoding. The pushbuffer-builder call at the bottom pins
 * the kernel macro's output to the same nvgpu literals, so a
 * co-regression where both macros drift together still fails this
 * standalone test (layout tests below also guard via literal-dword
 * checks, but this test is self-sufficient).
 *
 * The highest byte offset we exercise is 0x33E8 — near the top of
 * AMPERE_COMPUTE_B's method space (max method in clc7c0.h is
 * 0x33EC). The 11-bit NV906F ADDRESS spec would truncate any
 * method_id ≥ 0x800 (byte ≥ 0x2000) — AMPERE_COMPUTE_B has many
 * such methods, which is why the kernel macro uses a 13-bit mask. */
static void test_method_header_encoding(void)
{
    printf("== test_method_header_encoding ==\n");

    /* --- Test-side EXPECT_INC_HDR coverage --- */

    /* Host-family (subch 0, byte 0x5C-0x6C): nvgpu's exact literals
     * from docs/reference/nvgpu-hal-sync-sema_cmdbuf_gv11b.c. */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 0, 0x5Cu), 0x20010017u);  /* SEM_ADDR_LO */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 0, 0x60u), 0x20010018u);  /* SEM_ADDR_HI */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 0, 0x64u), 0x20010019u);  /* SEM_PAYLOAD_LO */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 0, 0x68u), 0x2001001Au);  /* SEM_PAYLOAD_HI */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 0, 0x6Cu), 0x2001001Bu);  /* SEM_EXECUTE */

    /* COMPUTE_B family (subch 1, byte 0x158-0x168): method_id range
     * 0x56-0x5A, + 0x2000 subchannel bit. */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 1, 0x00u),  0x20012000u);  /* SET_OBJECT */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 1, 0x158u), 0x20012056u);
    REQUIRE_EQ(EXPECT_INC_HDR(1, 1, 0x168u), 0x2001205Au);

    /* High-offset guard (byte 0x1424 = INVALIDATE_SAMPLER_CACHE_NO_WFI
     * on AMPERE_COMPUTE_B, used by NVK in nvk_cmd_buffer_begin_compute).
     * method_id = 0x1424 / 4 = 0x509. This test would fail under the
     * prior `byte_off & 0xFFFu` encoding — 0x1424 would truncate to
     * 0x424, yielding the wrong dword. Subch 1, count 1:
     *   (1<<29) | (1<<16) | (1<<13) | 0x509 = 0x20012509. */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 1, 0x1424u), 0x20012509u);

    /* Highest COMPUTE_B method offset in clc7c0.h (0x33EC, method_id
     * 0xCFB = 12-bit). Would truncate under the hypothetical 11-bit
     * mask (0x7FFu) that matches the original NV906F spec. Pins the
     * 13-bit mask in place as a load-bearing choice, not an accident. */
    REQUIRE_EQ(EXPECT_INC_HDR(1, 1, 0x33ECu), 0x20012CFBu);

    /* Count > 1 — header count field at [28:16], not affected by the
     * address-bit fix but worth pinning against accidental overlap. */
    REQUIRE_EQ(EXPECT_INC_HDR(4, 0, 0x5Cu), 0x20040017u);
    REQUIRE_EQ(EXPECT_INC_HDR(7, 2, 0x100u), 0x20074040u);  /* subch 2 = 0x4000 */

    /* --- Kernel-side macro coverage via the builder --- */

    /* Calling ga10b_build_sema_release_pushbuffer exercises
     * NVC56F_METHOD_HEADER_INC directly. Asserting the first header
     * against nvgpu's literal `0x20010017` catches any drift in the
     * kernel macro independently of EXPECT_INC_HDR — if both macros
     * regressed to `byte_off & 0xFFFu`, EXPECT_INC_HDR tests above
     * would pass but this one would fail. */
    uint32_t kpb[GA10B_SEMA_RELEASE_PB_DWORDS];
    ga10b_build_sema_release_pushbuffer(kpb, 0x1000ULL, 0x11u);
    REQUIRE_EQ(kpb[0], 0x20010017u);
}

static void test_sema_release_pb_layout(void)
{
    printf("== test_sema_release_pb_layout ==\n");
    uint32_t pb[GA10B_SEMA_RELEASE_PB_DWORDS];
    memset(pb, 0xAB, sizeof(pb));   /* poison non-written dwords */

    /* Real GA10B semaphore VA from a bringup on jetson-nano-2 — high
     * bits = 0x1f, low 32 = 0xfc010000. Tests both halves of the VA
     * split and the upper-byte mask. */
    uint64_t sem_va  = 0x1ffc010000ULL;
    uint32_t payload = 0x0000CAFEu;

    uint32_t dwords = ga10b_build_sema_release_pushbuffer(pb, sem_va, payload);
    REQUIRE_EQ(dwords, GA10B_SEMA_RELEASE_PB_DWORDS);

    /* Headers use INC (SEC_OP=1), count=1, subch=0, method_id at
     * bits [12:0]. Expressing expected values via EXPECT_INC_HDR
     * ensures this test catches a bit-position regression even if
     * someone swaps the underlying NVC56F_METHOD_HEADER_INC macro
     * for an algebraically-different but wrongly-placed version.
     * The literal-dword guards below (0x20010017 etc.) are a second
     * line of defense — they match nvgpu's own gv11b sema cmdbuf
     * byte-for-byte, so a co-regression of both macros is still
     * detected. */
    REQUIRE_EQ(pb[0], EXPECT_INC_HDR(1, 0, 0x5Cu));  /* SEM_ADDR_LO */
    REQUIRE_EQ(pb[0], 0x20010017u);                  /* literal guard */
    REQUIRE_EQ(pb[1], 0xFC010000u);                  /* VA[31:0] */
    REQUIRE_EQ(pb[2], EXPECT_INC_HDR(1, 0, 0x60u));  /* SEM_ADDR_HI */
    REQUIRE_EQ(pb[2], 0x20010018u);                  /* literal guard */
    REQUIRE_EQ(pb[3], 0x0000001Fu);                  /* VA[39:32] masked to 8 */
    REQUIRE_EQ(pb[4], EXPECT_INC_HDR(1, 0, 0x64u));  /* SEM_PAYLOAD_LO */
    REQUIRE_EQ(pb[4], 0x20010019u);                  /* literal guard */
    REQUIRE_EQ(pb[5], 0x0000CAFEu);
    REQUIRE_EQ(pb[6], EXPECT_INC_HDR(1, 0, 0x68u));  /* SEM_PAYLOAD_HI */
    REQUIRE_EQ(pb[6], 0x2001001Au);                  /* literal guard */
    REQUIRE_EQ(pb[7], 0u);                           /* hi word unused */
    REQUIRE_EQ(pb[8], EXPECT_INC_HDR(1, 0, 0x6Cu));  /* SEM_EXECUTE */
    REQUIRE_EQ(pb[8], 0x2001001Bu);                  /* literal guard */
    /* SEM_EXECUTE = OP_RELEASE(1) | PAYLOAD_32BIT(0) | RELEASE_WFI_EN(0) */
    REQUIRE_EQ(pb[9], 0x00000001u);
}

static void test_sema_release_pb_truncates_va_upper(void)
{
    printf("== test_sema_release_pb_truncates_va_upper ==\n");
    uint32_t pb[GA10B_SEMA_RELEASE_PB_DWORDS];

    /* VA with upper bits beyond the 8-bit field: 0x1234567890000000.
     * SEM_ADDR_HI must store only bits [39:32] = 0x78 (the byte just
     * above the low 32). */
    uint64_t sem_va = 0x1234567890000000ULL;
    ga10b_build_sema_release_pushbuffer(pb, sem_va, 0x11u);

    REQUIRE_EQ(pb[1], 0x90000000u);          /* VA[31:0] */
    REQUIRE_EQ(pb[3], 0x00000078u);          /* VA[39:32] only */
}

static void test_sema_release_pb_zero_payload(void)
{
    printf("== test_sema_release_pb_zero_payload ==\n");
    uint32_t pb[GA10B_SEMA_RELEASE_PB_DWORDS];
    memset(pb, 0xAB, sizeof(pb));

    /* Zero payload still produces valid pushbuffer; only the payload
     * data word and address words vary per call. */
    ga10b_build_sema_release_pushbuffer(pb, 0x2000000000ULL, 0u);
    REQUIRE_EQ(pb[0], EXPECT_INC_HDR(1, 0, 0x5Cu));  /* header unchanged */
    REQUIRE_EQ(pb[1], 0x00000000u);                  /* VA low 32 */
    REQUIRE_EQ(pb[3], 0x00000020u);                  /* VA[39:32] = 0x20 */
    REQUIRE_EQ(pb[5], 0u);
    REQUIRE_EQ(pb[9], 0x00000001u);                  /* SEM_EXECUTE same */
}

/* ======================================================================
 * Phase 7 COMPUTE_B SEMAPHORE_RELEASE pushbuffer builder
 *
 * Guards the parallel pushbuffer-builder that targets
 * AMPERE_COMPUTE_B's REPORT_SEMAPHORE_* methods at byte offsets
 * 0x158-0x168 (after SET_OBJECT 0xC7C0). Differs from the host
 * builder in:
 *   - Byte offsets (0x158-0x168 vs 0x5C-0x6C)
 *   - OPERATION encoding (RELEASE = 0 here vs 1 on host family)
 *   - STRUCTURE_SIZE field (required, not present on host family)
 *
 * Catches regressions of those three deltas independently via layout,
 * VA-truncation, and zero-payload checks — same scheme as the host
 * builder tests above.
 * ====================================================================== */

static void test_compute_sema_release_pb_layout(void)
{
    printf("== test_compute_sema_release_pb_layout ==\n");
    uint32_t pb[GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS];
    memset(pb, 0xAB, sizeof(pb));

    uint64_t sem_va  = 0x1ffc010000ULL;
    uint32_t payload = 0x0000CAFEu;

    uint32_t dwords = ga10b_build_compute_sema_release_pushbuffer(
        pb, sem_va, payload);
    REQUIRE_EQ(dwords, GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS);

    /* First pair: SET_OBJECT on subch 1 binding AMPERE_COMPUTE_B.
     * NVK pins compute classes to subch 1 in nv_push.h (graphics uses
     * subch 0); nvgpu's validate_class_veid_pbdma rejects the
     * (COMPUTE_B, subch 0, veid>=1) tuple — that was the #273 failure.
     * Literal-dword guards below duplicate the EXPECT_INC_HDR checks
     * so a co-regression of both macros is still caught. Method_id =
     * byte_off/4: 0x00→0x000, 0x158→0x056, 0x15C→0x057, 0x160→0x058,
     * 0x164→0x059, 0x168→0x05A. Subch 1 adds 0x2000 to each. */
    REQUIRE_EQ(pb[0], EXPECT_INC_HDR(1, 1, 0x00u));         /* SET_OBJECT */
    REQUIRE_EQ(pb[0], 0x20012000u);                         /* literal guard */
    REQUIRE_EQ(pb[1], GA10B_AMPERE_COMPUTE_B_CLASS_ID);     /* AMPERE_COMPUTE_B */

    /* Payload, then address, then execute — offsets from clc7c0.h.
     * All on subch 1 to match the SET_OBJECT above. */
    REQUIRE_EQ(pb[2],  EXPECT_INC_HDR(1, 1, 0x158u)); /* SET_REPORT_SEMAPHORE_PAYLOAD_LOWER */
    REQUIRE_EQ(pb[2],  0x20012056u);                  /* literal guard */
    REQUIRE_EQ(pb[3],  0x0000CAFEu);
    REQUIRE_EQ(pb[4],  EXPECT_INC_HDR(1, 1, 0x15Cu)); /* SET_REPORT_SEMAPHORE_PAYLOAD_UPPER */
    REQUIRE_EQ(pb[4],  0x20012057u);                  /* literal guard */
    REQUIRE_EQ(pb[5],  0u);                           /* 32-bit release */
    REQUIRE_EQ(pb[6],  EXPECT_INC_HDR(1, 1, 0x160u)); /* SET_REPORT_SEMAPHORE_ADDRESS_LOWER */
    REQUIRE_EQ(pb[6],  0x20012058u);                  /* literal guard */
    REQUIRE_EQ(pb[7],  0xFC010000u);                  /* VA[31:0] */
    REQUIRE_EQ(pb[8],  EXPECT_INC_HDR(1, 1, 0x164u)); /* SET_REPORT_SEMAPHORE_ADDRESS_UPPER */
    REQUIRE_EQ(pb[8],  0x20012059u);                  /* literal guard */
    REQUIRE_EQ(pb[9],  0x0000001Fu);                  /* VA[39:32] masked to 8 bits */
    REQUIRE_EQ(pb[10], EXPECT_INC_HDR(1, 1, 0x168u)); /* REPORT_SEMAPHORE_EXECUTE */
    REQUIRE_EQ(pb[10], 0x2001205Au);                  /* literal guard */
    /* OPERATION=RELEASE(0) | STRUCTURE_SIZE=ONE_WORD(1<<3). */
    REQUIRE_EQ(pb[11], 0x00000008u);
}

static void test_compute_sema_release_pb_truncates_va_upper(void)
{
    printf("== test_compute_sema_release_pb_truncates_va_upper ==\n");
    uint32_t pb[GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS];

    /* Same VA-upper-byte mask semantics as the host builder: only
     * VA[39:32] survive. Upper bits of the VA must be masked off by
     * the builder — else a 64-bit pointer with live upper bits would
     * get smuggled into the address register. */
    uint64_t sem_va = 0x1234567890000000ULL;
    ga10b_build_compute_sema_release_pushbuffer(pb, sem_va, 0x11u);

    REQUIRE_EQ(pb[7], 0x90000000u);   /* ADDRESS_LOWER = VA[31:0] */
    REQUIRE_EQ(pb[9], 0x00000078u);   /* ADDRESS_UPPER = VA[39:32] only */
}

static void test_compute_sema_release_pb_zero_payload(void)
{
    printf("== test_compute_sema_release_pb_zero_payload ==\n");
    uint32_t pb[GA10B_COMPUTE_SEMA_RELEASE_PB_DWORDS];
    memset(pb, 0xAB, sizeof(pb));

    ga10b_build_compute_sema_release_pushbuffer(pb, 0x2000000000ULL, 0u);
    REQUIRE_EQ(pb[0],  EXPECT_INC_HDR(1, 1, 0x00u));    /* SET_OBJECT (subch 1) unchanged */
    REQUIRE_EQ(pb[1],  GA10B_AMPERE_COMPUTE_B_CLASS_ID); /* class unchanged */
    REQUIRE_EQ(pb[3],  0u);                             /* zero payload */
    REQUIRE_EQ(pb[7],  0x00000000u);                    /* VA[31:0] */
    REQUIRE_EQ(pb[9],  0x00000020u);                    /* VA[39:32] = 0x20 */
    REQUIRE_EQ(pb[11], 0x00000008u);                    /* EXECUTE unchanged */
}

/* ======================================================================
 * ga10b_build_launch_kernel_pushbuffer (Phase 8 — compute kernel launch)
 *
 * Tests pin the 13-dword dispatch pushbuffer that SLM-OS emits after
 * inheriting a v3 channel handoff. The crucial Ampere-vs-Turing split
 * — SEND_SIGNALING_PCAS2_B at method 0x02C0 with PCAS_ACTION =
 * INVALIDATE_COPY_SCHEDULE (0xA), NOT the Turing-era
 * SEND_SIGNALING_PCAS_B at 0x02BC with {INVALIDATE, SCHEDULE} bits —
 * is pinned by a dedicated test because using the older method on
 * GA10B silently no-ops dispatch (PBDMA consumes the pushbuffer, no
 * dmesg error, kernel never runs). Worth protecting aggressively so
 * a well-meaning unification across arches can't regress it.
 * ====================================================================== */

/* Test-side immediate-header encoder — mirrors the kernel's
 * ga10b_hdr_immd (SEC_OP = 4, 13-bit data at [28:16], subch at
 * [15:13], method_id = byte_off / 4 at [12:0]). Kept independent of
 * the kernel macro so a co-regression of both sides drifting
 * together still fails a literal-dword guard below. */
#define EXPECT_IMMD_HDR(subch, byte_off, data)                         \
    ((4u << 29) | ((uint32_t)(data) << 16) |                           \
     ((uint32_t)(subch) << 13) | (((uint32_t)(byte_off) >> 2) & 0x1FFFu))

static void test_launch_kernel_pb_layout(void)
{
    printf("== test_launch_kernel_pb_layout ==\n");
    uint32_t pb[GA10B_LAUNCH_KERNEL_PB_DWORDS];
    memset(pb, 0xAB, sizeof(pb));

    /* 256 B-aligned QMD VA with non-zero upper bits so the shift tests
     * both halves. qmd_gva = 0x1ffc013000 → qmd_gva >> 8 = 0x1ffc0130. */
    uint64_t qmd_gva = 0x1ffc013000ULL;

    uint32_t dwords = ga10b_build_launch_kernel_pushbuffer(pb, qmd_gva);
    REQUIRE_EQ(dwords, GA10B_LAUNCH_KERNEL_PB_DWORDS);

    /* [0-1] SET_OBJECT binding AMPERE_COMPUTE_B on subch 1. Same shape
     * as the compute-sema builder above; literal-dword guard mirrors. */
    REQUIRE_EQ(pb[0], EXPECT_INC_HDR(1, 1, 0x00u));
    REQUIRE_EQ(pb[0], 0x20012000u);
    REQUIRE_EQ(pb[1], GA10B_AMPERE_COMPUTE_B_CLASS_ID);

    /* [2-4] SET_SHADER_SHARED_MEMORY_WINDOW_A (0x02A0) count=2.
     * Matches nvk_push_dispatch_state_init for cls_compute <
     * HOPPER_COMPUTE_A (window base = 0xfe000000 with upper = 0). */
    REQUIRE_EQ(pb[2], EXPECT_INC_HDR(2, 1, 0x02A0u));
    REQUIRE_EQ(pb[2], 0x200220A8u);
    REQUIRE_EQ(pb[3], 0u);                /* upper 17 bits */
    REQUIRE_EQ(pb[4], 0xFE000000u);       /* shared-mem window base */

    /* [5-7] SET_SHADER_LOCAL_MEMORY_WINDOW_A (0x07B0) count=2.
     * Window base = 0xff000000. */
    REQUIRE_EQ(pb[5], EXPECT_INC_HDR(2, 1, 0x07B0u));
    REQUIRE_EQ(pb[5], 0x200221ECu);
    REQUIRE_EQ(pb[6], 0u);
    REQUIRE_EQ(pb[7], 0xFF000000u);       /* local-mem window base */

    /* [8] INVALIDATE_SKED_CACHES (0x0298) immediate, data=0.
     * [9] INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI (0x0244) immediate,
     *     data=0 (= LINES_ALL). */
    REQUIRE_EQ(pb[8], EXPECT_IMMD_HDR(1, 0x0298u, 0));
    REQUIRE_EQ(pb[8], 0x800020A6u);
    REQUIRE_EQ(pb[9], EXPECT_IMMD_HDR(1, 0x0244u, 0));
    REQUIRE_EQ(pb[9], 0x80002091u);

    /* [10-11] SEND_PCAS_A (0x02B4). Data = qmd_gva >> 8. */
    REQUIRE_EQ(pb[10], EXPECT_INC_HDR(1, 1, 0x02B4u));
    REQUIRE_EQ(pb[10], 0x200120ADu);
    REQUIRE_EQ(pb[11], (uint32_t)(qmd_gva >> 8));
    REQUIRE_EQ(pb[11], 0x1FFC0130u);      /* literal guard */

    /* [12] SEND_SIGNALING_PCAS2_B (0x02C0) immediate, action=0xA.
     * CRITICAL REGRESSION GUARD: this must be PCAS2_B at 0x02C0 with
     * action 0xA (INVALIDATE_COPY_SCHEDULE), NOT PCAS_B at 0x02BC.
     * See this test file's launch-kernel section doc for why. */
    REQUIRE_EQ(pb[12], EXPECT_IMMD_HDR(1, 0x02C0u, 0xA));
    REQUIRE_EQ(pb[12], 0x800A20B0u);
}

static void test_launch_kernel_pb_qmd_shift_lower(void)
{
    printf("== test_launch_kernel_pb_qmd_shift_lower ==\n");
    uint32_t pb[GA10B_LAUNCH_KERNEL_PB_DWORDS];

    /* A QMD VA that fits entirely in the low 32 bits (plus the 8-bit
     * shift). SEND_PCAS_A_QMD_ADDRESS_SHIFTED8 is a 32-bit field, so
     * VAs above (1ULL << 40) cannot be represented — the helper
     * doesn't try, it just stuffs the low 32 of (va >> 8). Test that
     * the 256-byte-aligned zero-upper case works. */
    uint64_t qmd_gva = 0x00000000FEDCBA00ULL;
    ga10b_build_launch_kernel_pushbuffer(pb, qmd_gva);

    REQUIRE_EQ(pb[11], 0x00FEDCBAu);
}

static void test_launch_kernel_pb_qmd_shift_upper(void)
{
    printf("== test_launch_kernel_pb_qmd_shift_upper ==\n");
    uint32_t pb[GA10B_LAUNCH_KERNEL_PB_DWORDS];

    /* QMD VA above 4 GB. SEND_PCAS_A_QMD_ADDRESS_SHIFTED8 is a
     * 32-bit field, so only (va >> 8) & 0xFFFFFFFF survives — upper
     * bits above (1ULL << 40) are silently dropped. Test the 40-bit
     * boundary: 0x0000123456789A00 >> 8 = 0x000000123456789A, which
     * truncates to 0x3456789A when cast to uint32. */
    uint64_t qmd_gva = 0x0000123456789A00ULL;
    ga10b_build_launch_kernel_pushbuffer(pb, qmd_gva);

    REQUIRE_EQ(pb[11], (uint32_t)((qmd_gva >> 8) & 0xFFFFFFFFu));
    REQUIRE_EQ(pb[11], 0x3456789Au);      /* literal guard */
}

static void test_launch_kernel_pb_qmd_shift_at_40bit_boundary(void)
{
    printf("== test_launch_kernel_pb_qmd_shift_at_40bit_boundary ==\n");
    uint32_t pb[GA10B_LAUNCH_KERNEL_PB_DWORDS];

    /* Exactly (1ULL << 40): the first VA whose shifted form doesn't
     * fit in the 32-bit PCAS_A_QMD_ADDRESS_SHIFTED8 field. The
     * highest VA that DOES fit is (1ULL << 40) - 1; after >> 8 that
     * becomes 0xFFFFFFFF (32 bits). (1ULL << 40) >> 8 = 0x100000000
     * overflows uint32 by exactly one bit — the cast to uint32
     * drops that bit and pb[11] reads as zero. Documents the
     * silent-overflow behavior so a future check added to reject
     * out-of-range QMDs has a pre-existing test to contradict. */
    uint64_t qmd_gva = 1ULL << 40;
    ga10b_build_launch_kernel_pushbuffer(pb, qmd_gva);

    REQUIRE_EQ(pb[11], 0u);
}

static void test_launch_kernel_pb_qmd_misalignment_truncates(void)
{
    printf("== test_launch_kernel_pb_qmd_misalignment_truncates ==\n");
    uint32_t pb[GA10B_LAUNCH_KERNEL_PB_DWORDS];

    /* SEND_PCAS_A_QMD_ADDRESS_SHIFTED8 encodes va/256, so the
     * low 8 bits of the QMD VA are lost silently — the builder
     * doesn't assert alignment. Document this by feeding a
     * misaligned VA and checking that only the top bits survive.
     * Real callers must 256 B-align their QMD (the helper's nvmap
     * allocator enforces this via 4 KB page alignment); this test
     * exists so a future alignment assertion (if added) has a
     * pre-existing reminder of the current contract. */
    uint64_t qmd_gva_aligned = 0x1ffc013000ULL;
    uint64_t qmd_gva_misaligned = qmd_gva_aligned | 0xABu;
    ga10b_build_launch_kernel_pushbuffer(pb, qmd_gva_misaligned);

    /* Low 8 bits dropped — result matches the aligned case. */
    REQUIRE_EQ(pb[11], (uint32_t)(qmd_gva_aligned >> 8));
    REQUIRE_EQ(pb[11], 0x1FFC0130u);      /* literal guard */
}

static void test_launch_kernel_pb_idempotent(void)
{
    printf("== test_launch_kernel_pb_idempotent ==\n");
    uint32_t pb1[GA10B_LAUNCH_KERNEL_PB_DWORDS];
    uint32_t pb2[GA10B_LAUNCH_KERNEL_PB_DWORDS];

    /* Same input produces same output. Guards against future
     * refactors that accidentally read stale state (globals, etc.). */
    ga10b_build_launch_kernel_pushbuffer(pb1, 0x1ffc013000ULL);
    memset(pb2, 0xCC, sizeof(pb2));
    ga10b_build_launch_kernel_pushbuffer(pb2, 0x1ffc013000ULL);
    REQUIRE_EQ(memcmp(pb1, pb2, sizeof(pb1)), 0);
}

static void test_launch_kernel_pb_uses_ampere_pcas2_b(void)
{
    printf("== test_launch_kernel_pb_uses_ampere_pcas2_b ==\n");
    uint32_t pb[GA10B_LAUNCH_KERNEL_PB_DWORDS];
    ga10b_build_launch_kernel_pushbuffer(pb, 0x100000000ULL);

    /* Ampere split guard. The dispatch-kick method MUST be
     * SEND_SIGNALING_PCAS2_B (method_id = 0x02C0 / 4 = 0xB0). If a
     * future refactor changes pb[12] to PCAS_B (0x02BC / 4 = 0xAF)
     * or swaps the action to the PCAS_B-style bitfield, GA10B
     * dispatch will silently no-op on hardware. This test shouts
     * about that at build time, not at hardware-debug time. */
    uint32_t pb12 = pb[12];
    uint32_t method_id = pb12 & 0x1FFFu;
    uint32_t sec_op    = (pb12 >> 29) & 0x7u;
    uint32_t data      = (pb12 >> 16) & 0x1FFFu;
    uint32_t subch     = (pb12 >> 13) & 0x7u;

    REQUIRE_EQ(sec_op, 4u);                    /* IMMD opcode */
    REQUIRE_EQ(subch, 1u);                     /* compute subch */
    REQUIRE_EQ(method_id, 0x02C0u / 4u);       /* PCAS2_B, not PCAS_B */
    REQUIRE_EQ(data, 0xAu);                    /* INVALIDATE_COPY_SCHEDULE */
}

/* ======================================================================
 * Handoff v3 — channel + kernel-launch state
 * ====================================================================== */

static void test_handoff_v6_layout_size(void)
{
    printf("== test_handoff_v6_layout_size ==\n");
    /* Belt-and-suspenders runtime check. The header pins the size
     * with a _Static_assert but a fresh-eyes reader shouldn't have
     * to dig into compile-time errors to discover that v2 was 120,
     * v3 was 192, v4 was 200, v5 was 216, and v6 is 232. */
    REQUIRE_EQ(sizeof(struct ga10b_channel_handoff), 232u);
}

static void test_handoff_v6_input_buf_offsets(void)
{
    printf("== test_handoff_v6_input_buf_offsets ==\n");
    /* v6 extends v5 with input_buf_phys + input_buf_size at offsets
     * 216 / 224. SLM-OS's slm_gpu_set_mnist_input writes user-
     * supplied bytes to input_buf_phys with cache_clean. */
    REQUIRE_EQ(offsetof(struct ga10b_channel_handoff, input_buf_phys),
               216u);
    REQUIRE_EQ(offsetof(struct ga10b_channel_handoff, input_buf_size),
               224u);
}

static void test_pipeline_op_layout(void)
{
    printf("== test_pipeline_op_layout ==\n");
    /* Per-op struct is wire-format shared between Linux helper and
     * SLM-OS — same byte layout must be visible from both sides. */
    REQUIRE_EQ(sizeof(struct ga10b_pipeline_op), 24u);
    REQUIRE_EQ(offsetof(struct ga10b_pipeline_op, qmd_gpu_va), 0u);
    REQUIRE_EQ(offsetof(struct ga10b_pipeline_op, output_phys), 8u);
    REQUIRE_EQ(offsetof(struct ga10b_pipeline_op, expected_payload), 16u);
    REQUIRE_EQ(offsetof(struct ga10b_pipeline_op, flags), 20u);
}

static void test_handoff_v5_pipeline_offsets(void)
{
    printf("== test_handoff_v5_pipeline_offsets ==\n");
    /* The v5 pipeline pointer + count must extend the v4 layout
     * without disturbing the existing fields. */
    REQUIRE_EQ(offsetof(struct ga10b_channel_handoff, pipeline_n_ops),
               200u);
    REQUIRE_EQ(offsetof(struct ga10b_channel_handoff, pipeline_ops_phys),
               208u);
}

static void test_handoff_v4_expected_payload_offset(void)
{
    printf("== test_handoff_v4_expected_payload_offset ==\n");
    /* Pinned by _Static_assert at compile time but worth surfacing
     * at runtime: the Linux helper and SLM-OS both byte-index into
     * the struct; a silent reorder that preserved sizeof would
     * cause SLM-OS to poll for the wrong value. */
    REQUIRE_EQ(offsetof(struct ga10b_channel_handoff,
                        expected_payload), 192u);
}

/* --- Payload-selection fallback (v3 → 0xCAFE, v4 → per-kernel) --- */

static void test_pick_launch_payload_v3_uses_fallback(void)
{
    printf("== test_pick_launch_payload_v3_uses_fallback ==\n");
    /* v3 handoffs (pre-matmul era) don't carry expected_payload.
     * The field is unused memory on the wire, so its value must
     * not leak into the poll target — the caller-supplied fallback
     * is the only correct answer. */
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.version          = 3;
    h.expected_payload = 0x12345678u;  /* garbage — must be ignored */
    REQUIRE_EQ(ga10b_pick_launch_payload(&h, 0xCAFEu), 0xCAFEu);
}

static void test_pick_launch_payload_v4_zero_uses_fallback(void)
{
    printf("== test_pick_launch_payload_v4_zero_uses_fallback ==\n");
    /* v4 handoffs where the helper didn't populate expected_payload
     * (e.g. write_cafe routed through the new code path) must still
     * poll for the legacy 0xCAFE constant — otherwise the SLM-OS
     * nvgpu launch-kernel would time out waiting for zero. */
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.version          = 4;
    h.expected_payload = 0u;
    REQUIRE_EQ(ga10b_pick_launch_payload(&h, 0xCAFEu), 0xCAFEu);
}

static void test_pick_launch_payload_v4_uses_field(void)
{
    printf("== test_pick_launch_payload_v4_uses_field ==\n");
    /* The main v4 use case: dot4 sets 300, matmul4x4 sets 30, etc.
     * Any non-zero value the helper wrote must flow through. */
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.version          = 4;

    h.expected_payload = 300u;  /* dot4 */
    REQUIRE_EQ(ga10b_pick_launch_payload(&h, 0xCAFEu), 300u);

    h.expected_payload = 30u;   /* matmul4x4 Gram diagonal C[0][0] */
    REQUIRE_EQ(ga10b_pick_launch_payload(&h, 0xCAFEu), 30u);

    h.expected_payload = 0xDEADBEEFu;  /* arbitrary non-zero */
    REQUIRE_EQ(ga10b_pick_launch_payload(&h, 0xCAFEu), 0xDEADBEEFu);
}

static void test_pick_launch_payload_future_version_uses_field(void)
{
    printf("== test_pick_launch_payload_future_version_uses_field ==\n");
    /* A v5 handoff that preserves the v4 expected_payload field
     * (the normal forward-compatible extension pattern) should be
     * treated like v4: non-zero field takes precedence over the
     * fallback. The `version >= 4` predicate guarantees this. */
    struct ga10b_channel_handoff h;
    fill_valid_handoff(&h);
    h.version          = 5;
    h.expected_payload = 42u;
    REQUIRE_EQ(ga10b_pick_launch_payload(&h, 0xCAFEu), 42u);
}

static void test_handoff_validate_v3_accepted(void)
{
    printf("== test_handoff_validate_v3_accepted ==\n");
    struct ga10b_channel_handoff h;

    /* Both v2 and v3 are valid channel-inherit handoffs. Phase 8
     * (launch_kernel) gates on version at dispatch time. */
    fill_valid_handoff(&h);
    h.version = 2;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);

    h.version = 3;
    /* Populate v3 extension fields — the validator doesn't inspect
     * them but a realistic v3 handoff has them filled. */
    h.shader_phys   = 0x1c0010000ULL;
    h.shader_gpu_va = 0x1ffc010000ULL;
    h.cbuf_phys     = 0x1c0011000ULL;
    h.cbuf_gpu_va   = 0x1ffc011000ULL;
    h.qmd_phys      = 0x1c0013000ULL;
    h.qmd_gpu_va    = 0x1ffc013000ULL;
    h.output_phys   = 0x1c0012000ULL;
    h.output_gpu_va = 0x1ffc012000ULL;
    h.shader_size   = 640u;
    h.cbuf_size     = 512u;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);

    /* v3 with zeroed extension fields also passes validation —
     * Phase 8 will reject at dispatch time via its own zero-check. */
    h.shader_phys = 0;
    h.shader_gpu_va = 0;
    h.qmd_gpu_va = 0;
    h.output_phys = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
}

static void test_handoff_validate_null_addresses(void)
{
    printf("== test_handoff_validate_null_addresses ==\n");
    struct ga10b_channel_handoff h;

    /* Each of the 4 physical addresses must be non-zero. */
    fill_valid_handoff(&h); h.userd_phys = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);

    fill_valid_handoff(&h); h.gpfifo_phys = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);

    fill_valid_handoff(&h); h.pushbuf_phys = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);

    fill_valid_handoff(&h); h.semaphore_phys = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
}

static void test_handoff_validate_gpfifo_entries(void)
{
    printf("== test_handoff_validate_gpfifo_entries ==\n");
    struct ga10b_channel_handoff h;

    /* Zero entries rejected. */
    fill_valid_handoff(&h); h.gpfifo_entries = 0;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);

    /* Non-power-of-2 rejected (3, 5, 6, 7, 1000). */
    fill_valid_handoff(&h); h.gpfifo_entries = 3;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    h.gpfifo_entries = 5;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    h.gpfifo_entries = 7;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);
    h.gpfifo_entries = 1000;
    REQUIRE_EQ(ga10b_validate_handoff(&h), -1);

    /* Small powers of 2 accepted. */
    h.gpfifo_entries = 1;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.gpfifo_entries = 2;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.gpfifo_entries = 16;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
    h.gpfifo_entries = 2048;
    REQUIRE_EQ(ga10b_validate_handoff(&h), 0);
}

/* ======================================================================
 * Test 9: handoff scanner (finds magic in a memory buffer)
 * ====================================================================== */

static void test_scanner_finds_magic_at_start(void)
{
    printf("== test_scanner_finds_magic_at_start ==\n");
    /* Host-side buffer, 64 KB, page-aligned. */
    uint32_t *buf;
    if (posix_memalign((void **)&buf, 4096, 65536) != 0) {
        REQUIRE(0 && "posix_memalign");
        return;
    }
    memset(buf, 0, 65536);
    buf[0] = GA10B_CHANNEL_HANDOFF_MAGIC;

    uint64_t start = (uint64_t)(uintptr_t)buf;
    uint64_t end   = start + 65536;
    uint64_t found = ga10b_find_handoff_in_range(start, end, 4096);
    REQUIRE_EQ(found, start);
    free(buf);
}

static void test_scanner_finds_magic_midrange(void)
{
    printf("== test_scanner_finds_magic_midrange ==\n");
    uint32_t *buf;
    if (posix_memalign((void **)&buf, 4096, 65536) != 0) {
        REQUIRE(0 && "posix_memalign");
        return;
    }
    memset(buf, 0, 65536);
    /* Place magic at page 8 (offset 32 KB). */
    buf[8 * 1024] = GA10B_CHANNEL_HANDOFF_MAGIC;

    uint64_t start = (uint64_t)(uintptr_t)buf;
    uint64_t expected = start + 8 * 4096;
    uint64_t found = ga10b_find_handoff_in_range(start, start + 65536, 4096);
    REQUIRE_EQ(found, expected);
    free(buf);
}

static void test_scanner_returns_zero_on_miss(void)
{
    printf("== test_scanner_returns_zero_on_miss ==\n");
    uint32_t *buf;
    if (posix_memalign((void **)&buf, 4096, 65536) != 0) {
        REQUIRE(0 && "posix_memalign");
        return;
    }
    memset(buf, 0, 65536);
    /* No magic anywhere — scan should return 0. */
    uint64_t start = (uint64_t)(uintptr_t)buf;
    uint64_t found = ga10b_find_handoff_in_range(start, start + 65536, 4096);
    REQUIRE_EQ(found, 0);
    free(buf);
}

static void test_scanner_skips_between_pages(void)
{
    printf("== test_scanner_skips_between_pages ==\n");
    uint32_t *buf;
    if (posix_memalign((void **)&buf, 4096, 65536) != 0) {
        REQUIRE(0 && "posix_memalign");
        return;
    }
    memset(buf, 0, 65536);
    /* Put magic at offset 4 (inside first page, not at the top). With
     * stride 4096 the scanner reads only page-top words, so it misses. */
    buf[1] = GA10B_CHANNEL_HANDOFF_MAGIC;

    uint64_t start = (uint64_t)(uintptr_t)buf;
    uint64_t found = ga10b_find_handoff_in_range(start, start + 65536, 4096);
    REQUIRE_EQ(found, 0);
    free(buf);
}

static void test_scanner_empty_range(void)
{
    printf("== test_scanner_empty_range ==\n");
    /* start == end → no iterations → returns 0. */
    uint64_t found = ga10b_find_handoff_in_range(0x1000, 0x1000, 4096);
    REQUIRE_EQ(found, 0);
}

/* ======================================================================
 * Pipeline poll-match predicate (ga10b_channel_handoff.h
 * `ga10b_poll_match`). Used by both the kernel-side
 * ga10b_submit_and_poll and the Linux launcher's gpu_submit_and_poll.
 * Pin the two-mode contract so a future refactor doesn't silently
 * collapse "any non-zero" to "exact match against 0", which would
 * make an immediate (pre-cleared) buffer report success.
 * ====================================================================== */

static void test_poll_match_exact(void)
{
    printf("== test_poll_match_exact ==\n");
    /* Exact-match path used by v3/v4 single-shot dispatches and any
     * v5 op that pins a bit pattern. */
    REQUIRE(ga10b_poll_match(0xCAFEu, 0xCAFEu));
    REQUIRE(!ga10b_poll_match(0xCAFEu, 0xCAFFu));
    REQUIRE(!ga10b_poll_match(0u, 0xCAFEu));
    REQUIRE(ga10b_poll_match(0x41F00000u, 0x41F00000u));  /* 30.0f */
    REQUIRE(ga10b_poll_match(0xC003B6C9u, 0xC003B6C9u));  /* MNIST logits[0] */
}

static void test_poll_match_any_nonzero(void)
{
    printf("== test_poll_match_any_nonzero ==\n");
    /* expected_payload == 0 means "any non-zero". Used by v5 ops
     * whose output bit pattern isn't predictable (FFMA-vs-numpy
     * ULP drift in MNIST conv outputs). */
    REQUIRE(ga10b_poll_match(0xCAFEu, 0u));
    REQUIRE(ga10b_poll_match(0x00000001u, 0u));
    REQUIRE(ga10b_poll_match(0xFFFFFFFFu, 0u));

    /* Critical foot-gun guard: if the buffer is freshly cleared to
     * 0, "any non-zero" must NOT match — otherwise the launcher
     * would report success before the GPU actually wrote. */
    REQUIRE(!ga10b_poll_match(0u, 0u));
}

/* ======================================================================
 * Pipeline op array per-element validator
 * (ga10b_channel_handoff.h `ga10b_pipeline_op_is_valid`). The kernel
 * runner consults this before dispatching each op so a malformed
 * handoff fails fast instead of submitting a QMD address of 0
 * (which the GPU treats as a no-op with no error reported).
 * ====================================================================== */

static void test_pipeline_op_validator_accepts_well_formed(void)
{
    printf("== test_pipeline_op_validator_accepts_well_formed ==\n");
    struct ga10b_pipeline_op op = {
        .qmd_gpu_va       = 0x1ffc012000ULL,
        .output_phys      = 0x180000000ULL,
        .expected_payload = 0xCAFEu,
        .flags            = 0u,
    };
    REQUIRE(ga10b_pipeline_op_is_valid(&op));

    /* expected_payload == 0 (any-nonzero mode) is fine. */
    op.expected_payload = 0u;
    REQUIRE(ga10b_pipeline_op_is_valid(&op));
}

static void test_pipeline_op_validator_rejects_zero_qmd(void)
{
    printf("== test_pipeline_op_validator_rejects_zero_qmd ==\n");
    struct ga10b_pipeline_op op = {
        .qmd_gpu_va       = 0u,
        .output_phys      = 0x180000000ULL,
        .expected_payload = 0xCAFEu,
        .flags            = 0u,
    };
    REQUIRE(!ga10b_pipeline_op_is_valid(&op));
}

static void test_pipeline_op_validator_rejects_zero_output(void)
{
    printf("== test_pipeline_op_validator_rejects_zero_output ==\n");
    struct ga10b_pipeline_op op = {
        .qmd_gpu_va       = 0x1ffc012000ULL,
        .output_phys      = 0u,
        .expected_payload = 0xCAFEu,
        .flags            = 0u,
    };
    REQUIRE(!ga10b_pipeline_op_is_valid(&op));
}

static void test_pipeline_op_validator_rejects_null(void)
{
    printf("== test_pipeline_op_validator_rejects_null ==\n");
    REQUIRE(!ga10b_pipeline_op_is_valid(NULL));
}

/* The pipeline-runner caps `pipeline_n_ops` at GA10B_PIPELINE_MAX_OPS
 * to prevent a malformed handoff (n_ops = 0xFFFFFFFF) from looping
 * past the 4 KB ops array into adjacent memory. The cap matches the
 * launcher's allocation budget — one 4 KB page of pipeline_op
 * structs (170 of them at 24 bytes each). */
static void test_pipeline_max_ops_constant(void)
{
    printf("== test_pipeline_max_ops_constant ==\n");
    /* The constant must match the launcher's per-page capacity. */
    REQUIRE_EQ((unsigned)GA10B_PIPELINE_MAX_OPS, 170u);
    REQUIRE_EQ(GA10B_PIPELINE_MAX_OPS * sizeof(struct ga10b_pipeline_op),
               4080u);  /* < 4096, so a 4 KB page holds all ops */
    /* MNIST currently uses 8 ops — comfortably under the cap. If
     * GA10B_PIPELINE_MAX_OPS is ever lowered, the MNIST launcher
     * stops working without surfacing a clear build error; tests
     * fail loudly instead. */
    REQUIRE(GA10B_PIPELINE_MAX_OPS >= 8u);
}

/* ======================================================================
 * gpu_qmd_set_bits — pure-logic bit-range setter for QMDV03_00
 * (scripts/gpu-qmd-bits.h). Exercised here because the production
 * call-site (scripts/gpu-launch-common.c) only runs on Jetson and
 * has no host-side smoke coverage; bit-twiddling regressions would
 * otherwise only surface on hardware.
 * ====================================================================== */

static void test_qmd_set_bits_single_bit(void)
{
    printf("== test_qmd_set_bits_single_bit ==\n");
    uint32_t qmd[64] = {0};
    /* hi == lo: the simplest case, single-bit field. */
    gpu_qmd_set_bits(qmd, 5, 5, 1);
    REQUIRE_EQ(qmd[0], 1u << 5);

    /* Setting the same bit to 0 clears it without disturbing
     * neighbors. */
    qmd[0] = 0xFFFFFFFFu;
    gpu_qmd_set_bits(qmd, 5, 5, 0);
    REQUIRE_EQ(qmd[0], 0xFFFFFFFFu & ~(1u << 5));
}

static void test_qmd_set_bits_within_one_word(void)
{
    printf("== test_qmd_set_bits_within_one_word ==\n");
    uint32_t qmd[64] = {0};
    /* 8-bit field at bits [15:8], value 0xAB. */
    gpu_qmd_set_bits(qmd, 15, 8, 0xAB);
    REQUIRE_EQ(qmd[0], 0xABu << 8);

    /* Writing into a non-zero word: must mask out only the field's
     * bits and OR in the new value. */
    qmd[0] = 0x12345678u;
    gpu_qmd_set_bits(qmd, 15, 8, 0xAB);
    REQUIRE_EQ(qmd[0], (0x12345678u & ~(0xFFu << 8)) | (0xABu << 8));
}

static void test_qmd_set_bits_full_32_bit_word(void)
{
    printf("== test_qmd_set_bits_full_32_bit_word ==\n");
    uint32_t qmd[64] = {0};
    /* 32-bit field aligned to a word boundary. lo % 32 == 0,
     * mask = 0xFFFFFFFF, no bit-shift quirks. */
    gpu_qmd_set_bits(qmd, 31, 0, 0xDEADBEEFu);
    REQUIRE_EQ(qmd[0], 0xDEADBEEFu);

    /* Same field on a non-zero word should fully overwrite — the
     * 32-bit mask covers the whole word. */
    qmd[1] = 0xCAFEBABEu;
    gpu_qmd_set_bits(qmd, 63, 32, 0x11223344u);
    REQUIRE_EQ(qmd[1], 0x11223344u);
}

static void test_qmd_set_bits_spans_two_words(void)
{
    printf("== test_qmd_set_bits_spans_two_words ==\n");
    uint32_t qmd[64] = {0};
    /* 8-bit field at bits [35:28] — straddles the word_lo/word_hi
     * boundary at bit 32. Low 4 bits go to qmd[0][31:28]; high 4
     * bits go to qmd[1][3:0]. Pick a value where every nibble is
     * distinct so any bit-mismatch shows up. */
    gpu_qmd_set_bits(qmd, 35, 28, 0xC9);  /* 0b1100_1001 */
    REQUIRE_EQ(qmd[0], 0x9u << 28);
    REQUIRE_EQ(qmd[1], 0xCu);

    /* Same span over a non-zero qmd: must preserve bits outside
     * [35:28] in both qmd[0] and qmd[1]. */
    memset(qmd, 0, sizeof(qmd));
    qmd[0] = 0x0FFFFFFFu;  /* bits [27:0] set */
    qmd[1] = 0xFFFFFFF0u;  /* bits [31:4] set */
    gpu_qmd_set_bits(qmd, 35, 28, 0x55);  /* 0b0101_0101 */
    REQUIRE_EQ(qmd[0], 0x0FFFFFFFu | (0x5u << 28));
    REQUIRE_EQ(qmd[1], 0xFFFFFFF0u | 0x5u);
}

static void test_qmd_set_bits_truncates_excess_value(void)
{
    printf("== test_qmd_set_bits_truncates_excess_value ==\n");
    uint32_t qmd[64] = {0};
    /* 4-bit field at bits [3:0]; passing 0xFFu must truncate to
     * 0xF — high bits silently dropped. */
    gpu_qmd_set_bits(qmd, 3, 0, 0xFFu);
    REQUIRE_EQ(qmd[0], 0xFu);

    /* 16-bit field with a 17-bit-wide value — top bit must be
     * dropped, not bleed into adjacent bits. */
    memset(qmd, 0, sizeof(qmd));
    gpu_qmd_set_bits(qmd, 23, 8, 0x1FFFFu);
    REQUIRE_EQ(qmd[0], 0xFFFFu << 8);
}

static void test_qmd_set_bits_preserves_neighbors(void)
{
    printf("== test_qmd_set_bits_preserves_neighbors ==\n");
    uint32_t qmd[64];
    memset(qmd, 0xAB, sizeof(qmd));
    /* Pick a real Ampere field: QMD_PROGRAM_ADDRESS_LOWER spans
     * bits [1567:1536] (32 bits, word-aligned at qmd[48]). Writing
     * into it must touch nothing else. */
    uint32_t neighbor_lo = qmd[47];
    uint32_t neighbor_hi = qmd[49];
    gpu_qmd_set_bits(qmd, 1567, 1536, 0x1ffc010000ULL & 0xFFFFFFFFu);
    REQUIRE_EQ(qmd[47], neighbor_lo);
    REQUIRE_EQ(qmd[49], neighbor_hi);
    REQUIRE_EQ(qmd[48], (uint32_t)(0x1ffc010000ULL & 0xFFFFFFFFu));

    /* And a real two-word-spanning field: cbuf addr-lo BASE is at
     * bit 1024 (qmd[32]) and runs 32 bits. Word-aligned, so
     * single-word path — but useful sanity that we hit the right
     * word index from a high bit number. */
    memset(qmd, 0xAB, sizeof(qmd));
    uint32_t pre = qmd[33];
    gpu_qmd_set_bits(qmd, 1055, 1024, 0xDEADBEEFu);
    REQUIRE_EQ(qmd[32], 0xDEADBEEFu);
    REQUIRE_EQ(qmd[33], pre);
}

/* ======================================================================
 * Entry
 * ====================================================================== */

int main(void)
{
    printf("[test_ga10b_bringup] starting\n");

    test_firmware_accessor_sizes();
    test_firmware_accessor_rejects_out_of_range();
    test_firmware_accessor_null_out();

    test_prepare_rejects_null_platform();
    test_prepare_rejects_null_bringup();
    test_prepare_populates_state();

    test_phases_reject_wrong_state();
    test_phases_reject_null_bringup();

    test_acr_sequence_happy_path();
    test_acr_returns_error_on_brom_fail();

    test_fecs_happy_path();
    test_fecs_fails_on_fail_sentinel();
    test_fecs_fails_on_checksum_sentinel();
    test_gpccs_happy_path();
    test_gpccs_rejects_wrong_state();
    test_gpccs_fails_on_fail_sentinel();
    test_pmu_is_noop();
    test_phases_1_through_4_chain();

    test_inherit_happy_path();
    test_inherit_rejects_priv_lockdown();
    test_inherit_rejects_fecs_not_ready();
    test_inherit_rejects_gpccs_not_ready();

    test_fecs_method_gateway_happy_path();
    test_fecs_method_gateway_timeout();
    test_fecs_method_gateway_rejects_null_sentinel();
    test_phase5_rejects_wrong_state();

    test_handoff_validate_happy_path();
    test_handoff_validate_null_rejected();
    test_handoff_validate_bad_magic();
    test_handoff_validate_bad_version();
    test_handoff_validate_null_addresses();
    test_handoff_validate_gpfifo_entries();
    test_handoff_validate_missing_doorbell_token();

    test_method_header_encoding();

    test_sema_release_pb_layout();
    test_sema_release_pb_truncates_va_upper();
    test_sema_release_pb_zero_payload();

    test_compute_sema_release_pb_layout();
    test_compute_sema_release_pb_truncates_va_upper();
    test_compute_sema_release_pb_zero_payload();

    test_launch_kernel_pb_layout();
    test_launch_kernel_pb_qmd_shift_lower();
    test_launch_kernel_pb_qmd_shift_upper();
    test_launch_kernel_pb_qmd_shift_at_40bit_boundary();
    test_launch_kernel_pb_qmd_misalignment_truncates();
    test_launch_kernel_pb_idempotent();
    test_launch_kernel_pb_uses_ampere_pcas2_b();

    test_handoff_v6_layout_size();
    test_handoff_v4_expected_payload_offset();
    test_handoff_v5_pipeline_offsets();
    test_handoff_v6_input_buf_offsets();
    test_pipeline_op_layout();
    test_pick_launch_payload_v3_uses_fallback();
    test_pick_launch_payload_v4_zero_uses_fallback();
    test_pick_launch_payload_v4_uses_field();
    test_pick_launch_payload_future_version_uses_field();
    test_handoff_validate_v3_accepted();

    test_scanner_finds_magic_at_start();
    test_scanner_finds_magic_midrange();
    test_scanner_returns_zero_on_miss();
    test_scanner_skips_between_pages();
    test_scanner_empty_range();

    test_poll_match_exact();
    test_poll_match_any_nonzero();
    test_pipeline_op_validator_accepts_well_formed();
    test_pipeline_op_validator_rejects_zero_qmd();
    test_pipeline_op_validator_rejects_zero_output();
    test_pipeline_op_validator_rejects_null();
    test_pipeline_max_ops_constant();

    test_qmd_set_bits_single_bit();
    test_qmd_set_bits_within_one_word();
    test_qmd_set_bits_full_32_bit_word();
    test_qmd_set_bits_spans_two_words();
    test_qmd_set_bits_truncates_excess_value();
    test_qmd_set_bits_preserves_neighbors();

    if (failures) {
        fprintf(stderr, "[test_ga10b_bringup] %d FAILURES\n", failures);
        return 1;
    }
    printf("[test_ga10b_bringup] all OK\n");
    return 0;
}
