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
#include <stdlib.h>
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

    /* PCIR sub-image record — new in 2026-04-14 to satisfy the
     * sub-image chain walker. The synthetic single-image VBIOS is a
     * PciAt-only shape: code_type=0x00, indicator=0x80 (LAST). */
    buf[0x18] = 0x80; buf[0x19] = 0x00;    /* pcir_ptr = 0x80 */
    uint32_t pcir = 0x80;
    buf[pcir+0]='P'; buf[pcir+1]='C'; buf[pcir+2]='I'; buf[pcir+3]='R';
    buf[pcir+4]=0xDE; buf[pcir+5]=0x10;    /* vendor 0x10DE */
    buf[pcir+6]=0x84; buf[pcir+7]=0x25;    /* device 0x2584 */
    buf[pcir+0x0A]=0x18; buf[pcir+0x0B]=0; /* pcir_len = 24 */
    buf[pcir+0x10]=0x02; buf[pcir+0x11]=0; /* img_len = 2 blocks = 1024 */
    buf[pcir+0x14]=0x00;                    /* code_type = x86 legacy */
    buf[pcir+0x15]=0x80;                    /* indicator = LAST */

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

/* ---- Edge-case / defensive tests (2026-04-14)
 *
 * These exercise input shapes that real hardware won't produce but
 * that a malicious or corrupted VBIOS image could, to make sure the
 * parser fails closed (returns -1, leaves parsed_ok=false) instead
 * of dereferencing bad pointers or reading past the buffer. Modeled
 * on the classes of input the GSP-harness will hand the parser on
 * first-boot of a strange third-party card.
 */

static void test_reject_null_image(void)
{
    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(NULL, 1024, &vb) < 0);
}

static void test_reject_null_out(void)
{
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), NULL) < 0);
}

static void test_reject_tiny_image(void)
{
    /* Anything under the 64-byte floor must be rejected without
     * touching the buffer past its end. */
    uint8_t buf[64] = { 0x55, 0xAA };
    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, 16, &vb) < 0);
    REQUIRE(nvidia_vbios_parse(buf, 0,  &vb) < 0);
    REQUIRE(!vb.parsed_ok);
}

static void test_reject_oversized_image(void)
{
    /* Anything over NVIDIA_VBIOS_MAX_SIZE must be rejected — a 2 GB
     * "VBIOS" is almost certainly a framing error or attacker input. */
    struct nvidia_vbios vb;
    /* Pass a size larger than the cap; the first two bytes are PCI-sig
     * so earlier checks don't short-circuit us. */
    uint8_t buf[64] = { 0x55, 0xAA };
    REQUIRE(nvidia_vbios_parse(buf, NVIDIA_VBIOS_MAX_SIZE + 1, &vb) < 0);
}

static void test_reject_bit_at_end_of_buffer(void)
{
    /* BIT signature right at the boundary where the BIT header won't
     * fit — parser must reject rather than read past end. */
    uint8_t buf[VBIOS_SIZE];
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x55; buf[1] = 0xAA;
    /* Place BIT signature at sizeof(buf) - 6; hdr_size claimed = 12
     * so hdr would run past the end. */
    uint32_t bit = sizeof(buf) - 6;
    buf[bit+0]=0xFF; buf[bit+1]=0xB8;
    buf[bit+2]='B';  buf[bit+3]='I'; buf[bit+4]='T'; buf[bit+5]=0;

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
}

static void test_reject_hdr_size_too_small(void)
{
    /* hdr_size < 12 (min) must be rejected — the parser relies on
     * a specific header layout that only fits in ≥ 12 bytes. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    buf[0x1E0 + 8] = 8;    /* hdr_size = 8 (below minimum) */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
}

