//! 27-feature vector extraction for the ML eviction policies.
//!
//! Mirrors `FeatureExtractor.extract_candidate_features` in the sibling
//! `slm-os-page-sim/src/features/extractor.py` — 15 per-block features
//! followed by 12 global features.
//!
//! ## Approximations vs the Python reference
//!
//! - **Time units**: the simulator uses a logical tick counter that
//!   advances on every access. Our runtime stores `load_time` /
//!   `last_access_time` in nanoseconds from `kernel_ffi::get_time_ns`.
//!   We normalise both time deltas against [`AI_HORIZON_NS`] (1 second
//!   wall-clock) to keep the values in the simulator's training range.
//!   Without this scaling the int8 MLP saturates immediately — see
//!   `docs/eviction.md` for the rationale.
//! - **Access pattern** (feature 13): our `BlockMeta` does not carry
//!   the simulator's `AccessPattern` enum. We default to 0
//!   (Sequential) — a neutral value whose downstream effect is
//!   constant across candidates in a given decision.
//! - **Model priority / active-inferences** (features 11, 12): drawn
//!   from `BlockMeta.model_priority` and a caller-supplied table
//!   (`SlmHeuristicPolicy::set_active_inferences`) when available;
//!   otherwise zero.
//! - **Global features**: computed from
//!   [`mm::weight_pool_stats`] / [`mm::workspace_pool_stats`] for the
//!   utilisation signals; the rest (avg priority, deadline pressure,
//!   fault rate, hot-swap state, requesting-block identity) are
//!   zeroed in M4 and will populate in Phase AI-Sched when the
//!   scheduler feed is wired up.

use alloc::vec::Vec;

use crate::kernel_ffi;
use crate::mm::{weight_pool_stats, workspace_pool_stats};

use super::policy::{BlockFeatures, BlockMeta, PoolType};

extern "C" {
    /// Number of loaded models in the model-loader registry.
    /// Used to populate feature slot 17 (num_loaded_models).
    ///
    /// Lock-ordering contract: `extract_features` (and therefore
    /// `rust_model_count`) MUST NOT be called while the model-mem
    /// `LOCK` is held. The current call site at
    /// `model_mem::evict_and_retry` releases the pool lock before
    /// calling `select_victim` → `extract_features`; if a future edit
    /// inverts that ordering, this FFI call will deadlock against
    /// `rust_model_count`'s own loader-registry lock.
    fn rust_model_count() -> u32;
}

/// 1 second, in nanoseconds. Used to normalise `time_since_access`
/// and `time_since_load` into the [0, 1]-ish range the simulator
/// trained on.
pub const AI_HORIZON_NS: u64 = 1_000_000_000;

/// One logical simulator tick, expressed in the runtime's ns time base.
/// The sibling FeatureExtractor uses `horizon_window = 1000` ticks; the
/// runtime normalises against `AI_HORIZON_NS`. Driving block timestamps
/// (and the `now` reference) in multiples of this value makes the
/// runtime's normalised `time_since` equal the simulator's
/// `tick_diff / 1000` — required for the #979 trace-replay quality
/// comparison, where a tight replay loop would otherwise make real-ns
/// `time_since` collapse to ~0 and flatten the recency feature.
pub const SIM_TICK_NS: u64 = AI_HORIZON_NS / 1000; // 1_000_000 ns

/// Logical-clock override for the trace-replay harness (#979). When
/// non-zero, `extract_features` uses this as `now` instead of wall-clock
/// time, so feature-time stays on the simulator's tick base. Zero (the
/// default) means "use real time" — production behaviour is unchanged.
static CLOCK_OVERRIDE_NS: core::sync::atomic::AtomicU64 =
    core::sync::atomic::AtomicU64::new(0);

/// Set the feature-extraction clock override (ns). Pass 0 to disable.
pub fn set_clock_override(ns: u64) {
    CLOCK_OVERRIDE_NS.store(ns, core::sync::atomic::Ordering::Relaxed);
}

