/*
 * test_ga10b_ce.c — pin the GA10B Copy Engine pushbuffer encoding.
 *
 * Pure-logic test: builds a CE memcpy pushbuffer via
 * `ga10b_build_ce_memcpy_pushbuffer` and checks every dword against
 * the AMPERE_DMA_COPY_B method spec (class 0xC7B5, NVC7B5_*).
 *
 * Regressing the encoding (wrong class id, wrong method offset,
 * wrong LAUNCH_DMA flag value) silently produces "GPFIFO advanced
 * but the copy didn't happen" mysteries on hardware. This test
 * catches every byte at build time.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "ga10b_ce.h"

static int g_failures = 0;

#define REQUIRE_EQ(a, b)                                                    \
    do {                                                                    \
        uint64_t _a = (uint64_t)(a);                                        \
        uint64_t _b = (uint64_t)(b);                                        \
        if (_a != _b) {                                                     \
            printf("FAIL %s:%d: %s (=%lu / 0x%lx) != %s (=%lu / 0x%lx)\n",  \
                   __FILE__, __LINE__, #a, _a, _a, #b, _b, _b);             \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* Recompute the host-family method-header encoding so we can check
 * each `pb[i]` against expected method-id + subch + count. The
 * encoding (Volta+, INC opcode): bit 29 = 1, [25:16] = count,
 * [15:13] = subch, [12:0] = byte_off / 4. Mirror of the static
 * inline in ga10b_ce.c. */
static uint32_t hdr_inc(uint32_t count, uint32_t subch, uint32_t byte_off)
{
    return (1u << 29) | ((count & 0x3FFu) << 16) |
           ((subch & 0x7u) << 13) | (((byte_off) >> 2) & 0x1FFFu);
}

static void test_ce_pb_dwords_constant(void)
{
    printf("== test_ce_pb_dwords_constant ==\n");
    /* 13 method bursts × 2 dwords each (header + data) = 26. */
    REQUIRE_EQ(GA10B_CE_MEMCPY_PB_DWORDS, 26u);
}

static void test_ce_class_id_is_amper_dma_copy_b(void)
{
    printf("== test_ce_class_id_is_amper_dma_copy_b ==\n");
    REQUIRE_EQ(GA10B_AMPERE_DMA_COPY_B_CLASS_ID, 0xC7B5u);
}

static void test_ce_subchannel_is_4(void)
{
    printf("== test_ce_subchannel_is_4 ==\n");
    /* NVK convention pins copy classes to subch 4 (mesa nv_push.h).
     * Pinned here so a future "let's put CE on subch 3" surfaces in
     * review rather than as a silent class-binding race when the
     * compute path happens to bind to the same subch. */
    REQUIRE_EQ(GA10B_CE_SUBCHANNEL, 4u);
}

static void test_ce_method_offsets_match_clc7b5(void)
{
    printf("== test_ce_method_offsets_match_clc7b5 ==\n");
    /* Authoritative offsets from `~/slmos-ref/nvidia/nvidia-clc7b5.h`.
     * If a future NVIDIA refresh moves any of these, every CE call
     * silently broken until this test catches it. */
    REQUIRE_EQ(NVC7B5_LAUNCH_DMA,            0x300u);
    REQUIRE_EQ(NVC7B5_SET_SEMAPHORE_A,       0x240u);
    REQUIRE_EQ(NVC7B5_SET_SEMAPHORE_B,       0x244u);
    REQUIRE_EQ(NVC7B5_SET_SEMAPHORE_PAYLOAD, 0x248u);
    REQUIRE_EQ(NVC7B5_OFFSET_IN_UPPER,       0x400u);
    REQUIRE_EQ(NVC7B5_OFFSET_IN_LOWER,       0x404u);
    REQUIRE_EQ(NVC7B5_OFFSET_OUT_UPPER,      0x408u);
    REQUIRE_EQ(NVC7B5_OFFSET_OUT_LOWER,      0x40Cu);
    REQUIRE_EQ(NVC7B5_PITCH_IN,              0x410u);
    REQUIRE_EQ(NVC7B5_PITCH_OUT,             0x414u);
    REQUIRE_EQ(NVC7B5_LINE_LENGTH_IN,        0x418u);
    REQUIRE_EQ(NVC7B5_LINE_COUNT,            0x41Cu);
}

static void test_ce_launch_dma_flags_compose_expected_value(void)
{
    printf("== test_ce_launch_dma_flags_compose_expected_value ==\n");
    /* DATA_TRANSFER_TYPE_NON_PIPELINED (0x2 at [1:0])
     * | FLUSH_ENABLE (1<<2)
     * | SEMAPHORE_TYPE_RELEASE_NO_TIMESTAMP (1<<3)
     * | SRC_MEMORY_LAYOUT_PITCH (1<<7)
     * | DST_MEMORY_LAYOUT_PITCH (1<<8)
     * = 0x2 | 0x4 | 0x8 | 0x80 | 0x100 = 0x18E.
     * SRC_TYPE_VIRTUAL and DST_TYPE_VIRTUAL are 0-valued. */
    uint32_t expected = 0x18Eu;
    uint32_t composed =
        NVC7B5_LAUNCH_DMA_DATA_TRANSFER_TYPE_NON_PIPELINED |
        NVC7B5_LAUNCH_DMA_FLUSH_ENABLE |
        NVC7B5_LAUNCH_DMA_SEMAPHORE_TYPE_RELEASE_NO_TIMESTAMP |
        NVC7B5_LAUNCH_DMA_SRC_MEMORY_LAYOUT_PITCH |
        NVC7B5_LAUNCH_DMA_DST_MEMORY_LAYOUT_PITCH |
        NVC7B5_LAUNCH_DMA_SRC_TYPE_VIRTUAL |
        NVC7B5_LAUNCH_DMA_DST_TYPE_VIRTUAL;
    REQUIRE_EQ(composed, expected);
}

