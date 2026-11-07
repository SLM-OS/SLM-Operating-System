//! Next-token sampler for the SLM decoder.
//!
//! M5.1 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/design/slm-integration.md`). The decoder produces a vocab-sized
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
//! Per-token cost (#522): the filters work out of a [`SamplerWorkspace`]
//! owned by [`SamplerState`], so a session allocates on its first
//! filtered token and never again. Selection replaces the full sort
//! whenever the kept set is unambiguous. A tie across the keep boundary
//! makes the kept set depend on `sort_unstable_by`'s permutation, so
//! that case still runs the same sort, on workspace memory, and the
//! token stream for a given seed is unchanged. Selection cannot help
//! such a token and would be paid for on top of the sort, so a filter
//! whose last cut was tied goes straight to the sort.
//!
//! `no_std` + `alloc`.

#![allow(clippy::module_name_repetitions)]

extern crate alloc;

use alloc::vec::Vec;
use core::cmp::Ordering;

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

/// Scratch buffers for the top-k / top-p filters, sized to the vocabulary
/// on the first filtered token and reused for the rest of the session
/// (`16 * vocab` bytes: about 2.4 MB at vocab 151,936).
///
/// All three buffers are reserved together, even though `idx` is only
/// written when a tie forces the exact sort: a tie late in a generation
/// must not be the thing that allocates.
#[derive(Default)]
pub struct SamplerWorkspace {
    /// `exp(logit - max)` per token, from the top-p pass.
    exps: Vec<f32>,
    /// Values being selected over.
    scratch: Vec<f32>,
    /// Token indices for the exact tie-break sort. `usize`, not `u32`:
    /// the sort's permutation on ties may depend on the element type.
    idx: Vec<usize>,
    /// The last top-k cut was tied, so try the exact sort first. Only
    /// ever chooses between two routes to the same result.
    k_cut_tied: bool,
    /// The same for the top-p cut.
    p_cut_tied: bool,
    /// Number of exact sorts run. Lets tests pin the fast path.
    #[cfg(test)]
    exact_sorts: usize,
}

impl SamplerWorkspace {
    /// Grow the buffers to cover a vocabulary of `n`. No-op once sized.
    fn reserve_for(&mut self, n: usize) {
        if self.idx.len() < n {
            self.exps.resize(n, 0.0);
            self.scratch.resize(n, 0.0);
            self.idx.resize(n, 0);
        }
    }

    /// Record an exact sort and return `idx[..n]` filled with `0..n`.
    fn fresh_idx(&mut self, n: usize) -> &mut [usize] {
        #[cfg(test)]
        {
            self.exact_sorts += 1;
        }
        let idx = &mut self.idx[..n];
        for (i, slot) in idx.iter_mut().enumerate() {
            *slot = i;
        }
        idx
    }
}

/// Per-session sampler state: the 64-bit PRNG seed and the filter
/// workspace.
///
/// The seed is updated on every call to [`SamplerState::sample`], so
/// the same `(seed, sampler, logits-stream)` always produces the same
/// token sequence. Between tokens the workspace carries only the
/// tied-cut hints, which affect speed and never the result.
pub struct SamplerState {
    pub seed: u64,
    workspace: SamplerWorkspace,
}

impl SamplerState {
    /// Create state seeded with `seed`. Seed `0` is silently bumped to
    /// `1` because xorshift64 has a fixed point at zero.
    pub fn new(seed: u64) -> Self {
        Self {
            seed: if seed == 0 { 1 } else { seed },
            workspace: SamplerWorkspace::default(),
        }
    }

