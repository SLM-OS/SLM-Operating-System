/*
 * embedding_q4k_f16.cu — Token-ID → embedding-row gather over a
 * Q4_K-packed embedding table on Jetson GA10B (seventh SLM-on-GPU
 * operator from the #540 menu — closes the last gap in the SLM
 * forward path so end-to-end GPU decode never falls back to CPU).
 *
 * The first op of every transformer forward pass: given a token id,
 * produce the FP16 embedding vector for that token. Qwen2.5 / Llama /
 * Phi all store the embedding table in Q4_K (one super-block per
 * 256 elements), so the gather and the dequant happen in one kernel.
 *
 *   row_bytes    = (embedding_length / 256) * 144 bytes
 *                  (or padded up to the next 256-block boundary if
 *                   embedding_length % 256 != 0 — SmolLM2-135M has
 *                   embedding_length=576 → 3 super-blocks → 768
 *                   stored, only the first 576 used)
 *   row_offset   = token_id * table_row_bytes
 *   for each super-block b in [0, n_blocks):
 *     for each element e in [0, 256):
 *       elem_idx = b * 256 + e
 *       if elem_idx < embedding_length:
 *         out[elem_idx] = __float2half(d * scale(is) * nibble(e)
 *                                       - dmin * min(is))
 *
 * Same Q4_K decoding as `q4k_dequant_f16.cu` (#699): tid → (j, half, l)
 * → is = 2j+half, elem_off = j*64 + half*32 + l. Mirrors
 * `runtime/src/slm/forward.rs::embedding_lookup_any` (the
 * quant-aware variant `forward_one` actually calls).
 *
 * Mapping (one block per super-block in the row, 256 threads per block):
 *   blockIdx.x  = super-block index within the row (0..n_blocks-1)
 *   threadIdx.x = element-within-super-block index (0..255)
 *
 * Padding: trailing threads in the final super-block whose `elem_idx`
 * exceeds `embedding_length` simply return without writing. The
 * over-allocated row tail is intentional — Q4_K's 256-element
 * granularity rules out shorter rows.
 *
 * cbuf[0] layout (CUDA ABI for sm_87):
 *   [0x160] table_bytes    (const uint8_t *)  [n_tokens × table_row_bytes]
 *   [0x168] out            (half *)           [embedding_length]
 *   [0x170] token_id       (int)              [0, n_tokens)
 *   [0x174] embedding_length (int)            FP16 outputs to write
 *   [0x178] table_row_bytes  (int)            bytes per token row
 *
 * Compile + extract SASS on Jetson:
 *   nvcc -arch=sm_87 -o embedding_q4k_f16 embedding_q4k_f16.cu
 *   cuobjdump --extract-elf all embedding_q4k_f16
 *   readelf -SW embedding_q4k_f16.2.sm_87.cubin | grep text
 */
#include <cuda_fp16.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define Q4K_BLOCK_BYTES   144
#define Q4K_BLOCK_ELEMS   256

__device__ __forceinline__ uint8_t q4k_scale_d(const uint8_t *s, int i)
{
    if (i < 4) return s[i] & 0x3Fu;
    return (uint8_t)((s[i + 4] & 0x0Fu) | (((s[i - 4] >> 6) & 0x3u) << 4));
}

__device__ __forceinline__ uint8_t q4k_min_d(const uint8_t *s, int i)
{
    if (i < 4) return s[i + 4] & 0x3Fu;
    return (uint8_t)(((s[i + 4] >> 4) & 0x0Fu) | (((s[i] >> 6) & 0x3u) << 4));
}

