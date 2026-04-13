//! Tensor operator implementations for CPU inference.
//!
//! All operators are pure functions that read input tensors and write to
//! pre-allocated output tensors. No dynamic allocation.
//!
//! SIMD optimization:
//! - AArch64: NEON float32x4_t (4-wide FP32) for all operators
//! - Scalar fallback for tail elements and unsupported platforms

use core::sync::atomic::{AtomicBool, Ordering};
use super::tensor::{Tensor, TensorElemType};
use super::engine::EngineError;
use crate::loader::registry::fp16_to_f32;

/// Spinlock protecting static scratch buffers (IM2COL_BUF, FP16_BUF)
/// from concurrent access on SMP systems.
static OPS_LOCK: AtomicBool = AtomicBool::new(false);

fn ops_lock() {
    while OPS_LOCK.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        core::hint::spin_loop();
    }
}

fn ops_unlock() {
    OPS_LOCK.store(false, Ordering::Release);
}

// =============================================================================
// FP16 helpers
// =============================================================================

/// Maximum FP16 row conversion buffer (elements, not bytes).
/// Supports matrices up to 1024 columns wide for on-the-fly conversion.
const FP16_ROW_BUF_SIZE: usize = 1024;

/// Convert a row of FP16 values to FP32 in a scratch buffer.
/// Returns a pointer to the FP32 buffer. Uses a static buffer (not reentrant).
unsafe fn fp16_row_to_f32(src: *const u16, n: usize, buf: *mut f32) -> *const f32 {
    let count = core::cmp::min(n, FP16_ROW_BUF_SIZE);
    for i in 0..count {
        *buf.add(i) = fp16_to_f32(*src.add(i));
    }
    buf as *const f32
}

// =============================================================================
// INT8 quantized matmul
// =============================================================================

/// INT8 quantized matmul: C_fp32[M,N] = dequant(A_i8[M,K] × B_i8[K,N])
///
/// Accumulates in INT32, then converts to FP32 using:
///   C_fp32[i,j] = scale_a * scale_b * sum_k((A[i,k] - zp_a) * (B[k,j] - zp_b))
///
/// Output is always FP32 (dequantized).
unsafe fn matmul_int8(
    a: &Tensor, b: &Tensor, cp: *mut f32, m: usize, k: usize, n: usize,
) {
    let ap = a.data as *const i8;
    let bp = b.data as *const i8;
    let zp_a = a.quant.zero_point as i32;
    let zp_b = b.quant.zero_point as i32;
    let scale = a.quant.scale * b.quant.scale;

    for i in 0..m {
        for j in 0..n {
            let mut acc: i32 = 0;
            for kk in 0..k {
                let a_val = (*ap.add(i * k + kk) as i32) - zp_a;
                let b_val = (*bp.add(kk * n + j) as i32) - zp_b;
                acc += a_val * b_val;
            }
            *cp.add(i * n + j) = (acc as f32) * scale;
        }
    }
}

/// Quantize an FP32 tensor to INT8 in-place (for post-training quantization).
///
/// Computes scale and zero_point from min/max, writes INT8 values to `out_buf`.
/// Returns the QuantParams used.
pub fn quantize_fp32_to_int8(
    data: *const f32, n: usize, out_buf: *mut i8,
) -> super::tensor::QuantParams {
    unsafe {
        // Find min/max
        let mut min_val = *data;
        let mut max_val = *data;
        for i in 1..n {
            let v = *data.add(i);
            if v < min_val { min_val = v; }
            if v > max_val { max_val = v; }
        }

        // Compute scale and zero_point (asymmetric quantization)
        // Maps [min_val, max_val] to [-128, 127]
        let range = max_val - min_val;
        let scale = if range > 0.0 { range / 255.0 } else { 1.0 };
        // zero_point: the INT8 value that represents 0.0
        // 0.0 = scale * (zero_point - zero_point) when q = zero_point
        // zero_point = round(-min_val / scale) - 128
        // Simpler: map min_val -> -128, max_val -> 127
        let zp_f = -128.0 - min_val / scale;
        let zero_point = if zp_f < -128.0 { -128i8 }
            else if zp_f > 127.0 { 127i8 }
            else { libm::roundf(zp_f) as i8 };

        // Quantize: q = round(v / scale) + zero_point
        let inv_scale = 1.0 / scale;
        for i in 0..n {
            let v = *data.add(i);
            let q = libm::roundf(v * inv_scale) as i32 + zero_point as i32;
            let clamped = if q < -128 { -128i8 }
                else if q > 127 { 127i8 }
                else { q as i8 };
            *out_buf.add(i) = clamped;
        }

        super::tensor::QuantParams { scale, zero_point }
    }
}

