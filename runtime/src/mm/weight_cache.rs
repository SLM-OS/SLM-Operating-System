//! End-to-end storage-backed weight-cache harness (#979).
//!
//! Closes the gap between the eviction *policy* machinery and a real
//! storage reload path, on real hardware. The production inference
//! engine holds weights resident and never faults; the `model_mem`
//! pools are otherwise exercised only by synthetic latency
//! microbenchmarks. Here we replay the **simulator's actual per-scenario
//! access traces** (embedded via `include_bytes!`, exported by
//! `slm-os-page-sim/scripts/export_eviction_trace.py`) through the real
//! pools + the active eviction policy, so the per-policy fault rates can
//! be compared directly against the simulator's results — at real
//! reload latency.
//!
//! Two fidelity requirements make the comparison meaningful:
//!
//! 1. **Residency identity = `(model_id, layer_idx, pool_type)`** — the
//!    exact tuple the simulator keys on (`core.py` `content_key`). The
//!    trace records carry it, and on a miss `set_metadata` drives the
//!    same `EvictedContentTracker` feedback the simulator's core does.
//! 2. **Logical-tick time base** — feature-time is driven on the
//!    simulator's tick base (`SIM_TICK_NS` per access) via
//!    `set_clock_override` + `set_eviction_times`, so the recency
//!    feature doesn't collapse the way it would under a tight real-ns
//!    replay loop.
//!
//! Scope (honest): this replays *traces*, not a live LLM forward pass
//! (that maximal version is #980).

use alloc::collections::BTreeMap;
use alloc::vec::Vec;
use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::kernel_ffi;
use super::eviction;
use super::model_mem::{self, ModelHandle, BLOCK_SIZE};

/// Embedded simulator trace blob. Format documented in
/// `export_eviction_trace.py`: magic "EVT1", version u32, seed u32,
/// n_scenarios u32, then a directory of {name[16], n_accesses u32,
/// record_offset u32}, then 8-byte records {model_id u8, pool u8,
/// access_pattern u8, _pad u8, layer_idx i32 LE}.
static EVICTION_TRACE: &[u8] = include_bytes!("eviction_trace.bin");

const TRACE_MAGIC: &[u8; 4] = b"EVT1";
const DIR_ENTRY_LEN: usize = 24; // name[16] + n_accesses u32 + offset u32
const HEADER_LEN: usize = 16; // magic + version + seed + n_scenarios
const RECORD_LEN: usize = 8;
const PATH_MAX: usize = 128;

/// Model priority handed to `set_metadata` for harness blocks.
const HARNESS_PRIORITY: u8 = 4;

/// Number of file block-slots the `--read` path cycles offsets through,
/// so reads stay in-bounds for a modest staged file while still hitting
/// storage. The bytes are irrelevant to fault rate (latency only).
const HARNESS_READ_SLOTS: u64 = 16;

// -----------------------------------------------------------------------------
// Trace blob accessors (pure, on the embedded &[u8])
// -----------------------------------------------------------------------------

fn rd_u32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}
fn rd_i32(b: &[u8], off: usize) -> i32 {
    i32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

fn trace_valid() -> bool {
    if EVICTION_TRACE.len() < HEADER_LEN || &EVICTION_TRACE[0..4] != TRACE_MAGIC {
        return false;
    }
    // The directory must be fully present so scenario_dir/scenario_name
    // can't index past the (trusted, build-time) blob.
    let n = rd_u32(EVICTION_TRACE, 12) as usize;
    EVICTION_TRACE.len() >= HEADER_LEN + n * DIR_ENTRY_LEN
}

/// Number of scenarios in the embedded trace (0 if the blob is invalid).
pub fn scenario_count() -> u32 {
    if !trace_valid() {
        return 0;
    }
    rd_u32(EVICTION_TRACE, 12)
}

/// Scenario name (null-trimmed) for `idx`, or empty slice if out of range.
pub fn scenario_name(idx: u32) -> &'static [u8] {
    if idx >= scenario_count() {
        return &[];
    }
    let base = HEADER_LEN + (idx as usize) * DIR_ENTRY_LEN;
    let name = &EVICTION_TRACE[base..base + 16];
    let end = name.iter().position(|&c| c == 0).unwrap_or(16);
    &name[..end]
}

/// `(n_accesses, record_offset)` for scenario `idx`.
fn scenario_dir(idx: u32) -> Option<(u32, u32)> {
    if idx >= scenario_count() {
        return None;
    }
    let base = HEADER_LEN + (idx as usize) * DIR_ENTRY_LEN;
    Some((rd_u32(EVICTION_TRACE, base + 16), rd_u32(EVICTION_TRACE, base + 20)))
}

// -----------------------------------------------------------------------------
// Lock + state (mirrors the SpinGuard pattern used in model_mem.rs)
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