__global__ void embedding_q4k_f16(const uint8_t *table_bytes, half *out,
                                    int token_id, int embedding_length,
                                    int table_row_bytes)
{
    int b   = blockIdx.x;
    int tid = threadIdx.x;

    int n_blocks = table_row_bytes / Q4K_BLOCK_BYTES;
    if (b >= n_blocks) return;

    /* tid → (j, half, l) → element offset within the 256-elem block. */
    int j        = tid >> 6;
    int hi       = (tid >> 5) & 1;
    int l        = tid & 31;
    int is       = (j << 1) | hi;
    int qs_off   = (j << 5) + l;
    int elem_off = (j << 6) + (hi << 5) + l;

    int out_idx = (b << 8) + elem_off;
    if (out_idx >= embedding_length) return;       /* padding tail */

    const uint8_t *row = table_bytes + (size_t)token_id * table_row_bytes;
    const uint8_t *blk = row + b * Q4K_BLOCK_BYTES;

    half d_h              = *reinterpret_cast<const half *>(blk + 0);
    half dmin_h           = *reinterpret_cast<const half *>(blk + 2);
    const uint8_t *scales = blk + 4;
    const uint8_t *qs     = blk + 16;

    float d    = __half2float(d_h);
    float dmin = __half2float(dmin_h);

    uint8_t qpack = qs[qs_off];
    uint8_t nib   = hi ? (qpack >> 4) : (qpack & 0x0Fu);

    float sc = (float)q4k_scale_d(scales, is);
    float mn = (float)q4k_min_d(scales, is);
    float y  = d * sc * (float)nib - dmin * mn;

    out[out_idx] = __float2half(y);
}

/* ---- CPU reference (mirrors runtime/src/slm/forward.rs::embedding_lookup_any) ---- */

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

static void cpu_reference(const uint8_t *table_bytes, half *out,
                          int token_id, int embedding_length,
                          int table_row_bytes)
{
    int n_blocks = table_row_bytes / Q4K_BLOCK_BYTES;
    const uint8_t *row = table_bytes + (size_t)token_id * table_row_bytes;
    for (int b = 0; b < n_blocks; b++) {
        const uint8_t *blk    = row + b * Q4K_BLOCK_BYTES;
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
                int e_lo = b * 256 + j * 64 + l;
                int e_hi = b * 256 + j * 64 + 32 + l;
                uint8_t q = qs[j * 32 + l];
                if (e_lo < embedding_length) {
                    float v = d * sc1 * (float)(q & 0x0F) - dmin * mn1;
                    out[e_lo] = __float2half(v);
                }
                if (e_hi < embedding_length) {
                    float v = d * sc2 * (float)(q >> 4) - dmin * mn2;
                    out[e_hi] = __float2half(v);
                }
            }
        }
    }
}

/* ---- Fixture builder + harness ---- */

static uint16_t half_bits(half h)
{
    uint16_t b;
    memcpy(&b, &h, sizeof(b));
    return b;
}