    /// Sample one token id from `logits`, returning the chosen index.
    ///
    /// `logits` is mutated in place — top-k / top-p variants overwrite
    /// the masked-out entries with [`f32::NEG_INFINITY`]. The buffer is
    /// not safe to reuse without a fresh forward pass.
    ///
    /// Allocates only on the first call that reaches a top-k or top-p
    /// filter at a given vocabulary size.
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
            Sampler::Temperature { temperature } => self.finish(logits, temperature, false),
            Sampler::TopK { temperature, k } => {
                if k == 0 {
                    return argmax(logits) as u32;
                }
                self.workspace.reserve_for(logits.len());
                top_k_filter(logits, k, &mut self.workspace);
                self.finish(logits, temperature, false)
            }
            Sampler::TopP { temperature, p } => {
                if p <= 0.0 {
                    return argmax(logits) as u32;
                }
                self.workspace.reserve_for(logits.len());
                let exps_ready = top_p_filter(logits, p, &mut self.workspace);
                self.finish(logits, temperature, exps_ready)
            }
            Sampler::TopKTopP { temperature, k, p } => {
                if k == 0 || p <= 0.0 {
                    return argmax(logits) as u32;
                }
                self.workspace.reserve_for(logits.len());
                top_k_filter(logits, k, &mut self.workspace);
                let exps_ready = top_p_filter(logits, p, &mut self.workspace);
                self.finish(logits, temperature, exps_ready)
            }
        }
    }

    /// Temperature step and the draw, shared by the sampling variants.
    ///
    /// `exps_ready` says the workspace holds `exp(logit - max)` from
    /// [`top_p_filter`]. At `temperature == 1.0` scaling is the identity
    /// and those are exactly the values the softmax needs, so the two
    /// passes fold into one. Any other temperature needs
    /// `exp((logit - max) / T)`, which no cheap transform of the stored
    /// values reproduces bit for bit, so survivors are exponentiated
    /// again; [`softmax_sample`] skips the masked entries.
    fn finish(&mut self, logits: &mut [f32], temperature: f32, exps_ready: bool) -> u32 {
        if temperature <= 0.0 {
            return argmax(logits) as u32;
        }
        if exps_ready && temperature == 1.0 {
            return sample_from_exps(logits, &self.workspace.exps, &mut self.seed) as u32;
        }
        scale_by_inv_temperature(logits, temperature);
        softmax_sample(logits, &mut self.seed) as u32
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

    // 2) exp(logit - max), sum. Masked entries are the bulk of the
    //    vector after top-k / top-p; exp(-inf) is +0, so skip the call.
    let mut denom: f32 = 0.0;
    for x in logits.iter_mut() {
        if *x == f32::NEG_INFINITY {
            *x = 0.0;
            continue;
        }
        let e = libm::expf(*x - max_logit);
        *x = e;
        denom += e;
    }
    if denom <= 0.0 || !denom.is_finite() {
        return argmax(logits);
    }

    inverse_cdf(logits, denom, rng)
}

/// [`softmax_sample`] for `temperature == 1.0` after a top-p pass:
/// `exps` already holds `exp(logit - max)`, and the top-p mask never
/// removes the maximum, so these are the values the softmax would
/// compute. Masked entries contribute `+0`.
fn sample_from_exps(logits: &mut [f32], exps: &[f32], rng: &mut u64) -> usize {
    let mut denom: f32 = 0.0;
    for (x, &e) in logits.iter_mut().zip(exps.iter()) {
        let v = if *x == f32::NEG_INFINITY { 0.0 } else { e };
        *x = v;
        denom += v;
    }
    if denom <= 0.0 || !denom.is_finite() {
        return argmax(logits);
    }
    inverse_cdf(logits, denom, rng)
}

/// Inverse-CDF draw over unnormalized `weights`, whose sum in index
/// order is `denom`.
fn inverse_cdf(weights: &[f32], denom: f32, rng: &mut u64) -> usize {
    let r = rand_f32(rng) * denom;
    let mut acc: f32 = 0.0;
    for (i, &p) in weights.iter().enumerate() {
        acc += p;
        if r < acc {
            return i;
        }
    }
    // Floating-point drift fallback: last index.
    weights.len() - 1
}

/// Descending order for values already known not to be NaN.
fn desc(a: &f32, b: &f32) -> Ordering {
    b.partial_cmp(a).unwrap_or(Ordering::Equal)
}

/// Mask all but the `k` largest logits with `NEG_INFINITY`.
///
/// `k` is clamped to `logits.len()`. `k == 0` is a no-op (caller is
/// expected to special-case it as "argmax").
///
/// Selection finds the k-th largest value in O(n). If that value also
/// appears past position `k`, or a NaN is present, which tokens survive
/// depends on the sort's permutation, so the exact sort decides. Tied
/// logits tend to stay tied, so after a tied cut selection is skipped
/// until a sort finds the cut clean. The first tied token pays for both.
fn top_k_filter(logits: &mut [f32], k: usize, ws: &mut SamplerWorkspace) {
    let n = logits.len();
    if k == 0 || k >= n {
        return;
    }

    if !ws.k_cut_tied {
        let scratch = &mut ws.scratch[..n];
        let mut has_nan = false;
        for (s, &x) in scratch.iter_mut().zip(logits.iter()) {
            *s = x;
            has_nan |= x.is_nan();
        }
        if !has_nan {
            let (_, &mut kth, rest) = scratch.select_nth_unstable_by(k - 1, desc);
            if !rest.iter().any(|&x| x == kth) {
                for x in logits.iter_mut() {
                    if *x < kth {
                        *x = f32::NEG_INFINITY;
                    }
                }
                return;
            }
        }
    }

    // Ambiguous: the kept set is whatever this sort leaves in idx[..k].
    let idx = ws.fresh_idx(n);
    idx.sort_unstable_by(|&a, &b| {
        // Reverse order — largest first. NaNs compare Equal.
        match logits[b].partial_cmp(&logits[a]) {
            Some(ord) => ord,
            None => Ordering::Equal,
        }
    });
    let tied = logits[idx[k - 1]] == logits[idx[k]];
    for &i in idx[k..].iter() {
        logits[i] = f32::NEG_INFINITY;
    }
    ws.k_cut_tied = tied;
}

