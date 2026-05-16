//! Tensor operator implementations for CPU inference.
//!
//! All operators are pure functions that read input tensors and write to
//! pre-allocated output tensors. No dynamic allocation.
//!
//! ## SIMD platform dispatch
//!
//! Each SIMD helper uses a three-way `cfg` split:
//!
//! ```text
//! #[cfg(target_arch = "aarch64")]                                       // NEON path (live)
//! { use core::arch::aarch64::*; /* NEON intrinsics */ }
//! #[cfg(target_arch = "x86_64")]                                        // SSE slot
//! { /* TODO(x86-64 C1): SSE intrinsics; scalar placeholder for now. */ }
//! #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]     // Fallback
//! { /* scalar */ }
//! ```
//!
//! - **aarch64** runs NEON (Jetson capstone Track G fills in additional
//!   FP16/INT8 NEON paths in the same blocks).
//! - **x86_64** currently falls through to scalar; the x86-64 capstone
//!   plan's Phase C1 fills in SSE/SSE2 intrinsics in the marked
//!   `#[cfg(target_arch = "x86_64")]` blocks. Keeping the slot separate
//!   means C1 and Track G can edit the same file without merge conflicts.
//! - **Other arches** (RISC-V, host x86-64 unit-test binaries) get the
//!   scalar fallback for free.
//!
//! See `docs/jetson-capstone-execution-plan.md` "Shared Infrastructure
//! Prerequisites" for context on this dispatch scaffolding.

use core::sync::atomic::{AtomicBool, Ordering};
use super::tensor::Tensor;
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

/// RAII guard for OPS_LOCK. Use this anywhere a function holds the
/// lock across calls that could fail or panic — without RAII, the
/// lock would leak (panic = abort, but `?` early-returns or future
/// edits adding error paths still benefit). Currently used by the
/// im2col/matmul paths whose static scratch buffers it protects.
struct OpsGuard;

impl OpsGuard {
    fn new() -> Self {
        ops_lock();
        OpsGuard
    }
}

impl Drop for OpsGuard {
    fn drop(&mut self) {
        ops_unlock();
    }
}

// =============================================================================
// Software prefetch helper (#56 PR 3)
// =============================================================================

/// Prefetch one cache line into L1 for a future read (aarch64 `prfm
/// pldl1keep`). On other targets this is a no-op.
///
/// Used by `matmul_tiled` and the 4×4 micro-kernel to hide L1 miss
/// latency on the B-row stride. `prfm pldl1keep` is part of the ARMv8
/// mandatory base ISA and is a hint, not a fault-on-miss: an address
/// that's out of bounds, unmapped, or even invalid produces no fault,
/// no architectural state change, and (worst case) just wastes the
/// instruction slot.
///
/// `locality = "keep"` (PLDL1KEEP) tells the prefetcher the line
/// should be kept in L1 after first use; the alternative `pldl1strm`
/// hints that the line is streaming and can be evicted. Matmul
/// accumulators stay in L1 long enough that "keep" is the right
/// hint.
///
/// # Safety
///
/// `addr` is treated as a hint — the caller does not need to
/// guarantee the address is mapped or readable. The function is
/// `unsafe` only because it takes a raw pointer; calling it with a
/// non-canonical address on aarch64 is fine.
#[inline(always)]
#[cfg(target_arch = "aarch64")]
unsafe fn prefetch_l1_read(addr: *const i8) {
    core::arch::asm!(
        "prfm pldl1keep, [{0}]",
        in(reg) addr,
        options(nostack, readonly, preserves_flags),
    );
}

