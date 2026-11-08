//! Main's sampler at c69438a2 (`runtime/src/slm/sampler.rs` lines 36-513),
//! test-only: the oracle for "the same tokens as main" (#522). Verbatim
//! except that the `Sampler` enum comes from the current module. Do not
//! edit; replace it only together with the sampler it pins.

#![allow(dead_code)]

use super::Sampler;
use alloc::vec::Vec;
use core::cmp::Ordering;

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
