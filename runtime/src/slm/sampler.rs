//! Next-token sampler for the SLM decoder.
//!
//! M5.1 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/specs/slm-integration.md`). The decoder produces a vocab-sized
//! logits vector at the end of every forward pass; this module turns
//! those logits into a token id according to the configured strategy.
//!
//! Per the spec (M5-D2):
//!
//! - the **deterministic test default** is [`Sampler::Greedy`]
//!   (argmax — used by the `llama.cpp -temp 0` parity check);
//! - the **demo default** is [`Sampler::TopKTopP`] with
//!   `temperature = 0.7`, `top_k = 40`, `top_p = 0.9`.
//!
//! The PRNG is a tiny inline xorshift64 — no `rand` / `getrandom`
//! dependency, so the entire pipeline stays seeded and bit-reproducible
//! across `make test` and Pi 5 / Jetson runs.
//!
//! `no_std` + `alloc`.

#![allow(clippy::module_name_repetitions)]

extern crate alloc;

use alloc::vec::Vec;

/// Sampling strategy for next-token selection.
///
/// `temperature`, `k`, and `p` are caller-supplied hyperparameters; this
/// module makes no assumptions about their range beyond what each
/// variant's `sample` implementation documents.
#[derive(Debug, Clone, Copy)]
pub enum Sampler {
    /// `argmax(logits)`. Deterministic; ties broken by lowest index.
    Greedy,
    /// Scale logits by `1/T`, sample from the resulting softmax.
    /// `temperature ≤ 0` collapses to argmax (greedy).
    Temperature { temperature: f32 },
    /// Restrict to the `k` largest logits, then [`Sampler::Temperature`].
    /// `k == 0` collapses to argmax.
    TopK { temperature: f32, k: usize },
    /// Restrict to the smallest prefix of sorted logits whose softmax
    /// mass is `≥ p`, then [`Sampler::Temperature`]. `p ≤ 0` collapses
    /// to argmax; `p ≥ 1` keeps the full distribution.
    TopP { temperature: f32, p: f32 },
    /// Top-K filter, then top-P filter, then [`Sampler::Temperature`].
    /// The demo default per spec.
    TopKTopP { temperature: f32, k: usize, p: f32 },
}

/// Per-session sampler state — a single 64-bit PRNG seed.
///
/// The state is updated on every call to [`SamplerState::sample`], so
/// the same `(seed, sampler, logits-stream)` always produces the same
/// token sequence.
pub struct SamplerState {
    pub seed: u64,
}

impl SamplerState {
    /// Create state seeded with `seed`. Seed `0` is silently bumped to
    /// `1` because xorshift64 has a fixed point at zero.
    pub fn new(seed: u64) -> Self {
        Self {
            seed: if seed == 0 { 1 } else { seed },
        }
    }

    /// Sample one token id from `logits`, returning the chosen index.
    ///
    /// `logits` is mutated in place — top-k / top-p variants overwrite
    /// the masked-out entries with [`f32::NEG_INFINITY`]. The buffer is
    /// not safe to reuse without a fresh forward pass.
    ///
    /// An empty `logits` slice returns `0` (the only safe fallback in a
    /// `no_std` non-fallible signature). The decoder is expected to
    /// validate the vocab size before sampling, so this branch should
    /// never fire in practice.
    pub fn sample(&mut self, sampler: &Sampler, logits: &mut [f32]) -> u32 {
        if logits.is_empty() {
            return 0;
        }
        match *sampler {
            Sampler::Greedy => argmax(logits) as u32,
            Sampler::Temperature { temperature } => {
                if temperature <= 0.0 {
                    return argmax(logits) as u32;
                }
                scale_by_inv_temperature(logits, temperature);
                softmax_sample(logits, &mut self.seed) as u32
            }
            Sampler::TopK { temperature, k } => {
                if k == 0 {
                    return argmax(logits) as u32;
                }
                top_k_filter(logits, k);
                if temperature <= 0.0 {
                    return argmax(logits) as u32;
                }
                scale_by_inv_temperature(logits, temperature);
                softmax_sample(logits, &mut self.seed) as u32
            }
            Sampler::TopP { temperature, p } => {
                if p <= 0.0 {
                    return argmax(logits) as u32;
                }
                top_p_filter(logits, p);
                if temperature <= 0.0 {
                    return argmax(logits) as u32;
                }
                scale_by_inv_temperature(logits, temperature);
                softmax_sample(logits, &mut self.seed) as u32
            }
            Sampler::TopKTopP { temperature, k, p } => {
                if k == 0 || p <= 0.0 {
                    return argmax(logits) as u32;
                }
                top_k_filter(logits, k);
                top_p_filter(logits, p);
                if temperature <= 0.0 {
                    return argmax(logits) as u32;
                }
                scale_by_inv_temperature(logits, temperature);
                softmax_sample(logits, &mut self.seed) as u32
            }
        }
    }
}