// =============================================================================
// SIMD helpers
// =============================================================================

/// Zero a buffer using SIMD where possible.
///
/// # Safety
/// `ptr..ptr+n` must be writable and properly aligned for f32 SIMD stores.
/// Caller guarantees `target_feature(neon)` on aarch64 (enabled globally by
/// the build config; aarch64 baseline mandates NEON).
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn zero_buf(ptr: *mut f32, n: usize) {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let zero = vdupq_n_f32(0.0);
        let n4 = n & !3;
        let mut i = 0;
        while i < n4 {
            vst1q_f32(ptr.add(i), zero);
            i += 4;
        }
        while i < n {
            *ptr.add(i) = 0.0;
            i += 1;
        }
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        for i in 0..n {
            *ptr.add(i) = 0.0;
        }
    }
}

// =============================================================================
// Conv2D — 2D convolution (NCHW layout)
// =============================================================================

/// Conv2D: out = conv(input, weight) + bias
///
/// Uses im2col to reshape convolution into a matmul, then dispatches
/// to the SIMD-optimized matmul kernel for the inner computation.
///
/// MNIST uses: input [N,C_in,H,W], weight [C_out,C_in,kH,kW], bias [C_out]
pub fn conv2d(
    input: &Tensor,    // [N, C_in, H, W]
    weight: &Tensor,   // [C_out, C_in, kH, kW]
    bias: Option<&Tensor>, // [C_out]
    out: &mut Tensor,  // [N, C_out, H_out, W_out]
    kernel_h: u32,
    kernel_w: u32,
    stride_h: u32,
    stride_w: u32,
    pad_h: u32,
    pad_w: u32,
) -> Result<(), EngineError> {
    if input.ndim < 4 || weight.ndim < 4 {
        return Err(EngineError::ShapeMismatch);
    }

    let batch = input.dim(0) as usize;
    let c_in = input.dim(1) as usize;
    let h_in = input.dim(2) as usize;
    let w_in = input.dim(3) as usize;
    let c_out = weight.dim(0) as usize;
    let kh = kernel_h as usize;
    let kw = kernel_w as usize;
    let sh = stride_h as usize;
    let sw = stride_w as usize;
    let ph = pad_h as usize;
    let pw = pad_w as usize;

    let h_out = (h_in + 2 * ph - kh) / sh + 1;
    let w_out = (w_in + 2 * pw - kw) / sw + 1;

    if out.num_elements() != batch * c_out * h_out * w_out {
        return Err(EngineError::ShapeMismatch);
    }

    // im2col buffer size: c_in * kh * kw columns, h_out * w_out rows
    let col_rows = c_in * kh * kw;
    let col_cols = h_out * w_out;

    // Static im2col buffer for reshaping convolution into matmul.
    // im2col is only used on AArch64 (x86-64-unknown-none soft-float + LTO
    // triggers an LLVM crash with the large static buffer + FP operations).
    const IM2COL_MAX: usize = 16384;
    #[cfg(target_arch = "aarch64")]
    let use_im2col = col_rows * col_cols <= IM2COL_MAX;
    #[cfg(not(target_arch = "aarch64"))]
    let use_im2col = false;

    unsafe {
        let inp = input.data;
        let wt = weight.data;
        let outp = out.data_mut();

        zero_buf(outp, out.num_elements());

        if use_im2col {
            // im2col + matmul path — lock protects static buffer from SMP races
            ops_lock();
            static mut IM2COL_BUF: [f32; IM2COL_MAX] = [0.0; IM2COL_MAX];
            // SAFETY: OPS_LOCK held — no concurrent access to IM2COL_BUF.
            let col = IM2COL_BUF.as_mut_ptr();

            for n in 0..batch {
                // Build im2col matrix: unroll input patches into columns
                for c in 0..c_in {
                    for khi in 0..kh {
                        for kwi in 0..kw {
                            let row = (c * kh + khi) * kw + kwi;
                            for ho in 0..h_out {
                                for wo in 0..w_out {
                                    let hi = ho * sh + khi;
                                    let wi = wo * sw + kwi;
                                    let col_idx = row * col_cols + ho * w_out + wo;
                                    if hi >= ph && hi < h_in + ph && wi >= pw && wi < w_in + pw {
                                        let h_idx = hi - ph;
                                        let w_idx = wi - pw;
                                        let in_idx = ((n * c_in + c) * h_in + h_idx) * w_in + w_idx;
                                        *col.add(col_idx) = *inp.add(in_idx);
                                    } else {
                                        *col.add(col_idx) = 0.0;
                                    }
                                }
                            }
                        }
                    }
                }

                // Matmul: weight[c_out, col_rows] × col[col_rows, col_cols] → out[c_out, col_cols]
                let out_batch = outp.add(n * c_out * col_cols);
                matmul_inner(wt, col, out_batch, c_out, col_rows, col_cols);

                // Add bias
                if let Some(b) = bias {
                    for co in 0..c_out {
                        let bias_val = *b.data.add(co);
                        let row_ptr = out_batch.add(co * col_cols);
                        add_scalar_simd(row_ptr, bias_val, col_cols);
                    }
                }
            }
            ops_unlock();
        } else {
            // Direct computation fallback for large convolutions
            for n in 0..batch {
                for co in 0..c_out {
                    for ho in 0..h_out {
                        for wo in 0..w_out {
                            let mut sum = 0.0f32;
                            for ci in 0..c_in {
                                for khi in 0..kh {
                                    for kwi in 0..kw {
                                        let hi = ho * sh + khi;
                                        let wi = wo * sw + kwi;
                                        if hi >= ph && hi < h_in + ph && wi >= pw && wi < w_in + pw {
                                            let h_idx = hi - ph;
                                            let w_idx = wi - pw;
                                            let in_idx = ((n * c_in + ci) * h_in + h_idx) * w_in + w_idx;
                                            let wt_idx = ((co * c_in + ci) * kh + khi) * kw + kwi;
                                            sum += *inp.add(in_idx) * *wt.add(wt_idx);
                                        }
                                    }
                                }
                            }
                            if let Some(b) = bias {
                                sum += *b.data.add(co);
                            }
                            let out_idx = ((n * c_out + co) * h_out + ho) * w_out + wo;
                            *outp.add(out_idx) = sum;
                        }
                    }
                }
            }
        }
    }

    Ok(())
}

