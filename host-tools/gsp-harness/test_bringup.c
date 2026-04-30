/*
 * test_bringup.c — regression tests for the GSP-RM bringup
 * pure-logic helpers (kernel/gpu/nvidia/bringup.c).
 *
 * Two surfaces:
 *
 *   1. gsp_bringup_select_sig_index — nouveau ga102 sig-index
 *      algorithm. Validated against the values nouveau computes
 *      for a known fuse register / SignatureVersions pair.
 *   2. gsp_bringup_patch_dmemmapper_frts — DMEMMAPPER patcher.
 *      Builds a synthetic FWSEC DMEM, walks the interface table,
 *      asserts the {init_cmd, read_vbios, frts_region} structs
 *      land at the correct offsets with the right contents.
 *
 * These are pure functions — no GPU, no platform vtable. The
 * full FWSEC-FRTS bringup itself can only be tested against real
 * hardware via `gsp-harness --fwsec-frts`.
 *
 * Wired into `make test-bringup`.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/bringup.h"
#include "../../kernel/gpu/nvidia/gsp.h"
#include "../../kernel/gpu/nvidia/gsp_wpr_meta.h"
#include "../../kernel/gpu/nvidia/nvfw.h"

/* nvidia_vbios_platform_load is platform-specific (linux_platform.c
 * for the harness, nvidia_gsp_platform.c on bare-metal). The
 * unit-test binary doesn't link either — and doesn't call any
 * function that uses it — so a stub satisfies the linker. */
int nvidia_vbios_platform_load(const uint8_t **out_data, size_t *out_size)
{
    (void)out_data; (void)out_size;
    return -1;
}

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* ---- gsp_bringup_select_sig_index tests ---- */

static void test_sig_index_rtx3050_real_values(void)
{
    /* Real RTX 3050 values observed on test-pc 2026-04-15:
     *   fuse_reg[0x8241e0] = 0x3
     *   sig_versions = 0xF (4 sigs valid)
     *   sig_count = 4
     * Nouveau's algorithm yields idx = 2 (matches our hardware test). */
    int idx = gsp_bringup_select_sig_index(/*fuse_reg=*/ 0x3,
                                           /*sig_versions=*/ 0xF,
                                           /*sig_count=*/ 4);
    REQUIRE(idx == 2);
}

static void test_sig_index_first_signature(void)
{
    /* Card with only fuse-version 1 burned (reg_bit = 1 << 1 = 2 = 0x2)
     * and ucode supporting sigs for versions 0..3 (sig_versions=0xF).
     *
     *   fls(0x1) = 1
     *   reg_bit = 1 << 1 = 2
     *   sig_versions=0xF; reg_bit & sig_versions = 2 (matches)
     *   loop: !(2 & 0xf & 1) = !(0) → true, increment by sig_versions&1=1
     *         working=0x7, reg_bit=1
     *         !(1 & 7 & 1) = !(1) → false, break
     *   idx = 1
     */
    int idx = gsp_bringup_select_sig_index(0x1, 0xF, 4);
    REQUIRE(idx == 1);
}

static void test_sig_index_third_signature(void)
{
    /* Highest fuse burned (bit 2 in reg = 4) → sig version 3.
     *   fls(0x4) = 3
     *   reg_bit = 1 << 3 = 8
     *   But sig_versions = 0xF → bit 3 set → matches.
     *   Loop counts versions 0,1,2 (each contributing 1) → idx=3.
     */
    int idx = gsp_bringup_select_sig_index(0x4, 0xF, 4);
    REQUIRE(idx == 3);
}

static void test_sig_index_no_matching_sig(void)
{
    /* Fuse demands sig version 4 (reg_bit = 1 << 3 = 0x8) but ucode
     * only supplies versions 0 and 1 (sig_versions = 0x3). No sig
     * matches — must return -1. */
    int idx = gsp_bringup_select_sig_index(0x4, 0x3, 2);
    REQUIRE(idx < 0);
}

static void test_sig_index_no_fuse_burned(void)
{
    /* Dev-kit / no-fuse case: nova-core falls back to the last
     * available signature; we match. */
    int idx = gsp_bringup_select_sig_index(/*fuse_reg=*/ 0,
                                           /*sig_versions=*/ 0xF,
                                           /*sig_count=*/ 4);
    REQUIRE(idx == 3);
}

static void test_sig_index_zero_count(void)
{
    /* Pre-Turing or unsigned ucode — no sigs at all.
     * Must return -1 for the bringup to fail closed. */
    int idx = gsp_bringup_select_sig_index(0x3, 0xF, 0);
    REQUIRE(idx < 0);
}

static void test_sig_index_clamps_overflow(void)
{
    /* Pathological: fuse demands a sig the ucode says it has, but
     * sig_count is smaller than the index the algorithm computes.
     * Implementation clamps to sig_count - 1 to stay in-bounds. */
    int idx = gsp_bringup_select_sig_index(0x4, 0xF, /*sig_count=*/ 2);
    REQUIRE(idx >= 0);
    REQUIRE(idx < 2);
}

/* ---- gsp_bringup_patch_dmemmapper_frts tests ----
 *
 * Build a synthetic DMEM containing exactly one DMEMMAPPER app
 * entry, point cmd_in_buffer_offset at a known location, and
 * verify the patcher writes init_cmd / read_vbios / frts_region
 * at the right places with the right values.
 */

