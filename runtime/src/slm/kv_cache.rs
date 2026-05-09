//! Per-session KV (key/value) cache for autoregressive transformer
//! decoding.
//!
//! M5.1 of the SLM integration plan (see
//! `docs/plans/slm-integration-plan.md` §M5 and
//! `docs/fact-sheets/slm-integration.md`). The decode loop in M5.2 appends one
//! `(k, v)` pair per attention layer per generated token; the GQA op
//! [`crate::inference::ops_transformer::gqa_decode_step`] reads the
//! cumulative cache to compute attention.
//!
//! The cache stores both K and V in **FP16** (encoded as `u16` to match
//! the rest of the inference pipeline). Each cache occupies
//! `n_layers * max_ctx * n_kv_heads * head_dim` half-precision entries
//! per tensor (one for K, one for V). For a Qwen2.5-1.5B configuration
//! (`28 * 4096 * 2 * 128`) this lands at ≈ 56 MiB per tensor, ≈ 112 MiB
//! resident — well inside the 512 MiB KV-cache sub-pool that the M5
//! workspace plan reserves.
//!
//! The layout is row-major `[layer][pos][kv_head][dim]`. Slicing by
//! `(layer, 0..len)` yields the contiguous tensor that
//! `gqa_decode_step` already consumes (`seq_len × n_kv_heads × head_dim`),
//! so no transpose / gather is needed at decode time.
//!
//! `no_std` + `alloc` only.

#![allow(clippy::module_name_repetitions)]

extern crate alloc;

use alloc::vec;
use alloc::vec::Vec;
use core::mem::size_of;

/// Per-session KV cache.
///
/// Caller owns the full allocation; freed on `Drop` like any
/// [`Vec`]-backed buffer.
pub struct KvCache {
    pub n_layers: usize,
    pub max_ctx: usize,
    pub n_kv_heads: usize,
    pub head_dim: usize,
    /// Layout: `[layer][pos][kv_head][dim]` row-major (FP16 as `u16`).
    pub k: Vec<u16>,
    /// Layout: `[layer][pos][kv_head][dim]` row-major (FP16 as `u16`).
    pub v: Vec<u16>,
    /// Number of positions written so far (== current sequence length).
    /// The decoder bumps this with [`KvCache::commit_position`] after
    /// every layer of the current token has been appended.
    pub len: usize,
}

impl KvCache {
    /// Allocate the cache for the given dimensions.
    ///
    /// Returns `None` if any dim is zero or if
    /// `n_layers * max_ctx * n_kv_heads * head_dim` overflows
    /// [`usize`]. This is the FFI / parsed-input boundary, so all size
    /// math goes through [`usize::checked_mul`] per
    /// `runtime/CLAUDE.md`'s checked-arithmetic policy.
    pub fn new(
        n_layers: usize,
        max_ctx: usize,
        n_kv_heads: usize,
        head_dim: usize,
    ) -> Option<Self> {
        if n_layers == 0 || max_ctx == 0 || n_kv_heads == 0 || head_dim == 0 {
            return None;
        }
        let per_pos = n_kv_heads.checked_mul(head_dim)?;
        let per_layer = max_ctx.checked_mul(per_pos)?;
        let total = n_layers.checked_mul(per_layer)?;
        // The Vec allocation itself has a `total * size_of::<u16>()` byte
        // requirement; check that too so we don't punt the overflow into
        // the global allocator.
        let _ = total.checked_mul(size_of::<u16>())?;

        Some(Self {
            n_layers,
            max_ctx,
            n_kv_heads,
            head_dim,
            k: vec![0u16; total],
            v: vec![0u16; total],
            len: 0,
        })
    }

    /// Slot stride for a single position within one layer (in FP16
    /// elements).
    #[inline]
    fn per_pos(&self) -> usize {
        // Already validated overflow-free in `new`.
        self.n_kv_heads * self.head_dim
    }

    /// Slot stride for one whole layer (in FP16 elements).
    #[inline]
    fn per_layer(&self) -> usize {
        self.max_ctx * self.per_pos()
    }

