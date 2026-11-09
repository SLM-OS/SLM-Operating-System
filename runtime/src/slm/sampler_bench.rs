//! Host measurements of the sampler (#522), test-only.
//!
//! The same file runs against main and against later revisions, so it
//! uses only the sampler's public API (`Sampler`, `SamplerState`), the
//! test counting allocator and the `sampler::probe` counters. Two
//! ignored reports and the digest the identity test checks:
//!
//! - [`per_token_time`]: time per `sample` call, every mode, at vocab
//!   151936, on several logit shapes. Prints no revision-specific text,
//!   so runs from two revisions can be compared blind.
//! - [`per_token_counts`]: `expf` calls, max reductions and heap events
//!   per token after the first, and a digest of the token streams.
//! - [`stream_digests`]: `sampler::tests::token_streams_match_main_digest`
//!   requires the digest main's sampler produced (c69438a2), so every
//!   mode keeps main's token sequence for the same seeds and logits.
//!
//! Run the reports with:
//! `cargo test --release --target x86_64-unknown-linux-gnu --features slm \
//!  slm::sampler_bench -- --ignored --nocapture --test-threads=1`

use super::sampler::{probe, tests::counting, Sampler, SamplerState};
use alloc::vec::Vec;

const VOCAB: usize = 151_936;

fn xorshift64(s: &mut u64) -> u64 {
    let mut x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    x
}

fn unit(s: &mut u64) -> f32 {
    ((xorshift64(s) >> 40) as f32) / ((1u32 << 24) as f32)
}

/// Logit shapes for the reports.
#[derive(Clone, Copy, Debug)]
enum Shape {
    /// Distinct values, roughly normal in [-10, 10].
    Continuous,
    /// One token at 12 over a plateau near -40.
    Peaked,
    /// Fifty tokens 8 to 20 above a continuous body, so top-p keeps a
    /// few to a few dozen tokens, as in a language model's output.
    Head,
    /// Continuous snapped to a 1/8 grid: ties at every cut.
    TieHeavy,
}

const SHAPES: [Shape; 4] = [
    Shape::Continuous,
    Shape::Peaked,
    Shape::Head,
    Shape::TieHeavy,
];

fn make_logits(shape: Shape, n: usize, seed: u64) -> Vec<f32> {
    let mut s = seed | 1;
    let mut v: Vec<f32> = (0..n)
        .map(|_| (unit(&mut s) + unit(&mut s) + unit(&mut s) - 1.5) * 6.67)
        .collect();
    match shape {
        Shape::Continuous => {}
        Shape::Peaked => {
            for x in v.iter_mut() {
                *x -= 40.0;
            }
            let at = (xorshift64(&mut s) as usize) % n;
            v[at] = 12.0;
        }
        Shape::Head => {
            for _ in 0..50.min(n) {
                let at = (xorshift64(&mut s) as usize) % n;
                v[at] += 8.0 + 12.0 * unit(&mut s);
            }
        }
        Shape::TieHeavy => {
            for x in v.iter_mut() {
                *x = libm::roundf(*x * 8.0) / 8.0;
            }
        }
    }
    v
}

