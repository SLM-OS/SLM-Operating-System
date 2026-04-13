/*
 * sse_kernels.c — SSE2 inference kernels for x86-64 (C1 / P3-1).
 *
 * Called from the Rust runtime (runtime/src/inference/ops.rs) via
 * the extern "C" declarations in that file. Implements the four
 * hot SIMD paths — relu, zero, add-scalar, fused-multiply-add row —
 * using SSE2 intrinsics.
 *
 * Why a separate C file instead of Rust inline asm or intrinsics:
 * the `x86_64-unknown-none` target spec hardcodes `+soft-float`,
 * which makes it impossible for rustc to (a) allocate xmm regs in
 * inline asm, or (b) legalize `__m128` return values from intrinsics
 * — both documented in GitHub #72. Compiling this single translation
 * unit with `-msse -msse2` (see CMakeLists.txt) bypasses the Rust
 * toolchain entirely. The kernel's trampoline32.S / ap_trampoline.S
 * set CR4.OSFXSR + CR4.OSXMMEXCPT + CR0.MP with CR0.EM cleared so
 * these instructions execute at CPL=0 without #UD / #NM.
 *
 * All kernels use `movups` (unaligned loads/stores) so the callers
 * don't need to guarantee 16-byte alignment of input/output buffers.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stddef.h>

#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

/* Compile-time guard: CMake must have added -msse -msse2 to this
 * translation unit. If SSE is not enabled, fall back to scalar so
 * the file still builds (useful when cross-compiling to a target
 * without SSE). */
#if !defined(__SSE__)
# warning "sse_kernels.c built without -msse; falling back to scalar"
#endif

void slm_sse_relu_f32(const float *inp, float *outp, size_t n)
{
#if defined(__SSE__)
    size_t n4 = n & ~(size_t)3;
    const __m128 zero = _mm_setzero_ps();
    size_t i = 0;
    for (; i < n4; i += 4) {
        __m128 v = _mm_loadu_ps(inp + i);
        _mm_storeu_ps(outp + i, _mm_max_ps(v, zero));
    }
    for (; i < n; i++) {
        float v = inp[i];
        outp[i] = v > 0.0f ? v : 0.0f;
    }
#else
    for (size_t i = 0; i < n; i++) {
        float v = inp[i];
        outp[i] = v > 0.0f ? v : 0.0f;
    }
#endif
}

void slm_sse_zero_f32(float *ptr, size_t n)
{
#if defined(__SSE__)
    size_t n4 = n & ~(size_t)3;
    const __m128 zero = _mm_setzero_ps();
    size_t i = 0;
    for (; i < n4; i += 4) {
        _mm_storeu_ps(ptr + i, zero);
    }
    for (; i < n; i++) {
        ptr[i] = 0.0f;
    }
#else
    for (size_t i = 0; i < n; i++) {
        ptr[i] = 0.0f;
    }
#endif
}

/* ABI note on the scalar argument:
 *
 * The kernel and test_x86_boot.c are compiled with `-mno-sse`.
 * On that compilation, SysV AMD64 cannot pass a `float` parameter
 * in %xmm1 (SSE is disabled), so GCC falls back to the x87 FPU and
 * writes the value via `fstps` into the shadow-stack area. The
 * callee (this file, compiled with `-msse`) would then read stale
 * garbage from %xmm1.
 *
 * Passing the scalar as `uint32_t` (the IEEE 754 binary32 bit
 * representation) makes the FFI boundary integer-only: GCC puts it
 * in %esi regardless of `-mno-sse`, and the SSE callee bit-casts it
 * back to float via a union. Rust extern "C" with the matching
 * `u32` declaration does the same bit-cast on the caller side.
 */
static inline float slm_sse_bits_to_float(uint32_t bits)
{
    union { uint32_t u; float f; } u;
    u.u = bits;
    return u.f;
}

void slm_sse_add_scalar_f32(float *ptr, uint32_t scalar_bits, size_t n)
{
    const float scalar = slm_sse_bits_to_float(scalar_bits);
#if defined(__SSE__)
    size_t n4 = n & ~(size_t)3;
    const __m128 sv = _mm_set1_ps(scalar);
    size_t i = 0;
    for (; i < n4; i += 4) {
        __m128 v = _mm_loadu_ps(ptr + i);
        _mm_storeu_ps(ptr + i, _mm_add_ps(v, sv));
    }
    for (; i < n; i++) {
        ptr[i] += scalar;
    }
#else
    for (size_t i = 0; i < n; i++) {
        ptr[i] += scalar;
    }
#endif
}

void slm_sse_fma_row_f32(float *cp, const float *bp, uint32_t scalar_bits, size_t n)
{
    const float scalar = slm_sse_bits_to_float(scalar_bits);
#if defined(__SSE__)
    size_t n4 = n & ~(size_t)3;
    const __m128 a_vec = _mm_set1_ps(scalar);
    size_t j = 0;
    for (; j < n4; j += 4) {
        __m128 b = _mm_loadu_ps(bp + j);
        __m128 c = _mm_loadu_ps(cp + j);
        /* SSE2 has no FMA; use mul + add. */
        _mm_storeu_ps(cp + j, _mm_add_ps(c, _mm_mul_ps(a_vec, b)));
    }
    for (; j < n; j++) {
        cp[j] += scalar * bp[j];
    }
#else
    for (size_t j = 0; j < n; j++) {
        cp[j] += scalar * bp[j];
    }
#endif
}

#endif /* PLATFORM_X86_64 */