static void test_reject_entries_overrun_image(void)
{
    /* hdr_size + num_entries * entry_size must fit inside the image.
     * Keep num_entries under the cap but make the hdr_size large enough
     * that entries_end runs past image end. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    /* hdr_size = 200 (pushes entries past end of 1024-byte buffer at
     * bit_offset 0x1E0 + 200 + 3*6 = 0x1E0 + 218 = 0x2BA, still fits).
     * Try a bigger number: hdr_size = 0xC0 (192), entries at 0x1E0+192
     * = 0x2A0, + 3*6=18 = 0x2B2, still fits. Use hdr_size that pushes
     * past end: set num_entries to 63 (under 64 cap) and hdr_size to
     * a value that blows past 1024. */
    buf[0x1E0 + 8] = 200;   /* hdr_size */
    buf[0x1E0 + 10] = 63;   /* num_entries — 63 * 6 = 378 bytes */
    /* end = 0x1E0 + 200 + 378 = 0x4F2 = 1266, past 1024-byte buffer */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) < 0);
}

static void test_reject_zero_num_entries(void)
{
    /* A VBIOS with num_entries = 0 is malformed — every shipping
     * VBIOS has at least 'I' (init scripts). Parser currently accepts
     * zero (the find_entry loop just returns -1), which is safe but
     * wasteful. Document the behavior: parse succeeds, but nothing
     * is findable. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    buf[0x1E0 + 10] = 0;    /* num_entries */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);
    REQUIRE(vb.num_entries == 0);

    uint32_t off = 0, len = 0;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, &off, &len) < 0);
}

static void test_find_entry_handles_unparsed_struct(void)
{
    /* Calling find_entry on a struct that was never parsed (or
     * failed to parse) must not crash. */
    struct nvidia_vbios vb;
    memset(&vb, 0, sizeof(vb));
    vb.parsed_ok = false;

    uint32_t off = 0, len = 0;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, &off, &len) < 0);
}

static void test_find_entry_null_outputs(void)
{
    /* Caller that doesn't care about offset or length should get a
     * clean 0/-1 return, not a NULL deref. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    /* Both out pointers NULL */
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, NULL, NULL) == 0);
    /* Just length NULL */
    uint32_t off = 0xDEADBEEF;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, &off, NULL) == 0);
    REQUIRE(off == 0x300);
    /* Just offset NULL */
    uint32_t len = 0xDEADBEEF;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, -1, NULL, &len) == 0);
    REQUIRE(len == 0x20);
}

static void test_fwsec_rejects_truncated_entry(void)
{
    /* FWSEC (id 0x85) entry points at data whose off + len runs
     * past the end of the image — find_entry's in-range check
     * must reject it, and get_fwsec must return -1. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    /* Overwrite the FWSEC entry (entry 2 at bit_offset + 12 + 12)
     * to claim a length that runs past the 1024-byte image. */
    uint32_t e2 = 0x1E0 + 12 + 12;
    buf[e2 + 2] = 0xFF;    /* data_len low */
    buf[e2 + 3] = 0xFF;    /* data_len high — claims 0xFFFF bytes */
    buf[e2 + 4] = 0x00;    /* data_offset low */
    buf[e2 + 5] = 0x03;    /* data_offset high (0x300 base + 0xFFFF > 1024) */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    const uint8_t *p = NULL;
    uint32_t size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &p, &size) < 0);
    REQUIRE(p == NULL);
}

static void test_find_entry_multi_version(void)
{
    /* Entry 'P' appears with v2 in build_good_vbios. Add a second
     * 'P' v3 and confirm both can be found by version. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    /* Rewrite entry[1] ('P' v2) to keep it, and overwrite entry[2]
     * (FWSEC) with a second 'P' v3. */
    uint32_t e2 = 0x1E0 + 12 + 12;
    buf[e2+0] = VBIOS_BIT_ID_P;
    buf[e2+1] = 3;
    buf[e2+2] = 8; buf[e2+3] = 0;
    buf[e2+4] = 0x40; buf[e2+5] = 0x03;    /* off 0x340, len 8 */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    uint32_t off = 0, len = 0;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_P, 2, &off, &len) == 0);
    REQUIRE(off == 0x320);
    REQUIRE(len == 4);

    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_P, 3, &off, &len) == 0);
    REQUIRE(off == 0x340);
    REQUIRE(len == 8);

    /* Wildcard returns the first match (v2). */
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_P, -1, &off, &len) == 0);
    REQUIRE(off == 0x320);
}