/// Clear the clock override (revert to wall-clock).
pub fn clear_clock_override() {
    CLOCK_OVERRIDE_NS.store(0, core::sync::atomic::Ordering::Relaxed);
}

/// Current feature-extraction clock: the override if set, else wall-clock.
fn eviction_now() -> u64 {
    let o = CLOCK_OVERRIDE_NS.load(core::sync::atomic::Ordering::Relaxed);
    if o != 0 {
        o
    } else {
        kernel_ffi::get_time_ns()
    }
}

/// Canonical feature-name array matching the sibling project's
/// `FeatureConfig.feature_names` (15 per-block + 12 global = 27).
/// Used by `eviction features` shell command for runtime introspection
/// (#112). Names are listed in index order so `FEATURE_NAMES[i]`
/// documents what `BlockFeatures[i]` represents.
// Names mirror page-sim's `FeatureConfig.feature_names` exactly — the
// generated `xgb_policy_generated.rs` const-asserts this array against
// its `GENERATED_FEATURE_NAMES` (see #952 Phase 2 and
// `docs/eviction-xgboost-graduation.md` §Feature schema invariants).
// Six labels were renamed 2026-05-17 to reconcile pre-existing drift:
//   gpu_mapped               -> is_gpu_mapped
//   layer_position           -> layer_idx_norm
//   predicted_reuse_distance -> predicted_reuse_dist
//   weight_pool_utilization  -> weight_pool_util
//   workspace_pool_utilization -> workspace_pool_util
//   total_gpu_mapped_ratio   -> total_gpu_mapped
// The renames are label-only; indices and meaning are unchanged.
pub const FEATURE_NAMES: [&str; 27] = [
    // Per-block features (0..14)
    "recency_rank",
    "frequency_rank",
    "access_count",
    "time_since_access",
    "time_since_load",
    "ref_count",
    "is_gpu_mapped",
    "pool_type",
    "is_dirty",
    "layer_idx_norm",
    "model_priority",
    "model_active_inferences",
    "access_pattern",
    "predicted_reuse_dist",
    "eviction_cost",
    // Global features (15..26)
    "weight_pool_util",
    "workspace_pool_util",
    "num_loaded_models",
    "total_gpu_mapped",
    "pending_loads",
    "avg_model_priority",
    "max_deadline_pressure",
    "recent_fault_rate",
    "hot_swap_active",
    "req_block_pool",
    "req_block_model_id",
    "req_block_priority",
];

/// Maximum layer index used for normalisation (matches the sibling's
/// approximate cap — exact normalisation happens later in the
/// simulator's `FeatureNormalizer`). Negative layer indices map to 0.
const LAYER_NORM_MAX: f32 = 32.0;

/// Constants mirroring `FeatureNormalizer.normalize` in the sibling
/// project. The trained int8 MLP and XGBoost saw NORMALISED features,
/// not the raw counts `extract_block_features` produces. Without
/// these divisions the int8 L1 quantiser saturates immediately and
/// the two models' decisions diverge.
const MAX_PRIORITY: f32 = 7.0;
const MAX_MODELS: f32   = 8.0;

/// Soft ceiling used to normalise count-style features via
/// `log1p(raw) / log1p(ACCESS_COUNT_CEILING)`. Matches the sibling's
/// running-max approach with a fixed denominator: we don't maintain
/// stats across calls, so a conservative upper bound keeps the output
/// in [0, 1]-ish territory for any reasonable access count.
const ACCESS_COUNT_CEILING: f32 = 1024.0;
const ACTIVE_INFERENCES_CEILING: f32 = 10.0;

