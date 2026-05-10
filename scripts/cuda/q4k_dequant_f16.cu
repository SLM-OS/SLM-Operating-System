/*
 * q4k_dequant_f16.cu — Q4_K → FP16 dequantizer on Jetson GA10B
 * (first SLM-on-GPU operator from the #540 menu).
 *
 * Q4_K is the GGUF Q4_K_M quantization format used by Qwen2.5-1.5B
 * (and the rest of the SLM-OS shipping models). One super-block holds
 * 256 elements packed into 144 bytes:
 *
 *   offset  size  field
 *   0       2     d      (FP16, super-block scale)
 *   2       2     dmin   (FP16, super-block min)
 *   4       12    scales/mins, 6-bit packed (8 sub-blocks × 2 fields)
 *   16      128   qs     (4-bit nibbles, 256 quantized weights)
 *
 * 8 sub-blocks of 32 elements each. For sub-block index `is`:
 *   scale(is) ∈ [0..63]   (6 bits)
 *   min(is)   ∈ [0..63]   (6 bits)
 *   The 6-bit values are packed across `scales[0..11]` per the
 *   ggml-quants Q4_K layout (see Q4KBlockView in
 *   runtime/src/slm/gguf.rs and dequant_q4k_block in
 *   runtime/src/inference/quant.rs for the canonical CPU formula).
 *
 * Output formula per element (CPU reference, line-for-line):
 *   nibble = (lo half) ? (qs[l] & 0xF)        // l ∈ [0..32)
 *                      : (qs[l] >> 4)         // l ∈ [0..32)
 *   y      = d * scale(is) * nibble - dmin * min(is)
 *   out    = __float2half(y)
 *
 * Where for sub-block j ∈ [0..4) the layout walks two halves:
 *   is = 2*j      → first  32 outputs use low  nibbles of qs[j*32 + l]
 *   is = 2*j + 1  → second 32 outputs use high nibbles of qs[j*32 + l]
 *
 * Mapping (one block per super-block, 256 threads per block):
 *   blockIdx.x = super-block index (0..nb-1)
 *   threadIdx.x ∈ [0..256)
 *     j         = tid >> 6                 // sub-block pair index 0..3
 *     half_idx  = (tid >> 5) & 1           // 0 = low nibble, 1 = high
 *     l         = tid & 31                 // element within the half
 *     is        = (j << 1) | half_idx      // sub-block index 0..7
 *   one thread per output FP16 element
 *
 * cbuf[0] layout (CUDA ABI for sm_87, mirrors gemm_hmma_fp16 / add_bias):
 *   [0x160] blocks (const uint8_t *)   // packed Q4_K super-blocks
 *   [0x168] out    (half *)            // FP16 output, 256 * nb halves
 *   [0x170] nb     (int)               // number of super-blocks
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o q4k_dequant_f16 q4k_dequant_f16.cu
 *   cuobjdump --extract-elf all q4k_dequant_f16
 *   readelf -SW q4k_dequant_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define Q4K_BLOCK_BYTES   144
#define Q4K_ELEMS         256

/* Decode the 6-bit scale `i` (i ∈ [0..8)) from the 12-byte scales/mins
 * region. Mirrors Q4KBlockView::scale exactly:
 *     i < 4:  s[i]     & 0x3F
 *     i >= 4: (s[i+4] & 0x0F) | ((s[i-4] >> 6) << 4)
 */
__device__ __forceinline__ uint8_t q4k_scale(const uint8_t *s, int i)
{
    if (i < 4) return s[i] & 0x3Fu;
    return (uint8_t)((s[i + 4] & 0x0Fu) | (((s[i - 4] >> 6) & 0x3u) << 4));
}

/* Decode the 6-bit min `i`:
 *     i < 4:  s[i+4]   & 0x3F
 *     i >= 4: (s[i+4] >> 4) | ((s[i] >> 6) << 4)
 */
__device__ __forceinline__ uint8_t q4k_min(const uint8_t *s, int i)
{
    if (i < 4) return s[i + 4] & 0x3Fu;
    return (uint8_t)(((s[i + 4] >> 4) & 0x0Fu) | (((s[i] >> 6) & 0x3u) << 4));
}

