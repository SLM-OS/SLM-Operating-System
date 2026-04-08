//! Tensor operator implementations for CPU inference.
//!
//! All operators are pure functions that read input tensors and write to
//! pre-allocated output tensors. No dynamic allocation.

use super::tensor::Tensor;
use super::engine::EngineError;

// =============================================================================
// Conv2D — 2D convolution (NCHW layout)
// =============================================================================

/// Conv2D: out = conv(input, weight) + bias
///
/// MNIST uses: input [N,C_in,H,W], weight [C_out,C_in,kH,kW], bias [C_out]
/// Default attributes: strides=[1,1], pads=[0,0,0,0], dilations=[1,1], group=1
/// For MNIST: kernel_shape=[5,5], auto_pad not set
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

    unsafe {
        let inp = input.data;
        let wt = weight.data;
        let outp = out.data_mut();

        // Zero output
        for i in 0..out.num_elements() {
            *outp.add(i) = 0.0;
        }

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
                                    // Handle padding
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
                        // Add bias
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

    Ok(())
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
// Relu — element-wise max(0, x)
// =============================================================================

/// Relu: out[i] = max(0, input[i])
///
/// Can operate in-place if input.data == out.data.
pub fn relu(input: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    let n = input.num_elements();
    if out.num_elements() != n {
        return Err(EngineError::ShapeMismatch);
    }
    unsafe {
        let inp = input.data;
        let outp = out.data_mut();
        for i in 0..n {
            let v = *inp.add(i);
            *outp.add(i) = if v > 0.0 { v } else { 0.0 };
        }
    }
    Ok(())
}

// =============================================================================
// Add — element-wise addition with broadcast
// =============================================================================

/// Add: out = a + b, with broadcasting.
///
/// Supports:
/// - Same shape: element-wise
/// - Bias broadcast: a is [M, N], b is [N] → add b to each row of a
/// - Scalar broadcast: b is [1] → add b[0] to all elements
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
            // Same shape: element-wise
            for i in 0..a_n {
                *outp.add(i) = *ap.add(i) + *bp.add(i);
            }
        } else if b_n == 1 {
            // Scalar broadcast
            let scalar = *bp;
            for i in 0..a_n {
                *outp.add(i) = *ap.add(i) + scalar;
            }
        } else if a.ndim >= 2 && b_n == a.cols() as usize {
            // Bias broadcast: b[N] added to each row of a[M, N]
            let cols = a.cols() as usize;
            let rows = a_n / cols;
            for r in 0..rows {
                for c in 0..cols {
                    let idx = r * cols + c;
                    *outp.add(idx) = *ap.add(idx) + *bp.add(c);
                }
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

// =============================================================================
// MatMul — matrix multiplication
// =============================================================================

/// MatMul: C[M,N] = A[M,K] × B[K,N]
///
/// Row-major layout. Inner loop structured for auto-vectorization.
pub fn matmul(a: &Tensor, b: &Tensor, out: &mut Tensor) -> Result<(), EngineError> {
    // Get dimensions — handle both 2D and batched cases
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
        let ap = a.data;
        let bp = b.data;
        let cp = out.data_mut();

        // Zero output
        for i in 0..m * n {
            *cp.add(i) = 0.0;
        }

        // C[i,j] += A[i,k] * B[k,j]
        // Loop order: i, k, j — row-major friendly for A access,
        // and the inner j loop enables auto-vectorization
        for i in 0..m {
            for kk in 0..k {
                let a_ik = *ap.add(i * k + kk);
                for j in 0..n {
                    *cp.add(i * n + j) += a_ik * *bp.add(kk * n + j);
                }
            }
        }
    }

    Ok(())
}

// =============================================================================
// Gemm — generalized matrix multiply (Y = alpha * A * B + beta * C)
// =============================================================================

/// Gemm: Y = A × B + C (simplified: alpha=1, beta=1, no transpose)
///
/// ONNX Gemm with default attributes. A[M,K] × B[K,N] + C[N] → Y[M,N]
pub fn gemm(a: &Tensor, b: &Tensor, c: Option<&Tensor>, out: &mut Tensor) -> Result<(), EngineError> {
    // First do MatMul
    matmul(a, b, out)?;

    // Then add bias if present
    if let Some(bias) = c {
        // Add in-place: out = out + bias
        let out_copy = *out;
        add(&out_copy, bias, out)?;
    }

    Ok(())
}

// =============================================================================
// Softmax — numerically stable softmax
// =============================================================================

/// Softmax along last axis: out[i] = exp(x[i] - max) / sum(exp(x - max))
///
/// Uses libm::expf for no_std compatibility.
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

            // Find max for numerical stability
            let mut max_val = *inp.add(base);
            for c in 1..cols {
                let v = *inp.add(base + c);
                if v > max_val {
                    max_val = v;
                }
            }

            // Compute exp(x - max) and sum
            let mut sum = 0.0f32;
            for c in 0..cols {
                let v = libm::expf(*inp.add(base + c) - max_val);
                *outp.add(base + c) = v;
                sum += v;
            }

            // Normalize
            if sum > 0.0 {
                let inv_sum = 1.0 / sum;
                for c in 0..cols {
                    *outp.add(base + c) *= inv_sum;
                }
            }
        }
    }

    Ok(())
}
