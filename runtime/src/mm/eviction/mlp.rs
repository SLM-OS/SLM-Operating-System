//! Int8-MLP eviction policy.
//!
//! Mirrors [`XGBoostPolicy`] but routes each candidate's 27-feature
//! vector through [`super::generated::mlp_predict`] — the int8-
//! quantised 4-layer MLP imported from the sibling project
//! (27 → 64 → 32 → 16 → 1 with sigmoid on the output).
//!
//! The integer-arithmetic path is well over an order of magnitude
//! smaller than the XGBoost chain (~5 KB of weights vs ~140 KB of
//! generated code) and is the better fit for memory-constrained builds.
//!
//! When `ai_eviction_models` is OFF, `mlp_predict` is the stub that
//! returns 0.5 for any input — the policy reduces to a constant
//! "pick index 0" decision. See [`super::generated::MODELS_AVAILABLE`].
//!
//! ## GPU dispatch (PR-6 of docs/design/gpu-policy-models.md, #1020)
//!
//! When the operator flips both `gpu use inference on` (master) and
//! `gpu use eviction on`, [`MlpPolicy`] scores a decision's whole
//! candidate set with one [`gpu_score_batch`] call: one dispatch of the
//! batched pipeline staged by `scripts/gpu-kernel-eviction-qnet.c
//! --preserve-for-kexec` (default 64 rows) and inherited across kexec.
//! Either every candidate gets a GPU score or, on any failure (no
//! producer ran, 50 ms rate-limit window, try-lock contention, more
//! candidates than the producer's batch, a non-finite score, or any
//! downstream GA10B error), every candidate is scored on the CPU. One
//! argmax never mixes fp32 GPU and int8 CPU scores. `eviction stats`
//! reports attempts / ok / fallback (rate-limited, oversize) per
//! decision. GPU routing additionally requires the compiled-in models
//! (`EVICTION_MODELS=ON`) with no runtime MLP blob active — see
//! `MlpPolicy::gpu_route`.
//!
//! **Locking.** A decision through the registry
//! (`registry::select_victim`, the allocator's eviction path) runs the
//! dispatch with no lock held: the policy hands its feature rows out
//! under `REGISTRY_LOCK` ([`EvictionPolicy::gpu_batch_features`]), the
//! registry drops the lock and dispatches, then re-takes it and lets
//! the policy pick from the scores
//! ([`EvictionPolicy::finish_gpu_select`]). `model_mem::evict_and_retry`
//! already calls in without the allocator lock and re-validates the
//! victim's generation before freeing. The dispatch itself still runs
//! IRQs-off behind the GPU try-lock, bounded by the 2 s
//! `ga10b_submit_and_poll` timeout. Direct callers of
//! [`EvictionPolicy::select_victim`] (the bench harness) dispatch
//! inline.

use alloc::vec;
use alloc::vec::Vec;

use super::features::extract_features;
use super::generated;
use super::{active_blob, blob_status, BlobKind, RuntimeMlpModel};
use super::policy::{BlockFeatures, BlockMeta, EvictionPolicy};

extern "C" {
    /// `int slm_gpu_run_eviction_batch(const float *features,
    ///                                 size_t n_rows, float *scores_out);`
    ///
    /// SAFETY (FFI contract from kernel/include/slm_ffi.h):
    /// - `features` must point to `n_rows × 27` valid f32s (row-major).
    /// - `scores_out` must point to `n_rows` writable f32s.
    /// - Returns 0 on success (all `n_rows` scores written), negative
    ///   rc on failure (`scores_out` contents unspecified).
    /// - Never retains either pointer past the call.
    /// - On non-Jetson platforms counts the attempt and returns -1.
    fn slm_gpu_run_eviction_batch(
        features: *const f32,
        n_rows: usize,
        scores_out: *mut f32,
    ) -> i32;

    /// `int gpu_consumer_enabled(GPU_CONSUMER_EVICTION)` wrapped as
    /// a Rust-friendly extern. Defined in `kernel/src/slm_ffi.c` for
    /// this purpose (sched checks the same flag directly in C; the
    /// eviction MLP needs a Rust-callable predicate).
    fn slm_gpu_consumer_eviction_enabled() -> i32;

    /// Master `gpu use inference` toggle (also gates the MNIST
    /// fastpath in `inference/engine.rs`).
    ///
    /// SAFETY: no arguments; an acquire atomic-bool read in C with no
    /// side effects. Callable from any context.
    fn slm_gpu_inference_enabled() -> i32;
}