#define DMEM_SIZE             1024
#define INTERFACE_OFF         0x40
#define DMEMMAPPER_BASE       0x100
#define CMD_BUF_OFFSET        0x200
#define WPR2_ADDR             0x17FE00000ull
#define WPR2_SIZE             0x100000ull

static void build_synthetic_dmem(uint8_t *dmem)
{
    memset(dmem, 0, DMEM_SIZE);

    /* Interface header at INTERFACE_OFF: ver=1, hdr=4, len=12, cnt=1 */
    dmem[INTERFACE_OFF + 0] = 1;     /* ver */
    dmem[INTERFACE_OFF + 1] = 4;     /* hdr */
    dmem[INTERFACE_OFF + 2] = 12;    /* len */
    dmem[INTERFACE_OFF + 3] = 1;     /* cnt */

    /* Entry 0: id = 0x04 (DMEMMAPPER), dmem_base = DMEMMAPPER_BASE */
    uint8_t *e = dmem + INTERFACE_OFF + 4;
    e[0] = 0x04; e[1] = 0; e[2] = 0; e[3] = 0;       /* id u32 LE */
    /* dmem_base u32 LE — DMEMMAPPER_BASE = 0x100 */
    e[4] = DMEMMAPPER_BASE & 0xff;
    e[5] = (DMEMMAPPER_BASE >> 8) & 0xff;
    e[6] = (DMEMMAPPER_BASE >> 16) & 0xff;
    e[7] = (DMEMMAPPER_BASE >> 24) & 0xff;

    /* DMEMMAPPER app data at dmem[DMEMMAPPER_BASE]:
     *   +0x00  signature (u32) — leave 0
     *   +0x08  cmd_in_buffer_offset (u32) — point at CMD_BUF_OFFSET */
    uint8_t *m = dmem + DMEMMAPPER_BASE;
    m[8]  = CMD_BUF_OFFSET & 0xff;
    m[9]  = (CMD_BUF_OFFSET >> 8) & 0xff;
    m[10] = (CMD_BUF_OFFSET >> 16) & 0xff;
    m[11] = (CMD_BUF_OFFSET >> 24) & 0xff;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void test_patch_writes_init_cmd_at_2c(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) == 0);

    /* init_cmd at app.dmem_base + 0x2c = DMEMMAPPER_BASE + 0x2c */
    uint32_t init_cmd = rd32(dmem + DMEMMAPPER_BASE + 0x2c);
    REQUIRE(init_cmd == 0x15);    /* DMEMMAPPER_INIT_CMD_FRTS */
}

static void test_patch_writes_read_vbios_struct(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) == 0);

    /* read_vbios at cmd_in_buffer_offset + 0:
     *   ver = 1, hdr = 24, addr = 0, size = 0, flags = 2 */
    uint8_t *rv = dmem + CMD_BUF_OFFSET;
    REQUIRE(rd32(rv +  0) == 1);
    REQUIRE(rd32(rv +  4) == 24);
    REQUIRE(rd32(rv +  8) == 0);    /* addr lo */
    REQUIRE(rd32(rv + 12) == 0);    /* addr hi */
    REQUIRE(rd32(rv + 16) == 0);    /* size */
    REQUIRE(rd32(rv + 20) == 2);    /* flags */
}

static void test_patch_writes_frts_region(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) == 0);

    /* frts_region at cmd_in_buffer_offset + 24:
     *   ver=1 hdr=20 addr=(WPR2_ADDR>>12) size=(WPR2_SIZE>>12) type=2 (FB) */
    uint8_t *fr = dmem + CMD_BUF_OFFSET + 24;
    REQUIRE(rd32(fr +  0) == 1);
    REQUIRE(rd32(fr +  4) == 20);
    REQUIRE(rd32(fr +  8) == (uint32_t)(WPR2_ADDR >> 12));
    REQUIRE(rd32(fr + 12) == (uint32_t)(WPR2_SIZE >> 12));
    REQUIRE(rd32(fr + 16) == 2);    /* FRTS_REGION_TYPE_FB */
}

static void test_patch_rejects_no_dmemmapper(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);
    /* Change the entry id from 0x04 to something else. */
    dmem[INTERFACE_OFF + 4] = 0x99;

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) < 0);
}

static void test_patch_rejects_bad_interface_version(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);
    dmem[INTERFACE_OFF + 0] = 99;    /* ver != 1 */

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) < 0);
}

static void test_patch_rejects_interface_off_past_end(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              /*interface_off=*/ DMEM_SIZE,
                                              WPR2_ADDR, WPR2_SIZE) < 0);
}

static void test_patch_rejects_cmd_buf_overflow(void)
{
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);
    /* Point cmd_in_buffer_offset at a location too close to the
     * end of DMEM — read_vbios + frts_region (44 bytes total) won't
     * fit, patcher must reject. */
    uint8_t *m = dmem + DMEMMAPPER_BASE;
    uint32_t bad = DMEM_SIZE - 10;
    m[8]  = bad & 0xff;
    m[9]  = (bad >> 8) & 0xff;
    m[10] = (bad >> 16) & 0xff;
    m[11] = (bad >> 24) & 0xff;

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) < 0);
}