// ---------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------

/// xorshift64 — one of Marsaglia's three-shift triples (13, 7, 17).
///
/// Period 2^64 - 1; **must not be seeded with 0**. `SamplerState::new`
/// already enforces that invariant; helpers below assume the caller
/// passes a non-zero state.
fn xorshift64(state: &mut u64) -> u64 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    x
}

/// 24 bits of entropy in `[0, 1)`.
fn rand_f32(state: &mut u64) -> f32 {
    let bits = (xorshift64(state) >> 40) as u32;
    (bits as f32) / ((1u32 << 24) as f32)
}

/// argmax with low-index tie-breaking.
fn argmax(logits: &[f32]) -> usize {
    let mut best_idx = 0usize;
    let mut best_val = logits[0];
    // Iterate explicitly so NaN-aware comparison stays predictable.
    for (i, &x) in logits.iter().enumerate().skip(1) {
        if x > best_val {
            best_val = x;
            best_idx = i;
        }
    }
    best_idx
}

/// Multiply each logit by `1/temperature`. Caller has already verified
/// `temperature > 0.0`.
fn scale_by_inv_temperature(logits: &mut [f32], temperature: f32) {
    let inv_t = 1.0 / temperature;
    for x in logits.iter_mut() {
        *x *= inv_t;
    }
}

/// Numerically stable softmax sample: subtract max, exp, normalize, then
/// inverse-CDF sample. Returns the sampled index. Assumes `logits` is
/// non-empty (caller-checked).
fn softmax_sample(logits: &mut [f32], rng: &mut u64) -> usize {
    // 1) max for stability.
    let mut max_logit = f32::NEG_INFINITY;
    for &x in logits.iter() {
        if x > max_logit {
            max_logit = x;
        }
    }
    if !max_logit.is_finite() {
        // Every entry was masked to NEG_INFINITY (e.g. degenerate top-k=0
        // path that slipped past the guard). Fall back to argmax of the
        // raw values — `argmax` will pick index 0, which is the
        // documented degenerate behavior.
        return argmax(logits);
    }

    // 2) exp(logit - max), sum.
    let mut denom: f32 = 0.0;
    for x in logits.iter_mut() {
        let e = libm::expf(*x - max_logit);
        *x = e;
        denom += e;
    }
    if denom <= 0.0 || !denom.is_finite() {
        return argmax(logits);
    }

    // 3) inverse-CDF sample.
    let r = rand_f32(rng) * denom;
    let mut acc: f32 = 0.0;
    for (i, &p) in logits.iter().enumerate() {
        acc += p;
        if r < acc {
            return i;
        }
    }
    // Floating-point drift fallback: last index.
    logits.len() - 1
}

