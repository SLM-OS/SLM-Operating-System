//! Recently-evicted content tracker — feeds the CACHEUS feedback loop.
//!
//! Mirrors the simulator's `_evicted_content` design in
//! `slm-os-page-sim/src/simulator/core.py`. When the allocator evicts
//! a block it records a `ContentKey` (pool + model_id + layer_idx)
//! alongside the evicted block's ID and the time of eviction. When a
//! subsequent miss allocates a block whose content key matches a
//! recent eviction, the tracker reports a "fault" (bad eviction) so
//! the active policy can penalise experts that agreed with it.
//!
//! Entries older than [`EVICTION_FEEDBACK_WINDOW_NS`] are considered
//! good evictions and fire a `was_fault=false` feedback once, then
//! are dropped.
//!
//! M5 ships the tracker with a no-argument API; M6 wires
//! `alloc_weights` / `alloc_workspace` to call `record_eviction` and
//! `probe_on_alloc` at the right points in the allocation path.
//!
//! ## Cross-CPU visibility
//!
//! The global `EVICTED_CONTENT_TRACKER` instance lives in
//! `mm::model_mem` behind the allocator's `LOCK`. Its cross-CPU
//! visibility inherits from that lock, which uses `Acquire`/`Release`
//! ordering (→ `DMB ISH` on ARM64). On platforms where per-core L2
//! caches are incoherent (Pi 5), the same concern applies to the
//! whole `ModelAllocator` and is tracked under #116; no eviction-
//! specific workaround is needed.

use alloc::collections::VecDeque;

use super::policy::PoolType;
use super::registry;

/// Feedback window in nanoseconds — matches the simulator's 200-tick
/// window scaled to the runtime's 1 ns tick. 200 ns is too short in
/// practice, so we widen to 200 ms (200 * 1 M ns ≈ 2e8 ns) on the
/// assumption that realistic alloc cadences live in the millisecond
/// range. Rationale: the simulator's ticks advance one-per-access;
/// 200 ticks maps to roughly 200 accesses of allocator traffic.
pub const EVICTION_FEEDBACK_WINDOW_NS: u64 = 200_000_000;

/// Maximum entries retained. Prevents unbounded growth under bursty
/// eviction. Older entries are dropped from the front.
pub const EVICTION_TRACKER_CAPACITY: usize = 256;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ContentKey {
    pub pool_type: PoolType,
    pub model_id: u8,
    pub layer_idx: i16,
}

#[derive(Clone, Copy)]
struct TrackerEntry {
    key: ContentKey,
    evicted_block_id: u32,
    evicted_at_ns: u64,
}

/// Small FIFO cache of recently-evicted content keys. Lookups are
/// linear — capacity is 256, which is sound for the slow path.
pub struct EvictedContentTracker {
    entries: VecDeque<TrackerEntry>,
    capacity: usize,
    window_ns: u64,
}

impl Default for EvictedContentTracker {
    fn default() -> Self { Self::new() }
}

impl EvictedContentTracker {
    pub fn new() -> Self {
        Self::with_params(EVICTION_TRACKER_CAPACITY, EVICTION_FEEDBACK_WINDOW_NS)
    }

    pub fn with_params(capacity: usize, window_ns: u64) -> Self {
        Self {
            entries: VecDeque::with_capacity(capacity),
            capacity,
            window_ns,
        }
    }

    pub fn len(&self) -> usize { self.entries.len() }
    pub fn is_empty(&self) -> bool { self.entries.is_empty() }
    pub fn capacity(&self) -> usize { self.capacity }
    pub fn window_ns(&self) -> u64 { self.window_ns }

    /// Record an eviction. Evicts the oldest entry if the FIFO is full.
    pub fn record_eviction(
        &mut self,
        key: ContentKey,
        evicted_block_id: u32,
        now_ns: u64,
    ) {
        if self.entries.len() == self.capacity {
            self.entries.pop_front();
        }
        self.entries.push_back(TrackerEntry {
            key, evicted_block_id, evicted_at_ns: now_ns,
        });
    }

    /// Probe the tracker with the content key of a just-allocated
    /// block. If a recent eviction carried the same key, returns the
    /// old block_id and clears the entry (the policy learns it was a
    /// bad eviction). Older-than-window entries get flushed as
    /// `was_fault=false` feedback and removed.
    ///
    /// This is a pure data-structure operation: it does not call into
    /// the registry. Callers that want the feedback to reach the
    /// installed policy should chain
    /// `probe_on_alloc_and_notify` instead.
    pub fn probe_on_alloc(
        &mut self,
        key: ContentKey,
        now_ns: u64,
    ) -> Option<u32> {
        // First, flush any entries that have aged past the window.
        while let Some(front) = self.entries.front() {
            if now_ns.saturating_sub(front.evicted_at_ns) > self.window_ns {
                self.entries.pop_front();
            } else {
                break;
            }
        }
        // Scan newest-to-oldest for a matching key.
        let pos = self
            .entries
            .iter()
            .rposition(|e| e.key == key)?;
        let entry = self.entries.remove(pos)?;
        Some(entry.evicted_block_id)
    }

    /// Flush every entry older than the window, returning each one's
    /// evicted_block_id. Intended for periodic background cleanup so
    /// the policy gets `was_fault=false` feedback for good evictions.
    pub fn drain_expired(
        &mut self,
        now_ns: u64,
    ) -> alloc::vec::Vec<u32> {
        let mut out = alloc::vec::Vec::new();
        while let Some(front) = self.entries.front() {
            if now_ns.saturating_sub(front.evicted_at_ns) > self.window_ns {
                if let Some(entry) = self.entries.pop_front() {
                    out.push(entry.evicted_block_id);
                }
            } else {
                break;
            }
        }
        out
    }

    pub fn clear(&mut self) {
        self.entries.clear();
    }
}

/// Convenience wrapper: probe the tracker and, if we hit, drive
/// `registry::update_feedback(id, was_fault=true)` for the
/// currently-installed policy. Returns whether a match was found.
pub fn probe_and_report_fault(
    tracker: &mut EvictedContentTracker,
    key: ContentKey,
    now_ns: u64,
) -> bool {
    if let Some(block_id) = tracker.probe_on_alloc(key, now_ns) {
        registry::update_feedback(block_id, true);
        true
    } else {
        false
    }
}

/// Drain expired entries and report each one as a good eviction.
pub fn drain_and_report_good(
    tracker: &mut EvictedContentTracker,
    now_ns: u64,
) -> usize {
    let ids = tracker.drain_expired(now_ns);
    let n = ids.len();
    for id in ids {
        registry::update_feedback(id, false);
    }
    n
}