/// Add a scalar to each element of a buffer using SIMD.
///
/// # Safety
/// `ptr..ptr+n` readable/writable, aligned for f32 SIMD access.
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn add_scalar_simd(ptr: *mut f32, scalar: f32, n: usize) {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let sv = vdupq_n_f32(scalar);
        let n4 = n & !3;
        let mut i = 0;
        while i < n4 {
            let v = vld1q_f32(ptr.add(i));
            vst1q_f32(ptr.add(i), vaddq_f32(v, sv));
            i += 4;
        }
        while i < n {
            *ptr.add(i) += scalar;
            i += 1;
        }
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        for i in 0..n {
            *ptr.add(i) += scalar;
        }
    }
}

// =============================================================================
// MaxPool2D — 2D max pooling (NCHW layout)
// =============================================================================

/// MaxPool2D: out = maxpool(input)
///
/// MNIST uses: kernel_shape=[2,2], strides=[2,2]
pub fn maxpool2d(
    input: &Tensor,    // [N, C, H, W]
    out: &mut Tensor,  // [N, C, H_out, W_out]
    kernel_h: u32,
    kernel_w: u32,
    stride_h: u32,
    stride_w: u32,
) -> Result<(), EngineError> {
    if input.ndim < 4 {
        return Err(EngineError::ShapeMismatch);
    }

    let batch = input.dim(0) as usize;
    let channels = input.dim(1) as usize;
    let h_in = input.dim(2) as usize;
    let w_in = input.dim(3) as usize;
    let kh = kernel_h as usize;
    let kw = kernel_w as usize;
    let sh = stride_h as usize;
    let sw = stride_w as usize;

    let h_out = (h_in - kh) / sh + 1;
    let w_out = (w_in - kw) / sw + 1;

    if out.num_elements() != batch * channels * h_out * w_out {
        return Err(EngineError::ShapeMismatch);
    }

    unsafe {
        let inp = input.data;
        let outp = out.data_mut();

        for n in 0..batch {
            for c in 0..channels {
                for ho in 0..h_out {
                    for wo in 0..w_out {
                        let mut max_val = f32::NEG_INFINITY;
                        for khi in 0..kh {
                            for kwi in 0..kw {
                                let hi = ho * sh + khi;
                                let wi = wo * sw + kwi;
                                if hi < h_in && wi < w_in {
                                    let idx = ((n * channels + c) * h_in + hi) * w_in + wi;
                                    let v = *inp.add(idx);
                                    if v > max_val {
                                        max_val = v;
                                    }
                                }
                            }
                        }
                        let out_idx = ((n * channels + c) * h_out + ho) * w_out + wo;
                        *outp.add(out_idx) = max_val;
                    }
                }
            }
        }
    }

    Ok(())
}

