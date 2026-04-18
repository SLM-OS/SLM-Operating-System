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

/* Construct a minimal nvfw_image with v2-layout values that exercise
 * every field the helper copies. The exact numbers here are chosen so
 * each assertion in the tests below distinguishes the field —
 * apps[0].offset != os_code_offset so a regression that revives the
 * old `booter_boot_addr = os_code_offset` line fires immediately.
 *
 * `bytes`/`size` deliberately stay NULL/0: `gsp_bringup_set_booter_layout`
 * does not dereference the image buffer, only the parsed offset/size
 * scalars. If a future change starts reading `img->bytes`, it will
 * fault here in a controlled way and the test author will replace
 * this with a real backing allocation. */
static void make_v2_booter_image(struct nvfw_image *img)
{
    memset(img, 0, sizeof(*img));
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

    /* If we're still running, the helper returned cleanly. Verify
     * EVERY field the helper would have written is still its
     * zero-initialized value — partial copies (helper crashed
     * mid-assignment) would show up here. */
    REQUIRE(b.booter_imem_ns_off   == 0);
    REQUIRE(b.booter_imem_ns_size  == 0);
    REQUIRE(b.booter_imem_sec_off  == 0);
    REQUIRE(b.booter_imem_sec_size == 0);
    REQUIRE(b.booter_dmem_offset   == 0);
    REQUIRE(b.booter_dmem_size     == 0);
    REQUIRE(b.booter_dmem_sign     == 0);
    REQUIRE(b.booter_engine_id     == 0);
    REQUIRE(b.booter_ucode_id      == 0);
    REQUIRE(b.booter_boot_addr     == 0);
}

/* num_apps == 0: helper must refuse — apps[0] would otherwise be
 * the zero-initialized array entry and silently set BOOTVEC=0,
 * recreating the Phase 2 STOPPED symptom this PR fixed.
 *
 * If a future change relaxes the guard (e.g. defaults BOOTVEC to
 * os_code_offset for HS-only blobs without an apps entry), this
 * test must be updated to assert the new contract — don't just
 * delete it. */
static void test_booter_layout_rejects_zero_num_apps(void)
{
    struct nvfw_image img;
    struct gsp_bringup b = { 0 };
    make_v2_booter_image(&img);
    img.num_apps = 0;

    gsp_bringup_set_booter_layout(&b, &img);

    /* Same untouched-struct check as the NULL test — every field
     * stays at its zero init. */
    REQUIRE(b.booter_imem_ns_off   == 0);
    REQUIRE(b.booter_imem_ns_size  == 0);
    REQUIRE(b.booter_imem_sec_off  == 0);
    REQUIRE(b.booter_imem_sec_size == 0);
    REQUIRE(b.booter_dmem_offset   == 0);
    REQUIRE(b.booter_dmem_size     == 0);
    REQUIRE(b.booter_dmem_sign     == 0);
    REQUIRE(b.booter_engine_id     == 0);
    REQUIRE(b.booter_ucode_id      == 0);
    REQUIRE(b.booter_boot_addr     == 0);
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
    test_booter_layout_rejects_zero_num_apps();

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