static void pack_scale(uint8_t *s, int i, uint8_t v)
{
    v &= 0x3F;
    if (i < 4) {
        s[i] = (uint8_t)((s[i] & 0xC0) | v);
    } else {
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

/* Build one fixture super-block. Same shape as q4k_dequant_f16.cu /
 * q4k_dot_f16.cu: d = 0.125, dmin = 0.0625; scales = 1..8, mins = 0..7;
 * qs = pseudo-random nibble pairs. The token_id offset is what makes
 * one row distinguishable from another — let the seed encode it so
 * row 0 and row 5 are obviously different. */
static void build_fixture_block(uint8_t *blk, uint32_t *seed)
{
    memset(blk, 0, Q4K_BLOCK_BYTES);
    half d_h    = __float2half(0.125f);
    half dmin_h = __float2half(0.0625f);
    memcpy(blk + 0, &d_h, 2);
    memcpy(blk + 2, &dmin_h, 2);
    uint8_t *scales = blk + 4;
    uint8_t *qs     = blk + 16;
    for (int i = 0; i < 8; i++) {
        pack_scale(scales, i, (uint8_t)(i + 1));
        pack_min(scales, i, (uint8_t)i);
    }
    for (int k = 0; k < 128; k++) {
        *seed = (*seed) * 1664525u + 1013904223u;
        qs[k] = (uint8_t)((*seed) & 0xFF);
    }
}

/* Build a fake embedding table with `n_tokens` rows of `n_blocks`
 * super-blocks each. Seed mixes in token_id so each row is distinct. */
static void build_table(uint8_t *table, int n_tokens, int n_blocks,
                        uint32_t base_seed)
{
    int row_bytes = n_blocks * Q4K_BLOCK_BYTES;
    for (int t = 0; t < n_tokens; t++) {
        uint8_t *row = table + (size_t)t * row_bytes;
        uint32_t seed = base_seed ^ (0x9E3779B9u * (uint32_t)t);
        for (int b = 0; b < n_blocks; b++) {
            build_fixture_block(row + b * Q4K_BLOCK_BYTES, &seed);
        }
    }
}

static int run_one(int n_tokens, int embedding_length, int token_id,
                   const char *label)
{
    if (token_id < 0 || token_id >= n_tokens) {
        fprintf(stderr, "[%s] token_id=%d out of range\n", label, token_id);
        return 0;
    }
    int n_blocks = (embedding_length + 255) / 256;
    int row_bytes = n_blocks * Q4K_BLOCK_BYTES;
    size_t table_bytes_n = (size_t)n_tokens * row_bytes;

    uint8_t *h_table = (uint8_t *)malloc(table_bytes_n);
    half    *h_gpu   = (half    *)malloc(embedding_length * sizeof(half));
    half    *h_cpu   = (half    *)malloc(embedding_length * sizeof(half));
    if (!h_table || !h_gpu || !h_cpu) { fprintf(stderr, "oom\n"); return 0; }

    /* Fill h_gpu / h_cpu with a sentinel so an unwritten tail surfaces
     * as a diff against the CPU reference (which also leaves the tail
     * untouched). 0xCAFE is a recognisable FP16 bit pattern. */
    for (int i = 0; i < embedding_length; i++) {
        memcpy(&h_gpu[i], "\xfe\xca", 2);
        memcpy(&h_cpu[i], "\xfe\xca", 2);
    }

    build_table(h_table, n_tokens, n_blocks, 0xC0FFEE00u);

    cpu_reference(h_table, h_cpu, token_id, embedding_length, row_bytes);

    uint8_t *d_table; half *d_out;
    cudaMalloc(&d_table, table_bytes_n);
    cudaMalloc(&d_out,   embedding_length * sizeof(half));
    cudaMemcpy(d_table, h_table, table_bytes_n,                    cudaMemcpyHostToDevice);
    cudaMemcpy(d_out,   h_gpu,   embedding_length * sizeof(half),  cudaMemcpyHostToDevice);

    dim3 block(256, 1, 1);
    dim3 grid(n_blocks, 1, 1);
    embedding_q4k_f16<<<grid, block>>>(d_table, d_out,
                                        token_id, embedding_length,
                                        row_bytes);
    cudaError_t err = cudaDeviceSynchronize();
    if (err != cudaSuccess) {
        fprintf(stderr, "[%s] launch error: %s\n",
                label, cudaGetErrorString(err));
        cudaFree(d_table); cudaFree(d_out);
        free(h_table); free(h_gpu); free(h_cpu);
        return 0;
    }

    cudaMemcpy(h_gpu, d_out, embedding_length * sizeof(half),
               cudaMemcpyDeviceToHost);

    /* Bit-exact comparison: same FP32 → FP16 conversion path, no
     * reduction, no parallel sum. Should match Q4K_DEQUANT (#699). */
    int ok = 1;
    int reported = 0;
    for (int i = 0; i < embedding_length; i++) {
        if (half_bits(h_gpu[i]) != half_bits(h_cpu[i])) {
            if (reported < 4) {
                fprintf(stderr,
                        "[%s] MISMATCH out[%d] gpu=0x%04x cpu=0x%04x\n",
                        label, i, half_bits(h_gpu[i]), half_bits(h_cpu[i]));
                reported++;
            }
            ok = 0;
        }
    }

    if (ok) {
        printf("[%s] OK — %d halves match CPU reference bit-exactly "
               "(token=%d, n_tokens=%d)\n",
               label, embedding_length, token_id, n_tokens);
    }

    cudaFree(d_table); cudaFree(d_out);
    free(h_table); free(h_gpu); free(h_cpu);
    return ok;
}

/* Boundary case: padded row (embedding_length=576 → 3 super-blocks =
 * 768 stored, only first 576 written). Verify the kernel respects
 * the bounds and that the trailing 192 outputs are NOT written. */
static int run_padded_row(void)
{
    const int embedding_length = 576;          /* SmolLM2-135M */
    const int n_blocks = 3;                    /* (576 + 255) / 256 = 3 */
    const int row_bytes = n_blocks * Q4K_BLOCK_BYTES;
    const int n_tokens = 4;
    const int token_id = 1;
    size_t table_bytes_n = (size_t)n_tokens * row_bytes;

    /* Allocate output as 768 (full block tail) but only 576 should
     * be written. Initialize all to 0xCAFE; verify positions 576..767
     * remain 0xCAFE. */
    const int padded_n = 768;
    uint8_t *h_table = (uint8_t *)malloc(table_bytes_n);
    half *h_out      = (half *)malloc(padded_n * sizeof(half));
    if (!h_table || !h_out) { fprintf(stderr, "oom\n"); return 0; }
    for (int i = 0; i < padded_n; i++) {
        memcpy(&h_out[i], "\xfe\xca", 2);
    }
    build_table(h_table, n_tokens, n_blocks, 0xBEEF0000u);

    uint8_t *d_table; half *d_out;
    cudaMalloc(&d_table, table_bytes_n);
    cudaMalloc(&d_out,   padded_n * sizeof(half));
    cudaMemcpy(d_table, h_table, table_bytes_n, cudaMemcpyHostToDevice);
    cudaMemcpy(d_out,   h_out,   padded_n * sizeof(half), cudaMemcpyHostToDevice);

    dim3 block(256, 1, 1);
    dim3 grid(n_blocks, 1, 1);
    embedding_q4k_f16<<<grid, block>>>(d_table, d_out,
                                        token_id, embedding_length,
                                        row_bytes);
    cudaDeviceSynchronize();
    cudaMemcpy(h_out, d_out, padded_n * sizeof(half), cudaMemcpyDeviceToHost);

    int ok = 1;
    /* Positions 576..767 must still hold the 0xCAFE sentinel. */
    for (int i = embedding_length; i < padded_n; i++) {
        if (half_bits(h_out[i]) != 0xCAFEu) {
            fprintf(stderr,
                    "[padded len=%d] out[%d] = 0x%04x, expected 0xCAFE "
                    "(kernel wrote past embedding_length)\n",
                    embedding_length, i, half_bits(h_out[i]));
            ok = 0;
            break;
        }
    }
    if (ok) {
        printf("[padded len=%d alloc=%d] OK — kernel respects "
               "embedding_length bound (positions %d..%d untouched)\n",
               embedding_length, padded_n,
               embedding_length, padded_n - 1);
    }

    cudaFree(d_table); cudaFree(d_out);
    free(h_table); free(h_out);
    return ok;
}

int main()
{
    int all_ok = 1;

    /* Production shapes. Token IDs are random picks well inside the
     * vocab — exercises the row-offset arithmetic across non-trivial
     * stride values.
     *
     *   Qwen2.5-1.5B: vocab=151936, embedding=1536. Token=0 vs 75000
     *                 vs the very last id, vocab-1.
     *   Llama-2 7B:   vocab=32000,  embedding=4096.
     *   Phi-2:        vocab=51200,  embedding=2560.
     *   SmolLM2:      vocab=49152,  embedding=576 (padded — see below).
     *
     * The actual GGUF vocabularies are large; using a 1024-token mock
     * table here keeps the harness fast while still testing
     * non-zero token offsets. */
    all_ok &= run_one(1024, 1536, 0,    "qwen-tok0   ");
    all_ok &= run_one(1024, 1536, 1023, "qwen-tokLAST");
    all_ok &= run_one(1024, 1536, 512,  "qwen-tok512 ");
    all_ok &= run_one(256,  4096, 100,  "llama-tok100");
    all_ok &= run_one(512,  2560, 256,  "phi-tok256  ");

    all_ok &= run_padded_row();

    return all_ok ? 0 : 1;
}