// =============================================================================
// Reshape — zero-copy, just reinterpret shape
// =============================================================================

/// Reshape: reinterpret tensor with new shape.
///
/// Zero-copy — the output shares the input's data pointer.
/// Validates that element count is preserved.
pub fn reshape(input: &Tensor, new_shape: &[u32]) -> Result<Tensor, EngineError> {
    let mut new_count: usize = 1;
    for &d in new_shape {
        new_count *= d as usize;
    }
    if new_count != input.num_elements() {
        return Err(EngineError::ShapeMismatch);
    }
    Ok(Tensor::new(input.data, new_shape))
}

// =============================================================================
// Relu — element-wise max(0, x) with SIMD
// =============================================================================

/// Relu: out\[i\] = max(0, input\[i\])
///
/// NEON: vmaxq_f32 (4-wide).
pub fn relu(input: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    let n = input.num_elements();
    if out.num_elements() != n {
        return Err(EngineError::ShapeMismatch);
    }
    unsafe {
        let inp = input.data;
        let outp = out.data_mut();

        #[cfg(target_arch = "aarch64")]
        {
            use core::arch::aarch64::*;
            let zero = vdupq_n_f32(0.0);
            let n4 = n & !3;
            let mut i = 0;
            while i < n4 {
                let v = vld1q_f32(inp.add(i));
                vst1q_f32(outp.add(i), vmaxq_f32(v, zero));
                i += 4;
            }
            while i < n {
                let v = *inp.add(i);
                *outp.add(i) = if v > 0.0 { v } else { 0.0 };
                i += 1;
            }
        }

        #[cfg(not(target_arch = "aarch64"))]
        {
            for i in 0..n {
                let v = *inp.add(i);
                *outp.add(i) = if v > 0.0 { v } else { 0.0 };
            }
        }
    }
    Ok(())
}

// =============================================================================
// Add — element-wise addition with broadcast and SIMD
// =============================================================================

