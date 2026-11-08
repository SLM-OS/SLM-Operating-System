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
//! The order is llama.cpp's: top-k, then top-p on the distribution at
//! temperature 1, then the temperature-scaled draw from what both kept.
//!
//! # Per-token cost (#522)
//!
//! One softmax pass per token: the maximum is reduced once, and every
//! live logit the draw can still reach is exponentiated once against it.
//! Top-k picks its `k` tokens in one sweep of the vocabulary, and every
//! later step works on that short list in index order. Top-p alone
//! computes `exp(logit - max)` in one sweep, orders only the values its
//! mass can reach (bounded by an exponent histogram from that sweep), and
//! lists the tokens it keeps. At `T != 1` the kept tokens are
//! exponentiated a second time, at `exp((logit - max) / T)`: the top-p
//! cut needs the `T = 1` values of every live token (for the normalizer)
//! and the draw needs the `T` values of the kept ones, and no arithmetic
//! on one gives the other without another transcendental, so a kept
//! token costs two exps at `T != 1` in any implementation of this order.
//!
//! The filters work out of a [`SamplerWorkspace`] owned by
//! [`SamplerState`], sized on the first filtered token, so a session
//! allocates once and never again.
//!
//! The token stream for a given seed is the one the plain algorithm
//! gives (sort, mask, softmax over the vocabulary; [`reference`]). A tie
//! across a cut makes the kept set depend on `sort_unstable_by`'s
//! permutation, so that case runs the same sort, and a filter whose last
//! cut was tied goes straight to it. Logits holding NaN or `+inf`, and a
//! temperature or top-p that is NaN or whose reciprocal is not a normal
//! float, run [`reference`] itself.
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
/// (`20 * vocab` bytes: about 3 MB at vocab 151,936).
///
/// Every buffer is reserved at once, even those only a tie or the
/// reference path touches: a tie late in a generation must not be the
/// thing that allocates.
#[derive(Default)]
pub struct SamplerWorkspace {
    /// `exp(logit - max)` by token id, for the tokens the top-p pass saw.
    exps: Vec<f32>,
    /// Values being selected, then draw weights by kept position, or the
    /// probabilities the exact top-p sort orders.
    scratch: Vec<f32>,
    /// Token ids for the exact tie-break sort. `usize`, not `u32`: the
    /// sort's permutation on ties may depend on the element type.
    idx: Vec<usize>,
    /// Candidate token ids: the top-k heap, then the kept tokens in
    /// ascending order.
    cand: Vec<u32>,
    /// The last top-k cut was tied, so go straight to the exact sort.
    /// Only ever chooses between two routes to the same result.
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
            self.cand.resize(n, 0);
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

/// The tokens the filters kept.
enum Kept {
    /// Every token: no filter removed anything.
    All,
    /// `cand[..len]`, ascending.
    List(usize),
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
    /// `logits` is scratch: its contents after the call are unspecified,
    /// so it needs a fresh forward pass before it is sampled again.
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
        let (temperature, k, p) = match *sampler {
            Sampler::Greedy => return argmax(logits) as u32,
            Sampler::Temperature { temperature } => (temperature, None, None),
            Sampler::TopK { temperature, k } => {
                if k == 0 {
                    return argmax(logits) as u32;
                }
                (temperature, Some(k), None)
            }
            Sampler::TopP { temperature, p } => {
                if p <= 0.0 {
                    return argmax(logits) as u32;
                }
                (temperature, None, Some(p))
            }
            Sampler::TopKTopP { temperature, k, p } => {
                if k == 0 || p <= 0.0 {
                    return argmax(logits) as u32;
                }
                (temperature, Some(k), Some(p))
            }
        };
        let n = logits.len();
        // A filter that cannot remove anything is skipped.
        let k = k.filter(|&k| k < n);
        let p = p.filter(|&p| !(p >= 1.0));
        let modelled = !temperature.is_nan()
            && (temperature <= 0.0 || (1.0 / temperature).is_normal())
            && !p.is_some_and(f32::is_nan)
            && u32::try_from(n).is_ok();

        if k.is_none() && p.is_none() {
            if temperature <= 0.0 {
                return argmax(logits) as u32;
            }
            if modelled {
                if let Some(m) = max_of_finite(logits) {
                    if let Some(id) = temperature_draw(logits, m, temperature, &mut self.seed) {
                        return id;
                    }
                }
            }
            return reference::sample(&mut self.workspace, &mut self.seed, sampler, logits);
        }

        self.workspace.reserve_for(n);
        if modelled {
            if let Some(id) = self.filtered(logits, temperature, k, p) {
                return id;
            }
        }
        reference::sample(&mut self.workspace, &mut self.seed, sampler, logits)
    }