__global__ void q4k_dequant_f16(const uint8_t *blocks, half *out, int nb)
{
    int b = blockIdx.x;
    if (b >= nb) return;

    int tid = threadIdx.x;
    int j   = tid >> 6;          /* 0..3 — sub-block pair  */
    int hi  = (tid >> 5) & 1;    /* 0 = low nibble, 1 = high */
    int l   = tid & 31;          /* 0..31 — within half     */
    int is  = (j << 1) | hi;     /* 0..7 — sub-block index  */

    const uint8_t *blk    = blocks + b * Q4K_BLOCK_BYTES;
    half d_h              = *reinterpret_cast<const half *>(blk + 0);
    half dmin_h           = *reinterpret_cast<const half *>(blk + 2);
    const uint8_t *scales = blk + 4;
    const uint8_t *qs     = blk + 16;

    float d    = __half2float(d_h);
    float dmin = __half2float(dmin_h);

    uint8_t qpack = qs[(j << 5) + l];
    uint8_t nib   = hi ? (qpack >> 4) : (qpack & 0x0Fu);

    float sc = (float)q4k_scale(scales, is);
    float mn = (float)q4k_min(scales, is);

    float y = d * sc * (float)nib - dmin * mn;

    int out_idx = b * Q4K_ELEMS + (j * 64) + (hi * 32) + l;
    out[out_idx] = __float2half(y);
}

/* ---- CPU reference (mirrors runtime/src/inference/quant.rs) ---- */

static uint8_t cpu_scale(const uint8_t *s, int i)
{
    if (i < 4) return s[i] & 0x3F;
    return (uint8_t)((s[i + 4] & 0x0F) | ((s[i - 4] >> 6) << 4));
}

static uint8_t cpu_min(const uint8_t *s, int i)
{
    if (i < 4) return s[i + 4] & 0x3F;
    return (uint8_t)((s[i + 4] >> 4) | ((s[i] >> 6) << 4));
}

static void cpu_reference(const uint8_t *blocks, half *out, int nb)
{
    for (int b = 0; b < nb; b++) {
        const uint8_t *blk    = blocks + b * Q4K_BLOCK_BYTES;
        half d_h              = *reinterpret_cast<const half *>(blk + 0);
        half dmin_h           = *reinterpret_cast<const half *>(blk + 2);
        const uint8_t *scales = blk + 4;
        const uint8_t *qs     = blk + 16;
        float d    = __half2float(d_h);
        float dmin = __half2float(dmin_h);

        for (int j = 0; j < 4; j++) {
            int is1 = j * 2;
            int is2 = j * 2 + 1;
            float sc1 = (float)cpu_scale(scales, is1);
            float mn1 = (float)cpu_min(scales, is1);
            float sc2 = (float)cpu_scale(scales, is2);
            float mn2 = (float)cpu_min(scales, is2);

            for (int l = 0; l < 32; l++) {
                uint8_t q = qs[j * 32 + l];
                float lo  = d * sc1 * (float)(q & 0x0F) - dmin * mn1;
                float hi  = d * sc2 * (float)(q >> 4)   - dmin * mn2;
                out[b * 256 + j * 64 + l]      = __float2half(lo);
                out[b * 256 + j * 64 + 32 + l] = __float2half(hi);
            }
        }
    }
}

/* ---- Fixture builder ---- */

/* Pack a 6-bit scale value into the scales/mins layout slot `i`. */
static void pack_scale(uint8_t *s, int i, uint8_t v)
{
    v &= 0x3F;
    if (i < 4) {
        s[i] = (uint8_t)((s[i] & 0xC0) | v);
    } else {
        /* low 4 bits go to s[i+4][3:0]; high 2 bits go to s[i-4][7:6] */
        s[i + 4] = (uint8_t)((s[i + 4] & 0xF0) | (v & 0x0F));
        s[i - 4] = (uint8_t)((s[i - 4] & 0x3F) | (((v >> 4) & 0x3) << 6));
    }
}

static void pack_min(uint8_t *s, int i, uint8_t v)
{
    v &= 0x3F;
    if (i < 4) {
        s[i + 4] = (uint8_t)((s[i + 4] & 0xC0) | v);
    } else {
        s[i + 4] = (uint8_t)((s[i + 4] & 0x0F) | ((v & 0x0F) << 4));
        s[i] = (uint8_t)((s[i] & 0x3F) | (((v >> 4) & 0x3) << 6));
    }
}