/// Every mode at temperature `t`, with the demo-default k and p.
fn modes(t: f32) -> [(&'static str, Sampler); 4] {
    [
        ("Temperature", Sampler::Temperature { temperature: t }),
        (
            "TopK k=40",
            Sampler::TopK {
                temperature: t,
                k: 40,
            },
        ),
        (
            "TopP p=0.9",
            Sampler::TopP {
                temperature: t,
                p: 0.9,
            },
        ),
        (
            "TopKTopP k=40 p=0.9",
            Sampler::TopKTopP {
                temperature: t,
                k: 40,
                p: 0.9,
            },
        ),
    ]
}

/// (label, sampler) for every case the reports cover.
fn cases(temps: &[f32]) -> Vec<(std::string::String, Sampler)> {
    let mut out = Vec::new();
    out.push((std::string::String::from("Greedy"), Sampler::Greedy));
    for &t in temps {
        for (name, s) in modes(t) {
            out.push((std::format!("{name} T={t}"), s));
        }
    }
    out
}

/// Per-call time, every mode. Each trial is the mean of `ROUNDS` calls on
/// a warm state (one call made first, as in a generation); the report is
/// the median, min and max of `TRIALS` trial means, in microseconds.
#[test]
#[ignore = "timing report, not a pass/fail check"]
fn per_token_time() {
    const TRIALS: usize = 15;
    const ROUNDS: u32 = 20;
    std::println!(
        "sampler per-token time, vocab {VOCAB}, {TRIALS} trials x {ROUNDS} calls, \
         us: median [min - max] of trial means"
    );
    for shape in SHAPES {
        let base = make_logits(shape, VOCAB, 0x5EED);
        for (label, sampler) in cases(&[0.7, 1.0]) {
            let mut state = SamplerState::new(0x5EED);
            let mut buf = base.clone();
            std::hint::black_box(state.sample(&sampler, &mut buf));
            let mut means = [0f64; TRIALS];
            for m in means.iter_mut() {
                let mut total = 0u128;
                for _ in 0..ROUNDS {
                    buf.copy_from_slice(&base);
                    let t0 = std::time::Instant::now();
                    std::hint::black_box(state.sample(&sampler, std::hint::black_box(&mut buf)));
                    total += t0.elapsed().as_nanos();
                }
                *m = total as f64 / ROUNDS as f64 / 1000.0;
            }
            means.sort_by(|a, b| a.partial_cmp(b).unwrap());
            std::println!(
                "{:<11} {:<28} {:>9.1} [{:>9.1} - {:>9.1}]",
                std::format!("{shape:?}"),
                label,
                means[TRIALS / 2],
                means[0],
                means[TRIALS - 1]
            );
        }
    }
}

/// FNV-1a over 64-bit words.
struct Fnv(u64);

impl Fnv {
    fn new() -> Self {
        Fnv(0xcbf2_9ce4_8422_2325)
    }
    fn word(&mut self, w: u64) {
        for b in w.to_le_bytes() {
            self.0 ^= u64::from(b);
            self.0 = self.0.wrapping_mul(0x0000_0100_0000_01b3);
        }
    }
}

const SEEDS: [u64; 5] = [1, 0xCAFE, 0xDEAD_BEEF, 0x1234_5678_9ABC_DEF0, 0x5EED];
const STREAM_TOKENS: usize = 2;

/// Digest of `STREAM_TOKENS` tokens and the final PRNG state for every
/// shape, case and seed, plus one digest per (shape, case) for locating a
/// difference. Each token of a stream gets fresh logits, so a stream
/// crosses tied and clean cuts as a generation would.
pub(crate) fn stream_digests(n: usize) -> (u64, Vec<(std::string::String, u64)>) {
    let mut all = Fnv::new();
    let mut each = Vec::new();
    for shape in SHAPES {
        let streams: Vec<Vec<Vec<f32>>> = SEEDS
            .iter()
            .map(|&seed| {
                (0..STREAM_TOKENS as u64)
                    .map(|t| make_logits(shape, n, seed ^ (t.wrapping_mul(0x9E37_79B9_7F4A_7C15))))
                    .collect()
            })
            .collect();
        for (label, sampler) in cases(&[0.7, 1.0, 1.3]) {
            let mut h = Fnv::new();
            for (si, &seed) in SEEDS.iter().enumerate() {
                let mut state = SamplerState::new(seed);
                for logits in streams[si].iter() {
                    let mut buf = logits.clone();
                    h.word(u64::from(state.sample(&sampler, &mut buf)));
                }
                h.word(state.seed);
            }
            all.word(h.0);
            each.push((std::format!("{shape:?} {label}"), h.0));
        }
    }
    (all.0, each)
}

/// Per token after the first: `expf` calls, max reductions and heap
/// events, for every shape and case; then the stream digests.
#[test]
#[ignore = "measurement report, not a pass/fail check"]
fn per_token_counts() {
    const TOKENS: usize = 4;
    std::println!(
        "sampler per-token counts, vocab {VOCAB}, mean of {TOKENS} calls after the first: \
         expf calls, max reductions, heap events"
    );
    for shape in SHAPES {
        let base = make_logits(shape, VOCAB, 0x5EED);
        for (label, sampler) in cases(&[0.7, 1.0, 1.3]) {
            let mut state = SamplerState::new(0x5EED);
            let mut buf = base.clone();
            state.sample(&sampler, &mut buf);
            let (mut exps, mut maxes, mut heap) = (0usize, 0usize, 0usize);
            for _ in 0..TOKENS {
                buf.copy_from_slice(&base);
                let _ = probe::take();
                let (events, _) = counting::events_during(|| state.sample(&sampler, &mut buf));
                let (e, m) = probe::take();
                exps += e;
                maxes += m;
                heap += events;
            }
            std::println!(
                "{:<11} {:<28} expf {:>9.1}  max {:>4.1}  heap {:>4.1}",
                std::format!("{shape:?}"),
                label,
                exps as f64 / TOKENS as f64,
                maxes as f64 / TOKENS as f64,
                heap as f64 / TOKENS as f64
            );
        }
    }
    let (digest, each) = stream_digests(VOCAB);
    for (label, d) in each.iter() {
        std::println!("digest {label}: {d:#018x}");
    }
    std::println!("digest all: {digest:#018x}");
}