/// Add: out = a + b, with broadcasting.
///
/// Supports:
/// - Same shape: element-wise (SIMD)
/// - Bias broadcast: a is \[M, N\], b is \[N\] → add b to each row of a
/// - Scalar broadcast: b is \[1\] → add b\[0\] to all elements (SIMD)
pub fn add(a: &Tensor, b: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    let a_n = a.num_elements();
    let b_n = b.num_elements();

    if out.num_elements() != a_n {
        return Err(EngineError::ShapeMismatch);
    }

    unsafe {
        let ap = a.data;
        let bp = b.data;
        let outp = out.data_mut();

        if a_n == b_n {
            // Same shape: element-wise SIMD
            add_elementwise_simd(ap, bp, outp, a_n);
        } else if b_n == 1 {
            // Scalar broadcast: SIMD
            let scalar = *bp;
            // Copy a to out first, then add scalar in-place
            if ap != outp as *const f32 {
                core::ptr::copy_nonoverlapping(ap, outp, a_n);
            }
            add_scalar_simd(outp, scalar, a_n);
        } else if a.ndim >= 2 && b_n == a.cols() as usize {
            // Bias broadcast: b[N] added to each row of a[M, N]
            let cols = a.cols() as usize;
            let rows = a_n / cols;
            for r in 0..rows {
                let row_offset = r * cols;
                add_elementwise_simd(ap.add(row_offset), bp, outp.add(row_offset), cols);
            }
        } else if b_n > 0 && a_n % b_n == 0 {
            // General broadcast: repeat b across a
            for i in 0..a_n {
                *outp.add(i) = *ap.add(i) + *bp.add(i % b_n);
            }
        } else {
            return Err(EngineError::ShapeMismatch);
        }
    }

    Ok(())
}

/// Element-wise add using SIMD.
///
/// # Safety
/// `ap`, `bp` readable and `outp` writable for `n` f32s.
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn add_elementwise_simd(ap: *const f32, bp: *const f32, outp: *mut f32, n: usize) {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let n4 = n & !3;
        let mut i = 0;
        while i < n4 {
            let va = vld1q_f32(ap.add(i));
            let vb = vld1q_f32(bp.add(i));
            vst1q_f32(outp.add(i), vaddq_f32(va, vb));
            i += 4;
        }
        while i < n {
            *outp.add(i) = *ap.add(i) + *bp.add(i);
            i += 1;
        }
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        for i in 0..n {
            *outp.add(i) = *ap.add(i) + *bp.add(i);
        }
    }
}

// =============================================================================
// MatMul — matrix multiplication with SIMD and cache tiling
// =============================================================================

/// MatMul: C[M,N] = A[M,K] × B[K,N]
///
/// Uses NEON float32x4_t for 4-wide SIMD, with cache-friendly
/// tiling for matrices larger than L1 cache.
pub fn matmul(a: &Tensor, b: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    let (m, k_a) = if a.ndim >= 2 {
        (a.shape[a.ndim as usize - 2] as usize, a.shape[a.ndim as usize - 1] as usize)
    } else if a.ndim == 1 {
        (1, a.shape[0] as usize)
    } else {
        return Err(EngineError::ShapeMismatch);
    };

    let (k_b, n) = if b.ndim >= 2 {
        (b.shape[b.ndim as usize - 2] as usize, b.shape[b.ndim as usize - 1] as usize)
    } else if b.ndim == 1 {
        (b.shape[0] as usize, 1)
    } else {
        return Err(EngineError::ShapeMismatch);
    };

    if k_a != k_b {
        return Err(EngineError::ShapeMismatch);
    }
    let k = k_a;

    if out.num_elements() != m * n {
        return Err(EngineError::ShapeMismatch);
    }

    unsafe {
        let cp = out.data_mut();
        zero_buf(cp, m * n);

        if a.is_int8() && b.is_int8() {
            // INT8 quantized matmul: accumulate in INT32, dequantize to FP32
            matmul_int8(a, b, cp, m, k, n);
        } else if b.is_fp16() && n <= FP16_ROW_BUF_SIZE {
            // FP16 weight matrix — lock protects static buffer from SMP races
            ops_lock();
            static mut FP16_BUF: [f32; FP16_ROW_BUF_SIZE] = [0.0; FP16_ROW_BUF_SIZE];
            let ap = a.data;
            let bp_u16 = b.data as *const u16;
            let buf = FP16_BUF.as_mut_ptr();
            for i in 0..m {
                for kk in 0..k {
                    let a_ik = *ap.add(i * k + kk);
                    let bp_row = fp16_row_to_f32(bp_u16.add(kk * n), n, buf);
                    simd_fma_row(cp.add(i * n), bp_row, a_ik, n);
                }
            }
            ops_unlock();
        } else {
            // Standard FP32 matmul
            let ap = a.data;
            let bp = b.data;
            matmul_inner(ap, bp, cp, m, k, n);
        }
    }

    Ok(())
}