    /// The top-k / top-p paths. `None` hands the token to
    /// [`reference::sample`]: a NaN or `+inf` logit, no finite logit, or
    /// a scaled maximum that overflows. It returns before writing
    /// `logits` or drawing, so the reference sees the same input.
    fn filtered(
        &mut self,
        logits: &mut [f32],
        temperature: f32,
        k: Option<usize>,
        p: Option<f32>,
    ) -> Option<u32> {
        let ws = &mut self.workspace;
        let n = logits.len();
        let (kept, max, exps_ready) = match (k, p) {
            (Some(k), None) => {
                top_k(logits, k, ws)?;
                let max = max_of_list(logits, &ws.cand[..k])?;
                (Kept::List(k), max, false)
            }
            (Some(k), Some(p)) => {
                top_k(logits, k, ws)?;
                let (len, max) = top_p_of_list(logits, k, p, ws)?;
                (Kept::List(len), max, true)
            }
            (None, Some(p)) => {
                let (kept, max) = top_p_of_vocab(logits, p, ws)?;
                (kept, max, true)
            }
            (None, None) => return None,
        };

        match kept {
            Kept::All => {
                if temperature <= 0.0 {
                    return Some(argmax(logits) as u32);
                }
                if temperature == 1.0 {
                    // exps[i] is 0 for a -inf logit, so its sum is the
                    // top-p normalizer, added in the same order.
                    let w = &ws.exps[..n];
                    let denom = w.iter().fold(0.0f32, |a, &e| a + e);
                    return finite_positive(denom)
                        .then(|| inverse_cdf(w, denom, &mut self.seed) as u32);
                }
                temperature_draw(logits, max, temperature, &mut self.seed)
            }
            Kept::List(len) => {
                let list = &ws.cand[..len];
                if temperature <= 0.0 {
                    return Some(argmax_of_list(logits, list));
                }
                let w = &mut ws.scratch[..len];
                let mut denom = 0.0f32;
                if temperature == 1.0 && exps_ready {
                    // The top-p pass already holds exp(logit - max).
                    for (slot, &i) in w.iter_mut().zip(list.iter()) {
                        let e = if logits[i as usize] == f32::NEG_INFINITY {
                            0.0
                        } else {
                            ws.exps[i as usize]
                        };
                        *slot = e;
                        denom += e;
                    }
                } else {
                    let inv_t = 1.0 / temperature;
                    let max_t = max * inv_t;
                    if !max_t.is_finite() {
                        return None;
                    }
                    for (slot, &i) in w.iter_mut().zip(list.iter()) {
                        let y = logits[i as usize] * inv_t;
                        let e = if y == f32::NEG_INFINITY {
                            0.0
                        } else {
                            expf(y - max_t)
                        };
                        *slot = e;
                        denom += e;
                    }
                }
                if !finite_positive(denom) {
                    return None;
                }
                let r = rand_f32(&mut self.seed) * denom;
                let mut acc: f32 = 0.0;
                for (&e, &i) in w.iter().zip(list.iter()) {
                    acc += e;
                    if r < acc {
                        return Some(i);
                    }
                }
                // Floating-point drift fallback: the last token, as the
                // draw over the whole vocabulary gives.
                Some((n - 1) as u32)
            }
        }
    }
}

// ---------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------

/// Test-only counters for the #522 host tests: `expf` calls and max
/// reductions on this thread.
#[cfg(test)]
pub(crate) mod probe {
    use std::cell::Cell;
    std::thread_local! {
        static EXPS: Cell<usize> = const { Cell::new(0) };
        static MAXES: Cell<usize> = const { Cell::new(0) };
    }
    pub(crate) fn exp() {
        EXPS.with(|c| c.set(c.get() + 1));
    }
    pub(crate) fn max_reduction() {
        MAXES.with(|c| c.set(c.get() + 1));
    }
    /// `(expf calls, max reductions)` on this thread since the last call.
    pub fn take() -> (usize, usize) {
        (EXPS.with(|c| c.replace(0)), MAXES.with(|c| c.replace(0)))
    }
}

/// `libm::expf`, counted in tests.
fn expf(x: f32) -> f32 {
    #[cfg(test)]
    probe::exp();
    libm::expf(x)
}

/// Counts a max reduction in tests.
fn note_max() {
    #[cfg(test)]
    probe::max_reduction();
}

fn finite_positive(x: f32) -> bool {
    x > 0.0 && x.is_finite()
}

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

/// argmax with low-index tie-breaking: the first index of the largest
/// value, as a left-to-right scan keeping the first strictly greater
/// value finds it. NaN never wins; a NaN first entry wins outright,
/// since nothing compares greater than it.
///
/// Four interleaved scans keep the compares independent (one scan is a
/// chain of dependent compare-and-selects). Each keeps the first index
/// of its own maximum, with entry 0 as every scan's starting point, so
/// the lowest index among the scans holding the overall maximum is the
/// one a single scan returns.
fn argmax(logits: &[f32]) -> usize {
    let first = logits[0];
    if first.is_nan() {
        return 0;
    }
    let (mut b0, mut b1, mut b2, mut b3) = (first, first, first, first);
    let (mut i0, mut i1, mut i2, mut i3) = (0usize, 0usize, 0usize, 0usize);
    let rest = &logits[1..];
    let quads = rest.chunks_exact(4);
    let tail = quads.remainder();
    let mut i = 1usize;
    for q in quads {
        if q[0] > b0 {
            b0 = q[0];
            i0 = i;
        }
        if q[1] > b1 {
            b1 = q[1];
            i1 = i + 1;
        }
        if q[2] > b2 {
            b2 = q[2];
            i2 = i + 2;
        }
        if q[3] > b3 {
            b3 = q[3];
            i3 = i + 3;
        }
        i += 4;
    }
    for &x in tail.iter() {
        if x > b0 {
            b0 = x;
            i0 = i;
        }
        i += 1;
    }
    let (mut best, mut at) = (b0, i0);
    for (b, j) in [(b1, i1), (b2, i2), (b3, i3)] {
        if b > best || (b == best && j < at) {
            best = b;
            at = j;
        }
    }
    at
}

/// [`argmax`] over the tokens in `list` (ascending): the masked
/// vocabulary's argmax, since every other token is `-inf`.
fn argmax_of_list(logits: &[f32], list: &[u32]) -> u32 {
    let mut best = list[0];
    for &i in list.iter().skip(1) {
        if logits[i as usize] > logits[best as usize] {
            best = i;
        }
    }
    best
}

/// The largest logit, or `None` if one is NaN or `+inf` or none is
/// finite. The first of equal maxima wins, as in every max scan here.
fn max_of_finite(logits: &[f32]) -> Option<f32> {
    note_max();
    let mut max = f32::NEG_INFINITY;
    let mut bad = false;
    for &x in logits.iter() {
        bad |= !(x < f32::INFINITY);
        if x > max {
            max = x;
        }
    }
    (!bad && max != f32::NEG_INFINITY).then_some(max)
}

/// The largest logit among `list` (no NaN or `+inf` by then), or `None`
/// if all are `-inf`.
fn max_of_list(logits: &[f32], list: &[u32]) -> Option<f32> {
    note_max();
    let mut max = f32::NEG_INFINITY;
    for &i in list.iter() {
        let x = logits[i as usize];
        if x > max {
            max = x;
        }
    }
    (max != f32::NEG_INFINITY).then_some(max)
}