    /// Append one position's KV slice for the given layer.
    ///
    /// `k_layer` and `v_layer` must each have length
    /// `n_kv_heads * head_dim` (one position's worth of K / V data).
    /// Both are FP16 (`u16`).
    ///
    /// The decoder calls this once per layer at the current position,
    /// then calls [`KvCache::commit_position`] **once** after all layers
    /// have been appended for the same position.
    ///
    /// Returns `None` on:
    /// - cache full (`len == max_ctx`),
    /// - `layer >= n_layers`,
    /// - slice length mismatch.
    pub fn append(&mut self, layer: usize, k_layer: &[u16], v_layer: &[u16]) -> Option<()> {
        if self.len >= self.max_ctx {
            return None;
        }
        if layer >= self.n_layers {
            return None;
        }
        let per_pos = self.per_pos();
        if k_layer.len() != per_pos || v_layer.len() != per_pos {
            return None;
        }

        let layer_base = layer.checked_mul(self.per_layer())?;
        let pos_off = self.len.checked_mul(per_pos)?;
        let off = layer_base.checked_add(pos_off)?;
        let end = off.checked_add(per_pos)?;

        // Bounds: `off + per_pos <= n_layers * max_ctx * per_pos`,
        // guaranteed by the layer/len checks above plus the overflow-free
        // invariants from `new`. Belt-and-braces guard:
        if end > self.k.len() {
            return None;
        }

        self.k[off..end].copy_from_slice(k_layer);
        self.v[off..end].copy_from_slice(v_layer);
        Some(())
    }

    /// Borrow the K cache slice for `layer` covering positions
    /// `0..self.len`. Shape: `len × n_kv_heads × head_dim` row-major.
    ///
    /// Returns `None` if `layer >= n_layers`. An empty cache returns
    /// an empty slice.
    pub fn k_view(&self, layer: usize) -> Option<&[u16]> {
        if layer >= self.n_layers {
            return None;
        }
        let layer_base = layer.checked_mul(self.per_layer())?;
        let span = self.len.checked_mul(self.per_pos())?;
        let end = layer_base.checked_add(span)?;
        // Guard against `len` having been mutated past `max_ctx` via
        // the `pub len` field. Without this the indexed slice panics
        // instead of returning `None` cleanly.
        if end > self.k.len() {
            return None;
        }
        Some(&self.k[layer_base..end])
    }

    /// Borrow the V cache slice for `layer`. Shape and semantics match
    /// [`KvCache::k_view`].
    pub fn v_view(&self, layer: usize) -> Option<&[u16]> {
        if layer >= self.n_layers {
            return None;
        }
        let layer_base = layer.checked_mul(self.per_layer())?;
        let span = self.len.checked_mul(self.per_pos())?;
        let end = layer_base.checked_add(span)?;
        // See `k_view` for rationale on this `end > buf_len` guard.
        if end > self.v.len() {
            return None;
        }
        Some(&self.v[layer_base..end])
    }

    /// Borrow the K cache for `layer` covering positions
    /// `0..=self.len` — i.e. positions already committed PLUS the
    /// in-flight slot just written by [`KvCache::append`] but not yet
    /// committed via [`KvCache::commit_position`].
    ///
    /// This is the view the decoder hands to `gqa_decode_step`: the
    /// per-layer pipeline appends K and V at position `len`, then
    /// computes attention over `0..=len` (inclusive) before any layer
    /// commits. `commit_position` runs once after all layers, so
    /// `k_view`/`v_view` (which return `0..len`, exclusive) are the
    /// wrong shape during a forward pass.
    ///
    /// Returns `None` if `layer >= n_layers` or the cache is already
    /// at capacity (no in-flight slot exists).
    pub fn k_view_with_pending(&self, layer: usize) -> Option<&[u16]> {
        if layer >= self.n_layers || self.len >= self.max_ctx {
            return None;
        }
        let layer_base = layer.checked_mul(self.per_layer())?;
        let pending_len = self.len.checked_add(1)?;
        let span = pending_len.checked_mul(self.per_pos())?;
        let end = layer_base.checked_add(span)?;
        if end > self.k.len() {
            return None;
        }
        Some(&self.k[layer_base..end])
    }

    /// Borrow the V cache including the in-flight slot. See
    /// [`KvCache::k_view_with_pending`] for the contract.
    pub fn v_view_with_pending(&self, layer: usize) -> Option<&[u16]> {
        if layer >= self.n_layers || self.len >= self.max_ctx {
            return None;
        }
        let layer_base = layer.checked_mul(self.per_layer())?;
        let pending_len = self.len.checked_add(1)?;
        let span = pending_len.checked_mul(self.per_pos())?;
        let end = layer_base.checked_add(span)?;
        if end > self.v.len() {
            return None;
        }
        Some(&self.v[layer_base..end])
    }

    /// Advance `len` by one. Called by the decoder after appending KV
    /// slices for **all** layers at the current position.
    ///
    /// Returns `None` if the cache is already full.
    pub fn commit_position(&mut self) -> Option<()> {
        if self.len >= self.max_ctx {
            return None;
        }
        self.len += 1;
        Some(())
    }