static void test_bit_signature_deep_scan(void)
{
    /* BIT signatures appear at different offsets on different
     * boards — ours scans the first 64 KB. Put BIT well past
     * entry 0x1E0 to confirm the scan finds it. */
    uint8_t *buf = calloc(1, 8192);
    buf[0] = 0x55; buf[1] = 0xAA;
    buf[2] = 16;   /* 16 × 512 = 8192 bytes */

    /* Include a PCIR so the sub-image chain walker is satisfied. */
    buf[0x18] = 0x80; buf[0x19] = 0;
    uint32_t pcir = 0x80;
    buf[pcir+0]='P'; buf[pcir+1]='C'; buf[pcir+2]='I'; buf[pcir+3]='R';
    buf[pcir+4]=0xDE; buf[pcir+5]=0x10;
    buf[pcir+6]=0x84; buf[pcir+7]=0x25;
    buf[pcir+0x0A]=0x18; buf[pcir+0x0B]=0;
    buf[pcir+0x10]=0x10; buf[pcir+0x11]=0;  /* 16 blocks = 8192 */
    buf[pcir+0x14]=0x00;
    buf[pcir+0x15]=0x80;    /* LAST */

    uint32_t bit = 0x1200;   /* 4608 — well beyond the usual 0x1E0 */
    buf[bit+0] = 0xFF; buf[bit+1] = 0xB8;
    buf[bit+2] = 'B'; buf[bit+3] = 'I'; buf[bit+4] = 'T'; buf[bit+5] = 0;
    buf[bit+8] = 12;    /* hdr_size */
    buf[bit+9] = 6;     /* entry_size */
    buf[bit+10] = 0;    /* num_entries (zero — minimal) */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, 8192, &vb) == 0);
    REQUIRE(vb.bit_offset == bit);

    free(buf);
}

static void test_fwsec_rejects_unparsed_struct(void)
{
    struct nvidia_vbios vb;
    memset(&vb, 0, sizeof(vb));
    vb.parsed_ok = false;

    const uint8_t *p = NULL;
    uint32_t size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &p, &size) < 0);
}

static void test_reject_entry_overlaps_bit_table(void)
{
    /* Defensive rejection: an entry whose data region overlaps the
     * BIT header or entry list is structurally invalid (a real VBIOS
     * places all data after the entry list). A crafted image could
     * otherwise let a caller read the BIT table's metadata as if it
     * were payload. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    /* Rewrite entry[0] ('I' v1) to have data_offset pointing INTO the
     * BIT entry list itself. BIT is at 0x1E0, hdr = 12, entries span
     * 3*6 = 18 bytes — entries end at 0x1E0 + 30 = 0x1FE. Pointing
     * into the middle of that range (0x1F0) must be rejected. */
    uint32_t e0 = 0x1E0 + 12;
    buf[e0 + 2] = 0x08; buf[e0 + 3] = 0;    /* data_len = 8 */
    buf[e0 + 4] = 0xF0; buf[e0 + 5] = 0x01; /* data_offset = 0x1F0 */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    uint32_t off = 0, len = 0;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, 1, &off, &len) < 0);
}

/* ---- Multi-sub-image + Falcon ucode table tests (2026-04-14) ----
 *
 * Build a synthetic Ampere-shape VBIOS with PciAt + FwSec1 + FwSec2
 * sub-images, a BIT 'p' entry carrying a FalconUcodeTablePtr, a
 * Falcon ucode table with one FWSEC_PROD entry, and a
 * FalconUCodeDescV3 + IMEM + DMEM payload. Confirms the full
 * nova-core-style extraction path returns the right pointer + size.
 */
#define AMPERE_VBIOS_SIZE   (16 * 1024)