/// Inner matmul dispatch — called from both matmul() and conv2d im2col.
/// C must be pre-zeroed.
unsafe fn matmul_inner(ap: *const f32, bp: *const f32, cp: *mut f32,
                       m: usize, k: usize, n: usize) {
    // Use tiled matmul for larger matrices (tile fits in L1 cache)
    // Tile size 32: 32*32*4 = 4KB per tile, 3 tiles = 12KB < 64KB L1D
    const TILE: usize = 32;
    if m > TILE || k > TILE || n > TILE {
        matmul_tiled(ap, bp, cp, m, k, n, TILE);
    } else {
        matmul_simd(ap, bp, cp, m, k, n);
    }
}

/// Cache-tiled matmul: splits M, K, N into tiles that fit in L1 cache.
unsafe fn matmul_tiled(ap: *const f32, bp: *const f32, cp: *mut f32,
                       m: usize, k: usize, n: usize, tile: usize) {
    // Tile over K (accumulation dimension) first for cache reuse of C
    let mut kk = 0;
    while kk < k {
        let k_end = core::cmp::min(kk + tile, k);
        let mut ii = 0;
        while ii < m {
            let i_end = core::cmp::min(ii + tile, m);
            let mut jj = 0;
            while jj < n {
                let j_end = core::cmp::min(jj + tile, n);
                // Micro-kernel: C[ii..i_end, jj..j_end] += A[ii..i_end, kk..k_end] * B[kk..k_end, jj..j_end]
                for i in ii..i_end {
                    for kki in kk..k_end {
                        let a_ik = *ap.add(i * k + kki);
                        let b_row = kki * n;
                        let c_row = i * n;
                        let tile_n = j_end - jj;
                        simd_fma_row(cp.add(c_row + jj), bp.add(b_row + jj), a_ik, tile_n);
                    }
                }
                jj += tile;
            }
            ii += tile;
        }
        kk += tile;
    }
}

/// Non-tiled SIMD matmul for small matrices.
unsafe fn matmul_simd(ap: *const f32, bp: *const f32, cp: *mut f32,
                      m: usize, k: usize, n: usize) {
    for i in 0..m {
        for kk in 0..k {
            let a_ik = *ap.add(i * k + kk);
            simd_fma_row(cp.add(i * n), bp.add(kk * n), a_ik, n);
        }
    }
}

/// C[0..n] += scalar * B[0..n] using SIMD.
/// Core inner loop for matmul — processes 4 elements at a time.
///
/// # Safety
/// `cp`, `bp` cover `n` f32s and are properly aligned for SIMD.
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn simd_fma_row(cp: *mut f32, bp: *const f32, scalar: f32, n: usize) {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let a_vec = vdupq_n_f32(scalar);
        let n4 = n & !3;
        let mut j = 0;
        while j < n4 {
            let b_val = vld1q_f32(bp.add(j));
            let c_val = vld1q_f32(cp.add(j));
            vst1q_f32(cp.add(j), vfmaq_f32(c_val, a_vec, b_val));
            j += 4;
        }
        while j < n {
            *cp.add(j) += scalar * *bp.add(j);
            j += 1;
        }
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        for j in 0..n {
            *cp.add(j) += scalar * *bp.add(j);
        }
    }
}