static void test_ce_launch_dma_src_dst_type_bits_independent(void)
{
    printf("== test_ce_launch_dma_src_dst_type_bits_independent ==\n");
    /* SRC_TYPE is at bit 12, DST_TYPE at bit 13 per the
     * clc7b5 spec. Verifying both bits explicitly so a future
     * "let's reuse bit 12 for both" regression breaks the build,
     * not a CE dispatch in production. */
    REQUIRE_EQ(NVC7B5_LAUNCH_DMA_SRC_TYPE_PHYSICAL, 1u << 12);
    REQUIRE_EQ(NVC7B5_LAUNCH_DMA_DST_TYPE_PHYSICAL, 1u << 13);
    /* OR-composing src=PHYS dst=PHYS must set both bits 12 AND 13. */
    REQUIRE_EQ(
        NVC7B5_LAUNCH_DMA_SRC_TYPE_PHYSICAL |
        NVC7B5_LAUNCH_DMA_DST_TYPE_PHYSICAL,
        (1u << 12) | (1u << 13));
}

static void test_ce_build_pushbuffer_encoding(void)
{
    printf("== test_ce_build_pushbuffer_encoding ==\n");
    /* Realistic-ish inputs: small src/dst VAs that exercise both
     * the lower 32-bit register and the upper 17-bit register, and
     * a length below the LAUNCH_DMA per-submit cap. */
    uint32_t pb[GA10B_CE_MEMCPY_PB_DWORDS];
    memset(pb, 0xAA, sizeof(pb));
    uint64_t src_gva = 0x123456789ABCDEF0ull;
    uint64_t dst_gva = 0xFEDCBA9876543210ull;
    uint64_t sem_gva = 0x00007FFFAABBCCDDull;
    uint32_t bytes   = 0x10000u;       /* 64 KB — one bounce-buffer chunk */
    uint32_t payload = 0xCAFEu;

    uint32_t n = ga10b_build_ce_memcpy_pushbuffer(
        pb, src_gva, dst_gva, bytes, sem_gva, payload);
    REQUIRE_EQ(n, GA10B_CE_MEMCPY_PB_DWORDS);

    const uint32_t sc = 4u;

    /* SET_OBJECT */
    REQUIRE_EQ(pb[0],  hdr_inc(1, sc, 0x00));
    REQUIRE_EQ(pb[1],  0xC7B5u);
    /* Semaphore A/B/payload */
    REQUIRE_EQ(pb[2],  hdr_inc(1, sc, 0x240));
    REQUIRE_EQ(pb[3],  (uint32_t)((sem_gva >> 32) & 0x1FFFFu));
    REQUIRE_EQ(pb[4],  hdr_inc(1, sc, 0x244));
    REQUIRE_EQ(pb[5],  (uint32_t)(sem_gva & 0xFFFFFFFFu));
    REQUIRE_EQ(pb[6],  hdr_inc(1, sc, 0x248));
    REQUIRE_EQ(pb[7],  payload);
    /* OFFSET_IN_UPPER / LOWER */
    REQUIRE_EQ(pb[8],  hdr_inc(1, sc, 0x400));
    REQUIRE_EQ(pb[9],  (uint32_t)((src_gva >> 32) & 0x1FFFFu));
    REQUIRE_EQ(pb[10], hdr_inc(1, sc, 0x404));
    REQUIRE_EQ(pb[11], (uint32_t)(src_gva & 0xFFFFFFFFu));
    /* OFFSET_OUT_UPPER / LOWER */
    REQUIRE_EQ(pb[12], hdr_inc(1, sc, 0x408));
    REQUIRE_EQ(pb[13], (uint32_t)((dst_gva >> 32) & 0x1FFFFu));
    REQUIRE_EQ(pb[14], hdr_inc(1, sc, 0x40C));
    REQUIRE_EQ(pb[15], (uint32_t)(dst_gva & 0xFFFFFFFFu));
    /* PITCH_IN / PITCH_OUT (1D, pitch == line_length) */
    REQUIRE_EQ(pb[16], hdr_inc(1, sc, 0x410));
    REQUIRE_EQ(pb[17], bytes);
    REQUIRE_EQ(pb[18], hdr_inc(1, sc, 0x414));
    REQUIRE_EQ(pb[19], bytes);
    /* LINE_LENGTH / LINE_COUNT */
    REQUIRE_EQ(pb[20], hdr_inc(1, sc, 0x418));
    REQUIRE_EQ(pb[21], bytes);
    REQUIRE_EQ(pb[22], hdr_inc(1, sc, 0x41C));
    REQUIRE_EQ(pb[23], 1u);
    /* LAUNCH_DMA — kicks the engine. */
    REQUIRE_EQ(pb[24], hdr_inc(1, sc, 0x300));
    REQUIRE_EQ(pb[25], 0x18Eu);
}

int main(void)
{
    test_ce_pb_dwords_constant();
    test_ce_class_id_is_amper_dma_copy_b();
    test_ce_subchannel_is_4();
    test_ce_method_offsets_match_clc7b5();
    test_ce_launch_dma_flags_compose_expected_value();
    test_ce_launch_dma_src_dst_type_bits_independent();
    test_ce_build_pushbuffer_encoding();

    if (g_failures != 0) {
        printf("[test_ga10b_ce] %d FAILURES\n", g_failures);
        return 1;
    }
    printf("[test_ga10b_ce] OK\n");
    return 0;
}
