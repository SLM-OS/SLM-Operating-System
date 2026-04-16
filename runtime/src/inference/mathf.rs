//! Minimal scalar math helpers — replaces the subset of `libm` that
//! triggers the rustc-LLVM f16 soften-operand crash on
//! `x86_64-unknown-none` (issue #141).
//!
//! libm 0.2.13+ added f16 / f128 code paths that LLVM's x86_64 backend
//! cannot lower. Even though our G3 kernels never touch f16, fat LTO
//! drags those paths into the slm-runtime build and codegen aborts.
//! Calling `libm::sqrtf` / `libm::tanhf` reliably triggers it;
//! `libm::roundf` / `libm::expf` do not, so they remain on libm.
//!
//! Accuracy targets match what the existing G3 kernels compare
//! against in `lib.rs`:
//!   - `sqrtf`: ≤ 1e-6 relative error, sufficient for variance/std
//!     calculations whose downstream tolerance is 1e-4.
//!   - `tanhf`: Padé(7,7) is ≤ 1e-6 absolute error in [-3, 3] and
//!     grows to ~1.5e-5 at ±4; we saturate at ±4 since the Padé
//!     returns values > 1.0 past that. GELU's inputs land well
//!     inside [-3, 3] in practice (tanh arg = 0.798·(x + 0.045·x³),
//!     so x = ±2 → arg ≈ ±1.9), so the approximation stays tight
//!     for the actual caller even at the tail-saturation points.

/// Single-precision square root via bit-magic init + 4 Newton iterations.
///
/// Pure scalar arithmetic — no library calls. Compiles to ~12
/// instructions on aarch64 and ~14 on x86-64. Returns 0 for x ≤ 0
/// (the GSP code uses sqrt for variance regularization where any
/// negative argument is already a numerical error).
///
/// Iteration count was 3 originally (tight enough for the LayerNorm
/// / RMSNorm caller whose variance + eps stays > 1e-5). #177 bumped
/// it to 4 so the relative error stays ≤ 1e-6 across the full FP32
/// normalized range — one extra fused-multiply-add + divide, under
/// a nanosecond on any of our targets, in exchange for tighter
/// accuracy on any future caller outside the LayerNorm sweet spot.
///
/// The bit-magic initializer is only valid for normalized inputs
/// (`x >= 1.175e-38`). Subnormal `x` would yield a garbage initial
/// guess whose exponent bits are zero. Subnormal inputs are not in
/// scope for the G3 callers today; the test suite pins the tight
/// bound only across the normalized range.
#[inline]
pub fn sqrtf(x: f32) -> f32 {
    if x <= 0.0 {
        return 0.0;
    }
    // Initial guess from float bit pattern. The magic constant
    // 0x1FBD3F7D ≈ (2^23 * (3 * 127 - 1)) / 2 puts y within a
    // factor of ~1.5 of the true sqrt for any normalized x —
    // close enough that 4 Newton iterations land at full FP32
    // precision across the whole normalized range. Reference:
    // Lomont 2003 §3.
    let mut y = f32::from_bits(0x1FBD_3F7Du32.wrapping_add(x.to_bits() >> 1));
    // Newton: y_{n+1} = 0.5 * (y_n + x/y_n).
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y = 0.5 * (y + x / y);
    y
}

/// Hyperbolic tangent via a Padé(7,7) rational approximation.
///
/// Saturates at ±1 past x = ±4 because the Padé numerator
/// (degree 7 in x) outgrows the denominator past that point and
/// the rational returns values > 1.0 — mathematically impossible
/// for tanh (|tanh(x)| < 1 for all x ∈ ℝ). The ±4 boundary is
/// chosen to be as wide as the rational stays well-behaved:
/// ±3 would clamp earlier and throw away accuracy the Padé can
/// still deliver (~1e-5 up to ±4); ±6 would return > 1.0 values.
///
/// Exact form:
///   tanh(x) ≈ x · (135135 + x²·(17325 + x²·(378 + x²)))
///            ÷ (135135 + x²·(62370 + x²·(3150 + 28·x²)))
///
/// Inside [-3, 3] the approximation is < 1e-6 off; the GELU caller
/// hands us arguments well inside that range in practice.
/// No branches in the hot path beyond the saturation guard.
#[inline]
pub fn tanhf(x: f32) -> f32 {
    if x > 4.0 {
        return 1.0;
    }
    if x < -4.0 {
        return -1.0;
    }
    let x2 = x * x;
    let num = x * (135135.0 + x2 * (17325.0 + x2 * (378.0 + x2)));
    let den = 135135.0 + x2 * (62370.0 + x2 * (3150.0 + 28.0 * x2));
    num / den
}

#[cfg(test)]
mod tests {
    use super::*;

    fn approx(a: f32, b: f32, tol: f32) -> bool {
        (a - b).abs() <= tol
    }

    #[test]
    fn sqrtf_known_values() {
        assert!(approx(sqrtf(4.0), 2.0, 1.0e-6));
        assert!(approx(sqrtf(2.0), 1.4142135, 1.0e-6));
        assert!(approx(sqrtf(1.0e-5), 0.003162277, 1.0e-7));
        assert_eq!(sqrtf(0.0), 0.0);
        assert_eq!(sqrtf(-1.0), 0.0);
    }

    /// #177: full FP32 normalized-range coverage. Relative error must
    /// stay ≤ 1e-6 regardless of which call site consumes the helper.
    /// Subnormal inputs (`x < 1.175e-38`) are out of scope — the
    /// bit-magic init's exponent trick assumes normalized inputs, and
    /// no current caller hits that regime.
    #[test]
    fn sqrtf_wide_range_relative_error() {
        let samples: &[(f32, f32)] = &[
            (1.175e-38, 1.0843433e-19), // smallest normal FP32
            (1.0e-30,   1.0e-15),
            (1.0e-10,   1.0e-5),
            (1.0,       1.0),
            (123.456,   11.1110755),
            (1.0e10,    1.0e5),
            (1.0e20,    1.0e10),
            (1.0e30,    1.0e15),
            (1.0e37,    3.1622776e18),
        ];
        for &(x, expected) in samples {
            let got = sqrtf(x);
            let rel = ((got - expected) / expected).abs();
            assert!(rel <= 1.0e-6,
                "sqrtf({}): got {}, expected {}, rel err {}",
                x, got, expected, rel);
        }
    }

    #[test]
    fn tanhf_known_values() {
        assert!(approx(tanhf(0.0), 0.0, 1.0e-7));
        assert!(approx(tanhf(1.0), 0.7615942, 1.0e-6));
        assert!(approx(tanhf(-2.0), -0.9640276, 1.0e-6));
        assert_eq!(tanhf(10.0), 1.0);
        assert_eq!(tanhf(-10.0), -1.0);
        assert_eq!(tanhf(4.0001), 1.0);
        assert_eq!(tanhf(-4.0001), -1.0);
    }
}