// =============================================================================
// Gemm — generalized matrix multiply (Y = alpha * A * B + beta * C)
// =============================================================================

/// Gemm: Y = A × B + C (simplified: alpha=1, beta=1, no transpose)
///
/// ONNX Gemm with default attributes. A[M,K] × B[K,N] + C[N] → Y[M,N]
pub fn gemm(a: &Tensor, b: &Tensor, c: Option<&Tensor>, out: &mut Tensor) -> Result<(), EngineError> {
    matmul(a, b, out)?;

    if let Some(bias) = c {
        let out_copy = *out;
        add(&out_copy, bias, out)?;
    }

    Ok(())
}

// =============================================================================
// Softmax — numerically stable softmax with SIMD
// =============================================================================

/// Softmax along last axis: out\[i\] = exp(x\[i\] - max) / sum(exp(x - max))
///
/// SIMD used for max-reduce and normalization multiply.
/// exp() uses libm::expf (no SIMD exp available in no_std).
pub fn softmax(input: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    let n = input.num_elements();
    if out.num_elements() != n {
        return Err(EngineError::ShapeMismatch);
    }

    let cols = if input.ndim >= 1 {
        input.shape[input.ndim as usize - 1] as usize
    } else {
        return Err(EngineError::ShapeMismatch);
    };

    if cols == 0 {
        return Err(EngineError::ShapeMismatch);
    }

    let rows = n / cols;

    unsafe {
        let inp = input.data;
        let outp = out.data_mut();

        for r in 0..rows {
            let base = r * cols;

            // Find max using SIMD reduce
            let max_val = simd_max_reduce(inp.add(base), cols);

            // Compute exp(x - max) and sum
            let mut sum = 0.0f32;
            for c in 0..cols {
                let v = libm::expf(*inp.add(base + c) - max_val);
                *outp.add(base + c) = v;
                sum += v;
            }

            // Normalize: multiply by 1/sum using SIMD
            if sum > 0.0 {
                let inv_sum = 1.0 / sum;
                simd_mul_scalar(outp.add(base), inv_sum, cols);
            }
        }
    }

    Ok(())
}

/// Find max value in a buffer using SIMD.
///
/// # Safety
/// `ptr..ptr+n` readable and aligned for f32 SIMD loads.
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn simd_max_reduce(ptr: *const f32, n: usize) -> f32 {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let mut max_vec = vdupq_n_f32(f32::NEG_INFINITY);
        let n4 = n & !3;
        let mut i = 0;
        while i < n4 {
            let v = vld1q_f32(ptr.add(i));
            max_vec = vmaxq_f32(max_vec, v);
            i += 4;
        }
        // Horizontal max of the 4 lanes
        let mut max_val = vmaxvq_f32(max_vec);
        while i < n {
            let v = *ptr.add(i);
            if v > max_val { max_val = v; }
            i += 1;
        }
        max_val
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        let mut max_val = *ptr;
        for i in 1..n {
            let v = *ptr.add(i);
            if v > max_val { max_val = v; }
        }
        max_val
    }
}

/// Multiply each element by a scalar using SIMD.
///
/// # Safety
/// `ptr..ptr+n` readable/writable and aligned for f32 SIMD access.
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn simd_mul_scalar(ptr: *mut f32, scalar: f32, n: usize) {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let sv = vdupq_n_f32(scalar);
        let n4 = n & !3;
        let mut i = 0;
        while i < n4 {
            let v = vld1q_f32(ptr.add(i));
            vst1q_f32(ptr.add(i), vmulq_f32(v, sv));
            i += 4;
        }
        while i < n {
            *ptr.add(i) *= scalar;
            i += 1;
        }
    }
    #[cfg(not(target_arch = "aarch64"))]
    {
        for i in 0..n {
            *ptr.add(i) *= scalar;
        }
    }
}
