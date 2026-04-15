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

    /* DMEMMAPPER patcher */
    test_patch_writes_init_cmd_at_2c();
    test_patch_writes_read_vbios_struct();
    test_patch_writes_frts_region();
    test_patch_rejects_no_dmemmapper();
    test_patch_rejects_bad_interface_version();
    test_patch_rejects_interface_off_past_end();
    test_patch_rejects_cmd_buf_overflow();
    test_patch_skips_non_dmemmapper_entries();

    if (failures == 0) {
        printf("test_bringup: all tests PASS\n");
        return 0;
    }
    printf("test_bringup: %d FAIL\n", failures);
    return 1;
}