static void test_patch_skips_non_dmemmapper_entries(void)
{
    /* Two entries: first one is some other id, second is DMEMMAPPER.
     * Patcher must skip the first and find DMEMMAPPER at index 1. */
    uint8_t dmem[DMEM_SIZE];
    memset(dmem, 0, sizeof(dmem));
    dmem[INTERFACE_OFF + 0] = 1; dmem[INTERFACE_OFF + 1] = 4;
    dmem[INTERFACE_OFF + 2] = 20; dmem[INTERFACE_OFF + 3] = 2;    /* 2 entries */

    /* Entry 0: id=0x05 (FAKE), dmem_base = 0x180 */
    uint8_t *e0 = dmem + INTERFACE_OFF + 4;
    e0[0] = 0x05; e0[4] = 0x80; e0[5] = 0x01;

    /* Entry 1: id=0x04 (DMEMMAPPER), dmem_base = DMEMMAPPER_BASE */
    uint8_t *e1 = e0 + 8;
    e1[0] = 0x04;
    e1[4] = DMEMMAPPER_BASE & 0xff;
    e1[5] = (DMEMMAPPER_BASE >> 8) & 0xff;

    /* Wire cmd_in_buffer_offset on the real DMEMMAPPER. */
    uint8_t *m = dmem + DMEMMAPPER_BASE;
    m[8]  = CMD_BUF_OFFSET & 0xff;
    m[9]  = (CMD_BUF_OFFSET >> 8) & 0xff;

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(dmem, sizeof(dmem),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) == 0);
    /* init_cmd should have been written at DMEMMAPPER_BASE+0x2c, NOT
     * at the fake entry's 0x180+0x2c. */
    REQUIRE(rd32(dmem + DMEMMAPPER_BASE + 0x2c) == 0x15);
    REQUIRE(rd32(dmem + 0x180 + 0x2c) == 0);
}

/* ---- State-machine guard tests ----
 *
 * Both gsp_bringup_booter_load and gsp_bringup_riscv_start are
 * driven by `b->state`; calling them out of order — the only fast-
 * to-detect bringup mistake — must fail-closed before any hardware
 * is touched. These tests verify the state guards work without
 * requiring a populated platform vtable.
 */

static void test_booter_load_refuses_pre_fwsec(void)
{
    struct gsp_bringup b;
    memset(&b, 0, sizeof(b));
    b.state = GSP_BRINGUP_INIT;     /* not FWSEC_FRTS_DONE yet */
    /* Without FWSEC having set up WPR2, booter would dereference
     * uninitialized FB. The guard must catch this and return the
     * specific INVAL code (not generic -1). */
    REQUIRE(gsp_bringup_booter_load(&b) == GSP_ERR_INVAL);
    /* State should NOT advance to BOOTER_LOAD_DONE. */
    REQUIRE(b.state != GSP_BRINGUP_BOOTER_LOAD_DONE);
}

static void test_riscv_start_refuses_pre_booter(void)
{
    struct gsp_bringup b;
    memset(&b, 0, sizeof(b));
    b.state = GSP_BRINGUP_FWSEC_FRTS_DONE;     /* WPR2 set, but no booter */
    REQUIRE(gsp_bringup_riscv_start(&b) == GSP_ERR_INVAL);
    REQUIRE(b.state != GSP_BRINGUP_RISCV_RUNNING);
}

static void test_riscv_start_refuses_init_state(void)
{
    struct gsp_bringup b;
    memset(&b, 0, sizeof(b));
    b.state = GSP_BRINGUP_INIT;
    REQUIRE(gsp_bringup_riscv_start(&b) == GSP_ERR_INVAL);
}

static void test_null_args_rejected(void)
{
    REQUIRE(gsp_bringup_booter_load(NULL) == GSP_ERR_INVAL);
    REQUIRE(gsp_bringup_riscv_start(NULL) == GSP_ERR_INVAL);
}

static void test_error_codes_are_distinct_negative(void)
{
    /* The shared error constants must all be negative (callers do
     * `if (rc < 0)`) and pairwise distinct so the harness can map
     * the integer back to a meaningful failure mode. */
    REQUIRE(GSP_OK == 0);
    REQUIRE(GSP_ERR_INVAL   < 0);
    REQUIRE(GSP_ERR_IO      < 0);
    REQUIRE(GSP_ERR_NOMEM   < 0);
    REQUIRE(GSP_ERR_FAULT   < 0);
    REQUIRE(GSP_ERR_NOSPC   < 0);
    REQUIRE(GSP_ERR_NOSYS   < 0);
    REQUIRE(GSP_ERR_TIMEOUT < 0);

    int codes[] = { GSP_ERR_INVAL, GSP_ERR_IO, GSP_ERR_NOMEM, GSP_ERR_FAULT,
                    GSP_ERR_NOSPC, GSP_ERR_NOSYS, GSP_ERR_TIMEOUT };
    int n = (int)(sizeof(codes) / sizeof(codes[0]));
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            REQUIRE(codes[i] != codes[j]);
        }
    }
}

/* ---- gsp_bringup_patch_dmemmapper (generic, init_cmd-parameterised) ---- */