/// Values selected in the first round of [`mass_prefix`]; each later
/// round doubles. Peaked distributions usually finish in the first.
const TOP_P_FIRST_CHUNK: usize = 64;

/// Walk `probs` in descending order until the running sum reaches `p`.
/// Returns the prefix length and the last value in it, or `None` if the
/// sum never reaches `p`. Orders only as much of `probs` as the walk
/// needs.
///
/// The sum sees the same values in the same order as a full sort (ties
/// are equal values), so its rounding is unchanged. `probs` must hold no
/// NaN.
fn mass_prefix(probs: &mut [f32], p: f32) -> Option<(usize, f32)> {
    let len = probs.len();
    let mut cum: f32 = 0.0;
    let mut lo = 0usize;
    let mut chunk = TOP_P_FIRST_CHUNK;
    while lo < len {
        let hi = lo.saturating_add(chunk).min(len);
        if hi < len {
            probs[lo..].select_nth_unstable_by(hi - lo - 1, desc);
        }
        let window = &mut probs[lo..hi];
        window.sort_unstable_by(desc);
        for (off, &q) in window.iter().enumerate() {
            cum += q;
            if cum >= p {
                return Some((lo + off + 1, q));
            }
        }
        lo = hi;
        chunk = chunk.saturating_mul(2);
    }
    None
}

/// Mask all logits outside the smallest cumulative-probability prefix
/// `≥ p` (after softmax of the live entries) with `NEG_INFINITY`.
///
/// `p ≥ 1.0` is a no-op (every token is kept). Tokens that were already
/// `NEG_INFINITY` (e.g. from a prior top-k filter) stay masked.
///
/// Returns `true` when `ws.exps` holds `exp(logit - max)` for every live
/// token and no logit is NaN or infinite, so [`sample_from_exps`] may use
/// it. `false` means nothing was masked and the softmax must start from
/// the logits.
fn top_p_filter(logits: &mut [f32], p: f32, ws: &mut SamplerWorkspace) -> bool {
    let n = logits.len();
    if p >= 1.0 || n == 0 {
        return false;
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
        return false;
    }

    let mut denom: f32 = 0.0;
    for (e, &x) in ws.exps[..n].iter_mut().zip(logits.iter()) {
        if x == f32::NEG_INFINITY {
            *e = 0.0;
        } else {
            *e = libm::expf(x - max_logit);
            denom += *e;
        }
    }
    if denom <= 0.0 || !denom.is_finite() {
        return false;
    }

    // 2) Only positive probabilities can move the running sum, so only
    //    they need ordering. After top-k that is a few dozen values.
    //    Skipped when the last cut was tied: see `top_k_filter`.
    if !ws.p_cut_tied {
        let mut live = 0usize;
        for &e in ws.exps[..n].iter() {
            let q = e / denom;
            if q > 0.0 {
                ws.scratch[live] = q;
                live += 1;
            }
        }

        // 3) Keep tokens until cumulative mass ≥ p. Never reaching it
        //    keeps every token.
        let Some((m, last)) = mass_prefix(&mut ws.scratch[..live], p) else {
            return true;
        };

        if !ws.scratch[m..live].iter().any(|&q| q == last) {
            for (x, &e) in logits.iter_mut().zip(ws.exps.iter()) {
                if e / denom < last {
                    *x = f32::NEG_INFINITY;
                }
            }
            return true;
        }
    }

    // Ambiguous: the same value on both sides of the cut, so the kept
    // set is whatever this sort leaves in idx[..m].
    for (q, &e) in ws.scratch[..n].iter_mut().zip(ws.exps.iter()) {
        *q = e / denom;
    }
    ws.fresh_idx(n);
    let probs = &ws.scratch[..n];
    let idx = &mut ws.idx[..n];
    idx.sort_unstable_by(|&a, &b| {
        match probs[b].partial_cmp(&probs[a]) {
            Some(ord) => ord,
            None => Ordering::Equal,
        }
    });
    let mut cum: f32 = 0.0;
    let mut kept = n;
    for (pos, &i) in idx.iter().enumerate() {
        cum += probs[i];
        if cum >= p {
            kept = pos + 1;
            break;
        }
    }
    let tied = kept < n && probs[idx[kept - 1]] == probs[idx[kept]];
    for &i in idx[kept..].iter() {
        logits[i] = f32::NEG_INFINITY;
    }
    ws.p_cut_tied = tied;
    true
}