/// Build a feature matrix for a set of eviction candidates.
///
/// Each row is the concatenation of per-block + global features, for
/// a total of 27 f32 values — matches the trained XGBoost / MLP
/// input shape. The function queries
/// [`mm::weight_pool_stats`] / [`mm::workspace_pool_stats`] once per
/// call and shares the result across all candidate rows.
pub fn extract_features(candidates: &[BlockMeta]) -> Vec<BlockFeatures> {
    if candidates.is_empty() {
        return Vec::new();
    }
    let now = eviction_now();

    // Global features (shared across candidates).
    let weight = weight_pool_stats();
    let workspace = workspace_pool_stats();
    let weight_util = if weight.total_blocks > 0 {
        weight.allocated_blocks as f32 / weight.total_blocks as f32
    } else { 0.0 };
    let workspace_util = if workspace.total_blocks > 0 {
        workspace.allocated_blocks as f32 / workspace.total_blocks as f32
    } else { 0.0 };
    let total_gpu_mapped = candidates
        .iter()
        .filter(|c| c.gpu_mapped)
        .count() as f32;

    // recency / frequency ranks — computed over the candidate set.
    // Sort-once indices by last_access_time (ascending → older first)
    // and by access_count (ascending → less-used first). We assign
    // ranks 0..n-1 in that order. Equal values share rank with the
    // first-seen index (matches Python's stable sort).
    let n = candidates.len();
    let mut recency_order: Vec<usize> = (0..n).collect();
    recency_order.sort_by_key(|&i| candidates[i].last_access_time);
    let mut frequency_order: Vec<usize> = (0..n).collect();
    frequency_order.sort_by_key(|&i| candidates[i].access_count);

    let mut recency_rank = alloc::vec![0u32; n];
    for (rank, &idx) in recency_order.iter().enumerate() {
        recency_rank[idx] = rank as u32;
    }
    let mut frequency_rank = alloc::vec![0u32; n];
    for (rank, &idx) in frequency_order.iter().enumerate() {
        frequency_rank[idx] = rank as u32;
    }

    let rank_denom = ((n as i32) - 1).max(1) as f32;
    let gpu_denom = (n as f32).max(1.0);

    candidates
        .iter()
        .enumerate()
        .map(|(i, b)| build_row(
            b,
            recency_rank[i],
            frequency_rank[i],
            now,
            weight_util,
            workspace_util,
            total_gpu_mapped,
            rank_denom,
            gpu_denom,
        ))
        .collect()
}

/// `log(1 + x)` — `libm::log1pf` in no_std. Used for count normalisation.
#[inline]
fn log1pf(x: f32) -> f32 { libm::log1pf(x) }

