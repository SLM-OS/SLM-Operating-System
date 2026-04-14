/*
 * test_nvidia_vbios.c — standalone regression tests for the shared
 * VBIOS parser (kernel/gpu/nvidia/nvidia_vbios.c).
 *
 * Runs on the host — links nvidia_vbios.c directly, feeds it
 * hand-constructed VBIOS images covering:
 *
 *   1. Minimal well-formed VBIOS with BIT entries incl. FWSEC (0x85).
 *   2. Rejection of non-NVIDIA signatures (PCI ROM signature
 *      mismatch, missing BIT table).
 *   3. Rejection of malformed BIT headers (bad entry_size,
 *      num_entries over cap, entry offset past image end).
 *   4. Entry-lookup by id + version, including multi-version matches.
 *
 * Building:  gcc -o test_nvidia_vbios test_nvidia_vbios.c \
 *                kernel/gpu/nvidia/nvidia_vbios.c
 * Run:       ./test_nvidia_vbios
 *
 * Wired into `make test-vbios` for CI.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../kernel/gpu/nvidia/nvidia_vbios.h"

/* ---- Minimal valid VBIOS builder ---- */

/*
 * Lay out a synthetic VBIOS with the same structure a real NVIDIA
 * card ships:
 *
 *   0x00  55 AA          PCI ROM signature
 *   0x02  NN             image size in 512-byte units
 *   0x03..0x17           filler / x86 init code
 *   0x18  PCIR           PCI Data Structure
 *   0x40..0x1DF          more filler
 *   0x1E0 FF B8 "BIT\0"  BIT signature
 *   0x1E8 12 06 03 00    hdr_size, entry_size, num_entries, checksum
 *   0x1EC entries (3 × 6 = 18 bytes)
 *   0x1FE "FWSEC_DATA"   FWSEC ucode payload (13 bytes)
 *
 * The 'I' (Init) entry points at a stub region at 0x200.
 */
#define VBIOS_SIZE 1024

static void build_good_vbios(uint8_t *buf,
                             uint32_t fwsec_off, uint32_t fwsec_len)
{
    memset(buf, 0, VBIOS_SIZE);
    buf[0] = 0x55;
    buf[1] = 0xAA;
    buf[2] = 2;                         /* 2 × 512 = 1024 bytes */

    /* BIT signature at 0x1E0. */
    uint32_t bit = 0x1E0;
    buf[bit + 0] = 0xFF;
    buf[bit + 1] = 0xB8;
    buf[bit + 2] = 'B';
    buf[bit + 3] = 'I';
    buf[bit + 4] = 'T';
    buf[bit + 5] = 0;
    buf[bit + 6] = 0;                   /* reserved / version */
    buf[bit + 7] = 0x01;                /* bios_minor */
    buf[bit + 8] = 12;                  /* hdr_size */
    buf[bit + 9] = 6;                   /* entry_size */
    buf[bit + 10] = 3;                  /* num_entries */
    buf[bit + 11] = 0;                  /* checksum (not validated by parser) */

    uint32_t e = bit + 12;              /* first entry */

    /* Entry 0: 'I' v1 — length 0x20 at offset 0x300 */
    buf[e + 0] = VBIOS_BIT_ID_I;
    buf[e + 1] = 1;
    buf[e + 2] = 0x20; buf[e + 3] = 0;
    buf[e + 4] = 0x00; buf[e + 5] = 0x03;

    /* Entry 1: 'P' v2 — length 4 at offset 0x320 */
    buf[e + 6 + 0] = VBIOS_BIT_ID_P;
    buf[e + 6 + 1] = 2;
    buf[e + 6 + 2] = 4; buf[e + 6 + 3] = 0;
    buf[e + 6 + 4] = 0x20; buf[e + 6 + 5] = 0x03;

    /* Entry 2: FWSEC (0x85) v1 — at fwsec_off / fwsec_len */
    buf[e + 12 + 0] = VBIOS_BIT_ID_FWSEC;
    buf[e + 12 + 1] = 1;
    buf[e + 12 + 2] = (uint8_t)(fwsec_len & 0xFF);
    buf[e + 12 + 3] = (uint8_t)((fwsec_len >> 8) & 0xFF);
    buf[e + 12 + 4] = (uint8_t)(fwsec_off & 0xFF);
    buf[e + 12 + 5] = (uint8_t)((fwsec_off >> 8) & 0xFF);

    /* FWSEC payload — recognizable bytes. */
    const char magic[] = "FWSEC_DATA";
    memcpy(buf + fwsec_off, magic, sizeof(magic) - 1);
}