static void build_ampere_vbios(uint8_t *buf,
                               uint32_t *out_payload_off,
                               uint32_t *out_payload_size)
{
    memset(buf, 0, AMPERE_VBIOS_SIZE);

    /* Layout (bytes):
     *   0x0000 .. 0x0FFF  PciAt (4 KB)  — 0x55AA + PCIR + BIT
     *   0x1000 .. 0x1FFF  FwSec1 (4 KB) — 0x55AA + NPDS, payload filler
     *   0x2000 .. 0x3FFF  FwSec2 (8 KB) — 0x55AA + NPDS + PMU table
     *                                     + FWSEC descriptor
     */
    uint32_t pciat_off = 0x0000;
    uint32_t fw1_off   = 0x1000;
    uint32_t fw2_off   = 0x2000;
    uint32_t pciat_len = 0x1000;
    uint32_t fw1_len   = 0x1000;
    uint32_t fw2_len   = 0x2000;

    /* ---- PciAt image ---- */
    buf[pciat_off+0] = 0x55;
    buf[pciat_off+1] = 0xAA;
    buf[pciat_off+2] = pciat_len / 512;
    /* PCIR at offset 0x80 */
    buf[pciat_off+0x18] = 0x80; buf[pciat_off+0x19] = 0;
    {
        uint32_t pcir = pciat_off + 0x80;
        buf[pcir+0]='P'; buf[pcir+1]='C'; buf[pcir+2]='I'; buf[pcir+3]='R';
        buf[pcir+4]=0xDE; buf[pcir+5]=0x10;
        buf[pcir+6]=0x84; buf[pcir+7]=0x25;
        buf[pcir+0x0A]=0x18; buf[pcir+0x0B]=0;        /* pcir_len */
        buf[pcir+0x10]=pciat_len/512; buf[pcir+0x11]=0; /* img_len */
        buf[pcir+0x14]=0x00;                            /* code_type x86 */
        buf[pcir+0x15]=0x00;                            /* NOT LAST */
    }

    /* BIT at 0x200, with a 'p' entry pointing to the PMU table */
    uint32_t bit = 0x200;
    buf[bit+0]=0xFF; buf[bit+1]=0xB8;
    buf[bit+2]='B'; buf[bit+3]='I'; buf[bit+4]='T'; buf[bit+5]=0;
    buf[bit+8]=12; buf[bit+9]=6; buf[bit+10]=1;  /* 1 entry */

    /* Entry 0: 'p' (0x70) v2 — 4 bytes of data pointing at 0x400 */
    uint32_t ent = bit + 12;
    buf[ent+0]=VBIOS_BIT_ID_FALCON_DATA;
    buf[ent+1]=2;
    buf[ent+2]=4; buf[ent+3]=0;
    buf[ent+4]=0x00; buf[ent+5]=0x04;  /* data_offset = 0x400 */

    /* FalconUcodeTablePtr placed at 0x400 inside PciAt.
     * We want the target address (after PciAt|FwSec1|FwSec2 math) to
     * land inside FwSec2 at offset 0x100 (absolute 0x2100).
     * Pointer formula (nova-core):
     *   target_in_concat = PciAt_len + FwSec1_len + offset_in_fwsec2
     *                    = 0x1000 + 0x1000 + 0x100
     *                    = 0x2100
     */
    uint32_t falcon_ptr = pciat_len + fw1_len + 0x100;
    buf[0x400]=falcon_ptr & 0xff;
    buf[0x401]=(falcon_ptr >> 8) & 0xff;
    buf[0x402]=(falcon_ptr >> 16) & 0xff;
    buf[0x403]=(falcon_ptr >> 24) & 0xff;

    /* ---- FwSec1 image (4 KB) ---- */
    buf[fw1_off+0] = 0x55;
    buf[fw1_off+1] = 0xAA;
    buf[fw1_off+2] = fw1_len / 512;
    /* NPDS at offset 0x80 */
    buf[fw1_off+0x18] = 0x80; buf[fw1_off+0x19] = 0;
    {
        uint32_t pcir = fw1_off + 0x80;
        buf[pcir+0]='N'; buf[pcir+1]='P'; buf[pcir+2]='D'; buf[pcir+3]='S';
        buf[pcir+4]=0xDE; buf[pcir+5]=0x10;
        buf[pcir+6]=0x84; buf[pcir+7]=0x25;
        buf[pcir+0x0A]=0x18; buf[pcir+0x0B]=0;
        buf[pcir+0x10]=fw1_len/512; buf[pcir+0x11]=0;
        buf[pcir+0x14]=0xE0;                            /* code_type FwSec */
        buf[pcir+0x15]=0x00;                            /* NOT LAST */
    }

    /* ---- FwSec2 image (8 KB) ---- */
    buf[fw2_off+0] = 0x55;
    buf[fw2_off+1] = 0xAA;
    buf[fw2_off+2] = fw2_len / 512;
    buf[fw2_off+0x18] = 0x80; buf[fw2_off+0x19] = 0;
    {
        uint32_t pcir = fw2_off + 0x80;
        buf[pcir+0]='N'; buf[pcir+1]='P'; buf[pcir+2]='D'; buf[pcir+3]='S';
        buf[pcir+4]=0xDE; buf[pcir+5]=0x10;
        buf[pcir+6]=0x84; buf[pcir+7]=0x25;
        buf[pcir+0x0A]=0x18; buf[pcir+0x0B]=0;
        buf[pcir+0x10]=fw2_len/512; buf[pcir+0x11]=0;
        buf[pcir+0x14]=0xE0;
        buf[pcir+0x15]=0x80;                            /* LAST */
    }

    /* PMU table at FwSec2 offset 0x100 (absolute 0x2100).
     * FALCON_UCODE_TABLE_HDR_V1: version=1, hdr=6, entry_size=6,
     *   entry_count=1, desc_version=3 (V3 = Ampere), desc_size=44. */
    uint32_t tbl = fw2_off + 0x100;
    buf[tbl+0]=1; buf[tbl+1]=6; buf[tbl+2]=6;
    buf[tbl+3]=1; buf[tbl+4]=3; buf[tbl+5]=44;

    /* One entry: app_id=FWSEC_PROD, target=0, desc_ptr → 0x200 in FwSec2.
     * Pointer math (same scheme): concatenated offset =
     *   PciAt_len + FwSec1_len + 0x200 = 0x2200. */
    uint32_t entry = tbl + 6;
    uint32_t desc_ptr = pciat_len + fw1_len + 0x200;
    buf[entry+0]=VBIOS_FALCON_APPID_FWSEC_PROD;
    buf[entry+1]=0;
    buf[entry+2]=desc_ptr & 0xff;
    buf[entry+3]=(desc_ptr >> 8) & 0xff;
    buf[entry+4]=(desc_ptr >> 16) & 0xff;
    buf[entry+5]=(desc_ptr >> 24) & 0xff;

    /* FalconUCodeDescV3 header at FwSec2 offset 0x200 (absolute 0x2200).
     *   hdr = (size << 16) | (ver << 8) | 1
     *   ver = 3, size = 44 */
    uint32_t desc = fw2_off + 0x200;
    uint32_t hdr = (44u << 16) | (3u << 8) | 1u;
    buf[desc+0]=hdr & 0xff;
    buf[desc+1]=(hdr>>8) & 0xff;
    buf[desc+2]=(hdr>>16) & 0xff;
    buf[desc+3]=(hdr>>24) & 0xff;

    /* imem_load_size at offset 20 (V3) = 64 bytes */
    buf[desc+20]=64; buf[desc+21]=0; buf[desc+22]=0; buf[desc+23]=0;
    /* dmem_load_size at offset 32 = 32 bytes */
    buf[desc+32]=32; buf[desc+33]=0; buf[desc+34]=0; buf[desc+35]=0;
    /* sig_count at offset 39 = 0 (no signatures for test) */
    buf[desc+39]=0;

    if (out_payload_off)  *out_payload_off  = desc;
    if (out_payload_size) *out_payload_size = 44 + 0 + 64 + 32;    /* 140 */
}