/// A resident block in the harness cache. The harness owns
/// `access_count` + `load_ns` so feature-time is fully under its control
/// (independent of the allocator's wall-clock stamps).
#[derive(Clone, Copy)]
struct Resident {
    handle: ModelHandle,
    access_count: u32,
    load_ns: u64,
}

struct CacheState {
    /// Residency index keyed on the simulator tuple
    /// `(model_id, layer_idx, pool_type)`.
    residency: BTreeMap<(u8, i16, u8), Resident>,
    path: [u8; PATH_MAX],
    block_len: usize,
    do_read: bool,
    tick: u64,
    accesses: u64,
    hits: u64,
    faults: u64,
    bytes_reloaded: u64,
    reload_ns: Vec<u64>,
}

static mut STATE: Option<CacheState> = None;

/// Borrow the lazily-initialized state. Caller MUST hold `LOCK`.
///
/// # Safety
/// `LOCK` held; no other reference to `STATE` live.
unsafe fn state_mut() -> &'static mut CacheState {
    let slot = &mut *addr_of_mut!(STATE);
    if slot.is_none() {
        *slot = Some(CacheState {
            residency: BTreeMap::new(),
            path: [0u8; PATH_MAX],
            block_len: BLOCK_SIZE,
            do_read: false,
            tick: 0,
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

/// Aggregate result of replaying one scenario under one policy.
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

/// (Re)size the pools to match the simulator's cache (weight 64 +
/// workspace 32 blocks by default) and clear harness state for a fresh
/// run. DESTRUCTIVE: reinit frees the backing pages, invalidating any
/// loaded model — acceptable for this diagnostic bench. Only reinit
/// when the current weight-pool size differs, so a sweep reinits once.
///
/// Returns 0 on success, negative on failure.
pub fn reset(
    weight_mb: u32,
    workspace_mb: u32,
    path: &[u8],
    block_len: usize,
    do_read: bool,
) -> i32 {
    let want_w = core::cmp::max((weight_mb as usize) & !1usize, 2);
    let want_ws = core::cmp::max((workspace_mb as usize) & !1usize, 2);
    // Always reinit, not just on a size change. `model_mem_reinit` is what
    // clears the global `EvictedContentTracker` (and rebuilds the pools
    // from scratch), so each scenario starts from an identical state. The
    // tracker holds evicted-content keys stamped with the logical-tick
    // clock, which the harness resets to 0 at the start of every scenario;
    // a stale entry from the previous scenario then carries a "future"
    // timestamp that never expires and feeds nondeterministic feedback to
    // stateful policies (ARC ghost lists, CACHEUS online weights). This is
    // the root of the run-to-run instability in #981 (adversarial swung
    // 6%->93% across back-to-back runs). The per-scenario policy reset in
    // `run_scenario` handles within-row cross-scenario policy state; this
    // handles the pool + tracker.
    if model_mem::model_mem_reinit(want_w, want_ws).is_err() {
        return -1;
    }

    let _g = SpinGuard::new();
    // SAFETY: LOCK held.
    let st = unsafe { state_mut() };
    // Free any blocks we still own (stale handles no-op on generation
    // mismatch after a reinit).
    for (_, res) in st.residency.iter() {
        let _ = model_mem::free(res.handle);
    }
    st.residency.clear();
    st.path = [0u8; PATH_MAX];
    let n = core::cmp::min(path.len(), PATH_MAX - 1);
    st.path[..n].copy_from_slice(&path[..n]);
    st.block_len = if block_len == 0 { BLOCK_SIZE } else { block_len };
    st.do_read = do_read;
    st.tick = 0;
    st.accesses = 0;
    st.hits = 0;
    st.faults = 0;
    st.bytes_reloaded = 0;
    st.reload_ns.clear();
    0
}

/// One access on the simulator tuple `(model_id, layer_idx, pool)` with
/// the observed `access_pattern`. Returns `true` on a residency hit,
/// `false` on a miss (which ran a real eviction via the active policy +
/// an optional real storage read). Feature-time is driven on the
/// simulator's logical-tick base.
fn access(st: &mut CacheState, model_id: u8, layer_idx: i16, pool: u8, ap: u8) -> bool {
    st.tick += 1;
    let now_ns = st.tick.wrapping_mul(eviction::SIM_TICK_NS);
    // Drive feature-extraction `now` on the tick base — including the
    // eviction that may fire inside the alloc below.
    eviction::set_clock_override(now_ns);

    let key = (model_id, layer_idx, pool);
    st.accesses += 1;

    if let Some(res) = st.residency.get(&key).copied() {
        if model_mem::get_ptr(res.handle).is_some() {
            let count = res.access_count.saturating_add(1);
            let _ = model_mem::set_access_pattern(res.handle, ap);
            let _ = model_mem::set_eviction_times(res.handle, res.load_ns, now_ns, count);
            st.residency.insert(
                key,
                Resident { handle: res.handle, access_count: count, load_ns: res.load_ns },
            );
            st.hits += 1;
            return true;
        }
        st.residency.remove(&key);
    }

    // Miss: real eviction (if the target pool is full) + real reload.
    let t0 = kernel_ffi::get_time_ns(); // wall clock — for latency only
    let alloc = if pool == 0 {
        model_mem::alloc_weights(st.block_len)
    } else {
        model_mem::alloc_workspace(st.block_len)
    };
    let h = match alloc {
        Ok(h) => h,
        Err(_) => return false, // pool full + all pinned
    };
    let _ = model_mem::set_metadata(h, model_id, layer_idx, HARNESS_PRIORITY);
    let _ = model_mem::set_access_pattern(h, ap);
    let _ = model_mem::set_eviction_times(h, now_ns, now_ns, 1);
    if st.do_read {
        if let Some(ptr) = model_mem::get_ptr(h) {
            // Cycle offsets through the file's first 16 block-slots so
            // reads stay in-bounds for a modest staged file while still
            // hitting storage (the bytes are irrelevant to fault rate).
            let off = (st.tick % HARNESS_READ_SLOTS).wrapping_mul(st.block_len as u64);
            // SAFETY: ptr valid for BLOCK_SIZE; block_len <= BLOCK_SIZE.
            let dst = unsafe { core::slice::from_raw_parts_mut(ptr, st.block_len) };
            let _ = kernel_ffi::vfs_pread(&st.path, off, dst);
        }
    }
    st.residency
        .insert(key, Resident { handle: h, access_count: 1, load_ns: now_ns });

    let t1 = kernel_ffi::get_time_ns();
    st.faults += 1;
    st.bytes_reloaded += st.block_len as u64;
    st.reload_ns.push(t1.saturating_sub(t0));
    false
}

/// Replay scenario `idx` from the embedded trace through the active
/// policy + real pools, returning aggregate stats. Pools must already be
/// sized via `reset`.
pub fn run_scenario(idx: u32) -> Option<RunResult> {
    let (n_acc, rec_off) = scenario_dir(idx)?;
    let rec_off = rec_off as usize;

    // Reset each pool's active policy so stateful policies start fresh per
    // scenario, matching the sibling sim's per-run `policy.reset()`
    // (`slm-os-page-sim/scripts/benchmark.py`). Stateless policies
    // (LRU/LFU/SLM/first_candidate) rank from the passed-in BlockMeta and
    // are unaffected; CACHEUS (online weights) and ARC (ghost lists) carry
    // learned state across scenarios otherwise, making fault counts depend
    // on run history (#981). Done outside the harness LOCK — `reset` takes
    // the registry lock, which `access()` also acquires while we hold LOCK.
    let _ = eviction::with_active_policy_for_pool(eviction::PoolType::Weight, |p| p.reset());
    let _ = eviction::with_active_policy_for_pool(eviction::PoolType::Workspace, |p| p.reset());

    {
        let _g = SpinGuard::new();
        // SAFETY: LOCK held.
        let st = unsafe { state_mut() };
        // Fresh counters/residency for this scenario (pools already sized).
        for (_, res) in st.residency.iter() {
            let _ = model_mem::free(res.handle);
        }
        st.residency.clear();
        st.tick = 0;
        st.accesses = 0;
        st.hits = 0;
        st.faults = 0;
        st.bytes_reloaded = 0;
        st.reload_ns.clear();

        for a in 0..n_acc as usize {
            let r = rec_off + a * RECORD_LEN;
            if r + RECORD_LEN > EVICTION_TRACE.len() {
                break;
            }
            let model_id = EVICTION_TRACE[r];
            let pool = EVICTION_TRACE[r + 1];
            let ap = EVICTION_TRACE[r + 2];
            let layer_idx = rd_i32(EVICTION_TRACE, r + 4) as i16;
            access(st, model_id, layer_idx, pool, ap);
        }
    }

    // Revert the clock override so production feature extraction uses
    // wall-clock again.
    eviction::clear_clock_override();

    let _g = SpinGuard::new();
    // SAFETY: LOCK held.
    let st = unsafe { state_mut() };
    let mut v = st.reload_ns.clone();
    v.sort_unstable();
    let pct = |q_num: u64, q_den: u64| -> u64 {
        if v.is_empty() {
            0
        } else {
            v[((v.len() as u64 - 1) * q_num / q_den) as usize]
        }
    };
    let mean = if v.is_empty() {
        0
    } else {
        v.iter().sum::<u64>() / (v.len() as u64)
    };
    Some(RunResult {
        accesses: st.accesses,
        hits: st.hits,
        faults: st.faults,
        bytes_reloaded: st.bytes_reloaded,
        p50_ns: pct(50, 100),
        p99_ns: pct(99, 100),
        mean_ns: mean,
    })
}
