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
#include "../../kernel/gpu/nvidia/gsp.h"

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

    /* State should not have changed. */
    REQUIRE_EQ(b.state, GA10B_BRINGUP_INIT);
}

static void test_phases_reject_null_bringup(void)
{
    printf("== test_phases_reject_null_bringup ==\n");
    REQUIRE_EQ(ga10b_bringup_acr(NULL),            -1);
    REQUIRE_EQ(ga10b_bringup_fecs(NULL),           -1);
    REQUIRE_EQ(ga10b_bringup_gpccs(NULL),          -1);
    REQUIRE_EQ(ga10b_bringup_pmu(NULL),            -1);
    REQUIRE_EQ(ga10b_bringup_address_space(NULL),  -1);
    REQUIRE_EQ(ga10b_bringup_channel(NULL),        -1);
    REQUIRE_EQ(ga10b_bringup_smoke_test(NULL),     -1);
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

    if (failures) {
        fprintf(stderr, "[test_ga10b_bringup] %d FAILURES\n", failures);
        return 1;
    }
    printf("[test_ga10b_bringup] all OK\n");
    return 0;
}
