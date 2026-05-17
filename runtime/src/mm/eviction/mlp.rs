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
//! ## GPU dispatch (PR-6 of docs/design/gpu-policy-models.md)
//!
//! When the user flips `gpu use eviction on`, [`MlpPolicy`] tries
//! [`slm_gpu_run_eviction_inference`] for each candidate before
//! falling back to the CPU path. The dispatch consumes the v6 channel
//! handoff staged by `scripts/gpu-kernel-eviction-qnet.c
//! --preserve-for-kexec` and inherited across kexec. Dispatch failure
//! (no producer ran, rate-limit window, try-lock contention, or any
//! downstream GA10B error) silently falls back to CPU per-candidate.
//!
//! **v1 limitation: per-candidate dispatch.** This module dispatches
//! the GPU once per candidate (8 dispatches per `select_victim` on
//! the standard candidate set), not once for all candidates. Each
//! dispatch is gated by the system-wide 50 ms rate limiter
//! (`g_eviction_dispatch_last_ns` in `slm_ffi.c`), so the common
//! pattern in practice is "1 GPU dispatch succeeds + remaining
//! fall back to CPU". The bench latency consequently reflects this
//! mixed-mode cost rather than a pure all-GPU number. Batching
//! 8 candidates × 27 features into a single dispatch is the proper
//! shape and is tracked as the follow-up work on top of PR-6.

use alloc::vec::Vec;

use super::features::extract_features;
use super::generated;
use super::{active_blob, blob_status, BlobKind, RuntimeMlpModel};
use super::policy::{BlockMeta, EvictionPolicy};

extern "C" {
    /// `int slm_gpu_run_eviction_inference(const void *features_bytes,
    ///                                      size_t features_bytes_len,
    ///                                      void *score_out);`
    ///
    /// SAFETY (FFI contract from kernel/include/slm_ffi.h):
    /// - `features_bytes` must point to `features_bytes_len` valid bytes
    ///   (typically 27 × 4 = 108).
    /// - `score_out` must point to ≥ 4 writable bytes.
    /// - Returns 0 on success (caller may read 4 fp32 bytes from
    ///   `score_out`), negative rc on failure (output untouched).
    /// - On non-Jetson platforms returns -1 unconditionally.
    fn slm_gpu_run_eviction_inference(
        features_bytes: *const f32,
        features_bytes_len: usize,
        score_out: *mut f32,
    ) -> i32;

    /// `int gpu_consumer_enabled(GPU_CONSUMER_EVICTION)` wrapped as
    /// a Rust-friendly extern. Defined in `kernel/src/slm_ffi.c` for
    /// this purpose (sched checks the same flag directly in C; the
    /// eviction MLP needs a Rust-callable predicate).
    fn slm_gpu_consumer_eviction_enabled() -> i32;
}

/// `MlpPolicy`'s "should we even try GPU?" gate. Cheap atomic-bool
/// read in C; mirrored from the sched-side `gpu_consumer_enabled
/// (GPU_CONSUMER_SCHED)` pattern. False on any non-Jetson build.
fn eviction_gpu_eligible() -> bool {
    unsafe { slm_gpu_consumer_eviction_enabled() != 0 }
}

/// Try a GPU dispatch for one candidate's 27-element feature row.
/// Returns `Some(score)` on success, `None` on any failure (caller
/// falls back to CPU).
fn try_gpu_score(row: &super::policy::BlockFeatures) -> Option<f32> {
    let mut score = 0.0_f32;
    let rc = unsafe {
        slm_gpu_run_eviction_inference(
            row.as_ptr(),
            core::mem::size_of_val(row),
            &mut score as *mut f32,
        )
    };
    if rc == 0 { Some(score) } else { None }
}

#[derive(Clone)]
struct RuntimeCache {
    checksum: u32,
    model: RuntimeMlpModel,
}

#[derive(Default)]
pub struct MlpPolicy {
    runtime_cache: Option<RuntimeCache>,
}

impl MlpPolicy {
    pub fn new() -> Self { Self { runtime_cache: None } }

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

    /// CPU-only scoring path. Routes through the runtime blob if
    /// loaded (`MLP1` payload from `eviction model load mlp ...`),
    /// otherwise the compiled-in int8 generator.
    fn score_row_cpu(&self, row: &super::policy::BlockFeatures) -> f32 {
        if let Some(cache) = self.runtime_cache.as_ref() {
            return cache.model.predict(row);
        }
        generated::mlp_predict(row)
    }

    /// Score one row, trying GPU dispatch first when the toggle is
    /// on. Per-candidate dispatch (see module docs for v1
    /// limitations). On any GPU failure — including the common
    /// "rate-limit window after a successful dispatch" case — the
    /// caller silently falls back to CPU for THIS candidate, while
    /// other candidates in the same `select_victim` invocation may
    /// still succeed on the GPU path. The bench numbers reflect
    /// this mixed-mode behavior.
    fn score_row(&self, row: &super::policy::BlockFeatures, use_gpu: bool) -> f32 {
        if use_gpu {
            if let Some(score) = try_gpu_score(row) {
                return score;
            }
        }
        self.score_row_cpu(row)
    }
}

impl EvictionPolicy for MlpPolicy {
    fn select_victim(&mut self, candidates: &[BlockMeta]) -> usize {
        debug_assert!(
            !candidates.is_empty(),
            "MlpPolicy select_victim on empty list"
        );
        self.refresh_runtime_cache();
        let features = extract_features(candidates);

        // Sample the toggle once at the top of the call so all
        // candidates use a consistent eligibility decision. The
        // per-candidate dispatch inside the loop still rate-limits
        // independently — when the rate limit kicks in mid-loop,
        // remaining candidates fall back to CPU per-candidate (the
        // try_gpu_score path returns None silently).
        let use_gpu = eviction_gpu_eligible();

        let mut best = 0usize;
        let mut best_score = f32::MIN;
        for (i, row) in features.iter().enumerate() {
            let s = self.score_row(row, use_gpu);
            if s > best_score {
                best_score = s;
                best = i;
            }
        }
        best
    }

    fn score(&mut self, candidates: &[BlockMeta]) -> Vec<f32> {
        self.refresh_runtime_cache();
        let use_gpu = eviction_gpu_eligible();
        let features = extract_features(candidates);
        features
            .iter()
            .map(|row| self.score_row(row, use_gpu))
            .collect()
    }

    fn name(&self) -> &'static str { "MLP" }

    /// PR-6: signal that this policy has a GPU dispatch path. The
    /// registry's `any_active_policy_has_gpu_backend()` scan picks
    /// this up and the C-side `gpu_consumer_set(GPU_CONSUMER_EVICTION,
    /// true)` validation falls through to its "perf note" branch
    /// instead of the "scaffold only" warning. The dispatch itself
    /// still requires the producer (`scripts/gpu-kernel-eviction-qnet
    /// --preserve-for-kexec`) to have staged a v6 handoff before
    /// kexec; if no handoff is present, `slm_gpu_run_eviction_inference`
    /// silently returns -1 and `score_row` falls back to CPU.
    fn has_gpu_backend(&self) -> bool { true }
}