static void test_patch_generic_sb_writes_init_cmd(void)
{
    /* SB (0x19) path — caller must get init_cmd=0x19 at the same
     * offset, and the frts_region sub-struct MUST NOT be written
     * (matches nouveau nvkm_gsp_fwsec_patch which only writes
     * frts_region for CMD_FRTS). */
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);

    /* Pre-fill frts_region area with a known sentinel so we can
     * detect that the patcher didn't touch it. */
    uint8_t *frts = dmem + CMD_BUF_OFFSET + 24;
    for (int i = 0; i < 20; i++) frts[i] = 0xA5;

    REQUIRE(gsp_bringup_patch_dmemmapper(dmem, sizeof(dmem),
                                         INTERFACE_OFF,
                                         /*init_cmd*/ 0x19u,
                                         WPR2_ADDR, WPR2_SIZE) == 0);

    /* init_cmd at DMEMMAPPER_BASE + 0x2c == 0x19 */
    uint32_t init_cmd = rd32(dmem + DMEMMAPPER_BASE + 0x2c);
    REQUIRE(init_cmd == 0x19u);

    /* read_vbios sub-struct still populated (SB needs it). */
    uint8_t *rv = dmem + CMD_BUF_OFFSET;
    REQUIRE(rd32(rv +  0) == 1);
    REQUIRE(rd32(rv +  4) == 24);
    REQUIRE(rd32(rv + 20) == 2);

    /* frts_region sentinel untouched: no FRTS write happened. */
    for (int i = 0; i < 20; i++) {
        REQUIRE(frts[i] == 0xA5);
    }
}

static void test_patch_generic_frts_matches_legacy_wrapper(void)
{
    /* When init_cmd == FRTS (0x15), the generic API must produce
     * identical bytes to the legacy _frts wrapper. Cross-check by
     * running both on zeroed copies of the same DMEM template and
     * memcmp'ing the command-buffer region. */
    uint8_t a[DMEM_SIZE]; build_synthetic_dmem(a);
    uint8_t b[DMEM_SIZE]; build_synthetic_dmem(b);

    REQUIRE(gsp_bringup_patch_dmemmapper_frts(a, sizeof(a),
                                              INTERFACE_OFF,
                                              WPR2_ADDR, WPR2_SIZE) == 0);
    REQUIRE(gsp_bringup_patch_dmemmapper(b, sizeof(b),
                                         INTERFACE_OFF,
                                         /*init_cmd*/ 0x15u,
                                         WPR2_ADDR, WPR2_SIZE) == 0);

    /* The mutations live around DMEMMAPPER_BASE and CMD_BUF_OFFSET;
     * compare bytes 0 .. CMD_BUF_OFFSET + 44 which covers both. */
    REQUIRE(memcmp(a, b, CMD_BUF_OFFSET + 44) == 0);
}

static void test_patch_generic_unknown_cmd_skips_frts_region(void)
{
    /* An unknown init_cmd (any value != FRTS) behaves like SB:
     * writes init_cmd + read_vbios only. This is the behavior
     * the harness relies on for the Probe B path. */
    uint8_t dmem[DMEM_SIZE];
    build_synthetic_dmem(dmem);

    uint8_t *frts = dmem + CMD_BUF_OFFSET + 24;
    for (int i = 0; i < 20; i++) frts[i] = 0xDE;

    REQUIRE(gsp_bringup_patch_dmemmapper(dmem, sizeof(dmem),
                                         INTERFACE_OFF,
                                         /*init_cmd*/ 0xDEADu,
                                         WPR2_ADDR, WPR2_SIZE) == 0);

    uint32_t init_cmd = rd32(dmem + DMEMMAPPER_BASE + 0x2c);
    REQUIRE(init_cmd == 0xDEADu);
    for (int i = 0; i < 20; i++) REQUIRE(frts[i] == 0xDE);
}

/* ---- gsp_bringup_free — null-safety / idempotence ---- */

static void test_bringup_free_null_safe(void)
{
    /* Must not crash on a NULL pointer. */
    gsp_bringup_free(NULL);
}

static void test_bringup_free_idempotent_on_fresh_struct(void)
{
    /* A zero-initialized bringup struct has no DMA buffers; free must
     * be safe and leave the fields clear. Calling it twice must also
     * be safe. */
    struct gsp_bringup b;
    memset(&b, 0, sizeof(b));
    gsp_bringup_free(&b);
    REQUIRE(b.dma_imem_va == NULL);
    REQUIRE(b.dma_dmem_va == NULL);
    gsp_bringup_free(&b);    /* second call — still safe */
    REQUIRE(b.dma_imem_va == NULL);
    REQUIRE(b.dma_dmem_va == NULL);
}

/* ============================================================================
 * gsp_bringup_set_booter_layout — pins the v2 booter-blob → bringup
 * field-copy logic. Existed inline in gsp_bringup_booter_load until
 * PR #289 extracted it after a hardware experiment surfaced a wrong
 * assignment for `booter_boot_addr` (was os_code_offset → 0 → Falcon
 * jumped to non-secure preamble; now apps[0].offset → secure entry).
 * ============================================================================ */

/* Dummy bytes buffer for the test fixture — if a future change to
 * `gsp_bringup_set_booter_layout` starts reading `img->bytes`, we
 * want it to read from a known allocation instead of NULL-deref'ing.
 * The helper under test today doesn't touch this, so the buffer's
 * contents are irrelevant. */
static uint8_t test_image_dummy_bytes[0x10000];

/* Construct a minimal nvfw_image with v2-layout values that exercise
 * every field the helper copies. The exact numbers here are chosen so
 * each assertion in the tests below distinguishes the field —
 * apps[0].offset != os_code_offset so a regression that revives the
 * old `booter_boot_addr = os_code_offset` line fires immediately. */