static void test_fwsec_get_parts_splits_correctly(void)
{
    /* The Ampere VBIOS builder produces a V3 descriptor whose
     * payload is laid out as desc(44) | sigs(0) | imem(64) | dmem(32).
     * Confirm get_fwsec_parts returns pointers to each section
     * with the right sizes — this is the API E3.4 bringup uses
     * to extract IMEM/DMEM from the FWSEC payload. */
    uint8_t *buf = calloc(1, AMPERE_VBIOS_SIZE);
    uint32_t exp_off = 0, exp_size = 0;
    build_ampere_vbios(buf, &exp_off, &exp_size);

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, AMPERE_VBIOS_SIZE, &vb) == 0);

    struct nvidia_vbios_fwsec_parts p;
    REQUIRE(nvidia_vbios_get_fwsec_parts(&vb, &p) == 0);

    /* desc points at the 44-byte V3 header. */
    REQUIRE(p.desc == buf + exp_off);

    /* sigs immediately follow the header (zero size in this test). */
    REQUIRE(p.sigs == buf + exp_off + 44);
    REQUIRE(p.sigs_size == 0);

    /* imem and dmem sizes match what build_ampere_vbios wrote. */
    REQUIRE(p.imem_size == 64);
    REQUIRE(p.dmem_size == 32);

    /* imem starts at desc + 44 (since sigs_size = 0). */
    REQUIRE(p.imem == buf + exp_off + 44);
    /* dmem starts after imem. */
    REQUIRE(p.dmem == buf + exp_off + 44 + 64);

    free(buf);
}

