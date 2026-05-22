//! End-to-end storage-backed weight cache harness (#979).
//!
//! This module closes the gap between the eviction *policy* machinery
//! (`super::eviction`, `super::model_mem`) and a real storage reload
//! path. The production inference engine holds weights resident and
//! never faults; the eviction pools are otherwise exercised only by
//! synthetic latency microbenchmarks. Here we drive the **real** pool
//! (`alloc_weights` → real eviction via `evict_and_retry`/`select_victim`)
//! plus **real** storage reads (`kernel_ffi::vfs_pread`) over a
//! transformer-derived weight-access trace, so that:
//!
//! - a residency *miss* triggers a real `alloc_weights` (which evicts a
//!   victim chosen by the active policy when the pool is full), followed
//!   by a real read of that block's bytes from a file on SD/SSD;
//! - a residency *hit* returns the resident block with no reload.
//!
//! The result is a hardware-measurable per-policy fault rate **and**
//! reload latency — replacing both the simulator's abstract fault count
//! and the previously-assumed "ms-scale page fault" cost.
//!
//! Scope boundary (honest): this is *not* demand-paging during live LLM
//! inference (that is #980). The access sequence is a stylized,
//! transformer-shaped trace, not a live forward pass.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::kernel_ffi;
use super::model_mem::{self, ModelHandle, BLOCK_SIZE};

/// Max VFS path length the harness stores (null-terminated).
const PATH_MAX: usize = 128;

/// Model priority handed to `set_metadata` for harness blocks. Fixed —
/// the harness varies access patterns, not priorities.
const HARNESS_PRIORITY: u8 = 4;

// -----------------------------------------------------------------------------
// Lock (mirrors the SpinGuard pattern used in model_mem.rs)
// -----------------------------------------------------------------------------

static LOCK: AtomicBool = AtomicBool::new(false);

struct SpinGuard;
impl SpinGuard {
    fn new() -> Self {
        while LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard
    }
}
impl Drop for SpinGuard {
    fn drop(&mut self) {
        LOCK.store(false, Ordering::Release);
    }
}

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

/// Residency index + counters for one harness run. The residency map
/// keys on `(model_id, layer_idx)` — the harness only uses the weight
/// pool, so `PoolType` is implicit.
struct CacheState {
    residency: BTreeMap<(u8, i16), ModelHandle>,
    path: [u8; PATH_MAX],
    block_len: usize,
    accesses: u64,
    hits: u64,
    faults: u64,
    bytes_reloaded: u64,
    /// Per-fault reload latency in ns (for p50/p99). Bounded by the
    /// trace length, which the caller chooses.
    reload_ns: Vec<u64>,
}

static mut STATE: Option<CacheState> = None;

/// Borrow the lazily-initialized state. Caller MUST hold `LOCK`.
///
/// # Safety
/// `LOCK` must be held; no other reference to `STATE` may be live.
unsafe fn state_mut() -> &'static mut CacheState {
    let slot = &mut *addr_of_mut!(STATE);
    if slot.is_none() {
        *slot = Some(CacheState {
            residency: BTreeMap::new(),
            path: [0u8; PATH_MAX],
            block_len: BLOCK_SIZE,
            accesses: 0,
            hits: 0,
            faults: 0,
            bytes_reloaded: 0,
            reload_ns: Vec::new(),
        });
    }
    slot.as_mut().unwrap()
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

/// Aggregated result of a harness run.
#[derive(Clone, Copy)]
pub struct RunResult {
    pub accesses: u64,
    pub hits: u64,
    pub faults: u64,
    pub bytes_reloaded: u64,
    pub p50_ns: u64,
    pub p99_ns: u64,
    pub mean_ns: u64,
}

/// Reset the harness for a fresh run: size the pool (only if no model
/// has been loaded — a second `model_mem_init` would leak the prior
/// pools), free any blocks this harness still owns, and clear stats.
///
/// `pool_mb` is honored only when the pool is not already initialized.
/// `block_len` is the bytes read from storage per fault (0 → 2MB).
/// Returns 0 on success, negative on failure.
pub fn reset(path: &[u8], block_len: usize, pool_mb: u32) -> i32 {
    if !model_mem::is_pool_initialized() {
        // 2MB-align (round down to even MB); guarantee at least one block.
        let wmb = core::cmp::max((pool_mb as usize) & !1usize, 2);
        if model_mem::model_mem_init(wmb, 2).is_err() {
            return -1;
        }
    }

    let _g = SpinGuard::new();
    // SAFETY: LOCK held.
    let st = unsafe { state_mut() };

    // Free blocks we still own so a new run starts from an empty pool.
    // Stale handles (block already evicted/reused) fail the generation
    // check in `free` and no-op — only live handles are reclaimed.
    for (_, h) in st.residency.iter() {
        let _ = model_mem::free(*h);
    }
    st.residency.clear();

    st.path = [0u8; PATH_MAX];
    let n = core::cmp::min(path.len(), PATH_MAX - 1);
    st.path[..n].copy_from_slice(&path[..n]);

    st.block_len = if block_len == 0 { BLOCK_SIZE } else { block_len };
    st.accesses = 0;
    st.hits = 0;
    st.faults = 0;
    st.bytes_reloaded = 0;
    st.reload_ns.clear();
    0
}

