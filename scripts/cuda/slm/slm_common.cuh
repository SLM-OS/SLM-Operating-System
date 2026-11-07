/*
 * slm_common.cuh - shared device helpers for the #475 SLM kernel set.
 *
 * All kernels here are carved out of the cubin as raw SASS and launched by
 * SLM-OS with no CUDA runtime, so every helper must inline: no calls, no
 * stack, no constant-bank relocations, no libdevice functions.
 */
#ifndef SLM_COMMON_CUH
#define SLM_COMMON_CUH

#include <cstdint>
#include <cuda_fp16.h>

#define SLM_INL __device__ __forceinline__

/* Flag bits shared by the matmul kernels. */
#define SLM_MM_OUT_F16 1 /* out is FP16 (activation); clear means FP32 */
/* b64 only. Each launch covers weight blocks [start, start + count) and bias is
 * then a running FP32 sum shaped [B][N], which may alias out. */
#define SLM_MM_BIAS_RUNNING 2
/* b8 and b64: one x tile per CTA, grid (ceil(N/15), xtiles, 1), B up to 64. More
 * warps, repeats the weight decode per tile; for small N, limited by serial warp
 * latency. */
#define SLM_MM_SPLIT_XT 4
#define SLM_MM_BSTART(flags) (((flags) >> 8) & 0xFF)
#define SLM_MM_BCOUNT(flags) (((flags) >> 16) & 0xFF)

/* FP16 1.0. See the "offset encoding" note in slm_mm_q4k.cu. */
#define SLM_H_ONE 0x3C00u
#define SLM_H_ONE2 0x3C003C00u

/* One 16-byte load. The address must be 16-byte aligned. */
struct __align__(16) slm_x16 {
    uint32_t w0, w1, w2, w3;
};

SLM_INL float slm_h2f(uint32_t bits)
{
    return __half2float(__ushort_as_half((unsigned short)bits));
}

SLM_INL uint32_t slm_f2h(float v)
{
    return (uint32_t)__half_as_ushort(__float2half(v));
}

/*
 * mma.sync.m16n8k16: C[16,8] += A[16,16] @ B[16,8], where B is indexed
 * [k][n]. Register layout per lane, group g = lane / 4, c = lane % 4:
 *
 *   a0 = A[g][2c..2c+1]     a1 = A[g+8][2c..2c+1]
 *   a2 = A[g][2c+8..2c+9]   a3 = A[g+8][2c+8..2c+9]
 *   b0 = B[2c..2c+1][g]     b1 = B[2c+8..2c+9][g]
 *   c0 = C[g][2c]   c1 = C[g][2c+1]   c2 = C[g+8][2c]   c3 = C[g+8][2c+1]
 *
 * A and B hold two FP16 per register. All 32 lanes must be active.
 * The harness's mma_layout test pins this mapping on hardware.
 */
SLM_INL void slm_mma(float &c0, float &c1, float &c2, float &c3,
                     uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                     uint32_t b0, uint32_t b1)
{
    __asm__ volatile(
        "mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 "
        "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
        : "+f"(c0), "+f"(c1), "+f"(c2), "+f"(c3)
        : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
}

/* Row-major store for one matmul output element, FP16 or FP32. */
SLM_INL void slm_store_out(void *out, long idx, float v, int flags)
{
    if (flags & SLM_MM_OUT_F16) {
        reinterpret_cast<half *>(out)[idx] = __float2half(v);
    } else {
        reinterpret_cast<float *>(out)[idx] = v;
    }
}

#endif /* SLM_COMMON_CUH */