/// `MlpPolicy`'s "should we even try GPU?" gate: the master
/// `gpu use inference` toggle AND the per-consumer `gpu use eviction`
/// toggle, per the #965 spec. Either off → CPU only. Both are cheap
/// atomic-bool reads in C. Neither can be switched on when
/// `slm_gpu_available()` is 0, so this is false on non-Jetson
/// production builds (tests force the flags via a seam).
fn eviction_gpu_eligible() -> bool {
    unsafe {
        slm_gpu_inference_enabled() != 0
            && slm_gpu_consumer_eviction_enabled() != 0
    }
}

/// Score every row with one batched GPU dispatch. `Some` holds one
/// finite score per row; `None` on any failure, including a
/// non-finite score (a corrupted pipeline must not steer eviction),
/// in which case the caller scores every row on the CPU.
pub(crate) fn gpu_score_batch(rows: &[BlockFeatures]) -> Option<Vec<f32>> {
    if rows.is_empty() {
        return None;
    }
    let mut scores = vec![0.0_f32; rows.len()];
    // SAFETY: `[f32; 27]` rows are contiguous in the slice, so
    // `rows.as_ptr()` addresses `rows.len() × 27` valid f32s; `scores`
    // holds `rows.len()` writable f32s. Both outlive the call and C
    // keeps neither pointer.
    let rc = unsafe {
        slm_gpu_run_eviction_batch(
            rows.as_ptr() as *const f32,
            rows.len(),
            scores.as_mut_ptr(),
        )
    };
    if rc != 0 || !scores.iter().all(|s| s.is_finite()) {
        return None;
    }
    Some(scores)
}

/// Index of the highest score; ties go to the lowest index, and a NaN
/// never wins (matches the pre-#1020 per-candidate loop).
fn argmax(scores: &[f32]) -> usize {
    let mut best = 0usize;
    let mut best_score = f32::MIN;
    for (i, &s) in scores.iter().enumerate() {
        if s > best_score {
            best_score = s;
            best = i;
        }
    }
    best
}

#[derive(Clone)]
struct RuntimeCache {
    checksum: u32,
    model: RuntimeMlpModel,
}

pub struct MlpPolicy {
    runtime_cache: Option<RuntimeCache>,
    /// Whether this instance may route scores to the GPU. `true` for
    /// the standalone `mlp` policy; `false` for the expert embedded in
    /// CACHEUS, which reports `has_gpu_backend() == false` and so must
    /// not dispatch behind the operator's back (see [`Self::cpu_only`]).
    gpu_capable: bool,
}

impl Default for MlpPolicy {
    fn default() -> Self { Self::new() }
}

impl MlpPolicy {
    pub fn new() -> Self { Self { runtime_cache: None, gpu_capable: true } }

    /// CPU-only instance for composition inside CACHEUS. CACHEUS has
    /// no GPU backend in PR-6 (#965 diverges from its spec here): its
    /// `has_gpu_backend()` stays false, so `gpu use eviction on` warns
    /// "scaffold only" under CACHEUS, and this constructor keeps that
    /// warning truthful by never attempting a GPU dispatch.
    pub fn cpu_only() -> Self { Self { runtime_cache: None, gpu_capable: false } }

    fn refresh_runtime_cache(&mut self) {
        let active_checksum = blob_status(BlobKind::Mlp)
            .active
            .map(|meta| meta.checksum);
        if self.runtime_cache.as_ref().map(|c| c.checksum) == active_checksum {
            return;
        }

        if let Some(checksum) = active_checksum {
            self.runtime_cache = active_blob(BlobKind::Mlp)
                .and_then(|blob| super::runtime_mlp::parse_payload(&blob.payload).ok())
                .map(|model| RuntimeCache { checksum, model });
        } else {
            self.runtime_cache = None;
        }
    }