/// Access weight block `(model_id, layer_idx)`. Returns `true` on a
/// residency hit, `false` on a miss (which triggered a real eviction +
/// storage reload). This is the heart of the harness: it exercises the
/// real `alloc_weights` eviction path and a real `vfs_pread`.
pub fn access(model_id: u8, layer_idx: i16) -> bool {
    let _g = SpinGuard::new();
    // SAFETY: LOCK held.
    let st = unsafe { state_mut() };
    st.accesses += 1;
    let key = (model_id, layer_idx);

    // Hit path: cached handle must still validate (generation check in
    // get_ptr). A stale handle means the block was evicted out from
    // under us — drop it and fall through to the miss path.
    if let Some(&h) = st.residency.get(&key) {
        if model_mem::get_ptr(h).is_some() {
            let _ = model_mem::touch(h);
            st.hits += 1;
            return true;
        }
        st.residency.remove(&key);
    }

    // Miss path: real eviction (if the pool is full) + real storage read.
    let t0 = kernel_ffi::get_time_ns();
    let h = match model_mem::alloc_weights(st.block_len) {
        Ok(h) => h,
        Err(_) => return false, // pool full and every block pinned — give up
    };
    // Label the block so the policy sees real per-block features and the
    // EvictedContentTracker can credit a re-admit (fault feedback).
    let _ = model_mem::set_metadata(h, model_id, layer_idx, HARNESS_PRIORITY);
    if let Some(ptr) = model_mem::get_ptr(h) {
        let off = (layer_idx.max(0) as u64).wrapping_mul(st.block_len as u64);
        // SAFETY: ptr is valid for BLOCK_SIZE bytes (a fresh 2MB block);
        // block_len <= BLOCK_SIZE by construction in reset().
        let dst = unsafe { core::slice::from_raw_parts_mut(ptr, st.block_len) };
        let _ = kernel_ffi::vfs_pread(&st.path, off, dst);
    }
    let _ = model_mem::touch(h);
    st.residency.insert(key, h);

    let t1 = kernel_ffi::get_time_ns();
    st.faults += 1;
    st.bytes_reloaded += st.block_len as u64;
    st.reload_ns.push(t1.saturating_sub(t0));
    false
}

/// Replay a stylized transformer-derived trace and return aggregate
/// stats. The trace interleaves a small **hot** working set (layers
/// `0..hot`, re-accessed every iteration) with a rolling **cold** scan
/// (layers `hot..n_layers`, streamed once each). With the pool sized
/// below `n_layers`, this rewards frequency-/reuse-aware policies (which
/// keep the hot set resident) and penalizes LRU (whose cold scan evicts
/// the hot blocks) — the same structure the sibling simulator exercises.
pub fn run_trace(n_layers: u32, hot: u32, n_iters: u32) -> RunResult {
    let n_layers = n_layers.max(1);
    let hot = hot.min(n_layers);
    let cold = n_layers - hot;
    let mut cold_cursor: u32 = 0;

    for i in 0..n_iters {
        let layer: u32 = if cold > 0 && (i % 3 == 2) {
            // Every third access advances the cold streaming scan.
            let l = hot + (cold_cursor % cold);
            cold_cursor += 1;
            l
        } else if hot > 0 {
            i % hot
        } else {
            i % n_layers
        };
        access(0, layer as i16);
    }

    let _g = SpinGuard::new();
    // SAFETY: LOCK held.
    let st = unsafe { state_mut() };
    let mut v = st.reload_ns.clone();
    v.sort_unstable();
    let pct = |q_num: u64, q_den: u64| -> u64 {
        if v.is_empty() {
            0
        } else {
            // Integer percentile index: (len-1) * q_num / q_den.
            let idx = ((v.len() as u64 - 1) * q_num / q_den) as usize;
            v[idx]
        }
    };
    let mean = if v.is_empty() {
        0
    } else {
        v.iter().sum::<u64>() / (v.len() as u64)
    };

    RunResult {
        accesses: st.accesses,
        hits: st.hits,
        faults: st.faults,
        bytes_reloaded: st.bytes_reloaded,
        p50_ns: pct(50, 100),
        p99_ns: pct(99, 100),
        mean_ns: mean,
    }
}