/// Mask all but the `k` largest logits with `NEG_INFINITY`.
///
/// `k` is clamped to `logits.len()`. `k == 0` is a no-op (caller is
/// expected to special-case it as "argmax").
fn top_k_filter(logits: &mut [f32], k: usize) {
    let n = logits.len();
    if k == 0 || k >= n {
        return;
    }

    // Find the k-th largest value: collect indices, partial sort by
    // logit descending. For typical SLM vocab sizes (tens of thousands)
    // a full sort is acceptable here; the heavy lifting in decode is the
    // matmul, not the sampler. M6 can revisit if profiling demands it.
    let mut idx: Vec<usize> = (0..n).collect();
    idx.sort_unstable_by(|&a, &b| {
        // Reverse order — largest first. NaNs sort to the end so they
        // get masked.
        match logits[b].partial_cmp(&logits[a]) {
            Some(ord) => ord,
            None => core::cmp::Ordering::Equal,
        }
    });

    // Keep idx[0..k]; mask the rest.
    let mut keep = alloc::vec![false; n];
    for &i in idx.iter().take(k) {
        keep[i] = true;
    }
    for (i, x) in logits.iter_mut().enumerate() {
        if !keep[i] {
            *x = f32::NEG_INFINITY;
        }
    }
}

/// Mask all logits outside the smallest cumulative-probability prefix
/// `≥ p` (after softmax of the live entries) with `NEG_INFINITY`.
///
/// `p ≥ 1.0` is a no-op (every token is kept). Tokens that were already
/// `NEG_INFINITY` (e.g. from a prior top-k filter) stay masked.
fn top_p_filter(logits: &mut [f32], p: f32) {
    let n = logits.len();
    if p >= 1.0 || n == 0 {
        return;
    }

    // 1) Stable softmax over the live entries (NEG_INFINITY ones
    //    contribute 0 to the partition).
    let mut max_logit = f32::NEG_INFINITY;
    for &x in logits.iter() {
        if x > max_logit {
            max_logit = x;
        }
    }
    if !max_logit.is_finite() {
        return;
    }

    let mut probs: Vec<f32> = Vec::with_capacity(n);
    let mut denom: f32 = 0.0;
    for &x in logits.iter() {
        if x == f32::NEG_INFINITY {
            probs.push(0.0);
        } else {
            let e = libm::expf(x - max_logit);
            probs.push(e);
            denom += e;
        }
    }
    if denom <= 0.0 || !denom.is_finite() {
        return;
    }
    for q in probs.iter_mut() {
        *q /= denom;
    }

    // 2) Sort indices by probability descending.
    let mut idx: Vec<usize> = (0..n).collect();
    idx.sort_unstable_by(|&a, &b| {
        match probs[b].partial_cmp(&probs[a]) {
            Some(ord) => ord,
            None => core::cmp::Ordering::Equal,
        }
    });

    // 3) Keep tokens until cumulative mass ≥ p.
    let mut keep = alloc::vec![false; n];
    let mut cum: f32 = 0.0;
    for &i in idx.iter() {
        keep[i] = true;
        cum += probs[i];
        if cum >= p {
            break;
        }
    }

    for (i, x) in logits.iter_mut().enumerate() {
        if !keep[i] {
            *x = f32::NEG_INFINITY;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn greedy_picks_argmax() {
        let mut state = SamplerState::new(0xCAFE);
        let mut logits = vec![0.1_f32, 0.5, 0.9, 0.3];
        assert_eq!(state.sample(&Sampler::Greedy, &mut logits), 2);
    }

    #[test]
    fn greedy_breaks_ties_low_index() {
        let mut state = SamplerState::new(0xCAFE);
        let mut logits = vec![1.0_f32, 1.0, 1.0, 1.0];
        assert_eq!(state.sample(&Sampler::Greedy, &mut logits), 0);
    }

    #[test]
    fn temperature_zero_acts_like_greedy() {
        let mut state = SamplerState::new(0xCAFE);
        let mut logits = vec![0.1_f32, 0.5, 0.9, 0.3];
        assert_eq!(
            state.sample(&Sampler::Temperature { temperature: 1e-6 }, &mut logits),
            // 1e-6 is positive but tiny; with such a high inv-T the
            // softmax becomes effectively a delta on argmax. Sampling
            // once must still return the argmax with overwhelming
            // probability — and even if it did not, the documented
            // collapse path runs when temperature ≤ 0. So go through
            // the strict-zero alias to keep the test deterministic.
            2
        );
        let mut state = SamplerState::new(0xCAFE);
        let mut logits = vec![0.1_f32, 0.5, 0.9, 0.3];
        assert_eq!(
            state.sample(&Sampler::Temperature { temperature: 0.0 }, &mut logits),
            2,
        );
    }

    #[test]
    fn temperature_high_smooths_distribution() {
        // High T → near-uniform → all 4 tokens should appear over many
        // samples.
        let mut state = SamplerState::new(0xDEAD_BEEF);
        let mut seen = [false; 4];
        for _ in 0..1000 {
            let mut logits = vec![0.1_f32, 0.5, 0.9, 0.3];
            let id = state.sample(&Sampler::Temperature { temperature: 100.0 }, &mut logits);
            seen[id as usize] = true;
        }
        assert!(seen.iter().all(|&s| s));
    }

    #[test]
    fn top_k_eliminates_low_probability_tokens() {
        // Logits [10, 9, 1, 0]; top_k=2 ⇒ only ids 0/1 ever sampled.
        let mut state = SamplerState::new(0xABCD_1234);
        for _ in 0..200 {
            let mut logits = vec![10.0_f32, 9.0, 1.0, 0.0];
            let id = state.sample(
                &Sampler::TopK { temperature: 1.0, k: 2 },
                &mut logits,
            );
            assert!(id == 0 || id == 1, "unexpected id {id}");
        }
    }

    #[test]
    fn top_p_eliminates_low_probability_tokens() {
        // Logits [10, 9, 1, 0]; the top-2 captures > 0.5 of the mass,
        // so p=0.5 should keep ids 0+1 only.
        let mut state = SamplerState::new(0xABCD_1234);
        for _ in 0..200 {
            let mut logits = vec![10.0_f32, 9.0, 1.0, 0.0];
            let id = state.sample(
                &Sampler::TopP { temperature: 1.0, p: 0.5 },
                &mut logits,
            );
            assert!(id == 0 || id == 1, "unexpected id {id}");
        }
    }

    #[test]
    fn top_k_top_p_combo_runs_and_stays_in_set() {
        // Verifies the full demo-default pipeline executes and stays
        // within the top-k window.
        let mut state = SamplerState::new(7);
        for _ in 0..200 {
            let mut logits = vec![10.0_f32, 9.0, 1.0, 0.0, -2.0];
            let id = state.sample(
                &Sampler::TopKTopP {
                    temperature: 0.7,
                    k: 2,
                    p: 0.9,
                },
                &mut logits,
            );
            assert!(id == 0 || id == 1, "unexpected id {id}");
        }
    }

    #[test]
    fn xorshift64_deterministic() {
        let mut a = 0x1234_5678_DEAD_BEEFu64;
        let mut b = 0x1234_5678_DEAD_BEEFu64;
        for _ in 0..32 {
            assert_eq!(xorshift64(&mut a), xorshift64(&mut b));
        }
        // Different seed, different stream.
        let mut c = 0x1234_5678_DEAD_BEF0u64;
        let mut diverged = false;
        for _ in 0..32 {
            if xorshift64(&mut a) != xorshift64(&mut c) {
                diverged = true;
                break;
            }
        }
        assert!(diverged);
    }

    #[test]
    fn sampler_state_seed_zero_normalized() {
        // Zero seed must be replaced — xorshift64 has 0 as a fixed
        // point.
        let s = SamplerState::new(0);
        assert_eq!(s.seed, 1);
    }

    #[test]
    fn empty_logits_returns_zero() {
        let mut state = SamplerState::new(1);
        let mut empty: [f32; 0] = [];
        assert_eq!(state.sample(&Sampler::Greedy, &mut empty), 0);
    }
}
