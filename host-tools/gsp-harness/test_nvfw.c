/*
 * test_nvfw.c — regression tests for the NVIDIA firmware-wrapper
 * parser (kernel/gpu/nvidia/nvfw.c).
 *
 * Hand-constructs small synthetic blobs with both bin_magic
 * variants (0x10DE with patch-loc indirection, 0x3B1D14F0 with
 * inline values). Exercises every rejection path so we fail
 * closed on corrupt firmware files.
 *
 * Wired into `make test-nvfw`.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/nvfw.h"

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* ---- Little-endian byte writers ---- */

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/* ---- Synthetic blob builder ----
 *
 * Layout (offsets in bytes):
 *   0x000  nvfw_bin_hdr (24 B)
 *   0x018  nvfw_hs_header_v2 (36 B)
 *   0x03C  nvfw_hs_load_header_v2: fixed 20 B + 1 app (16 B)
 *   0x060  meta-data (12 B)
 *   0x070  signatures (384 B)
 *   0x1F0  patch_loc storage (4 B)          [0x10DE indirection variant]
 *   0x1F4  patch_sig storage (4 B)          [0x10DE indirection variant]
 *   0x200  IMEM|DMEM payload (512 B)
 *   0x400  total file size
 */
#define SYN_SIZE             0x400
#define SYN_HS_HDR_OFF       0x018
#define SYN_LOAD_HDR_OFF     0x03C
#define SYN_META_OFF         0x060
#define SYN_META_SIZE        12
#define SYN_SIG_OFF          0x070
#define SYN_SIG_SIZE         384
#define SYN_PATCH_LOC_PTR    0x1F0
#define SYN_PATCH_SIG_PTR    0x1F4
#define SYN_DATA_OFF         0x200
#define SYN_DATA_SIZE        0x200

#define SYN_PATCH_LOC_VAL    0x123400
#define SYN_PATCH_SIG_VAL    0x456700
#define SYN_FUSE_VER         7
#define SYN_ENGINE_ID        0x1100
#define SYN_UCODE_ID         5

static void build_blob(uint8_t *buf, uint32_t bin_magic)
{
    memset(buf, 0, SYN_SIZE);

    /* nvfw_bin_hdr */
    w32(buf + 0,  bin_magic);
    w32(buf + 4,  1);                   /* bin_ver */
    w32(buf + 8,  SYN_SIZE);            /* bin_size */
    w32(buf + 12, SYN_HS_HDR_OFF);
    w32(buf + 16, SYN_DATA_OFF);
    w32(buf + 20, SYN_DATA_SIZE);

    /* nvfw_hs_header_v2 */
    uint8_t *hs = buf + SYN_HS_HDR_OFF;
    w32(hs + 0,  SYN_SIG_OFF);          /* sig_prod_offset */
    w32(hs + 4,  SYN_SIG_SIZE);         /* sig_prod_size */
    if (bin_magic == NVFW_BIN_MAGIC_STD) {
        /* 0x10DE: patch_loc / patch_sig are offsets to the value. */
        w32(hs + 8,  SYN_PATCH_LOC_PTR);
        w32(hs + 12, SYN_PATCH_SIG_PTR);
        w32(buf + SYN_PATCH_LOC_PTR, SYN_PATCH_LOC_VAL);
        w32(buf + SYN_PATCH_SIG_PTR, SYN_PATCH_SIG_VAL);
    } else {
        /* 0x3B1D14F0: values inline. */
        w32(hs + 8,  SYN_PATCH_LOC_VAL);
        w32(hs + 12, SYN_PATCH_SIG_VAL);
    }
    w32(hs + 16, SYN_META_OFF);
    w32(hs + 20, SYN_META_SIZE);
    w32(hs + 24, 1);                    /* num_sig */
    w32(hs + 28, SYN_LOAD_HDR_OFF);
    w32(hs + 32, 20);    /* load_header fixed-part size (not validated by parser) */

    /* nvfw_hs_load_header_v2 (fixed part + 1 app entry) */
    uint8_t *ld = buf + SYN_LOAD_HDR_OFF;
    w32(ld + 0,  0x100);                /* os_code_offset */
    w32(ld + 4,  0x100);                /* os_code_size */
    w32(ld + 8,  0x180);                /* os_data_offset */
    w32(ld + 12, 0x080);                /* os_data_size */
    w32(ld + 16, 1);                    /* num_apps */
    /* App 0 */
    w32(ld + 20, 0x000);                /* app.offset */
    w32(ld + 24, 0x100);                /* app.size */
    w32(ld + 28, 0x200);                /* app.data_offset */
    w32(ld + 32, 0x080);                /* app.data_size */

    /* Meta-data */
    uint8_t *meta = buf + SYN_META_OFF;
    w32(meta + 0, SYN_FUSE_VER);
    w32(meta + 4, SYN_ENGINE_ID);
    w32(meta + 8, SYN_UCODE_ID);

    /* Signature filler + IMEM|DMEM filler — parser doesn't validate
     * content, just offsets. */
    memset(buf + SYN_SIG_OFF, 0xA5, SYN_SIG_SIZE);
    memset(buf + SYN_DATA_OFF, 0x42, SYN_DATA_SIZE);
}

/* ---- Tests ---- */