    /// Reset to empty for a fresh conversation.
    ///
    /// Underlying KV bytes are intentionally **not** zeroed: the decoder
    /// only ever reads `0..len`, so stale tail data is unreachable. This
    /// keeps `slm reset <session>` cheap (no full-buffer write).
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// Total resident bytes across the K and V buffers (for `slm stats`).
    pub fn bytes_resident(&self) -> usize {
        // Multiplication is overflow-checked at `new` time; reuse the
        // already-allocated `Vec::len`s here so a single accessor is the
        // source of truth.
        self.k.len().saturating_mul(size_of::<u16>())
            .saturating_add(self.v.len().saturating_mul(size_of::<u16>()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn new_rejects_zero_dim() {
        assert!(KvCache::new(0, 4, 2, 8).is_none());
        assert!(KvCache::new(1, 0, 2, 8).is_none());
        assert!(KvCache::new(1, 4, 0, 8).is_none());
        assert!(KvCache::new(1, 4, 2, 0).is_none());
    }

    #[test]
    fn new_rejects_overflow() {
        // n_layers = usize::MAX, max_ctx = 2 → overflow on first mul step.
        assert!(KvCache::new(usize::MAX, 2, 2, 8).is_none());
        // max_ctx = usize::MAX, others small → overflow within the
        // per-layer step.
        assert!(KvCache::new(1, usize::MAX, 2, 8).is_none());
        // Overflow on the final byte-size guard.
        assert!(KvCache::new(2, 2, 2, usize::MAX / 4).is_none());
    }

    #[test]
    fn new_succeeds_for_small_dims() {
        let cache = KvCache::new(2, 4, 2, 8).expect("alloc");
        assert_eq!(cache.n_layers, 2);
        assert_eq!(cache.max_ctx, 4);
        assert_eq!(cache.n_kv_heads, 2);
        assert_eq!(cache.head_dim, 8);
        assert_eq!(cache.len, 0);
        assert_eq!(cache.k.len(), 2 * 4 * 2 * 8);
        assert_eq!(cache.v.len(), 2 * 4 * 2 * 8);
    }

    #[test]
    fn append_then_view_round_trip() {
        let mut cache = KvCache::new(2, 4, 2, 4).expect("alloc");

        // Pos 0, layer 0: K = [1..16], V = [101..116].
        let k0: [u16; 8] = [1, 2, 3, 4, 5, 6, 7, 8];
        let v0: [u16; 8] = [101, 102, 103, 104, 105, 106, 107, 108];
        cache.append(0, &k0, &v0).expect("append l0 pos0");

        // Pos 0, layer 1: distinct values to prove layer indexing.
        let k1: [u16; 8] = [11, 12, 13, 14, 15, 16, 17, 18];
        let v1: [u16; 8] = [201, 202, 203, 204, 205, 206, 207, 208];
        cache.append(1, &k1, &v1).expect("append l1 pos0");

        cache.commit_position().expect("commit");

        let k_layer0 = cache.k_view(0).expect("k view");
        let v_layer0 = cache.v_view(0).expect("v view");
        assert_eq!(k_layer0, &k0);
        assert_eq!(v_layer0, &v0);

        let k_layer1 = cache.k_view(1).expect("k view");
        let v_layer1 = cache.v_view(1).expect("v view");
        assert_eq!(k_layer1, &k1);
        assert_eq!(v_layer1, &v1);
    }

    #[test]
    fn view_with_pending_includes_just_appended_slot() {
        // Reproduces the M5.3.2-review-Critical scenario: forward_one
        // calls `append(layer, k, v)` then `gqa_decode_step(... seq_len
        // = pos + 1, ...)`. The pre-commit `k_view` returns slots
        // [0..len) which is one slot SHORT of seq_len; the new
        // `*_view_with_pending` returns slots [0..=len] inclusive,
        // matching what gqa expects.
        let mut cache = KvCache::new(1, 4, 1, 2).expect("alloc");
        let k_first: [u16; 2] = [10, 11];
        let v_first: [u16; 2] = [110, 111];
        cache.append(0, &k_first, &v_first).expect("append");

        // Pre-commit: plain k_view returns the empty 0..0 prefix.
        let k_pre = cache.k_view(0).expect("k_view");
        assert_eq!(k_pre.len(), 0);

        // The "with pending" view sees the just-appended slot too.
        let k_pending = cache.k_view_with_pending(0).expect("k_view_with_pending");
        let v_pending = cache.v_view_with_pending(0).expect("v_view_with_pending");
        assert_eq!(k_pending, &k_first);
        assert_eq!(v_pending, &v_first);

        // After commit, pre-commit view catches up; "with pending"
        // would now require slot index 1 which is also writable.
        cache.commit_position().expect("commit");
        let k_post = cache.k_view(0).expect("k_view post");
        assert_eq!(k_post, &k_first);

        // Append at position 1 and verify the pending view is the
        // 2-slot slice covering both positions.
        let k_second: [u16; 2] = [22, 23];
        let v_second: [u16; 2] = [222, 223];
        cache.append(0, &k_second, &v_second).expect("append 2");
        let k_pending2 = cache.k_view_with_pending(0).expect("pending 2");
        assert_eq!(k_pending2.len(), 4);
        assert_eq!(&k_pending2[0..2], &k_first);
        assert_eq!(&k_pending2[2..4], &k_second);
    }

    #[test]
    fn view_with_pending_rejects_full_cache() {
        let mut cache = KvCache::new(1, 1, 1, 2).expect("alloc");
        let k: [u16; 2] = [0, 0];
        let v: [u16; 2] = [0, 0];
        cache.append(0, &k, &v).expect("append");
        cache.commit_position().expect("commit");
        // Cache is full (max_ctx = 1, len = 1). No pending slot
        // exists; the view should refuse rather than overrun.
        assert!(cache.k_view_with_pending(0).is_none());
        assert!(cache.v_view_with_pending(0).is_none());
    }

    #[test]
    fn append_advances_len() {
        let mut cache = KvCache::new(1, 4, 1, 2).expect("alloc");
        let k: [u16; 2] = [0, 0];
        let v: [u16; 2] = [0, 0];
        for _ in 0..3 {
            cache.append(0, &k, &v).expect("append");
            cache.commit_position().expect("commit");
        }
        assert_eq!(cache.len, 3);
    }

    #[test]
    fn append_rejects_full_cache() {
        let mut cache = KvCache::new(1, 2, 1, 2).expect("alloc");
        let k: [u16; 2] = [9, 9];
        let v: [u16; 2] = [9, 9];
        cache.append(0, &k, &v).expect("0");
        cache.commit_position().expect("c0");
        cache.append(0, &k, &v).expect("1");
        cache.commit_position().expect("c1");
        // Cache is now full.
        assert!(cache.append(0, &k, &v).is_none());
        assert!(cache.commit_position().is_none());
    }

    #[test]
    fn append_rejects_bad_layer_or_shape() {
        let mut cache = KvCache::new(2, 4, 1, 2).expect("alloc");
        let k_ok: [u16; 2] = [1, 2];
        let v_ok: [u16; 2] = [3, 4];
        let k_bad: [u16; 3] = [1, 2, 3];

        // Layer out of range.
        assert!(cache.append(2, &k_ok, &v_ok).is_none());
        // Shape mismatch on K.
        assert!(cache.append(0, &k_bad, &v_ok).is_none());
        // Shape mismatch on V.
        assert!(cache.append(0, &k_ok, &k_bad).is_none());
    }

    #[test]
    fn clear_resets_len_only() {
        let mut cache = KvCache::new(1, 4, 1, 2).expect("alloc");
        let k: [u16; 2] = [42, 43];
        let v: [u16; 2] = [44, 45];
        cache.append(0, &k, &v).expect("append");
        cache.commit_position().expect("commit");

        // Snapshot the underlying bytes.
        let k_before = cache.k.clone();
        let v_before = cache.v.clone();

        cache.clear();
        assert_eq!(cache.len, 0);
        assert_eq!(cache.k, k_before);
        assert_eq!(cache.v, v_before);

        // After clear, k_view / v_view return empty slices.
        assert_eq!(cache.k_view(0).expect("view").len(), 0);
        assert_eq!(cache.v_view(0).expect("view").len(), 0);
    }

    #[test]
    fn bytes_resident_matches_allocation() {
        let cache = KvCache::new(2, 4, 2, 8).expect("alloc");
        // 2 * 4 * 2 * 8 = 128 FP16 entries per tensor, 2 tensors,
        // 2 bytes each → 512.
        assert_eq!(cache.bytes_resident(), 2 * 128 * 2);
    }

    #[test]
    fn views_reject_bad_layer() {
        let cache = KvCache::new(2, 4, 1, 2).expect("alloc");
        assert!(cache.k_view(2).is_none());
        assert!(cache.v_view(2).is_none());
    }
}