static void build_fixture_block(uint8_t *blk, uint32_t *seed)
{
    memset(blk, 0, Q4K_BLOCK_BYTES);

    /* d = 0.125, dmin = 0.0625 — exact in FP16, keeps reference math
     * deterministic and easy to inspect. */
    half d_h    = __float2half(0.125f);
    half dmin_h = __float2half(0.0625f);
    memcpy(blk + 0, &d_h, 2);
    memcpy(blk + 2, &dmin_h, 2);

    uint8_t *scales = blk + 4;
    uint8_t *qs     = blk + 16;

    /* Scales = 1..8, mins = 0..7 — distinct per sub-block so a
     * mis-decode of the 6-bit packing surfaces in the diff. */
    for (int i = 0; i < 8; i++) {
        pack_scale(scales, i, (uint8_t)(i + 1));
        pack_min(scales, i, (uint8_t)i);
    }

    /* qs = pseudo-random nibble pairs from a tiny LCG. */
    for (int k = 0; k < 128; k++) {
        *seed = (*seed) * 1664525u + 1013904223u;
        qs[k] = (uint8_t)((*seed) & 0xFF);
    }
}

static int run_one(int nb)
{
    size_t in_bytes  = (size_t)nb * Q4K_BLOCK_BYTES;
    size_t out_count = (size_t)nb * Q4K_ELEMS;
    size_t out_bytes = out_count * sizeof(half);

    uint8_t *h_blocks = (uint8_t *)malloc(in_bytes);
    half    *h_gpu    = (half    *)malloc(out_bytes);
    half    *h_cpu    = (half    *)malloc(out_bytes);
    if (!h_blocks || !h_gpu || !h_cpu) { fprintf(stderr, "oom\n"); return 0; }

    uint32_t seed = 0xC0FFEEu;
    for (int b = 0; b < nb; b++) {
        build_fixture_block(h_blocks + (size_t)b * Q4K_BLOCK_BYTES, &seed);
    }

    cpu_reference(h_blocks, h_cpu, nb);

    uint8_t *d_blocks; half *d_out;
    cudaMalloc(&d_blocks, in_bytes);
    cudaMalloc(&d_out,    out_bytes);
    cudaMemcpy(d_blocks, h_blocks, in_bytes, cudaMemcpyHostToDevice);

    dim3 block(256, 1, 1);
    dim3 grid(nb, 1, 1);
    q4k_dequant_f16<<<grid, block>>>(d_blocks, d_out, nb);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[nb=%d] launch error: %s\n",
                nb, cudaGetErrorString(err));
        cudaFree(d_blocks); cudaFree(d_out);
        free(h_blocks); free(h_gpu); free(h_cpu);
        return 0;
    }

    cudaMemcpy(h_gpu, d_out, out_bytes, cudaMemcpyDeviceToHost);

    int ok = 1;
    int reported = 0;
    for (size_t i = 0; i < out_count; i++) {
        uint16_t g = *reinterpret_cast<uint16_t *>(&h_gpu[i]);
        uint16_t c = *reinterpret_cast<uint16_t *>(&h_cpu[i]);
        if (g != c) {
            if (reported < 4) {
                fprintf(stderr,
                        "[nb=%d] MISMATCH out[%zu] gpu=0x%04x cpu=0x%04x "
                        "(gpu=%f cpu=%f)\n",
                        nb, i, g, c,
                        (double)__half2float(h_gpu[i]),
                        (double)__half2float(h_cpu[i]));
                reported++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[nb=%d] OK — %zu halves match CPU reference bit-exactly\n",
               nb, out_count);
    }

    cudaFree(d_blocks); cudaFree(d_out);
    free(h_blocks); free(h_gpu); free(h_cpu);
    return ok;
}

int main()
{
    int all_ok = 1;
    /* 1 block — minimal smoke test.
     * 32 blocks — exercises grid > 1, mirrors a single GGUF row chunk
     *             for a typical 4096-wide weight slice (32 * 256 = 8192).
     * 128 blocks — multi-warp launch on real workload size. */
    all_ok &= run_one(1);
    all_ok &= run_one(32);
    all_ok &= run_one(128);
    return all_ok ? 0 : 1;
}