static void make_v2_booter_image(struct nvfw_image *img)
{
    memset(img, 0, sizeof(*img));
    /* Point bytes/size at a real allocation so a future helper
     * change that dereferences them fails in a controlled way, not
     * with a segfault that confuses the test framework. */
    img->bytes          = test_image_dummy_bytes;
    img->size           = sizeof(test_image_dummy_bytes);
    img->os_code_offset = 0x100;       /* non-secure preamble */
    img->os_code_size   = 0x100;
    img->os_data_offset = 0x8400;      /* DMEM section */
    img->os_data_size   = 0x6200;
    img->num_apps       = 1;
    img->apps[0].offset = 0x200;       /* secure entry — distinct from os_code_offset */
    img->apps[0].size   = 0x8200;
    img->patch_loc      = 0x8410;      /* signature 16 bytes into DMEM */
    img->engine_id      = 0x1;         /* SEC2 */
    img->ucode_id       = 0x3;
    img->fuse_ver       = 0xF;
    img->has_meta       = true;
}

/* Regression: BOOTVEC = apps[0].offset (the entry point HS-bootrom
 * jumps to after signature verify), NOT os_code_offset (which is the
 * non-secure preamble's IMEM location). Phase 2 used to STOP at first
 * instruction on R535 booter_load because os_code_offset == 0 sent
 * the Falcon to IMEM[0]. */
static void test_booter_layout_bootvec_uses_apps0_offset(void)
{
    struct nvfw_image img;
    struct gsp_bringup b = { 0 };
    make_v2_booter_image(&img);

    gsp_bringup_set_booter_layout(&b, &img);

    REQUIRE(b.booter_boot_addr == 0x200);   /* apps[0].offset */
    REQUIRE(b.booter_boot_addr != img.os_code_offset);
    REQUIRE(b.booter_boot_addr == img.apps[0].offset);
}

/* Companion: every other booter_* field also copied correctly. */
static void test_booter_layout_all_fields_set(void)
{
    struct nvfw_image img;
    struct gsp_bringup b = { 0 };
    make_v2_booter_image(&img);

    gsp_bringup_set_booter_layout(&b, &img);

    REQUIRE(b.booter_imem_ns_off   == 0x100);
    REQUIRE(b.booter_imem_ns_size  == 0x100);
    REQUIRE(b.booter_imem_sec_off  == 0x200);
    REQUIRE(b.booter_imem_sec_size == 0x8200);
    REQUIRE(b.booter_dmem_offset   == 0x8400);
    REQUIRE(b.booter_dmem_size     == 0x6200);
    /* dmem_sign = patch_loc - os_data_offset = 0x10 (signature
     * lands at byte 16 of DMEM, after a small DMEM header). */
    REQUIRE(b.booter_dmem_sign     == 0x10);
    REQUIRE(b.booter_engine_id     == 0x1);
    REQUIRE(b.booter_ucode_id      == 0x3);
}

/* Layout where apps[0].offset == os_code_offset: BOOTVEC must still
 * pick apps[0].offset (the field-of-record), not os_code_offset.
 * Tests the precondition the old buggy code relied on (contiguous
 * non-secure-then-secure layout) is no longer special-cased. */
static void test_booter_layout_bootvec_consistent_when_offsets_match(void)
{
    struct nvfw_image img;
    struct gsp_bringup b = { 0 };
    make_v2_booter_image(&img);
    img.os_code_offset = 0x200;        /* matches apps[0].offset now */

    gsp_bringup_set_booter_layout(&b, &img);

    REQUIRE(b.booter_boot_addr == 0x200);
    REQUIRE(b.booter_boot_addr == img.apps[0].offset);
}

/* NULL args must be no-ops (no crash). The helper is extern-visible
 * for tests, so a future caller that forgets to validate shouldn't
 * segfault. */
static void test_booter_layout_null_args_no_crash(void)
{
    struct nvfw_image img;
    struct gsp_bringup b = { 0 };
    make_v2_booter_image(&img);

    /* NULL b: the assignments would crash on dereference — verify
     * the early-return works. */
    gsp_bringup_set_booter_layout(NULL, &img);
    /* NULL img: same check from the other side. */
    gsp_bringup_set_booter_layout(&b, NULL);
    /* Both NULL. */
    gsp_bringup_set_booter_layout(NULL, NULL);

    /* If we're still running, the helper returned cleanly. Also
     * verify the b struct is untouched in the NULL-img case. */
    REQUIRE(b.booter_boot_addr == 0);
    REQUIRE(b.booter_imem_sec_off == 0);
}

/* Zero-size non-secure section (the actual R535 booter_load layout
 * if the producer chose to omit the preamble): helper still copies
 * the 0 size correctly and BOOTVEC still uses apps[0].offset. */
static void test_booter_layout_zero_ns_size(void)
{
    struct nvfw_image img;
    struct gsp_bringup b = { 0 };
    make_v2_booter_image(&img);
    img.os_code_size = 0;
    img.apps[0].offset = 0;            /* secure starts at byte 0 */

    gsp_bringup_set_booter_layout(&b, &img);

    REQUIRE(b.booter_imem_ns_size == 0);
    REQUIRE(b.booter_boot_addr == 0);
    REQUIRE(b.booter_imem_sec_off == 0);
}

/* ============================================================================
 * Stage A: GspFwWprMeta + radix3 chain helpers.
 *
 * These pin the field-write logic the SEC2 HS booter consumes via DMA
 * before it'll start executing the GSP-RM bootloader. A regression in
 * field assignment OR struct layout would make the booter either
 * NULL-deref the radix3 walk (current baseline behavior — infinite
 * hang) or quietly accept wrong values and corrupt GSP-RM state.
 * ============================================================================ */

/* The header's `_Static_assert`s already pin sizeof + 11 field
 * offsets at compile time; this runtime test makes the same
 * guarantees visible in the test report so a successful test run
 * also documents that the layout matched. */