#[cfg(test)]
pub(crate) mod tests {
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

    // -----------------------------------------------------------------
    // #522: identical results, no per-token allocation
    // -----------------------------------------------------------------

    /// The implementation as it stood before #522, kept as the oracle.
    mod legacy {
        use super::super::{argmax, rand_f32, scale_by_inv_temperature, Sampler};
        use alloc::vec::Vec;

        pub fn sample(seed: &mut u64, sampler: &Sampler, logits: &mut [f32]) -> u32 {
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
                    softmax_sample(logits, seed) as u32
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
                    softmax_sample(logits, seed) as u32
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
                    softmax_sample(logits, seed) as u32
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
                    softmax_sample(logits, seed) as u32
                }
            }
        }

        fn softmax_sample(logits: &mut [f32], rng: &mut u64) -> usize {
            let mut max_logit = f32::NEG_INFINITY;
            for &x in logits.iter() {
                if x > max_logit {
                    max_logit = x;
                }
            }
            if !max_logit.is_finite() {
                return argmax(logits);
            }
            let mut denom: f32 = 0.0;
            for x in logits.iter_mut() {
                let e = libm::expf(*x - max_logit);
                *x = e;
                denom += e;
            }
            if denom <= 0.0 || !denom.is_finite() {
                return argmax(logits);
            }
            let r = rand_f32(rng) * denom;
            let mut acc: f32 = 0.0;
            for (i, &p) in logits.iter().enumerate() {
                acc += p;
                if r < acc {
                    return i;
                }
            }
            logits.len() - 1
        }

        fn top_k_filter(logits: &mut [f32], k: usize) {
            let n = logits.len();
            if k == 0 || k >= n {
                return;
            }
            let mut idx: Vec<usize> = (0..n).collect();
            idx.sort_unstable_by(|&a, &b| match logits[b].partial_cmp(&logits[a]) {
                Some(ord) => ord,
                None => core::cmp::Ordering::Equal,
            });
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

        fn top_p_filter(logits: &mut [f32], p: f32) {
            let n = logits.len();
            if p >= 1.0 || n == 0 {
                return;
            }
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
            let mut idx: Vec<usize> = (0..n).collect();
            idx.sort_unstable_by(|&a, &b| match probs[b].partial_cmp(&probs[a]) {
                Some(ord) => ord,
                None => core::cmp::Ordering::Equal,
            });
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
    }

    const QWEN_VOCAB: usize = 151_936;

    #[derive(Clone, Copy, Debug)]
    enum Shape {
        /// Distinct values: the case real logits are in.
        Continuous,
        /// One token far above a low plateau of distinct values.
        Peaked,
        /// Near-uniform, so top-p keeps most of the vocabulary.
        NearFlat,
        /// Snapped to a 1/8 grid: ties on both sides of every cut.
        Quantized,
        /// Every value equal: one big tie.
        AllEqual,
        /// A sprinkling of NaN.
        WithNan,
        /// A +inf entry.
        WithInf,
        /// Some values already -inf, as after an earlier mask.
        WithNegInf,
    }

    const ORDERED_SHAPES: [Shape; 6] = [
        Shape::Continuous,
        Shape::Peaked,
        Shape::NearFlat,
        Shape::Quantized,
        Shape::AllEqual,
        Shape::WithNegInf,
    ];

    /// Reproducible logits in about `[-10, 10]`.
    fn make_logits(shape: Shape, n: usize, seed: u64) -> Vec<f32> {
        let mut rng = seed | 1;
        let mut v: Vec<f32> = (0..n)
            .map(|_| {
                let u = rand_f32(&mut rng) + rand_f32(&mut rng) + rand_f32(&mut rng);
                (u - 1.5) * 6.67
            })
            .collect();
        match shape {
            Shape::Continuous => {}
            Shape::Peaked => {
                for x in v.iter_mut() {
                    *x -= 40.0;
                }
                let spike = (xorshift64(&mut rng) as usize) % n;
                v[spike] = 12.0;
            }
            Shape::NearFlat => {
                for x in v.iter_mut() {
                    *x *= 0.001;
                }
            }
            Shape::Quantized => {
                for x in v.iter_mut() {
                    *x = libm::roundf(*x * 8.0) / 8.0;
                }
            }
            Shape::AllEqual => {
                for x in v.iter_mut() {
                    *x = 1.5;
                }
            }
            Shape::WithNan => {
                for _ in 0..(n / 64 + 1) {
                    let at = (xorshift64(&mut rng) as usize) % n;
                    v[at] = f32::NAN;
                }
            }
            Shape::WithInf => {
                let at = (xorshift64(&mut rng) as usize) % n;
                v[at] = f32::INFINITY;
            }
            Shape::WithNegInf => {
                for _ in 0..(n / 2 + 1) {
                    let at = (xorshift64(&mut rng) as usize) % n;
                    v[at] = f32::NEG_INFINITY;
                }
            }
        }
        v
    }

    fn samplers(k: usize) -> Vec<Sampler> {
        vec![
            Sampler::Greedy,
            Sampler::Temperature { temperature: 0.7 },
            Sampler::TopK { temperature: 0.7, k },
            Sampler::TopK { temperature: 1.0, k },
            Sampler::TopK { temperature: 0.0, k },
            Sampler::TopP { temperature: 0.7, p: 0.9 },
            Sampler::TopP { temperature: 1.0, p: 0.9 },
            Sampler::TopP { temperature: 0.0, p: 0.9 },
            Sampler::TopP { temperature: 1.0, p: 1.0 },
            Sampler::TopP { temperature: 1.0, p: 1e-6 },
            Sampler::TopP { temperature: 1.0, p: f32::NAN },
            Sampler::TopKTopP { temperature: 0.7, k, p: 0.9 },
            Sampler::TopKTopP { temperature: 1.0, k, p: 0.9 },
            Sampler::TopKTopP { temperature: 1.0, k, p: 0.5 },
            Sampler::TopKTopP { temperature: f32::INFINITY, k, p: 0.9 },
        ]
    }

    /// The filtered paths, for vocabulary-sized runs. The legacy oracle
    /// takes around 100 ms per token unoptimized, and the small-vector
    /// test covers the full list.
    fn hot_samplers() -> Vec<Sampler> {
        vec![
            Sampler::TopK { temperature: 0.7, k: 40 },
            Sampler::TopP { temperature: 0.7, p: 0.9 },
            Sampler::TopP { temperature: 1.0, p: 0.9 },
            Sampler::TopKTopP { temperature: 0.7, k: 40, p: 0.9 },
            Sampler::TopKTopP { temperature: 1.0, k: 40, p: 0.9 },
        ]
    }

    /// Token, PRNG state and final buffer must match bit for bit.
    fn assert_same(shape: Shape, sampler: &Sampler, seed: u64, base: &[f32], tokens: usize) {
        let mut old_seed = if seed == 0 { 1 } else { seed };
        let mut state = SamplerState::new(seed);
        let mut old_buf = base.to_vec();
        let mut new_buf = base.to_vec();
        for t in 0..tokens {
            old_buf.copy_from_slice(base);
            new_buf.copy_from_slice(base);
            let old = legacy::sample(&mut old_seed, sampler, &mut old_buf);
            let new = state.sample(sampler, &mut new_buf);
            assert_eq!(
                old, new,
                "token {t} diverged: {shape:?} {sampler:?} n={} seed={seed:#x}",
                base.len()
            );
            assert_eq!(old_seed, state.seed, "PRNG diverged: {shape:?} {sampler:?}");
            let same = old_buf
                .iter()
                .zip(new_buf.iter())
                .all(|(a, b)| a.to_bits() == b.to_bits() || (a.is_nan() && b.is_nan()));
            assert!(same, "buffer diverged: {shape:?} {sampler:?} seed={seed:#x}");
        }
    }

    #[test]
    fn matches_legacy_on_vocabulary_sized_logits() {
        for seed in [1u64, 0xCAFE, 0xDEAD_BEEF, 0x1234_5678_9ABC_DEF0] {
            for shape in ORDERED_SHAPES {
                let base = make_logits(shape, QWEN_VOCAB, seed);
                for sampler in hot_samplers() {
                    assert_same(shape, &sampler, seed, &base, 1);
                }
            }
        }
    }

    #[test]
    fn matches_legacy_over_a_token_stream() {
        // Same state across tokens, as in a generation.
        let base = make_logits(Shape::Continuous, QWEN_VOCAB, 0xF00D);
        assert_same(
            Shape::Continuous,
            &Sampler::TopKTopP { temperature: 0.7, k: 40, p: 0.9 },
            0xF00D,
            &base,
            16,
        );
    }

    #[test]
    fn matches_legacy_across_sizes_shapes_and_cuts() {
        // Small vectors reach every cut position, tie pattern and
        // chunk boundary in mass_prefix (64, 192).
        let sizes = [1usize, 2, 3, 5, 8, 20, 21, 63, 64, 65, 192, 193, 500];
        let shapes = [
            Shape::Continuous,
            Shape::Peaked,
            Shape::NearFlat,
            Shape::Quantized,
            Shape::AllEqual,
            Shape::WithNegInf,
            Shape::WithInf,
        ];
        for &n in sizes.iter() {
            for &shape in shapes.iter() {
                for seed in 1..=8u64 {
                    let base = make_logits(shape, n, seed.wrapping_mul(0x9E37_79B9));
                    for k in [1, 2, n / 2 + 1, n.saturating_sub(1).max(1), n, n + 1] {
                        for sampler in samplers(k) {
                            assert_same(shape, &sampler, seed, &base, 2);
                        }
                    }
                }
            }
        }
    }

    /// NaN makes the legacy comparator non-total, and the standard sort
    /// may panic on that. The new code must do whatever the old one did.
    #[test]
    fn matches_legacy_with_nan_logits() {
        let hook = std::panic::take_hook();
        std::panic::set_hook(alloc::boxed::Box::new(|_| {}));
        for &n in [5usize, 20, 21, 64, 500, 5000].iter() {
            for seed in 1..=8u64 {
                let base = make_logits(Shape::WithNan, n, seed.wrapping_mul(0x9E37_79B9));
                for sampler in samplers(n / 2 + 1) {
                    let run = |f: &dyn Fn(&mut [f32], &mut u64) -> u32| {
                        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                            let mut buf = base.clone();
                            let mut s = seed;
                            let tok = f(&mut buf, &mut s);
                            let bits: Vec<u32> = buf
                                .iter()
                                .map(|x| if x.is_nan() { u32::MAX } else { x.to_bits() })
                                .collect();
                            (tok, s, bits)
                        }))
                        .ok()
                    };
                    let old = run(&|b, s| legacy::sample(s, &sampler, b));
                    let new = run(&|b, s| {
                        let mut st = SamplerState::new(*s);
                        let tok = st.sample(&sampler, b);
                        *s = st.seed;
                        tok
                    });
                    assert!(old == new, "NaN case diverged: n={n} {sampler:?} seed={seed}");
                }
            }
        }
        std::panic::set_hook(hook);
    }

    #[test]
    fn distinct_logits_never_take_the_exact_sort() {
        let base = make_logits(Shape::Continuous, QWEN_VOCAB, 0xBEEF);
        for sampler in hot_samplers() {
            let mut state = SamplerState::new(0xBEEF);
            let mut buf = base.clone();
            state.sample(&sampler, &mut buf);
            assert_eq!(state.workspace.exact_sorts, 0, "full sort on {sampler:?}");
        }
    }

    #[test]
    fn a_tie_across_the_cut_takes_the_exact_sort() {
        // Without this the equivalence tests could pass with the fallback
        // dead, or the fast path could be swallowing ties.
        let mut state = SamplerState::new(9);
        let mut logits = vec![3.0_f32, 2.0, 2.0, 1.0];
        state.sample(&Sampler::TopK { temperature: 1.0, k: 2 }, &mut logits);
        assert_eq!(state.workspace.exact_sorts, 1);

        let mut state = SamplerState::new(9);
        let mut logits = vec![1.0_f32; 8];
        state.sample(&Sampler::TopP { temperature: 1.0, p: 0.5 }, &mut logits);
        assert_eq!(state.workspace.exact_sorts, 1);
    }

    #[test]
    fn tied_cut_hint_skips_selection_and_clears_on_a_clean_cut() {
        for sampler in [
            Sampler::TopK { temperature: 0.7, k: 2 },
            Sampler::TopP { temperature: 0.7, p: 0.5 },
        ] {
            let mut state = SamplerState::new(9);
            let is_top_k = matches!(sampler, Sampler::TopK { .. });
            let hint = |st: &SamplerState| {
                if is_top_k { st.workspace.k_cut_tied } else { st.workspace.p_cut_tied }
            };
            let run = |st: &mut SamplerState, src: &[f32]| {
                let mut buf = src.to_vec();
                st.sample(&sampler, &mut buf);
            };
            let tied = [1.0_f32; 8];
            let clean = [8.0_f32, 7.0, 6.0, 5.0, 4.0, 3.0, 2.0, 1.0];

            run(&mut state, &clean);
            assert_eq!(state.workspace.exact_sorts, 0, "{sampler:?}");
            assert!(!hint(&state), "{sampler:?}");

            run(&mut state, &tied);
            assert_eq!(state.workspace.exact_sorts, 1, "{sampler:?}");
            assert!(hint(&state), "{sampler:?}: tie not remembered");

            // The hint sends a clean cut to the sort once, which clears it.
            run(&mut state, &clean);
            assert_eq!(state.workspace.exact_sorts, 2, "{sampler:?}");
            assert!(!hint(&state), "{sampler:?}: hint stuck on");

            run(&mut state, &clean);
            assert_eq!(state.workspace.exact_sorts, 2, "{sampler:?}: fast path lost");
        }
    }

    #[test]
    fn matches_legacy_while_the_hint_changes_route() {
        // A stream alternating tied and clean logits crosses every hint
        // transition; each token must still match the oracle.
        for n in [8usize, 65, 500, 5000] {
            for sampler in samplers(n / 2 + 1) {
                let streams = [
                    make_logits(Shape::Quantized, n, 0x11),
                    make_logits(Shape::Continuous, n, 0x22),
                    make_logits(Shape::AllEqual, n, 0x33),
                    make_logits(Shape::Peaked, n, 0x44),
                    make_logits(Shape::WithNan, n, 0x55),
                    make_logits(Shape::Quantized, n, 0x66),
                    make_logits(Shape::Continuous, n, 0x77),
                ];
                let mut old_seed = 0xABCDu64;
                let mut state = SamplerState::new(0xABCD);
                for base in streams.iter() {
                    let (mut a, mut b) = (base.clone(), base.clone());
                    // The legacy comparator can panic on NaN at this size;
                    // the small sizes above cover that outcome.
                    if base.iter().any(|x| x.is_nan()) && n > 20 {
                        continue;
                    }
                    let old = legacy::sample(&mut old_seed, &sampler, &mut a);
                    let new = state.sample(&sampler, &mut b);
                    assert_eq!(old, new, "n={n} {sampler:?}");
                    assert_eq!(old_seed, state.seed);
                    let same = a
                        .iter()
                        .zip(b.iter())
                        .all(|(x, y)| x.to_bits() == y.to_bits() || (x.is_nan() && y.is_nan()));
                    assert!(same, "buffer diverged: n={n} {sampler:?}");
                }
            }
        }
    }

    #[test]
    fn mass_prefix_crosses_chunks() {
        let n = 1000usize;
        let mut probs: Vec<f32> = (0..n).map(|i| ((i * 37) % n) as f32 + 1.0).collect();
        let total: f32 = (1..=n).map(|v| v as f32).sum();
        // The whole sum: every chunk runs.
        let (m, last) = mass_prefix(&mut probs, total).expect("reached");
        assert_eq!((m, last), (n, 1.0));
        // Unreachable: the walk ends empty-handed.
        assert_eq!(mass_prefix(&mut probs, total * 2.0), None);
        // Reached on the very first value.
        assert_eq!(mass_prefix(&mut probs, 1.0), Some((1, 1000.0)));
        assert_eq!(mass_prefix(&mut [], 0.5), None);
    }

    // Counting allocator. The crate's own allocator is `cfg(not(test))`,
    // so the host test binary can install this one. Counts only on a
    // thread that armed it, so concurrent tests cannot disturb the count.
    pub(crate) mod counting {
        use std::alloc::{GlobalAlloc, Layout, System};
        use std::cell::Cell;
        use std::sync::atomic::{AtomicUsize, Ordering};

        pub struct Counting;
        pub static EVENTS: AtomicUsize = AtomicUsize::new(0);

        std::thread_local! {
            static ARMED: Cell<bool> = const { Cell::new(false) };
        }

        fn note() {
            if ARMED.try_with(Cell::get).unwrap_or(false) {
                EVENTS.fetch_add(1, Ordering::Relaxed);
            }
        }

        // SAFETY: every call forwards to `System` with the caller's
        // arguments. `note` touches a const thread-local and an atomic,
        // and does not allocate.
        unsafe impl GlobalAlloc for Counting {
            unsafe fn alloc(&self, l: Layout) -> *mut u8 {
                note();
                System.alloc(l)
            }
            unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
                note();
                System.alloc_zeroed(l)
            }
            unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
                note();
                System.realloc(p, l, n)
            }
            unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
                note();
                System.dealloc(p, l)
            }
        }

        /// Heap events on this thread while `f` runs.
        pub fn events_during<R>(f: impl FnOnce() -> R) -> (usize, R) {
            let before = EVENTS.load(Ordering::Relaxed);
            ARMED.with(|a| a.set(true));
            let out = f();
            ARMED.with(|a| a.set(false));
            (EVENTS.load(Ordering::Relaxed) - before, out)
        }
    }

    #[global_allocator]
    static COUNTING: counting::Counting = counting::Counting;

    #[test]
    fn counting_allocator_sees_allocations() {
        // Otherwise a broken counter would make the next test vacuous.
        let (events, v) = counting::events_during(|| alloc::vec![0u8; 64]);
        assert!(events >= 1, "allocator not counting");
        drop(v);
    }

    #[test]
    fn sampling_does_not_allocate_after_the_first_token() {
        // Includes the exact-sort paths: a tie on a late token must not
        // be the first thing to touch `idx`.
        for shape in [Shape::Continuous, Shape::Quantized, Shape::AllEqual] {
            let base = make_logits(shape, QWEN_VOCAB, 0xACE);
            for sampler in hot_samplers() {
                let mut state = SamplerState::new(0xACE);
                let mut buf = base.clone();
                state.sample(&sampler, &mut buf);
                for _ in 0..3 {
                    buf.copy_from_slice(&base);
                    let (events, _) = counting::events_during(|| state.sample(&sampler, &mut buf));
                    assert_eq!(events, 0, "{shape:?} {sampler:?} allocated");
                }
            }
        }
        // A tie first appearing after a clean first token.
        let clean = make_logits(Shape::Continuous, QWEN_VOCAB, 0xACE);
        let tied = make_logits(Shape::AllEqual, QWEN_VOCAB, 0xACE);
        let sampler = Sampler::TopKTopP { temperature: 0.7, k: 40, p: 0.9 };
        let mut state = SamplerState::new(0xACE);
        let mut buf = clean.clone();
        state.sample(&sampler, &mut buf);
        assert_eq!(state.workspace.exact_sorts, 0);
        buf.copy_from_slice(&tied);
        let (events, _) = counting::events_during(|| state.sample(&sampler, &mut buf));
        assert_eq!(events, 0, "late tie allocated");
        assert!(state.workspace.exact_sorts >= 1);
    }

    /// Per-token time, old against new, in one process. Each trial times
    /// `ROUNDS` tokens of each, in an order that swaps every trial; the
    /// report is the median and min-max of the trial means. `cold` is the
    /// new code's first token in a fresh state, which includes sizing the
    /// workspace and, on tied logits, both selection and the sort.
    ///
    /// Run with:
    /// `cargo test --release --target x86_64-unknown-linux-gnu --features slm \
    ///  sampler::tests::per_token_time -- --ignored --nocapture --test-threads=1`
    #[test]
    #[ignore = "timing report, not a pass/fail check"]
    fn per_token_time() {
        const TRIALS: usize = 9;
        const ROUNDS: u32 = 10;
        let cases = [
            ("TopK t=0.7", Sampler::TopK { temperature: 0.7, k: 40 }),
            ("TopP t=0.7", Sampler::TopP { temperature: 0.7, p: 0.9 }),
            ("TopP t=1.0", Sampler::TopP { temperature: 1.0, p: 0.9 }),
            ("TopKTopP t=0.7", Sampler::TopKTopP { temperature: 0.7, k: 40, p: 0.9 }),
            ("TopKTopP t=1.0", Sampler::TopKTopP { temperature: 1.0, k: 40, p: 0.9 }),
        ];
        // (median, min, max) in microseconds.
        fn spread(v: &mut [u128]) -> (u128, u128, u128) {
            v.sort_unstable();
            (v[v.len() / 2] / 1000, v[0] / 1000, v[v.len() - 1] / 1000)
        }
        for shape in [Shape::Continuous, Shape::Quantized] {
            let base = make_logits(shape, QWEN_VOCAB, 0x5EED);
            for (name, sampler) in cases.iter() {
                let mut buf = base.clone();
                let mut old_seed = 0x5EEDu64;
                let mut state = SamplerState::new(0x5EED);

                let mut cold = [0u128; TRIALS];
                for slot in cold.iter_mut() {
                    let mut fresh = SamplerState::new(0x5EED);
                    buf.copy_from_slice(&base);
                    let t = std::time::Instant::now();
                    std::hint::black_box(fresh.sample(sampler, &mut buf));
                    *slot = t.elapsed().as_nanos();
                }

                // Warm: the state a generation is in after its first token.
                buf.copy_from_slice(&base);
                state.sample(sampler, &mut buf);

                let mut old_t = [0u128; TRIALS];
                let mut new_t = [0u128; TRIALS];
                for trial in 0..TRIALS {
                    for leg in 0..2 {
                        let run_old = (trial + leg) % 2 == 0;
                        let mut total = 0u128;
                        for _ in 0..ROUNDS {
                            buf.copy_from_slice(&base);
                            let t = std::time::Instant::now();
                            if run_old {
                                std::hint::black_box(legacy::sample(&mut old_seed, sampler, &mut buf));
                            } else {
                                std::hint::black_box(state.sample(sampler, &mut buf));
                            }
                            total += t.elapsed().as_nanos();
                        }
                        let mean = total / ROUNDS as u128;
                        if run_old {
                            old_t[trial] = mean;
                        } else {
                            new_t[trial] = mean;
                        }
                    }
                }
                let (om, ol, oh) = spread(&mut old_t);
                let (nm, nl, nh) = spread(&mut new_t);
                let (cm, _, _) = spread(&mut cold);
                std::println!(
                    "{shape:?} {name}: old {om} [{ol}-{oh}] new {nm} [{nl}-{nh}] cold {cm} us"
                );
            }
        }
    }
}