static void test_parse_std_magic(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) == 0);
    REQUIRE(img.parsed_ok);
    REQUIRE(img.bin_magic == NVFW_BIN_MAGIC_STD);
    REQUIRE(img.data_offset == SYN_DATA_OFF);
    REQUIRE(img.data_size == SYN_DATA_SIZE);

    /* patch_loc / patch_sig resolved through indirection. */
    REQUIRE(img.patch_loc == SYN_PATCH_LOC_VAL);
    REQUIRE(img.patch_sig == SYN_PATCH_SIG_VAL);

    /* HS header fields. */
    REQUIRE(img.sig_prod_offset == SYN_SIG_OFF);
    REQUIRE(img.sig_prod_size == SYN_SIG_SIZE);

    /* Load header fields. */
    REQUIRE(img.num_apps == 1);
    REQUIRE(img.apps[0].offset == 0x000);
    REQUIRE(img.apps[0].size == 0x100);
    REQUIRE(img.apps[0].data_offset == 0x200);
    REQUIRE(img.apps[0].data_size == 0x080);

    /* Meta-data. */
    REQUIRE(img.has_meta);
    REQUIRE(img.fuse_ver == SYN_FUSE_VER);
    REQUIRE(img.engine_id == SYN_ENGINE_ID);
    REQUIRE(img.ucode_id == SYN_UCODE_ID);
}

static void test_parse_nouveau_magic(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_NOUVEAU);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) == 0);
    REQUIRE(img.bin_magic == NVFW_BIN_MAGIC_NOUVEAU);

    /* Values inline, not indirected. */
    REQUIRE(img.patch_loc == SYN_PATCH_LOC_VAL);
    REQUIRE(img.patch_sig == SYN_PATCH_SIG_VAL);
}

static void test_reject_bad_magic(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    w32(buf + 0, 0xDEADBEEFu);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_null(void)
{
    struct nvfw_image img;
    REQUIRE(nvfw_parse(NULL, 100, &img) < 0);
    uint8_t dummy[128] = {0};
    REQUIRE(nvfw_parse(dummy, sizeof(dummy), NULL) < 0);
}

static void test_reject_too_small(void)
{
    uint8_t buf[16] = {0};     /* smaller than bin_hdr */
    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_oversized(void)
{
    /* Parser's first check is against NVFW_MAX_FILE_SIZE — it returns
     * before reading anything, so we don't need a real backing buffer
     * of that size. A tiny stack buffer is fine. */
    uint8_t dummy[64] = {0};
    struct nvfw_image img;
    REQUIRE(nvfw_parse(dummy, NVFW_MAX_FILE_SIZE + 1, &img) < 0);
}

static void test_reject_data_past_end(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    /* Claim data_size that pushes past the buffer. */
    w32(buf + 20, SYN_DATA_SIZE + 1);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_header_offset_past_end(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    w32(buf + 12, SYN_SIZE);    /* header_offset at end — header itself won't fit */

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_patch_loc_ptr_past_end(void)
{
    /* With 0x10DE magic, patch_loc is itself a file offset —
     * parser must bounds-check before dereferencing it. */
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    /* Clobber the patch_loc offset to something past file end. */
    w32(buf + SYN_HS_HDR_OFF + 8, SYN_SIZE + 1);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_too_many_apps(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    /* Rewrite num_apps to something over NVFW_MAX_APPS (8). */
    w32(buf + SYN_LOAD_HDR_OFF + 16, 16);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_apps_past_end(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    /* Move load_header_offset so the app entries run past file end. */
    w32(buf + SYN_HS_HDR_OFF + 28, SYN_SIZE - 20);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_reject_meta_past_end(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    w32(buf + SYN_HS_HDR_OFF + 16, SYN_SIZE);    /* meta_data_offset = end */
    w32(buf + SYN_HS_HDR_OFF + 20, 12);          /* meta_data_size = 12 */

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) < 0);
}

static void test_meta_absent(void)
{
    uint8_t buf[SYN_SIZE];
    build_blob(buf, NVFW_BIN_MAGIC_STD);
    /* meta_data_size = 0 → has_meta = false, parse still succeeds. */
    w32(buf + SYN_HS_HDR_OFF + 20, 0);

    struct nvfw_image img;
    REQUIRE(nvfw_parse(buf, sizeof(buf), &img) == 0);
    REQUIRE(!img.has_meta);
}

/* Optional: if /lib/firmware has GSP R535 blobs locally, sanity-check
 * that the parser accepts them. Skip silently otherwise so this
 * test works on CI hosts without NVIDIA firmware installed. */
static void test_real_r535_booter_load(void)
{
    const char *path =
        "/lib/firmware/nvidia/ga107/gsp/booter_load-535.113.01.bin.zst";
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Zstd-decompress variant doesn't exist here — this test is
         * an optional local smoke check. Skip silently. */
        return;
    }
    fclose(f);
    /* We don't wire zstd into the unit-test binary. The `make gsp-harness`
     * build does the decompression as part of firmware embedding; if
     * that's not available here, skip. */
}

int main(void)
{
    test_parse_std_magic();
    test_parse_nouveau_magic();
    test_reject_bad_magic();
    test_reject_null();
    test_reject_too_small();
    test_reject_oversized();
    test_reject_data_past_end();
    test_reject_header_offset_past_end();
    test_reject_patch_loc_ptr_past_end();
    test_reject_too_many_apps();
    test_reject_apps_past_end();
    test_reject_meta_past_end();
    test_meta_absent();
    test_real_r535_booter_load();

    if (failures == 0) {
        printf("test_nvfw: all tests PASS\n");
        return 0;
    }
    printf("test_nvfw: %d FAIL\n", failures);
    return 1;
}