    /// Whether this call may try the GPU. Beyond the operator toggles,
    /// the CPU fallback must be computing the *same model* the GPU
    /// runs, so that a decision's victim does not depend on which path
    /// happened to serve it. The PR-5 producer bakes the compiled-in
    /// weights (`generated/mlp_policy_f32.rs`), so GPU routing is
    /// skipped when the CPU side would score with something else:
    /// the 0.5 stub (`EVICTION_MODELS=OFF`) or a runtime OTA blob
    /// (`eviction model load mlp ...`). What remains is fp32 (GPU)
    /// vs int8 (CPU) quantisation of one model.
    fn gpu_route(&self) -> bool {
        self.gpu_capable
            && generated::MODELS_AVAILABLE
            && self.runtime_cache.is_none()
            && eviction_gpu_eligible()
    }

    /// CPU-only scoring path. Routes through the runtime blob if
    /// loaded (`MLP1` payload from `eviction model load mlp ...`),
    /// otherwise the compiled-in int8 generator.
    fn score_row_cpu(&self, row: &BlockFeatures) -> f32 {
        if let Some(cache) = self.runtime_cache.as_ref() {
            return cache.model.predict(row);
        }
        generated::mlp_predict(row)
    }

    fn score_rows_cpu(&self, rows: &[BlockFeatures]) -> Vec<f32> {
        rows.iter().map(|row| self.score_row_cpu(row)).collect()
    }

    /// Score every row: one batched GPU dispatch when `use_gpu`, all
    /// rows on the CPU otherwise or if the dispatch fails. Never a
    /// mix of the two.
    fn score_rows(&self, rows: &[BlockFeatures], use_gpu: bool) -> Vec<f32> {
        if use_gpu {
            if let Some(scores) = gpu_score_batch(rows) {
                return scores;
            }
        }
        self.score_rows_cpu(rows)
    }
}

impl EvictionPolicy for MlpPolicy {
    /// Direct (non-registry) entry point: dispatches inline. The
    /// registry path goes through `gpu_batch_features` /
    /// `finish_gpu_select` instead so the dispatch runs unlocked.
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "MlpPolicy select_victim on empty list"
        );
        self.refresh_runtime_cache();
        let use_gpu = self.gpu_route();
        let features = extract_features(candidates);
        argmax(&self.score_rows(&features, use_gpu))
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        self.refresh_runtime_cache();
        let use_gpu = self.gpu_route();
        let features = extract_features(candidates);
        self.score_rows(&features, use_gpu)
    }

    fn gpu_batch_features(&mut self, candidates: &[BlockMeta])
        -> Option<Vec<BlockFeatures>>
    {
        self.refresh_runtime_cache();
        if candidates.is_empty() || !self.gpu_route() {
            return None;
        }
        Some(extract_features(candidates))
    }

    /// Never dispatches. GPU scores are used only when there is exactly
    /// one per candidate; anything else (dispatch failed, or a caller
    /// bug) scores the candidates on the CPU.
    fn finish_gpu_select(&mut self, candidates: &[BlockMeta],
                         gpu_scores: Option<&[f32]>) -> usize
    {
        debug_assert!(
            !candidates.is_empty(),
            "MlpPolicy finish_gpu_select on empty list"
        );
        if let Some(scores) = gpu_scores {
            if scores.len() == candidates.len() {
                return argmax(scores);
            }
        }
        self.refresh_runtime_cache();
        let features = extract_features(candidates);
        argmax(&self.score_rows_cpu(&features))
    }

    fn name(&self) -> &'static str { "MLP" }

    /// PR-6: signal that this policy has a GPU dispatch path. The
    /// registry's `any_active_policy_has_gpu_backend()` scan picks
    /// this up and the C-side `gpu_consumer_set(GPU_CONSUMER_EVICTION,
    /// true)` validation falls through to its "perf note" branch
    /// instead of the "scaffold only" warning. The dispatch itself
    /// still requires the producer (`scripts/gpu-kernel-eviction-qnet
    /// --preserve-for-kexec`) to have staged a v6 handoff before
    /// kexec; if no handoff is present, `slm_gpu_run_eviction_batch`
    /// silently returns -1 and every candidate is scored on the CPU.
    fn has_gpu_backend(&self) -> bool { self.gpu_capable }
}