static void test_wpr_meta_struct_size_runtime(void)
{
    REQUIRE(sizeof(GspFwWprMeta) == 256);
    REQUIRE(offsetof(GspFwWprMeta, magic) == 0x00);
    REQUIRE(offsetof(GspFwWprMeta, revision) == 0x08);
    REQUIRE(offsetof(GspFwWprMeta, sysmemAddrOfRadix3Elf) == 0x10);
    REQUIRE(offsetof(GspFwWprMeta, gspFwWprStart) == 0x70);
    REQUIRE(offsetof(GspFwWprMeta, gspFwWprEnd) == 0xa8);
    REQUIRE(offsetof(GspFwWprMeta, fbSize) == 0xb0);
    REQUIRE(offsetof(GspFwWprMeta, verified) == 0xf8);
}

/* The MAGIC + REVISION constants are what booter validates first.
 * Pin the literal values against the upstream nouveau reference;
 * a typo here would silently make every populate_minimum() call
 * fail booter validation. */
static void test_wpr_meta_constants_match_upstream(void)
{
    /* Reference: ../slmos-reference-cache/nouveau/nouveau-r535-nvrm-gsp.h:557-559. */
    REQUIRE(GSP_FW_WPR_META_MAGIC == 0xdc3aae21371a60b3ULL);
    REQUIRE(GSP_FW_WPR_META_REVISION == 1ULL);
    REQUIRE(GSP_FW_WPR_META_VERIFIED == 0xa0a0a0a0a0a0a0a0ULL);
}

/* Picked numbers that exercise every field independently — a
 * mixed-up assignment in `populate_minimum` (e.g. swapping
 * wpr2_addr and wpr2_size) shows up because no two fields share
 * a value. */
#define TEST_RADIX3_L0_IOVA   0x1234500000ULL
#define TEST_RADIX3_ELF_SIZE  0x4567ULL
#define TEST_WPR2_ADDR        0x17FE00000ULL    /* matches GA107 6 GB */
#define TEST_WPR2_SIZE        0x100000ULL       /* 1 MB */
#define TEST_FB_SIZE          0x180000000ULL    /* 6 GB */

static void test_wpr_meta_populate_sets_required_fields(void)
{
    GspFwWprMeta meta;
    /* Pre-fill with 0xee so the populate's memset(0) is exercised
     * — a bug that skipped the memset would leave 0xee in the
     * "should be zero" fields and the next test catches it. */
    memset(&meta, 0xee, sizeof(meta));

    gsp_wpr_meta_populate_minimum(&meta,
                                  TEST_RADIX3_L0_IOVA,
                                  TEST_RADIX3_ELF_SIZE,
                                  TEST_WPR2_ADDR,
                                  TEST_WPR2_SIZE,
                                  TEST_FB_SIZE);

    REQUIRE(meta.magic                 == GSP_FW_WPR_META_MAGIC);
    REQUIRE(meta.revision              == GSP_FW_WPR_META_REVISION);
    REQUIRE(meta.sysmemAddrOfRadix3Elf == TEST_RADIX3_L0_IOVA);
    REQUIRE(meta.sizeOfRadix3Elf       == TEST_RADIX3_ELF_SIZE);
    REQUIRE(meta.gspFwWprStart         == TEST_WPR2_ADDR);
    REQUIRE(meta.gspFwWprEnd           == TEST_WPR2_ADDR + TEST_WPR2_SIZE);
    REQUIRE(meta.fbSize                == TEST_FB_SIZE);
}

/* Deliberately NOT populated by Stage A: bootloader, signature,
 * heap, partition-RPC. They MUST stay zero — booter rejects the
 * missing bootloader with a specific status code, and that's the
 * Stage A diagnostic signal. If a future change starts populating
 * one of these fields, this test should be updated to assert the
 * new contract, not deleted. */
static void test_wpr_meta_populate_leaves_other_fields_zero(void)
{
    GspFwWprMeta meta;
    memset(&meta, 0xee, sizeof(meta));    /* same pre-fill trick */

    gsp_wpr_meta_populate_minimum(&meta,
                                  TEST_RADIX3_L0_IOVA,
                                  TEST_RADIX3_ELF_SIZE,
                                  TEST_WPR2_ADDR,
                                  TEST_WPR2_SIZE,
                                  TEST_FB_SIZE);

    REQUIRE(meta.sysmemAddrOfBootloader   == 0);
    REQUIRE(meta.sizeOfBootloader         == 0);
    REQUIRE(meta.bootloaderCodeOffset     == 0);
    REQUIRE(meta.bootloaderDataOffset     == 0);
    REQUIRE(meta.bootloaderManifestOffset == 0);
    REQUIRE(meta.sysmemAddrOfSignature    == 0);
    REQUIRE(meta.sizeOfSignature          == 0);
    REQUIRE(meta.gspFwRsvdStart           == 0);
    REQUIRE(meta.nonWprHeapOffset         == 0);
    REQUIRE(meta.nonWprHeapSize           == 0);
    REQUIRE(meta.gspFwHeapOffset          == 0);
    REQUIRE(meta.gspFwHeapSize            == 0);
    REQUIRE(meta.gspFwOffset              == 0);
    REQUIRE(meta.bootBinOffset            == 0);
    REQUIRE(meta.frtsOffset               == 0);
    REQUIRE(meta.frtsSize                 == 0);
    REQUIRE(meta.vgaWorkspaceOffset       == 0);
    REQUIRE(meta.vgaWorkspaceSize         == 0);
    REQUIRE(meta.bootCount                == 0);
    /* Second-union (partitionRpc + crashReport) and the trailing
     * scalars. These are the fields a future Stage B/C might start
     * populating; pinning them zero today means the test will fail
     * on first such change and the author will deliberately update
     * the contract here. */
    REQUIRE(meta.partitionRpcAddr         == 0);
    REQUIRE(meta.partitionRpcRequestOffset == 0);
    REQUIRE(meta.partitionRpcReplyOffset  == 0);
    REQUIRE(meta.elfCodeOffset            == 0);
    REQUIRE(meta.elfDataOffset            == 0);
    REQUIRE(meta.elfCodeSize              == 0);
    REQUIRE(meta.elfDataSize              == 0);
    REQUIRE(meta.lsUcodeVersion           == 0);
    REQUIRE(meta.gspFwHeapVfPartitionCount == 0);
    REQUIRE(meta.verified                 == 0);
}

