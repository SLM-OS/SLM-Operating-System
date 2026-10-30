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
//! When the operator flips both `gpu use inference on` (master) and
//! `gpu use eviction on`, [`MlpPolicy`] tries
//! [`slm_gpu_run_eviction_inference`] for each candidate before
//! falling back to the CPU path. The dispatch consumes the v6 channel
//! handoff staged by `scripts/gpu-kernel-eviction-qnet.c
//! --preserve-for-kexec` and inherited across kexec. Dispatch failure
//! (no producer ran, rate-limit window, try-lock contention, or any
//! downstream GA10B error) silently falls back to CPU per-candidate;
//! `eviction stats` reports attempts / ok / fallback (rate-limited)
//! so the two cases can be told apart. GPU routing additionally
//! requires the compiled-in models (`EVICTION_MODELS=ON`) with no
//! runtime MLP blob active — see `MlpPolicy::gpu_route`.
//!
//! **v1 limitation: per-candidate dispatch.** This module dispatches
//! the GPU once per candidate (8 dispatches per `select_victim` on
//! the standard candidate set), not once for all candidates. Each
//! dispatch is gated by the system-wide 50 ms rate limiter
//! (`g_eviction_dispatch_last_ns` in `slm_ffi.c`), so the common
//! pattern in practice is "at most 1 GPU dispatch per 50 ms, every
//! other candidate on CPU" — i.e. 1 GPU + 7 CPU fallbacks for a
//! `select_victim` that opens a fresh window, 0 + 8 for one inside
//! it. The bench latency consequently reflects this mixed-mode cost
//! rather than a pure all-GPU number, and the argmax compares an
//! fp32 GPU score against int8 CPU scores. Batching
//! 8 candidates × 27 features into a single dispatch is the proper
//! shape and is tracked in #1020.
//!
//! **v1 limitation: dispatch runs under the allocator and registry
//! locks.** `select_victim` is reached from
//! `model_mem::evict_and_retry` with the pool allocator `SpinGuard`
//! held, and through `registry::with_active_policy_for_pool` with
//! `REGISTRY_LOCK` held. A GPU-routed candidate therefore takes
//! `g_gpu_dispatch_lock` (try-lock, IRQs off) and busy-polls GA10B
//! inside both spinlocks — up to the 2 s `ga10b_submit_and_poll`
//! timeout when GR faults (the 2026-05-17 FECS_ERROR_PENDING case),
//! during which every other CPU allocating model memory spins. The
//! existing mitigations bound frequency, not duration: at most one
//! dispatch per 50 ms window, try-lock instead of a blocking acquire
//! (no deadlock), and the #552 circuit breaker after repeated
//! failures. The path is reachable only with both opt-in toggles on,
//! which requires a GPU. Moving the GPU scoring outside both locks
//! (score a snapshot, then re-validate under the lock) is part of the
//! #1020 restructure.

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
    /// runs, because per-candidate dispatch mixes GPU and CPU scores
    /// inside one argmax. The PR-5 producer bakes the compiled-in
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
        let use_gpu = self.gpu_route();

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
        let use_gpu = self.gpu_route();
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
    fn has_gpu_backend(&self) -> bool { self.gpu_capable }
}