#[inline(always)]
#[cfg(not(target_arch = "aarch64"))]
unsafe fn prefetch_l1_read(_addr: *const i8) {
    // No portable prefetch in core for x86-64-unknown-none here; the
    // x86-64 C SSE kernels can issue PREFETCHT0 themselves once #847
    // wires them up. RISC-V and other archs: no-op.
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
            // i64 accumulator: a_val/b_val are in [-256, 255] after
            // zero-point subtraction, so a_val * b_val is in
            // [-65536, 65536]. With K = 32768 the running sum can
            // reach ~2^31 and overflow i32. i64 has headroom for
            // any realistic K value (would need ~2^47 K to overflow
            // i64, far past any plausible weight matrix). The cast
            // back to f32 truncates harmlessly.
            let mut acc: i64 = 0;
            for kk in 0..k {
                let a_val = (*ap.add(i * k + kk) as i32) - zp_a;
                let b_val = (*bp.add(kk * n + j) as i32) - zp_b;
                acc += (a_val * b_val) as i64;
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
    // Reject empty / null inputs before dereferencing. The original
    // form unconditionally read `*data` even for n == 0, which is
    // either a NULL deref (caller passes a null sentinel pointer)
    // or reads one byte past the buffer if `data` happens to point
    // at a real but length-0 buffer.
    if n == 0 || data.is_null() {
        return super::tensor::QuantParams { scale: 1.0, zero_point: 0 };
    }
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
///
/// Pre-2 dispatch skeleton: the three arms below are the shape every
/// hot SIMD kernel in this file follows. The `aarch64` arm holds NEON
/// code; the `x86_64` arm holds SSE-asm once C1 fills it in (today
/// it's scalar); the final arm is the generic scalar fallback for
/// any other target.
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
    #[cfg(target_arch = "x86_64")]
    {
        slm_sse_zero_f32(ptr, n);
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
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
    // x86-64 plan Phase C1 may re-enable this when SSE/AVX kernels land.
    const IM2COL_MAX: usize = 16384;
    #[cfg(target_arch = "aarch64")]
    let use_im2col = col_rows * col_cols <= IM2COL_MAX;
    #[cfg(target_arch = "x86_64")]
    let use_im2col = false;  // TODO(x86-64 C1): re-enable when SSE matmul lands.
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
    let use_im2col = false;

    unsafe {
        let inp = input.data;
        let wt = weight.data;
        let outp = out.data_mut();

        zero_buf(outp, out.num_elements());

        if use_im2col {
            // im2col + matmul path — RAII guard protects static buffer
            // from SMP races. If matmul_inner or add_scalar_simd ever
            // grow a panicking branch, the guard releases the lock on
            // unwind/return; the previous bare ops_lock()/ops_unlock()
            // pair would have leaked the lock.
            let _g = OpsGuard::new();
            static mut IM2COL_BUF: [f32; IM2COL_MAX] = [0.0; IM2COL_MAX];
            // SAFETY: OPS_LOCK held via OpsGuard — no concurrent access.
            // addr_of_mut! avoids materialising a `&mut [f32; N]` reference
            // (static_mut_refs UB pattern); the raw pointer is the only
            // route to the buffer from here on.
            let col = core::ptr::addr_of_mut!(IM2COL_BUF) as *mut f32;

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
            // _g (OpsGuard) drops here, releasing OPS_LOCK.
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
///
/// Dispatch: aarch64 NEON; x86_64 scalar placeholder (C1 fills in SSE);
/// other scalar fallback.
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
    #[cfg(target_arch = "x86_64")]
    {
        slm_sse_add_scalar_f32(ptr, scalar.to_bits(), n);
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
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
/// NEON: vmaxq_f32 (4-wide). x86-64 falls through to scalar
/// (see GitHub #72).
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

        #[cfg(target_arch = "x86_64")]
        {
            slm_sse_relu_f32(inp, outp, n);
        }
        #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
        {
            for i in 0..n {
                let v = *inp.add(i);
                *outp.add(i) = if v > 0.0 { v } else { 0.0 };
            }
        }
    }
    Ok(())
}

/* x86-64 SSE kernels are implemented in kernel/arch/x86_64/sse_kernels.c,
 * compiled with `-msse -msse2` per-file. This avoids the rustc
 * `x86_64-unknown-none` soft-float ABI limitations documented in #72
 * (rustc inline asm refuses xmm register allocation, and `__m128`
 * intrinsics trip LLVM's soft-float legalizer). CR4.OSFXSR +
 * CR4.OSXMMEXCPT + CR0.MP are set in trampoline32.S / ap_trampoline.S
 * (C2) so these execute at CPL=0.
 */
/* Scalar arguments are passed as `u32` (IEEE 754 bit pattern)
 * because the kernel is compiled `-mno-sse`; a `float` arg would
 * traverse the x87 FPU via the C caller, not xmm1 as the SSE callee
 * expects. Integer ABI is unambiguous regardless of -mno-sse. The
 * C side bit-casts back via a union. */
#[cfg(target_arch = "x86_64")]
extern "C" {
    fn slm_sse_relu_f32(inp: *const f32, outp: *mut f32, n: usize);
    fn slm_sse_zero_f32(ptr: *mut f32, n: usize);
    fn slm_sse_add_scalar_f32(ptr: *mut f32, scalar_bits: u32, n: usize);
    fn slm_sse_fma_row_f32(cp: *mut f32, bp: *const f32, scalar_bits: u32, n: usize);
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
///
/// Dispatch: aarch64 NEON; x86_64 scalar placeholder (C1 fills in SSE);
/// other scalar fallback.
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
    #[cfg(target_arch = "x86_64")]
    {
        // TODO(x86-64 C1): SSE addps.
        for i in 0..n {
            *outp.add(i) = *ap.add(i) + *bp.add(i);
        }
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
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
            // FP16 weight matrix — RAII guard protects FP16_BUF from
            // SMP races even if fp16_row_to_f32 / simd_fma_row ever
            // grow a panicking path.
            let _g = OpsGuard::new();
            static mut FP16_BUF: [f32; FP16_ROW_BUF_SIZE] = [0.0; FP16_ROW_BUF_SIZE];
            let ap = a.data;
            let bp_u16 = b.data as *const u16;
            // SAFETY: OPS_LOCK held via OpsGuard. addr_of_mut! avoids the
            // `&mut [f32; N]` reference that triggers static_mut_refs UB.
            let buf = core::ptr::addr_of_mut!(FP16_BUF) as *mut f32;
            for i in 0..m {
                for kk in 0..k {
                    let a_ik = *ap.add(i * k + kk);
                    let bp_row = fp16_row_to_f32(bp_u16.add(kk * n), n, buf);
                    simd_fma_row(cp.add(i * n), bp_row, a_ik, n);
                }
            }
            // _g drops here, releasing OPS_LOCK.
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
///
/// Per-tile inner micro-kernel dispatch (#56 PR 2):
///
/// - On aarch64, the 4-aligned interior of each tile is processed by a
///   register-blocked 4×4 outer-product kernel (`matmul_4x4_kernel_neon`).
///   That kernel keeps the 16-element C block resident in 4 NEON
///   registers across the full K sweep, issuing 4 FMAs per (B-load +
///   4 A-broadcasts) cycle. C is loaded once at block entry and stored
///   once at block exit, eliminating `kn − 1` memory round-trips per
///   4×4 block compared to the row×scalar form.
/// - The right strip (cols past the 4-aligned interior) and bottom
///   strip (rows past the 4-aligned interior) fall back to the
///   row×scalar `simd_fma_row` kernel — same path used pre-PR 2.
/// - On non-aarch64 targets the row×scalar form runs uniformly.
unsafe fn matmul_tiled(ap: *const f32, bp: *const f32, cp: *mut f32,
                       m: usize, k: usize, n: usize, tile: usize) {
    // Tile over K (accumulation dimension) first for cache reuse of C
    let mut kk = 0;
    while kk < k {
        let k_end = core::cmp::min(kk + tile, k);
        let kn = k_end - kk;
        let mut ii = 0;
        while ii < m {
            let i_end = core::cmp::min(ii + tile, m);
            let tile_m = i_end - ii;
            let mut jj = 0;
            while jj < n {
                let j_end = core::cmp::min(jj + tile, n);
                let tile_n = j_end - jj;

                // C[ii..i_end, jj..j_end] += A[ii..i_end, kk..k_end] * B[kk..k_end, jj..j_end]
                #[cfg(target_arch = "aarch64")]
                {
                    let i_blocks = tile_m & !3;
                    let j_blocks = tile_n & !3;

                    // 4×4-aligned interior — fastest path.
                    let mut ib = 0;
                    while ib < i_blocks {
                        // Prefetch the head of the next 4-row group's
                        // A rows (#56 PR 3). Each 4×4 kernel call
                        // sweeps `kn` elements of A starting at
                        // A[(ii+ib+next)*k + kk]; prefetching the
                        // first cache line of each of the 4 rows
                        // hides the cold-line miss when we move to
                        // the next `ib` group. The prefetched
                        // address may walk past the M dimension on
                        // the last iteration — `prfm` ignores that.
                        let next_ib = ib + 4;
                        if next_ib < i_blocks {
                            let a_next = ap.add((ii + next_ib) * k + kk);
                            prefetch_l1_read(a_next as *const i8);
                            prefetch_l1_read(a_next.add(k) as *const i8);
                            prefetch_l1_read(a_next.add(2 * k) as *const i8);
                            prefetch_l1_read(a_next.add(3 * k) as *const i8);
                        }

                        let mut jb = 0;
                        while jb < j_blocks {
                            matmul_4x4_kernel_neon(
                                ap.add((ii + ib) * k + kk),
                                bp.add(kk * n + jj + jb),
                                cp.add((ii + ib) * n + jj + jb),
                                kn, k, n,
                            );
                            jb += 4;
                        }
                        ib += 4;
                    }

                    // Right strip: 4-aligned rows, partial cols
                    // (tile_n - j_blocks ∈ {0,1,2,3}).
                    if j_blocks < tile_n {
                        for ir in 0..i_blocks {
                            for kki in 0..kn {
                                let a_ik = *ap.add((ii + ir) * k + kk + kki);
                                simd_fma_row(
                                    cp.add((ii + ir) * n + jj + j_blocks),
                                    bp.add((kk + kki) * n + jj + j_blocks),
                                    a_ik,
                                    tile_n - j_blocks,
                                );
                            }
                        }
                    }

                    // Bottom strip: partial rows
                    // (tile_m - i_blocks ∈ {0,1,2,3}), full tile_n.
                    for ir in i_blocks..tile_m {
                        for kki in 0..kn {
                            let a_ik = *ap.add((ii + ir) * k + kk + kki);
                            simd_fma_row(
                                cp.add((ii + ir) * n + jj),
                                bp.add((kk + kki) * n + jj),
                                a_ik,
                                tile_n,
                            );
                        }
                    }
                }
                #[cfg(not(target_arch = "aarch64"))]
                {
                    // Non-aarch64: keep the original row×scalar form so
                    // x86-64 + scalar-fallback paths are untouched by
                    // PR 2 (x86-64 work is tracked in #847).
                    for i in ii..i_end {
                        for kki in kk..k_end {
                            let a_ik = *ap.add(i * k + kki);
                            simd_fma_row(
                                cp.add(i * n + jj),
                                bp.add(kki * n + jj),
                                a_ik,
                                tile_n,
                            );
                        }
                    }
                }
                jj += tile;
            }
            ii += tile;
        }
        kk += tile;
    }
}

/// 4×4 NEON outer-product micro-kernel (#56 PR 2).
///
/// Computes the contraction step:
///   C[ii..ii+4, jj..jj+4] += A[ii..ii+4, kk..kk+kn] * B[kk..kk+kn, jj..jj+4]
///
/// Strategy: keep the 4×4 C block resident in `q0..q3` for the whole
/// K loop. Each iteration loads one B row (4 elements, one `vld1q_f32`),
/// broadcasts 4 scalars from A's 4 rows (`vdupq_n_f32` ×4), and issues
/// 4 `vfmaq_f32` accumulations — for 8 ops/cycle of arithmetic against
/// just one vector load. The row×scalar baseline issues 1 FMA, 1 B-load,
/// 1 C-load, and 1 C-store per cycle, so per (i, k) pair the new kernel
/// trades 1 B-load + 4 A-broadcasts + 4 FMAs (and no C traffic) for the
/// row-scalar's 4 B-loads + 4 C-loads + 4 C-stores + 4 FMAs across the
/// same 4 output rows.
///
/// AAPCS register usage at peak (inside the FMA chain): 4 NEON regs
/// for C (q0..q3) + 1 for the loaded B row + 4 for the broadcast A
/// scalars (`vdupq_n_f32` materializes a vector register, not a GP
/// register) = 9 simultaneously live NEON regs. The Cortex-A76 NEON
/// register file is 32 wide so there's ample headroom; a future
/// 4×8 or 8×4 expansion could keep more C state resident.
///
/// # Safety
///
/// - `ap_block` must be the address of A[ii*k + kk], with at least
///   `(3 * k_stride) + kn` f32 elements reachable from it.
/// - `bp_block` must be the address of B[kk*n + jj], with at least
///   `(kn - 1) * n_stride + 4` f32 elements reachable.
/// - `cp_block` must be the address of C[ii*n + jj], writable for at
///   least `3 * n_stride + 4` f32 elements; the kernel both reads and
///   writes the 4×4 C block.
/// - `kn` ≤ remaining K, `k_stride == k`, `n_stride == n`.
/// - Caller is on aarch64 with NEON enabled (mandated by ARMv8-A; the
///   `target_feature` attribute documents intent).
#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
unsafe fn matmul_4x4_kernel_neon(
    ap_block: *const f32,
    bp_block: *const f32,
    cp_block: *mut f32,
    kn: usize,
    k_stride: usize,
    n_stride: usize,
) {
    use core::arch::aarch64::*;

    let cp1 = cp_block.add(n_stride);
    let cp2 = cp_block.add(2 * n_stride);
    let cp3 = cp_block.add(3 * n_stride);

    // Load existing C — accumulating into pre-existing values is the
    // correct behaviour under K-tiling (later K tiles add to earlier
    // partial sums) and matches what `simd_fma_row` does.
    let mut c0 = vld1q_f32(cp_block);
    let mut c1 = vld1q_f32(cp1);
    let mut c2 = vld1q_f32(cp2);
    let mut c3 = vld1q_f32(cp3);

    let ap1 = ap_block.add(k_stride);
    let ap2 = ap_block.add(2 * k_stride);
    let ap3 = ap_block.add(3 * k_stride);

    for kki in 0..kn {
        // Note (#56 PR 3): an in-loop B-row prefetch was tried here
        // (`prfm pldl1keep` with PFDIST_K = 4 / 8 / 16 lookaheads) and
        // measured ~3% SLOWER on Pi 5 / Cortex-A76, MNIST 200-iter
        // bench: 483 µs → 500 µs avg. The address arithmetic
        // (`bp_block + (kki + PFDIST_K) * n_stride`) added a multiply
        // and a compare per K-step that the Cortex-A76 hardware
        // prefetcher already covers — the matmul tile working set
        // (12 KB) fits in 64 KB L1D with comfortable headroom. The
        // in-loop prefetch is intentionally NOT included. The
        // tile-level A-row prefetch in `matmul_tiled` runs only once
        // per 4-row group and remained neutral-to-positive, so it
        // was kept.
        let b_row = vld1q_f32(bp_block.add(kki * n_stride));

        let a0 = vdupq_n_f32(*ap_block.add(kki));
        let a1 = vdupq_n_f32(*ap1.add(kki));
        let a2 = vdupq_n_f32(*ap2.add(kki));
        let a3 = vdupq_n_f32(*ap3.add(kki));

        c0 = vfmaq_f32(c0, a0, b_row);
        c1 = vfmaq_f32(c1, a1, b_row);
        c2 = vfmaq_f32(c2, a2, b_row);
        c3 = vfmaq_f32(c3, a3, b_row);
    }

    vst1q_f32(cp_block, c0);
    vst1q_f32(cp1, c1);
    vst1q_f32(cp2, c2);
    vst1q_f32(cp3, c3);
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
///
/// Dispatch: aarch64 NEON FMA; x86_64 scalar placeholder (C1 fills in
/// SSE/SSE2 mulps+addps or AVX FMA); other scalar fallback.
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
    #[cfg(target_arch = "x86_64")]
    {
        slm_sse_fma_row_f32(cp, bp, scalar.to_bits(), n);
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
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
///
/// Dispatch: aarch64 NEON max + horizontal reduce; x86_64 scalar
/// placeholder (C1 fills in SSE max + horizontal reduce); other scalar.
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
    #[cfg(target_arch = "x86_64")]
    {
        // TODO(x86-64 C1): SSE maxps + horizontal reduce.
        let mut max_val = *ptr;
        for i in 1..n {
            let v = *ptr.add(i);
            if v > max_val { max_val = v; }
        }
        max_val
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
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
///
/// Dispatch: aarch64 NEON; x86_64 scalar placeholder (C1 fills in SSE);
/// other scalar fallback.
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
    #[cfg(target_arch = "x86_64")]
    {
        // TODO(x86-64 C1): SSE mulps.
        for i in 0..n {
            *ptr.add(i) *= scalar;
        }
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
    {
        for i in 0..n {
            *ptr.add(i) *= scalar;
        }
    }
}

/// Sum of all elements and sum of squares, computed together in one
/// SIMD-accumulated pass. Returns `(sum, sum_sq)`.
///
/// Single pass so the input cache line is touched once per element,
/// not twice. LayerNorm needs both reductions and an naive
/// implementation would do two separate passes; doing them together
/// roughly halves the memory traffic on large vectors.
///
/// # Safety
/// `ptr..ptr+n` readable and properly aligned for f32 SIMD loads.
///
/// Dispatch: aarch64 NEON vaddvq_f32; x86_64 scalar placeholder (C1);
/// other scalar fallback.
#[cfg_attr(target_arch = "aarch64", target_feature(enable = "neon"))]
unsafe fn simd_sum_sumsq(ptr: *const f32, n: usize) -> (f32, f32) {
    #[cfg(target_arch = "aarch64")]
    {
        use core::arch::aarch64::*;
        let mut sum_vec = vdupq_n_f32(0.0);
        let mut sq_vec  = vdupq_n_f32(0.0);
        let n4 = n & !3;
        let mut i = 0;
        while i < n4 {
            let v = vld1q_f32(ptr.add(i));
            sum_vec = vaddq_f32(sum_vec, v);
            sq_vec  = vfmaq_f32(sq_vec, v, v);
            i += 4;
        }
        let mut sum = vaddvq_f32(sum_vec);
        let mut sq  = vaddvq_f32(sq_vec);
        while i < n {
            let v = *ptr.add(i);
            sum += v;
            sq  += v * v;
            i += 1;
        }
        (sum, sq)
    }
    #[cfg(target_arch = "x86_64")]
    {
        // TODO(x86-64 C1): SSE haddps + vfmadd.
        let mut sum = 0.0_f32;
        let mut sq  = 0.0_f32;
        for i in 0..n {
            let v = *ptr.add(i);
            sum += v;
            sq  += v * v;
        }
        (sum, sq)
    }
    #[cfg(not(any(target_arch = "aarch64", target_arch = "x86_64")))]
    {
        let mut sum = 0.0_f32;
        let mut sq  = 0.0_f32;
        for i in 0..n {
            let v = *ptr.add(i);
            sum += v;
            sq  += v * v;
        }
        (sum, sq)
    }
}

// =============================================================================
// LayerNorm — (x - mean) / sqrt(var + eps) * gamma + beta
// =============================================================================

/// LayerNorm over the last dimension of `input`.
///
/// input:  [*, D]
/// gamma:  [D]   — scale (per-feature)
/// beta:   [D] or None — bias (per-feature)
/// eps:    small constant added to variance for numerical stability
///
/// out[i, j] = (input[i, j] - mean_i) / sqrt(var_i + eps) * gamma[j]
///             (+ beta[j] if provided)
///
/// NEON-accelerated via `simd_sum_sumsq` for the per-row reductions
/// and scalar-per-element normalization for the output — the
/// gamma/beta multiply-add loop is already memory-bound so a dedicated
/// NEON fused-multiply helper would not help.
pub fn layer_norm(
    input: &Tensor,
    gamma: &Tensor,
    beta: Option<&Tensor>,
    out: &mut Tensor,
    eps: f32,
) -> Result<(), EngineError> {
    let total = input.num_elements();
    if out.num_elements() != total {
        return Err(EngineError::ShapeMismatch);
    }
    if input.ndim < 1 {
        return Err(EngineError::ShapeMismatch);
    }
    let d = input.shape[input.ndim as usize - 1] as usize;
    if d == 0 || total % d != 0 {
        return Err(EngineError::ShapeMismatch);
    }
    if gamma.num_elements() != d {
        return Err(EngineError::ShapeMismatch);
    }
    if let Some(b) = beta {
        if b.num_elements() != d {
            return Err(EngineError::ShapeMismatch);
        }
    }
    let rows = total / d;
    let d_inv = 1.0_f32 / d as f32;

    unsafe {
        let inp = input.data;
        let outp = out.data_mut();
        let gp = gamma.data;
        let bp = beta.map(|t| t.data);

        for r in 0..rows {
            let base = r * d;
            let (sum, sumsq) = simd_sum_sumsq(inp.add(base), d);
            let mean = sum * d_inv;
            // variance via E[x^2] - (E[x])^2
            let var = (sumsq * d_inv) - mean * mean;
            // guard against negative due to FP32 round-off on near-constant rows
            let var = if var > 0.0 { var } else { 0.0 };
            let inv_std = 1.0_f32 / super::mathf::sqrtf(var + eps);

            if let Some(bp) = bp {
                for j in 0..d {
                    let x = *inp.add(base + j);
                    *outp.add(base + j) =
                        (x - mean) * inv_std * *gp.add(j) + *bp.add(j);
                }
            } else {
                for j in 0..d {
                    let x = *inp.add(base + j);
                    *outp.add(base + j) = (x - mean) * inv_std * *gp.add(j);
                }
            }
        }
    }
    Ok(())
}

// =============================================================================
// RMSNorm — x / sqrt(mean(x²) + eps) * gamma  (no centering)
// =============================================================================

/// Root-mean-square layer normalization (Llama-style).
///
/// out[i, j] = input[i, j] / sqrt(mean_of_squares_i + eps) * gamma[j]
///
/// Simpler than LayerNorm — no mean subtraction, one reduction.
pub fn rms_norm(
    input: &Tensor,
    gamma: &Tensor,
    out: &mut Tensor,
    eps: f32,
) -> Result<(), EngineError> {
    let total = input.num_elements();
    if out.num_elements() != total {
        return Err(EngineError::ShapeMismatch);
    }
    if input.ndim < 1 {
        return Err(EngineError::ShapeMismatch);
    }
    let d = input.shape[input.ndim as usize - 1] as usize;
    if d == 0 || total % d != 0 {
        return Err(EngineError::ShapeMismatch);
    }
    if gamma.num_elements() != d {
        return Err(EngineError::ShapeMismatch);
    }
    let rows = total / d;
    let d_inv = 1.0_f32 / d as f32;

    unsafe {
        let inp = input.data;
        let outp = out.data_mut();
        let gp = gamma.data;

        for r in 0..rows {
            let base = r * d;
            let (_sum, sumsq) = simd_sum_sumsq(inp.add(base), d);
            let mean_sq = sumsq * d_inv;
            let inv_rms = 1.0_f32 / super::mathf::sqrtf(mean_sq + eps);

            for j in 0..d {
                *outp.add(base + j) = *inp.add(base + j) * inv_rms * *gp.add(j);
            }
        }
    }
    Ok(())
}

// =============================================================================
// GELU — Gaussian Error Linear Unit
// =============================================================================

/// Element-wise GELU activation, tanh approximation:
///   y = 0.5 · x · (1 + tanh(√(2/π) · (x + 0.044715 · x³)))
///
/// This is the "approximate" GELU used by GPT-2/3 and most transformer
/// implementations — fast and within ~1e-4 of the exact erf-based GELU.
pub fn gelu(input: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    let n = input.num_elements();
    if out.num_elements() != n {
        return Err(EngineError::ShapeMismatch);
    }
    // √(2/π)  ≈ 0.7978845608028654
    const K: f32 = 0.7978845608028654;
    const C: f32 = 0.044715;

    unsafe {
        let inp = input.data;
        let outp = out.data_mut();
        for i in 0..n {
            let x = *inp.add(i);
            let x3 = x * x * x;
            let t = K * (x + C * x3);
            // tanh via our scalar Padé(7,7) approximation in
            // `mathf` — replaces libm::tanhf, which would otherwise
            // pull libm's f16 paths into the build and trip the
            // x86_64-unknown-none soften-operand crash (#141).
            // Padé(7,7) is accurate to ~1e-6 inside [-5, 5] and
            // saturates at ±1 outside; tighter than the GELU test's
            // 1e-5 tolerance and faster than the library call.
            *outp.add(i) = 0.5 * x * (1.0 + super::mathf::tanhf(t));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// PR-465 regression: `quantize_fp32_to_int8` must short-circuit
    /// on `n == 0` instead of dereferencing `*data`. The previous
    /// form read `*data` unconditionally, which was UB for n=0.
    #[test]
    fn quantize_fp32_to_int8_rejects_zero_n() {
        // Pass null and zero — must not dereference.
        let qp = quantize_fp32_to_int8(core::ptr::null(), 0, core::ptr::null_mut());
        assert_eq!(qp.scale, 1.0);
        assert_eq!(qp.zero_point, 0);
    }

    /// And on null data even with non-zero n.
    #[test]
    fn quantize_fp32_to_int8_rejects_null_data() {
        let qp = quantize_fp32_to_int8(core::ptr::null(), 16, core::ptr::null_mut());
        assert_eq!(qp.scale, 1.0);
        assert_eq!(qp.zero_point, 0);
    }
}