/// Assemble one 27-wide feature vector. Keeps feature ordering pinned
/// to the PER_BLOCK_FEATURES + GLOBAL_FEATURES layout in the sibling's
/// `extractor.py`; changing the order breaks model compatibility.
#[allow(clippy::too_many_arguments)]
fn build_row(
    b: &BlockMeta,
    recency_rank: u32,
    frequency_rank: u32,
    now: u64,
    weight_util: f32,
    workspace_util: f32,
    total_gpu_mapped: f32,
    rank_denom: f32,
    gpu_denom: f32,
) -> BlockFeatures {
    let time_since_access =
        saturating_sub_u64(now, b.last_access_time) as f32 / AI_HORIZON_NS as f32;
    let time_since_load =
        saturating_sub_u64(now, b.load_time) as f32 / AI_HORIZON_NS as f32;

    let layer_norm = if b.layer_idx < 0 {
        0.0
    } else {
        (b.layer_idx as f32 / LAYER_NORM_MAX).min(1.0)
    };

    let pool_type_f = match b.pool_type {
        PoolType::Weight => 0.0,
        PoolType::Workspace => 1.0,
    };

    // Normalised per-block features (15 values). See FeatureNormalizer
    // in the sibling project — the trained models assume these
    // post-normalisation shapes.
    let access_count_norm =
        log1pf(b.access_count as f32) / log1pf(ACCESS_COUNT_CEILING);

    let mut row: BlockFeatures = [0.0; 27];
    row[0]  = recency_rank as f32 / rank_denom;         // [0, 1]
    row[1]  = frequency_rank as f32 / rank_denom;       // [0, 1]
    row[2]  = access_count_norm;                         // log1p-normalised
    row[3]  = time_since_access;                         // already /horizon
    row[4]  = time_since_load;                           // already /horizon
    row[5]  = b.ref_count as f32;                        // raw small int (bounded by MAX_TASKS)
    row[6]  = if b.gpu_mapped { 1.0 } else { 0.0 };
    row[7]  = pool_type_f;
    row[8]  = if b.is_dirty { 1.0 } else { 0.0 };
    row[9]  = layer_norm;
    row[10] = b.model_priority as f32 / MAX_PRIORITY;    // [0, 1]
    // #122: wire slot 11 from the global active-inferences table (#113).
    row[11] = log1pf(super::slm_heuristic::get_active(b.model_id) as f32)
              / log1pf(ACTIVE_INFERENCES_CEILING);
    // access_pattern, normalised by the max enum value (3 = BURST), mirroring
    // the sibling FeatureNormalizer. Threaded through BlockMeta as of #979.
    row[12] = (b.access_pattern as f32) / 3.0;
    row[13] = predicted_reuse_heuristic(b, time_since_access, layer_norm);
    row[14] = compute_eviction_cost(b);

    // Normalised global features (12 values).
    row[15] = weight_util;                               // already [0, 1]
    row[16] = workspace_util;                            // already [0, 1]
    // #122: wire slot 17 from the model loader registry.
    row[17] = unsafe { rust_model_count() as f32 / MAX_MODELS };
    row[18] = total_gpu_mapped / gpu_denom;              // [0, 1]
    row[19] = 0.0;  // pending_loads — log1p-normalised when wired up
    row[20] = 0.0;  // avg_model_priority / MAX_PRIORITY
    row[21] = 0.0;  // max_deadline_pressure
    row[22] = 0.0;  // recent_fault_rate
    row[23] = 0.0;  // hot_swap_active
    row[24] = 0.0;  // req_block_pool
    row[25] = 0.0;  // req_block_model_id
    row[26] = 0.0;  // req_block_priority

    // MAX_MODELS and ACTIVE_INFERENCES_CEILING now consumed above (#122).

    row
}

// AccessPattern enum values (mirror the sibling `AccessPattern` IntEnum
// in slm-os-page-sim `src/simulator/block.py`).
const AP_SEQUENTIAL: u8 = 0;
const AP_RANDOM: u8 = 1;
const AP_STRIDED: u8 = 2;
const AP_BURST: u8 = 3;

/// Heuristic estimate of reuse distance — mirrors the Python
/// `_predict_reuse_heuristic` (extractor.py:192) branch-for-branch.
/// `time_since_access_normalised` is `time_since / horizon`, so the
/// Python `time_since / (horizon * k)` terms become `tsn / k`. Now that
/// `access_pattern` is threaded through `BlockMeta` (#979), all four
/// branches are active rather than collapsing to Sequential.
fn predicted_reuse_heuristic(
    b: &BlockMeta,
    time_since_access_normalised: f32,
    _layer_norm: f32,
) -> f32 {
    let tsn = time_since_access_normalised;
    match b.access_pattern {
        AP_SEQUENTIAL => tsn.clamp(0.0, 1.0),
        // Burst: time_since / (horizon * 0.1) = tsn / 0.1 = tsn * 10,
        // clamped, then halved (low reuse distance for burst).
        AP_BURST => (tsn * 10.0).min(1.0) * 0.5,
        // Strided: time_since / (horizon * 0.5) = tsn * 2, clamped.
        AP_STRIDED => (tsn * 2.0).min(1.0),
        // Random (and any unknown value): no predictable reuse.
        AP_RANDOM => 0.8,
        _ => 0.8,
    }
}

/// Normalised eviction cost — mirrors the Python `_compute_eviction_cost`.
fn compute_eviction_cost(b: &BlockMeta) -> f32 {
    let mut cost = 0.0_f32;
    if b.is_dirty    { cost += 0.3; }
    if b.gpu_mapped  { cost += 0.5; }
    if b.pool_type == PoolType::Workspace {
        cost *= 0.5;
    }
    (cost + 0.2).min(1.0)
}

fn saturating_sub_u64(a: u64, b: u64) -> u64 {
    if a >= b { a - b } else { 0 }
}