/* ---- Test harness ---- */

static int failures;

#define REQUIRE(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void test_good_vbios_parses(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);
    REQUIRE(vb.parsed_ok);
    REQUIRE(vb.bit_offset == 0x1E0);
    REQUIRE(vb.hdr_size == 12);
    REQUIRE(vb.entry_size == 6);
    REQUIRE(vb.num_entries == 3);
}

static void test_reject_bad_pci_signature(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    buf[0] = 0x00;    /* clobber PCI ROM signature */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
    REQUIRE(!vb.parsed_ok);
}

static void test_reject_missing_bit(void)
{
    uint8_t buf[VBIOS_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x55; buf[1] = 0xAA;
    /* No BIT signature anywhere. */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
}

static void test_reject_malformed_entry_size(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    /* BIT entry_size is at bit_offset + 9. Set it to a bogus value. */
    buf[0x1E0 + 9] = 0x10;

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
}

static void test_reject_too_many_entries(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    buf[0x1E0 + 10] = 0xFF;    /* num_entries well past the defensive cap */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
}

static void test_reject_entry_off_end(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    /* Rewrite entry[0]'s data_offset to point well past image end. */
    uint32_t e = 0x1E0 + 12;
    buf[e + 4] = 0x00;
    buf[e + 5] = 0xF0;    /* offset 0xF000, well past 1024-byte buffer */
    /* Parsing succeeds (the parser allows entries to be out-of-range
     * until you try to USE them); find_entry is the layer that
     * rejects them. */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);
    uint32_t off = 0, len = 0;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, &off, &len) < 0);
}

static void test_find_entry_basics(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    uint32_t off = 0, len = 0;

    /* 'I' v1 at 0x300 len 0x20 */
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, 1, &off, &len) == 0);
    REQUIRE(off == 0x300);
    REQUIRE(len == 0x20);

    /* 'I' with wrong version */
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, 99, &off, &len) < 0);

    /* 'I' with any version (-1) */
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, &off, &len) == 0);

    /* Unknown id */
    REQUIRE(nvidia_vbios_find_entry(&vb, 0xCC, -1, &off, &len) < 0);
}

static void test_fwsec_extraction(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    const uint8_t *p = NULL;
    uint32_t size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &p, &size) == 0);
    REQUIRE(size == 10);
    REQUIRE(p == buf + 0x3E0);
    REQUIRE(memcmp(p, "FWSEC_DATA", 10) == 0);
}

static void test_fwsec_absent(void)
{
    /* Pascal-era VBIOS layout — no type 0x85 entry. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    /* Overwrite entry[2]'s id to something else. */
    buf[0x1E0 + 12 + 12 + 0] = 0x42;    /* 'B' */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    const uint8_t *p = NULL;
    uint32_t size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &p, &size) < 0);
}

int main(void)
{
    test_good_vbios_parses();
    test_reject_bad_pci_signature();
    test_reject_missing_bit();
    test_reject_malformed_entry_size();
    test_reject_too_many_entries();
    test_reject_entry_off_end();
    test_find_entry_basics();
    test_fwsec_extraction();
    test_fwsec_absent();

    if (failures == 0) {
        printf("test_nvidia_vbios: all tests PASS\n");
        return 0;
    }
    printf("test_nvidia_vbios: %d FAIL\n", failures);
    return 1;
}