static void test_fwsec_get_parts_rejects_no_fwsec(void)
{
    /* Pascal-shape VBIOS — no FWSEC entry. get_fwsec_parts must
     * return -1 cleanly so bringup fails closed, not on a NULL deref. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);
    /* Overwrite FWSEC entry id to 'B' so it isn't found. */
    buf[0x1E0 + 12 + 12 + 0] = 0x42;

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    struct nvidia_vbios_fwsec_parts p;
    REQUIRE(nvidia_vbios_get_fwsec_parts(&vb, &p) < 0);
}

static void test_ampere_fwsec_full_chain(void)
{
    uint8_t *buf = calloc(1, AMPERE_VBIOS_SIZE);
    uint32_t exp_off = 0, exp_size = 0;
    build_ampere_vbios(buf, &exp_off, &exp_size);

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, AMPERE_VBIOS_SIZE, &vb) == 0);
    REQUIRE(vb.num_subimages == 3);
    REQUIRE(vb.pciat_idx == 0);
    REQUIRE(vb.first_fwsec_idx == 1);
    REQUIRE(vb.second_fwsec_idx == 2);

    const uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &fw, &fw_size) == 0);
    REQUIRE(fw == buf + exp_off);
    REQUIRE(fw_size == exp_size);

    free(buf);
}

static void test_ampere_fwsec_rejects_ptr_past_fwsec2(void)
{
    /* If FalconUcodeTablePtr resolves to a position past the end of
     * FwSec2 (which happens if the image is truncated), extraction
     * must fail cleanly instead of reading off the buffer end. */
    uint8_t *buf = calloc(1, AMPERE_VBIOS_SIZE);
    build_ampere_vbios(buf, NULL, NULL);

    /* Rewrite the FalconUcodeTablePtr to point past FwSec2 end. */
    uint32_t bad_ptr = 0x1000 + 0x1000 + 0x2000 + 0x100;   /* FwSec2 end + 256 */
    buf[0x400]=bad_ptr & 0xff;
    buf[0x401]=(bad_ptr>>8) & 0xff;
    buf[0x402]=(bad_ptr>>16) & 0xff;
    buf[0x403]=(bad_ptr>>24) & 0xff;

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, AMPERE_VBIOS_SIZE, &vb) == 0);

    const uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &fw, &fw_size) < 0);
    REQUIRE(fw == NULL);
    REQUIRE(fw_size == 0);

    free(buf);
}