/// Softmax draw over the whole vocabulary at `temperature > 0`, given
/// `max`, the largest logit. Scales in place: `logits` ends up holding
/// the weights. `None` (before writing anything) if the scaled maximum
/// is not finite.
fn temperature_draw(logits: &mut [f32], max: f32, temperature: f32, rng: &mut u64) -> Option<u32> {
    let inv_t = 1.0 / temperature;
    // Scaling by a positive number keeps the order, so this is the
    // largest scaled logit.
    let max_t = max * inv_t;
    if !max_t.is_finite() {
        return None;
    }
    let mut denom: f32 = 0.0;
    for x in logits.iter_mut() {
        let y = *x * inv_t;
        if y == f32::NEG_INFINITY {
            // Masked or underflowed: exp is +0, so skip the call.
            *x = 0.0;
            continue;
        }
        let e = expf(y - max_t);
        *x = e;
        denom += e;
    }
    if !finite_positive(denom) {
        return Some(argmax(logits) as u32);
    }
    Some(inverse_cdf(logits, denom, rng) as u32)
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

/// How a single-sweep top-k selection ended.
enum Select {
    /// The `k` largest are strictly above every other logit.
    Clean,
    /// A logit outside equals the k-th largest: the sort decides.
    Tied,
    /// A NaN or `+inf` logit.
    Bad,
}

/// Restore the min-heap (by logit) below `at`.
fn sift_down(heap: &mut [u32], mut at: usize, logits: &[f32]) {
    let len = heap.len();
    loop {
        let l = 2 * at + 1;
        if l >= len {
            return;
        }
        let r = l + 1;
        let c = if r < len && logits[heap[r] as usize] < logits[heap[l] as usize] {
            r
        } else {
            l
        };
        if logits[heap[c] as usize] < logits[heap[at] as usize] {
            heap.swap(at, c);
            at = c;
        } else {
            return;
        }
    }
}

/// The `k` largest logits into `heap`, in one sweep of the vocabulary.
///
/// The heap's root is the k-th largest so far, and a logit enters only
/// if it beats the root. Every logit left outside is below the final
/// root, except one that equaled the root when it was rejected or was
/// the root when evicted; those values never decrease, so the last of
/// them is the largest, and the cut is tied exactly when it equals the
/// final root.
fn heap_select(logits: &[f32], heap: &mut [u32]) -> Select {
    let k = heap.len();
    let mut bad = false;
    for (i, slot) in heap.iter_mut().enumerate() {
        *slot = i as u32;
        bad |= !(logits[i] < f32::INFINITY);
    }
    if bad {
        return Select::Bad;
    }
    for at in (0..k / 2).rev() {
        sift_down(heap, at, logits);
    }
    let mut root = logits[heap[0] as usize];
    let mut out_max: Option<f32> = None;
    for (i, &x) in logits.iter().enumerate().skip(k) {
        // One compare per logit: below the root is the common case, and
        // NaN fails `<` so it lands here too.
        if !(x < root) {
            if x > root {
                out_max = Some(root);
                heap[0] = i as u32;
                sift_down(heap, 0, logits);
                root = logits[heap[0] as usize];
            } else if x == root {
                out_max = Some(x);
            } else {
                return Select::Bad;
            }
        }
    }
    if heap.iter().any(|&i| logits[i as usize] == f32::INFINITY) {
        return Select::Bad;
    }
    if out_max == Some(root) {
        Select::Tied
    } else {
        Select::Clean
    }
}

/// Top-k: the `k` largest logits (`0 < k < n`) into `cand[..k]`,
/// ascending. `None` for a NaN or `+inf` logit.
///
/// One sweep finds them unless the cut is tied, in which case which of
/// the tied tokens survive is whatever the exact sort leaves in
/// `idx[..k]`. Tied logits tend to stay tied, so after a tied cut the
/// sweep is skipped until a sort finds the cut clean.
fn top_k(logits: &[f32], k: usize, ws: &mut SamplerWorkspace) -> Option<()> {
    let n = logits.len();
    if !ws.k_cut_tied {
        match heap_select(logits, &mut ws.cand[..k]) {
            Select::Clean => {
                ws.cand[..k].sort_unstable();
                return Some(());
            }
            Select::Tied => {}
            Select::Bad => return None,
        }
    } else if logits.iter().any(|&x| !(x < f32::INFINITY)) {
        return None;
    }
    ws.fresh_idx(n);
    let idx = &mut ws.idx[..n];
    idx.sort_unstable_by(|&a, &b| {
        // Reverse order — largest first. NaNs compare Equal.
        match logits[b].partial_cmp(&logits[a]) {
            Some(ord) => ord,
            None => Ordering::Equal,
        }
    });
    ws.k_cut_tied = logits[idx[k - 1]] == logits[idx[k]];
    for (c, &i) in ws.cand[..k].iter_mut().zip(idx[..k].iter()) {
        *c = i as u32;
    }
    ws.cand[..k].sort_unstable();
    Some(())
}

/// Values selected in the first round of [`nucleus`]; each later round
/// doubles. Peaked distributions usually finish in the first.
const TOP_P_FIRST_CHUNK: usize = 64;

/// Where the top-p walk stopped.
#[derive(Debug, Clone, Copy, PartialEq)]
struct Cut {
    /// Tokens in the prefix.
    len: usize,
    /// `exp(logit - max)` of the last one.
    exp_last: f32,
    /// Its probability.
    prob_last: f32,
    /// The largest probability outside the prefix (0 if none).
    prob_next: f32,
}

/// Walk `exps` (positive `exp(logit - max)` values, in any order) in
/// descending order, summing `exp / denom`, until the sum reaches `p`.
/// `None` if it never does. Orders only as much of `exps` as the walk
/// needs.
///
/// Dividing by a positive `denom` keeps the order (equal values stay
/// equal, and distinct ones can only become equal), so the probabilities
/// are summed in the same order, with the same rounding, as a sort of
/// the probabilities themselves.
fn nucleus(exps: &mut [f32], denom: f32, p: f32) -> Option<Cut> {
    let len = exps.len();
    let mut cum: f32 = 0.0;
    let mut lo = 0usize;
    let mut chunk = TOP_P_FIRST_CHUNK;
    while lo < len {
        let hi = lo.saturating_add(chunk).min(len);
        if hi < len {
            exps[lo..].select_nth_unstable_by(hi - lo - 1, desc);
        }
        exps[lo..hi].sort_unstable_by(desc);
        for at in lo..hi {
            let q = exps[at] / denom;
            cum += q;
            if cum >= p {
                // Everything past `hi` is at most exps[hi - 1].
                let next = if at + 1 < hi {
                    exps[at + 1]
                } else {
                    exps[hi..]
                        .iter()
                        .fold(0.0f32, |m, &e| if e > m { e } else { m })
                };
                return Some(Cut {
                    len: at + 1,
                    exp_last: exps[at],
                    prob_last: q,
                    prob_next: next / denom,
                });
            }
        }
        lo = hi;
        chunk = chunk.saturating_mul(2);
    }
    None
}

/// The largest power of two whose exps and those above it should already
/// reach `p`, from `mass`, the sum of exps by exponent field: their
/// buckets hold more than `p * denom`, with 1/64 to spare for rounding
/// in the bucket sums and in the walk's running sum. 0 when no bucket
/// qualifies. Only an estimate: [`nucleus`] still makes the cut, and the
/// caller walks every exp if the cut does not fall strictly inside it.
fn nucleus_floor(mass: &[f32; 128], denom: f32, p: f32) -> f32 {
    let target = f64::from(p) * f64::from(denom) * (1.0 + 1.0 / 64.0);
    let mut above = 0.0f64;
    for field in (1..128u32).rev() {
        above += f64::from(mass[field as usize]);
        if above >= target {
            return f32::from_bits(field << 23);
        }
    }
    0.0
}

/// [`nucleus`] over the positive `exps`, ordering only those at or above
/// `floor` when the mass is reached among them. They are a prefix of the
/// full descending order, so the walk is the same; if the cut takes all
/// of them, the next value is the largest exp below the floor.
fn cut_from_floor(
    exps: &[f32],
    scratch: &mut [f32],
    denom: f32,
    p: f32,
    floor: f32,
) -> Option<Cut> {
    let gather = |scratch: &mut [f32], floor: f32| {
        let mut live = 0usize;
        for &e in exps.iter() {
            if e > 0.0 && e >= floor {
                scratch[live] = e;
                live += 1;
            }
        }
        live
    };
    if floor > 0.0 {
        let live = gather(scratch, floor);
        match nucleus(&mut scratch[..live], denom, p) {
            Some(cut) if cut.len < live => return Some(cut),
            Some(mut cut) => {
                let below = exps
                    .iter()
                    .fold(0.0f32, |m, &e| if e < floor && e > m { e } else { m });
                cut.prob_next = below / denom;
                return Some(cut);
            }
            None => {}
        }
    }
    let live = gather(scratch, 0.0);
    nucleus(&mut scratch[..live], denom, p)
}

/// The exact top-p cut: sort token ids by `probs` (descending) and keep
/// the shortest prefix whose mass reaches `p`. Returns its length (`n`
/// if the mass never reaches `p`), with the prefix in `idx[..len]`.
fn exact_top_p(ws: &mut SamplerWorkspace, n: usize, p: f32) -> usize {
    ws.fresh_idx(n);
    let probs = &ws.scratch[..n];
    let idx = &mut ws.idx[..n];
    idx.sort_unstable_by(|&a, &b| match probs[b].partial_cmp(&probs[a]) {
        Some(ord) => ord,
        None => Ordering::Equal,
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
    ws.p_cut_tied = kept < n && probs[idx[kept - 1]] == probs[idx[kept]];
    kept
}

/// After [`exact_top_p`] kept `idx[..kept]` (`kept < n`): put those ids
/// in `cand[..kept]`, ascending, and return the largest kept logit.
/// That is `max` unless the sort put a token with the same probability
/// but a smaller logit first and cut the maximum away.
fn take_exact_prefix(logits: &[f32], ws: &mut SamplerWorkspace, kept: usize, max: f32) -> f32 {
    let first = ws.idx[0];
    for (c, &i) in ws.cand[..kept].iter_mut().zip(ws.idx[..kept].iter()) {
        *c = i as u32;
    }
    ws.cand[..kept].sort_unstable();
    if logits[first] == max {
        max
    } else {
        max_of_list(logits, &ws.cand[..kept]).unwrap_or(max)
    }
}

/// Top-p over the top-k tokens in `cand[..len]` (ascending). Leaves the
/// kept tokens in `cand[..kept]`, ascending, and `exp(logit - max)` for
/// them in `exps` by token id. Returns `(kept, largest kept logit)`, or
/// `None` if no top-k logit is finite.
fn top_p_of_list(
    logits: &[f32],
    len: usize,
    p: f32,
    ws: &mut SamplerWorkspace,
) -> Option<(usize, f32)> {
    let n = logits.len();
    let max = max_of_list(logits, &ws.cand[..len])?;
    let mut denom: f32 = 0.0;
    let mut live = 0usize;
    for &i in ws.cand[..len].iter() {
        let x = logits[i as usize];
        let e = if x == f32::NEG_INFINITY {
            0.0
        } else {
            expf(x - max)
        };
        ws.exps[i as usize] = e;
        denom += e;
        if e > 0.0 {
            ws.scratch[live] = e;
            live += 1;
        }
    }
    if !finite_positive(denom) {
        return None;
    }

    if !ws.p_cut_tied {
        let Some(cut) = nucleus(&mut ws.scratch[..live], denom, p) else {
            return Some((len, max));
        };
        if cut.prob_next != cut.prob_last {
            // Keep exactly the prefix: everything at or above its last exp.
            let mut kept = 0usize;
            for at in 0..len {
                let i = ws.cand[at];
                if ws.exps[i as usize] >= cut.exp_last {
                    ws.cand[kept] = i;
                    kept += 1;
                }
            }
            return Some((kept, max));
        }
    }

    // Tied at the cut: the exact sort over the vocabulary, every token
    // outside the top-k at probability 0.
    ws.scratch[..n].fill(0.0);
    for at in 0..len {
        let i = ws.cand[at] as usize;
        ws.scratch[i] = ws.exps[i] / denom;
    }
    let kept = exact_top_p(ws, n, p);
    if kept == n {
        return Some((len, max));
    }
    let max_kept = take_exact_prefix(logits, ws, kept, max);
    Some((kept, max_kept))
}

/// Top-p over the whole vocabulary. Leaves `exp(logit - max)` in
/// `exps[..n]` (0 for `-inf`) and the kept tokens in `cand`. Returns them
/// with the largest kept logit, or `None` for a NaN or `+inf` logit or
/// no finite one.
fn top_p_of_vocab(logits: &[f32], p: f32, ws: &mut SamplerWorkspace) -> Option<(Kept, f32)> {
    let n = logits.len();
    let max = max_of_finite(logits)?;
    let mut denom: f32 = 0.0;
    // Sum of exps by binary exponent: each is in [0, 1], so its exponent
    // field is at most 127.
    let mut mass = [0.0f32; 128];
    for (slot, &x) in ws.exps[..n].iter_mut().zip(logits.iter()) {
        if x == f32::NEG_INFINITY {
            *slot = 0.0;
            continue;
        }
        let e = expf(x - max);
        *slot = e;
        denom += e;
        mass[(e.to_bits() >> 23) as usize & 127] += e;
    }
    if !finite_positive(denom) {
        return None;
    }

    if !ws.p_cut_tied {
        let floor = nucleus_floor(&mass, denom, p);
        let Some(cut) = cut_from_floor(&ws.exps[..n], &mut ws.scratch, denom, p, floor) else {
            return Some((Kept::All, max));
        };
        if cut.prob_next != cut.prob_last {
            let mut kept = 0usize;
            for (i, &e) in ws.exps[..n].iter().enumerate() {
                if e >= cut.exp_last {
                    ws.cand[kept] = i as u32;
                    kept += 1;
                }
            }
            return Some((Kept::List(kept), max));
        }
    }

    for (q, &e) in ws.scratch[..n].iter_mut().zip(ws.exps.iter()) {
        *q = e / denom;
    }
    let kept = exact_top_p(ws, n, p);
    if kept == n {
        return Some((Kept::All, max));
    }
    let max_kept = take_exact_prefix(logits, ws, kept, max);
    Some((Kept::List(kept), max_kept))
}

/// The sampler as first specified: sort, mask, then a softmax over the
/// whole vocabulary. It defines the token stream (the fast paths are
/// tested against a copy of it) and runs the inputs they leave to it.
/// Uses only workspace memory.
pub(crate) mod reference {
    use super::{argmax, expf, inverse_cdf, note_max, Sampler, SamplerWorkspace};
    use core::cmp::Ordering;

    pub(crate) fn sample(
        ws: &mut SamplerWorkspace,
        rng: &mut u64,
        sampler: &Sampler,
        logits: &mut [f32],
    ) -> u32 {
        if logits.is_empty() {
            return 0;
        }
        let (temperature, k, p) = match *sampler {
            Sampler::Greedy => return argmax(logits) as u32,
            Sampler::Temperature { temperature } => (temperature, 0, 1.0),
            Sampler::TopK { temperature, k } => {
                if k == 0 {
                    return argmax(logits) as u32;
                }
                (temperature, k, 1.0)
            }
            Sampler::TopP { temperature, p } => {
                if p <= 0.0 {
                    return argmax(logits) as u32;
                }
                (temperature, 0, p)
            }
            Sampler::TopKTopP { temperature, k, p } => {
                if k == 0 || p <= 0.0 {
                    return argmax(logits) as u32;
                }
                (temperature, k, p)
            }
        };
        top_k(logits, k, ws);
        top_p(logits, p, ws);
        if temperature <= 0.0 {
            return argmax(logits) as u32;
        }
        let inv_t = 1.0 / temperature;
        for x in logits.iter_mut() {
            *x *= inv_t;
        }
        softmax_sample(logits, rng) as u32
    }

    fn by_value_desc(v: &[f32], a: usize, b: usize) -> Ordering {
        // Reverse order — largest first. NaNs compare Equal.
        v[b].partial_cmp(&v[a]).unwrap_or(Ordering::Equal)
    }

    /// Mask all but the `k` largest; `k == 0` or `k >= n` keeps all.
    fn top_k(logits: &mut [f32], k: usize, ws: &mut SamplerWorkspace) {
        let n = logits.len();
        if k == 0 || k >= n {
            return;
        }
        let idx = ws.fresh_idx(n);
        idx.sort_unstable_by(|&a, &b| by_value_desc(logits, a, b));
        for &i in idx[k..].iter() {
            logits[i] = f32::NEG_INFINITY;
        }
    }

    /// Mask all but the shortest prefix (by probability) whose mass
    /// reaches `p`; `p >= 1` keeps all.
    fn top_p(logits: &mut [f32], p: f32, ws: &mut SamplerWorkspace) {
        let n = logits.len();
        if p >= 1.0 {
            return;
        }
        note_max();
        let mut max = f32::NEG_INFINITY;
        for &x in logits.iter() {
            if x > max {
                max = x;
            }
        }
        if !max.is_finite() {
            return;
        }
        let mut denom: f32 = 0.0;
        for (q, &x) in ws.scratch[..n].iter_mut().zip(logits.iter()) {
            *q = if x == f32::NEG_INFINITY {
                0.0
            } else {
                let e = expf(x - max);
                denom += e;
                e
            };
        }
        if denom <= 0.0 || !denom.is_finite() {
            return;
        }
        for q in ws.scratch[..n].iter_mut() {
            *q /= denom;
        }
        ws.fresh_idx(n);
        let probs = &ws.scratch[..n];
        let idx = &mut ws.idx[..n];
        idx.sort_unstable_by(|&a, &b| by_value_desc(probs, a, b));
        let mut cum: f32 = 0.0;
        let mut kept = n;
        for (pos, &i) in idx.iter().enumerate() {
            cum += probs[i];
            if cum >= p {
                kept = pos + 1;
                break;
            }
        }
        for &i in idx[kept..].iter() {
            logits[i] = f32::NEG_INFINITY;
        }
    }

    /// Stable softmax over the whole vector, then the inverse-CDF draw.
    fn softmax_sample(logits: &mut [f32], rng: &mut u64) -> usize {
        note_max();
        let mut max = f32::NEG_INFINITY;
        for &x in logits.iter() {
            if x > max {
                max = x;
            }
        }
        if !max.is_finite() {
            return argmax(logits);
        }
        let mut denom: f32 = 0.0;
        for x in logits.iter_mut() {
            let e = expf(*x - max);
            *x = e;
            denom += e;
        }
        if denom <= 0.0 || !denom.is_finite() {
            return argmax(logits);
        }
        inverse_cdf(logits, denom, rng)
    }
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
    fn argmax_matches_a_plain_scan() {
        fn plain(v: &[f32]) -> usize {
            let (mut at, mut best) = (0usize, v[0]);
            for (i, &x) in v.iter().enumerate().skip(1) {
                if x > best {
                    best = x;
                    at = i;
                }
            }
            at
        }
        // Few values, so ties, NaN, infinities and both zeros meet in
        // every lane and every tail length.
        let pool = [
            f32::NAN,
            f32::INFINITY,
            f32::NEG_INFINITY,
            0.0,
            -0.0,
            1.0,
            2.0,
            -3.0,
            2.0,
            1.5,
        ];
        let mut rng = 0x1234u64;
        for trial in 0..20_000 {
            let n = 1 + (xorshift64(&mut rng) % 23) as usize;
            let v: Vec<f32> = (0..n)
                .map(|_| pool[(xorshift64(&mut rng) % pool.len() as u64) as usize])
                .collect();
            assert_eq!(argmax(&v), plain(&v), "trial {trial}: {v:?}");
        }
        for shape in [
            Shape::Continuous,
            Shape::Quantized,
            Shape::AllEqual,
            Shape::WithNan,
        ] {
            let big = make_logits(shape, QWEN_VOCAB, 3);
            assert_eq!(argmax(&big), plain(&big), "{shape:?}");
        }
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
                &Sampler::TopK {
                    temperature: 1.0,
                    k: 2,
                },
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
                &Sampler::TopP {
                    temperature: 1.0,
                    p: 0.5,
                },
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
    /// Main's sampler (c69438a2) was tested equal to it, and
    /// `token_streams_match_main_digest` checks main's own output.
    mod legacy {
        use super::super::{argmax, rand_f32, Sampler};
        use alloc::vec::Vec;

        fn scale_by_inv_temperature(logits: &mut [f32], temperature: f32) {
            let inv_t = 1.0 / temperature;
            for x in logits.iter_mut() {
                *x *= inv_t;
            }
        }

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

        pub(super) fn top_k_filter(logits: &mut [f32], k: usize) {
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

        pub(super) fn top_p_filter(logits: &mut [f32], p: f32) {
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
        let mut v = vec![Sampler::Greedy];
        // 1e-39: 1/T overflows, and infinity: 1/T is 0 (the reference path).
        for t in [0.0, 0.7, 1.0, 1.3, f32::INFINITY, f32::NAN, 1e-39] {
            v.push(Sampler::Temperature { temperature: t });
            v.push(Sampler::TopK { temperature: t, k });
            v.push(Sampler::TopP {
                temperature: t,
                p: 0.9,
            });
            v.push(Sampler::TopKTopP {
                temperature: t,
                k,
                p: 0.9,
            });
        }
        // 0.999_999_94 is the largest p below 1: the mass may never reach it.
        for t in [0.7, 1.0] {
            for p in [1.0, 0.999_999_94, 0.5, 1e-6, f32::NAN] {
                v.push(Sampler::TopP { temperature: t, p });
                v.push(Sampler::TopKTopP {
                    temperature: t,
                    k,
                    p,
                });
            }
        }
        v
    }

    /// Every mode at the temperatures the criteria name, with the demo
    /// default k and p.
    fn every_mode() -> Vec<Sampler> {
        let mut v = vec![Sampler::Greedy];
        for t in [0.7, 1.0, 1.3] {
            v.push(Sampler::Temperature { temperature: t });
            v.push(Sampler::TopK {
                temperature: t,
                k: 40,
            });
            v.push(Sampler::TopP {
                temperature: t,
                p: 0.9,
            });
            v.push(Sampler::TopKTopP {
                temperature: t,
                k: 40,
                p: 0.9,
            });
        }
        v
    }

    /// The filtered paths, for vocabulary-sized runs. The legacy oracle
    /// takes around 100 ms per token unoptimized, and the small-vector
    /// test covers the full list.
    fn hot_samplers() -> Vec<Sampler> {
        vec![
            Sampler::TopK {
                temperature: 0.7,
                k: 40,
            },
            Sampler::TopK {
                temperature: 1.0,
                k: 40,
            },
            Sampler::TopP {
                temperature: 0.7,
                p: 0.9,
            },
            Sampler::TopP {
                temperature: 1.0,
                p: 0.9,
            },
            Sampler::TopKTopP {
                temperature: 0.7,
                k: 40,
                p: 0.9,
            },
            Sampler::TopKTopP {
                temperature: 1.0,
                k: 40,
                p: 0.9,
            },
            Sampler::TopKTopP {
                temperature: 1.3,
                k: 40,
                p: 0.9,
            },
        ]
    }

    /// Token and PRNG state must match. The logits buffer is scratch
    /// after the call, so its contents are not compared.
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
                old,
                new,
                "token {t} diverged: {shape:?} {sampler:?} n={} seed={seed:#x}",
                base.len()
            );
            assert_eq!(old_seed, state.seed, "PRNG diverged: {shape:?} {sampler:?}");
        }
    }

    #[test]
    fn matches_legacy_on_vocabulary_sized_logits() {
        for seed in [1u64, 0xDEAD_BEEF] {
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
            &Sampler::TopKTopP {
                temperature: 0.7,
                k: 40,
                p: 0.9,
            },
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
                for seed in 1..=3u64 {
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
                            (tok, s)
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
                    assert!(
                        old == new,
                        "NaN case diverged: n={n} {sampler:?} seed={seed}"
                    );
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
        state.sample(
            &Sampler::TopK {
                temperature: 1.0,
                k: 2,
            },
            &mut logits,
        );
        assert_eq!(state.workspace.exact_sorts, 1);

        let mut state = SamplerState::new(9);
        let mut logits = vec![1.0_f32; 8];
        state.sample(
            &Sampler::TopP {
                temperature: 1.0,
                p: 0.5,
            },
            &mut logits,
        );
        assert_eq!(state.workspace.exact_sorts, 1);
    }

    #[test]
    fn tied_cut_hint_skips_selection_and_clears_on_a_clean_cut() {
        for sampler in [
            Sampler::TopK {
                temperature: 0.7,
                k: 2,
            },
            Sampler::TopP {
                temperature: 0.7,
                p: 0.5,
            },
        ] {
            let mut state = SamplerState::new(9);
            let is_top_k = matches!(sampler, Sampler::TopK { .. });
            let hint = |st: &SamplerState| {
                if is_top_k {
                    st.workspace.k_cut_tied
                } else {
                    st.workspace.p_cut_tied
                }
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
            assert_eq!(
                state.workspace.exact_sorts, 2,
                "{sampler:?}: fast path lost"
            );
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
                    assert_eq!(old_seed, state.seed, "n={n} {sampler:?}");
                }
            }
        }
    }

    #[test]
    fn nucleus_crosses_chunks() {
        let n = 1000usize;
        let mut exps: Vec<f32> = (0..n).map(|i| ((i * 37) % n) as f32 + 1.0).collect();
        let total: f32 = (1..=n).map(|v| v as f32).sum();
        // The whole sum: every chunk runs and nothing is left outside.
        let cut = nucleus(&mut exps, 1.0, total).expect("reached");
        assert_eq!((cut.len, cut.exp_last, cut.prob_next), (n, 1.0, 0.0));
        // Unreachable: the walk ends empty-handed.
        assert_eq!(nucleus(&mut exps, 1.0, total * 2.0), None);
        // Reached on the very first value; the next is the runner-up.
        let cut = nucleus(&mut exps, 1.0, 1.0).expect("reached");
        assert_eq!((cut.len, cut.exp_last, cut.prob_next), (1, 1000.0, 999.0));
        // Ending on a chunk's last value reads the next from the rest.
        let first64: f32 = (937..=1000).map(|v| v as f32).sum();
        let cut = nucleus(&mut exps, 1.0, first64).expect("reached");
        assert_eq!((cut.len, cut.exp_last, cut.prob_next), (64, 937.0, 936.0));
        // Probabilities are the values over the normalizer.
        let cut = nucleus(&mut exps, 4.0, 250.0).expect("reached");
        assert_eq!((cut.len, cut.prob_last, cut.prob_next), (1, 250.0, 249.75));
        assert_eq!(nucleus(&mut [], 1.0, 0.5), None);
    }

    #[test]
    fn the_nucleus_floor_bounds_the_walk() {
        // Exact powers of two, so every sum below is exact.
        let tiny = f32::from_bits(107 << 23); // 2^-20
        let exps = [0.25_f32, 1.0, 0.5, 0.125, 0.5, tiny, 0.0];
        let denom: f32 = exps.iter().sum();
        let mut mass = [0.0f32; 128];
        for &e in exps.iter() {
            mass[(e.to_bits() >> 23) as usize & 127] += e;
        }
        // 1.0 + 0.5 + 0.5 = 2.0 covers 0.8 * 2.375 * (1 + 1/64).
        assert_eq!(nucleus_floor(&mass, denom, 0.8), 0.5);
        // Nothing covers almost all of the mass with 1/64 to spare.
        assert_eq!(nucleus_floor(&mass, denom, 0.999), 0.0);

        let full = |p: f32| {
            let mut v: Vec<f32> = exps.iter().copied().filter(|&e| e > 0.0).collect();
            nucleus(&mut v, denom, p)
        };
        let mut scratch = [0.0_f32; 7];
        for (p, floor) in [
            (0.8, 0.25),      // cut inside the floor: three of four values
            (0.8, 0.5),       // cut on the floor's last value
            (0.99, 0.125),    // the same, the next value far below
            (0.99, 0.0),      // no floor
            (0.9999999, 0.5), // never reached above the floor: walk all
        ] {
            let got = cut_from_floor(&exps, &mut scratch, denom, p, floor);
            assert_eq!(got, full(p), "p={p} floor={floor}");
        }
        // On the floor's last value, the next one comes from below it.
        let cut = cut_from_floor(&exps, &mut scratch, denom, 0.99, 0.125).expect("reached");
        assert_eq!((cut.len, cut.prob_next), (5, tiny / denom));
        let cut = cut_from_floor(&exps, &mut scratch, denom, 0.8, 0.5).expect("reached");
        assert_eq!((cut.len, cut.prob_next), (3, 0.25 / denom));
    }

    #[test]
    fn heap_select_agrees_with_a_sort() {
        // Few distinct levels, some -inf: ties at and around most cuts.
        let mut rng = 0x7777u64;
        for trial in 0..3000 {
            let n = 2 + (xorshift64(&mut rng) % 300) as usize;
            let k = 1 + (xorshift64(&mut rng) as usize) % (n - 1);
            let levels = 1 + xorshift64(&mut rng) % 60;
            let v: Vec<f32> = (0..n)
                .map(|_| match xorshift64(&mut rng) % levels {
                    0 => f32::NEG_INFINITY,
                    l => l as f32 * 0.25 - 3.0,
                })
                .collect();
            let mut sorted = v.clone();
            sorted.sort_by(|a, b| b.partial_cmp(a).unwrap());
            let tied = sorted[k] == sorted[k - 1];
            let mut heap = vec![0u32; k];
            match heap_select(&v, &mut heap) {
                Select::Tied => assert!(tied, "trial {trial}: tie reported, none there"),
                Select::Clean => {
                    assert!(!tied, "trial {trial}: tie missed");
                    let mut got: Vec<f32> = heap.iter().map(|&i| v[i as usize]).collect();
                    got.sort_by(|a, b| b.partial_cmp(a).unwrap());
                    assert_eq!(got, sorted[..k], "trial {trial}");
                }
                Select::Bad => panic!("trial {trial}: no NaN or +inf here"),
            }
        }
        // NaN or +inf anywhere, inside the first k or after.
        for bad in [f32::NAN, f32::INFINITY] {
            for at in [0usize, 1, 5, 9] {
                let mut v: Vec<f32> = (0..10).map(|i| i as f32).collect();
                v[at] = bad;
                let mut heap = vec![0u32; 3];
                assert!(
                    matches!(heap_select(&v, &mut heap), Select::Bad),
                    "{bad} at {at}"
                );
            }
        }
    }

    /// Top-p's tie sort can cut the maximum away and keep tokens whose
    /// `exp(logit - max)` also rounds to 1.0. The draw at `T != 1` then
    /// scales against the largest kept logit, as the oracle does, which
    /// costs one more max reduction, over the kept tokens.
    #[test]
    fn a_cut_maximum_scales_against_the_kept_one() {
        let base = [-1.0e-8_f32, -1.5e-8, 0.0, -20.0, -20.5];
        for sampler in [
            Sampler::TopP {
                temperature: 0.25,
                p: 0.6,
            },
            Sampler::TopKTopP {
                temperature: 0.25,
                k: 4,
                p: 0.6,
            },
        ] {
            let mut state = SamplerState::new(1);
            let mut buf = base.to_vec();
            let _ = probe::take();
            state.sample(&sampler, &mut buf);
            assert_eq!(
                state.workspace.exact_sorts, 1,
                "{sampler:?}: no tie at the cut"
            );
            assert_eq!(probe::take().1, 2, "{sampler:?}: kept maximum not taken");
            for seed in 1..=64u64 {
                assert_same(Shape::Continuous, &sampler, seed, &base, 2);
            }
        }
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
    fn no_allocation_after_warm_up_every_mode() {
        // Includes the exact-sort paths and the reference path (+inf): a
        // tie or an odd logit on a late token must not be the first thing
        // to touch the workspace.
        let shapes = [Shape::Continuous, Shape::Quantized, Shape::WithInf];
        for shape in shapes {
            let base = make_logits(shape, QWEN_VOCAB, 0xACE);
            for sampler in every_mode() {
                let mut state = SamplerState::new(0xACE);
                let mut buf = base.clone();
                state.sample(&sampler, &mut buf);
                for _ in 0..2 {
                    buf.copy_from_slice(&base);
                    let (events, _) = counting::events_during(|| state.sample(&sampler, &mut buf));
                    assert_eq!(events, 0, "{shape:?} {sampler:?} allocated");
                }
            }
        }
        // A tie or an odd logit first appearing after a clean first token.
        let clean = make_logits(Shape::Continuous, QWEN_VOCAB, 0xACE);
        for late in [Shape::AllEqual, Shape::WithInf] {
            let later = make_logits(late, QWEN_VOCAB, 0xACE);
            for sampler in every_mode() {
                let mut state = SamplerState::new(0xACE);
                let mut buf = clean.clone();
                state.sample(&sampler, &mut buf);
                buf.copy_from_slice(&later);
                let (events, _) = counting::events_during(|| state.sample(&sampler, &mut buf));
                assert_eq!(events, 0, "late {late:?} {sampler:?} allocated");
            }
        }
    }

    /// Live (not `-inf`) logits left by the oracle's filters.
    fn live_after(base: &[f32], k: Option<usize>, p: Option<f32>) -> usize {
        let mut v = base.to_vec();
        if let Some(k) = k {
            legacy::top_k_filter(&mut v, k);
        }
        if let Some(p) = p {
            legacy::top_p_filter(&mut v, p);
        }
        v.iter().filter(|&&x| x != f32::NEG_INFINITY).count()
    }

    #[test]
    fn one_softmax_pass_per_token_every_mode() {
        // Per token: one max reduction, and one exp per live candidate
        // (the vocabulary, or the top-k set). At T != 1 a top-p pass adds
        // one exp per kept token for the temperature-scaled draw; no
        // value is exponentiated twice for the same purpose.
        for shape in [Shape::Continuous, Shape::Peaked, Shape::Quantized] {
            let base = make_logits(shape, QWEN_VOCAB, 0x50F7);
            // every_mode() uses k = 40 and p = 0.9 throughout.
            let live = live_after(&base, None, None);
            let top_k = live_after(&base, Some(40), None);
            let top_p = live_after(&base, None, Some(0.9));
            let both = live_after(&base, Some(40), Some(0.9));
            for sampler in every_mode() {
                let (maxes, exps) = match sampler {
                    Sampler::Greedy => (0, 0),
                    Sampler::Temperature { .. } => (1, live),
                    Sampler::TopK { k, .. } => {
                        assert_eq!(k, 40);
                        (1, top_k)
                    }
                    Sampler::TopP { temperature, p } => {
                        assert_eq!(p, 0.9);
                        (1, live + if temperature == 1.0 { 0 } else { top_p })
                    }
                    Sampler::TopKTopP { temperature, k, p } => {
                        assert_eq!((k, p), (40, 0.9));
                        (1, top_k + if temperature == 1.0 { 0 } else { both })
                    }
                };
                let mut state = SamplerState::new(0x50F7);
                let mut buf = base.clone();
                state.sample(&sampler, &mut buf);
                for token in 1..=2 {
                    buf.copy_from_slice(&base);
                    let _ = probe::take();
                    state.sample(&sampler, &mut buf);
                    let (e, m) = probe::take();
                    assert_eq!(
                        m, maxes,
                        "{shape:?} {sampler:?} token {token}: max reductions"
                    );
                    assert_eq!(e, exps, "{shape:?} {sampler:?} token {token}: expf calls");
                }
            }
        }
    }

    /// Digest of main's token streams (c69438a2), printed by
    /// `sampler_bench::per_token_counts` on that revision.
    const MAIN_STREAM_DIGEST: u64 = 0x48c8_3e6b_a059_9de0;

    #[test]
    fn token_streams_match_main_digest() {
        let (digest, each) = crate::slm::sampler_bench::stream_digests(QWEN_VOCAB);
        if digest != MAIN_STREAM_DIGEST {
            for (label, d) in each.iter() {
                std::println!("{label}: {d:#018x}");
            }
        }
        assert_eq!(
            digest, MAIN_STREAM_DIGEST,
            "token streams differ from main's"
        );
    }
}