/* gspFwWprEnd is computed (wpr2_addr + wpr2_size). Verify the
 * arithmetic with a pair where no overflow risk exists and the
 * sum has a distinct high-byte from either operand. */
static void test_wpr_meta_populate_computes_wpr_end(void)
{
    GspFwWprMeta meta = { 0 };

    /* Distinctive values: addr=0x1000_0000 + size=0x0080_0000 =
     * 0x1080_0000. None of the three values share a byte. */
    gsp_wpr_meta_populate_minimum(&meta,
                                  /*l0=*/   0x4000ULL,
                                  /*elfsz=*/ 0x4000ULL,
                                  /*wpr_a=*/ 0x10000000ULL,
                                  /*wpr_s=*/ 0x00800000ULL,
                                  /*fbsz=*/  0x40000000ULL);

    REQUIRE(meta.gspFwWprStart == 0x10000000ULL);
    REQUIRE(meta.gspFwWprEnd   == 0x10800000ULL);
}

/* NULL @meta must be a clean no-op (not a segfault). The helper
 * is extern-visible for tests, and the in-tree caller does
 * pre-validate, but defense-in-depth matters because NULL gets
 * fed in if the WprMeta DMA alloc itself fails and the caller
 * forgets to bail.
 *
 * Allocate a sentinel-filled meta on the stack alongside the NULL
 * call so a positive REQUIRE confirms the helper actually returned
 * (vs. exited via undefined behavior the harness happens to swallow). */
static void test_wpr_meta_populate_null_safe(void)
{
    GspFwWprMeta sentinel;
    memset(&sentinel, 0xee, sizeof(sentinel));

    gsp_wpr_meta_populate_minimum(NULL, 1, 1, 1, 1, 1);

    /* If we're still running, the helper returned. Sentinel meta
     * must not have been touched (it was never passed in). */
    REQUIRE(sentinel.magic == 0xeeeeeeeeeeeeeeeeULL);
    REQUIRE(sentinel.verified == 0xeeeeeeeeeeeeeeeeULL);
}

/* The radix3 chain helper writes one entry per page. Verify L0[0]
 * lands at byte 0 of the L0 page and the rest stays untouched.
 * Each test pre-fills the page with a distinctive byte pattern;
 * the assertion at offset 8 onward catches a regression that
 * accidentally writes more than the first u64. */
static void test_radix3_fill_writes_l0_entry_only(void)
{
    uint64_t l0[512], l1[512], l2[512];
    /* 512 u64 = 4096 bytes = one DMA page, matching the runtime. */
    memset(l0, 0xa5, sizeof(l0));
    memset(l1, 0xa5, sizeof(l1));
    memset(l2, 0xa5, sizeof(l2));

    gsp_radix3_fill_dummy_chain(l0, /*l1_iova=*/ 0x1000,
                                l1, /*l2_iova=*/ 0x2000,
                                l2, /*elf_iova=*/ 0x3000);

    REQUIRE(l0[0] == 0x1000);
    /* Remaining entries: helper must not touch — still 0xa5a5...
     * (the per-byte fill makes a u64 entry equal 0xa5a5a5a5a5a5a5a5). */
    REQUIRE(l0[1] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l0[511] == 0xa5a5a5a5a5a5a5a5ULL);
}

static void test_radix3_fill_writes_l1_entry_only(void)
{
    uint64_t l0[512], l1[512], l2[512];
    memset(l0, 0xa5, sizeof(l0));
    memset(l1, 0xa5, sizeof(l1));
    memset(l2, 0xa5, sizeof(l2));

    gsp_radix3_fill_dummy_chain(l0, 0x1000, l1, 0x2000, l2, 0x3000);

    REQUIRE(l1[0] == 0x2000);
    REQUIRE(l1[1] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l1[511] == 0xa5a5a5a5a5a5a5a5ULL);
}

static void test_radix3_fill_writes_l2_entry_only(void)
{
    uint64_t l0[512], l1[512], l2[512];
    memset(l0, 0xa5, sizeof(l0));
    memset(l1, 0xa5, sizeof(l1));
    memset(l2, 0xa5, sizeof(l2));

    gsp_radix3_fill_dummy_chain(l0, 0x1000, l1, 0x2000, l2, 0x3000);

    REQUIRE(l2[0] == 0x3000);
    REQUIRE(l2[1] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l2[511] == 0xa5a5a5a5a5a5a5a5ULL);
}

/* NULL in any of the three page args = no-op for the entire chain.
 * Verify by checking every page stays untouched. The helper
 * deliberately treats partial-NULL as "caller is in some error
 * state, don't write a half-chain". */