static void test_ampere_fwsec_rejects_no_fwsec_prod_entry(void)
{
    /* Falcon ucode table exists but has no FWSEC_PROD (0x85) entry —
     * e.g. only debug-signed entries. Must return -1. */
    uint8_t *buf = calloc(1, AMPERE_VBIOS_SIZE);
    build_ampere_vbios(buf, NULL, NULL);

    /* Change entry app_id to FWSEC_DBG (0x45) — we only accept
     * production. */
    uint32_t entry = 0x2000 + 0x100 + 6;
    buf[entry+0] = VBIOS_FALCON_APPID_FWSEC_DBG;

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, AMPERE_VBIOS_SIZE, &vb) == 0);

    const uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &fw, &fw_size) < 0);

    free(buf);
}

static void test_ampere_fwsec_rejects_bad_desc_version(void)
{
    uint8_t *buf = calloc(1, AMPERE_VBIOS_SIZE);
    build_ampere_vbios(buf, NULL, NULL);

    /* Clobber descriptor version to something bogus (V1 is not used). */
    uint32_t desc = 0x2000 + 0x200;
    buf[desc+1] = 1;    /* version byte — 1 is rejected */

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, AMPERE_VBIOS_SIZE, &vb) == 0);

    const uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    REQUIRE(nvidia_vbios_get_fwsec(&vb, &fw, &fw_size) < 0);

    free(buf);
}

static void test_entry_at_bit_region_boundary(void)
{
    /* An entry whose data starts exactly at the byte after the BIT
     * entry list must be accepted — the region-overlap check uses
     * strict inequality on the end bound. */
    uint8_t buf[VBIOS_SIZE];
    build_good_vbios(buf, 0x3E0, 10);

    /* BIT region ends at 0x1E0 + 12 + 3*6 = 0x1FE.
     * Point 'I' entry at 0x1FE with length 2 — should parse fine. */
    uint32_t e0 = 0x1E0 + 12;
    buf[e0 + 2] = 0x02; buf[e0 + 3] = 0;
    buf[e0 + 4] = 0xFE; buf[e0 + 5] = 0x01;

    struct nvidia_vbios vb;
    REQUIRE(nvidia_vbios_parse(buf, sizeof(buf), &vb) == 0);

    uint32_t off = 0, len = 0;
    REQUIRE(nvidia_vbios_find_entry(&vb, VBIOS_BIT_ID_I, 1, &off, &len) == 0);
    REQUIRE(off == 0x1FE);
    REQUIRE(len == 2);
}

int main(void)
{
    /* ---- Happy paths ---- */
    test_good_vbios_parses();
    test_find_entry_basics();
    test_fwsec_extraction();
    test_fwsec_absent();

    /* ---- Malformed VBIOS rejection ---- */
    test_reject_bad_pci_signature();
    test_reject_missing_bit();
    test_reject_malformed_entry_size();
    test_reject_too_many_entries();
    test_reject_entry_off_end();

    /* ---- Defensive / edge cases (2026-04-14) ---- */
    test_reject_null_image();
    test_reject_null_out();
    test_reject_tiny_image();
    test_reject_oversized_image();
    test_reject_bit_at_end_of_buffer();
    test_reject_hdr_size_too_small();
    test_reject_entries_overrun_image();
    test_reject_zero_num_entries();
    test_find_entry_handles_unparsed_struct();
    test_find_entry_null_outputs();
    test_fwsec_rejects_truncated_entry();
    test_find_entry_multi_version();
    test_bit_signature_deep_scan();
    test_fwsec_rejects_unparsed_struct();
    test_reject_entry_overlaps_bit_table();
    test_entry_at_bit_region_boundary();

    /* ---- Ampere multi-sub-image + PMU descriptor walk ---- */
    test_fwsec_get_parts_splits_correctly();
    test_fwsec_get_parts_rejects_no_fwsec();
    test_ampere_fwsec_full_chain();
    test_ampere_fwsec_rejects_ptr_past_fwsec2();
    test_ampere_fwsec_rejects_no_fwsec_prod_entry();
    test_ampere_fwsec_rejects_bad_desc_version();

    if (failures == 0) {
        printf("test_nvidia_vbios: all tests PASS\n");
        return 0;
    }
    printf("test_nvidia_vbios: %d FAIL\n", failures);
    return 1;
}