static void test_radix3_fill_null_pages_no_op(void)
{
    uint64_t l0[512], l1[512], l2[512];
    memset(l0, 0xa5, sizeof(l0));
    memset(l1, 0xa5, sizeof(l1));
    memset(l2, 0xa5, sizeof(l2));

    /* NULL l0. */
    gsp_radix3_fill_dummy_chain(NULL, 0x1000, l1, 0x2000, l2, 0x3000);
    REQUIRE(l1[0] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l2[0] == 0xa5a5a5a5a5a5a5a5ULL);

    /* NULL l1. */
    gsp_radix3_fill_dummy_chain(l0, 0x1000, NULL, 0x2000, l2, 0x3000);
    REQUIRE(l0[0] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l2[0] == 0xa5a5a5a5a5a5a5a5ULL);

    /* NULL l2. */
    gsp_radix3_fill_dummy_chain(l0, 0x1000, l1, 0x2000, NULL, 0x3000);
    REQUIRE(l0[0] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l1[0] == 0xa5a5a5a5a5a5a5a5ULL);

    /* All NULL. */
    gsp_radix3_fill_dummy_chain(NULL, 0, NULL, 0, NULL, 0);
}

/* Zero IOVAs are a legal-but-noteworthy input: the chain still
 * gets written, and booter would later NULL-deref on the walk.
 * The helper itself doesn't reject this — that's the caller's job
 * (the in-tree caller's `gsp_dma_alloc_checked` returns NULL on
 * alloc failure, which the caller catches before reaching us).
 * Pin the contract so a future hardening change in the helper
 * forces a deliberate test update.
 *
 * Pages are pre-filled with 0xa5 so an asserted `[0] == 0` proves
 * the helper actually wrote zero — not just that the page was
 * already zero. */
static void test_radix3_fill_accepts_zero_iovas(void)
{
    uint64_t l0[512], l1[512], l2[512];
    memset(l0, 0xa5, sizeof(l0));
    memset(l1, 0xa5, sizeof(l1));
    memset(l2, 0xa5, sizeof(l2));

    gsp_radix3_fill_dummy_chain(l0, 0, l1, 0, l2, 0);

    /* Helper wrote 0 (the IOVA argument), overwriting the 0xa5
     * pre-fill. Subsequent entries must remain 0xa5 — the helper
     * still touches only entry [0] regardless of value written. */
    REQUIRE(l0[0] == 0);
    REQUIRE(l1[0] == 0);
    REQUIRE(l2[0] == 0);
    REQUIRE(l0[1] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l1[1] == 0xa5a5a5a5a5a5a5a5ULL);
    REQUIRE(l2[1] == 0xa5a5a5a5a5a5a5a5ULL);
}

int main(void)
{
    /* Sig-index algorithm */
    test_sig_index_rtx3050_real_values();
    test_sig_index_first_signature();
    test_sig_index_third_signature();
    test_sig_index_no_matching_sig();
    test_sig_index_no_fuse_burned();
    test_sig_index_zero_count();
    test_sig_index_clamps_overflow();

    /* DMEMMAPPER patcher (legacy FRTS wrapper) */
    test_patch_writes_init_cmd_at_2c();
    test_patch_writes_read_vbios_struct();
    test_patch_writes_frts_region();
    test_patch_rejects_no_dmemmapper();
    test_patch_rejects_bad_interface_version();
    test_patch_rejects_interface_off_past_end();
    test_patch_rejects_cmd_buf_overflow();
    test_patch_skips_non_dmemmapper_entries();

    /* DMEMMAPPER patcher (generic, init_cmd-parameterised) */
    test_patch_generic_sb_writes_init_cmd();
    test_patch_generic_frts_matches_legacy_wrapper();
    test_patch_generic_unknown_cmd_skips_frts_region();

    /* gsp_bringup_free helper */
    test_bringup_free_null_safe();
    test_bringup_free_idempotent_on_fresh_struct();

    /* gsp_bringup_set_booter_layout — pins the BOOTVEC=apps[0].offset
     * rule and the full v2 booter-blob field-copy. */
    test_booter_layout_bootvec_uses_apps0_offset();
    test_booter_layout_all_fields_set();
    test_booter_layout_bootvec_consistent_when_offsets_match();
    test_booter_layout_null_args_no_crash();
    test_booter_layout_zero_ns_size();

    /* Stage A: GspFwWprMeta + radix3 chain helpers. */
    test_wpr_meta_struct_size_runtime();
    test_wpr_meta_constants_match_upstream();
    test_wpr_meta_populate_sets_required_fields();
    test_wpr_meta_populate_leaves_other_fields_zero();
    test_wpr_meta_populate_computes_wpr_end();
    test_wpr_meta_populate_null_safe();
    test_radix3_fill_writes_l0_entry_only();
    test_radix3_fill_writes_l1_entry_only();
    test_radix3_fill_writes_l2_entry_only();
    test_radix3_fill_null_pages_no_op();
    test_radix3_fill_accepts_zero_iovas();

    /* State-machine guards (E3.4.d / E3.4.e) */
    test_booter_load_refuses_pre_fwsec();
    test_riscv_start_refuses_pre_booter();
    test_riscv_start_refuses_init_state();
    test_null_args_rejected();
    test_error_codes_are_distinct_negative();

    if (failures == 0) {
        printf("test_bringup: all tests PASS\n");
        return 0;
    }
    printf("test_bringup: %d FAIL\n", failures);
    return 1;
}
