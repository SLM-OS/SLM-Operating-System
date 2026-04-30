//! Model memory allocator for AI inference.
//!
//! Provides specialized allocation for:
//! - Weight pool: Read-only model parameters
//! - Workspace pool: Per-inference scratch space
//!
//! All allocations are 2MB aligned for ARM L2 block efficiency.

use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use crate::kernel_ffi;

// =============================================================================
// Constants
// =============================================================================

/// Block size: 2MB (ARM L2 block size for huge page efficiency)
pub const BLOCK_SIZE: usize = 2 * 1024 * 1024;

/// Maximum blocks per pool (16-bit index in handle).
///
/// At `BLOCK_SIZE = 2 MB` this is also the per-pool size cap in MB.
/// `model_mem_init(weight_mb, workspace_mb)` returns
/// `AllocError::Oversized` when either request exceeds
/// `MAX_BLOCKS_PER_POOL × BLOCK_SIZE`, so a future bump of the
/// C-side `MODEL_MEM_WEIGHT_MB` past 1024 fails loudly until this
/// constant is bumped in lockstep (see the `_Static_assert` block in
/// `kernel/include/config.h`).
///
/// Sized for Jetson's `MODEL_MEM_WEIGHT_MB = 1024` so the requested
/// 1 GB is fully addressable; smaller platforms (Pi 5 512, QEMU 256)
/// fit comfortably below the cap. Each `BlockSlot` is ~48 B, so the
/// BSS footprint is `2 pools × 512 slots × 48 B ≈ 48 KB`.
const MAX_BLOCKS_PER_POOL: usize = 512;

/// Pool identifiers
const POOL_WEIGHT: u8 = 0;
const POOL_WORKSPACE: u8 = 1;

// =============================================================================
// Error Types
// =============================================================================

/// Allocation error type.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AllocError {
    /// Pool exhausted, no free blocks available.
    OutOfMemory,
    /// Handle doesn't correspond to allocated block.
    InvalidHandle,
    /// Generation mismatch - block was freed and reallocated.
    StaleHandle,
    /// Internal alignment error.
    AlignmentError,
    /// model_mem_init() not called.
    NotInitialized,
    /// Allocation failed in underlying PMM.
    PmmFailed,
    /// Requested pool size exceeds the static `MAX_BLOCKS_PER_POOL`
    /// cap. Surfaced from `model_mem_init` so a caller asking for
    /// (e.g.) 2 GB never quietly gets 1 GB. Bumping the C-side
    /// `MODEL_MEM_WEIGHT_MB` past `MAX_BLOCKS_PER_POOL × BLOCK_SIZE`
    /// requires bumping `MAX_BLOCKS_PER_POOL` in lockstep.
    Oversized,
}

// =============================================================================
// Handle Type
// =============================================================================

/// Opaque handle to allocated model memory.
///
/// Contains block index, pool ID, and generation for validation.
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct ModelHandle {
    block_index: u16,
    pool_id: u8,
    generation: u8,
    _reserved: u32,
}

impl ModelHandle {
    /// Create a null/invalid handle.
    pub const fn null() -> Self {
        Self {
            block_index: 0xFFFF,
            pool_id: 0xFF,
            generation: 0,
            _reserved: 0,
        }
    }

    /// Check if handle is null/invalid.
    ///
    /// The "null" sentinel requires BOTH fields to match (block_index ==
    /// 0xFFFF AND pool_id == 0xFF). A handle with one but not both is
    /// treated as live by this check — that is intentional, because
    /// `null()` is the only constructor that produces both sentinels
    /// together, so a single-sentinel handle would itself be an
    /// internal corruption (e.g. stale partial write from a torn
    /// memcpy on the FFI boundary). Such a handle would still fail
    /// validation in the allocator's bounds + generation checks
    /// downstream — keep the strict AND so callers can distinguish
    /// "constructor-produced null" from "garbage".
    pub fn is_null(&self) -> bool {
        self.block_index == 0xFFFF && self.pool_id == 0xFF
    }
}

impl core::fmt::Debug for ModelHandle {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        if self.is_null() {
            write!(f, "ModelHandle(null)")
        } else {
            write!(f, "ModelHandle(pool={}, block={}, gen={})",
                   self.pool_id, self.block_index, self.generation)
        }
    }
}

// =============================================================================
// Block Metadata
// =============================================================================

/// Block state.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum BlockState {
    Free = 0,
    Allocated = 1,
    Shared = 2,
}

/// Per-slot pool metadata.
///
/// Represents one 2 MB slot in a `MemoryPool`. The first group of
/// fields is the allocator's own bookkeeping; the second group is
/// eviction-policy tracking (populated on alloc and bumped by
/// `touch()`) that M6 will feed into `ACTIVE_POLICY.select_victim`.
struct BlockSlot {
    // Allocator bookkeeping.
    state: BlockState,
    refcount: u16,
    generation: u8,
    size_blocks: u8,    // Contiguous blocks (for future multi-block alloc)
    owner_task: u32,

    // Eviction-policy tracking (always on; ~24 B / slot).
    //
    // Units match the sibling `slm-os-page-sim` simulator: timestamps
    // are a monotonic tick counter in whatever resolution the kernel
    // feeds in (initially `kernel_ffi::tick_ns` — callers pick). Set
    // to 0 when the slot is free.
    load_time: u64,
    last_access_time: u64,
    access_count: u32,
    model_id: u8,
    layer_idx: i16,
    gpu_mapped: bool,
    is_dirty: bool,
    model_priority: u8,
}

impl BlockSlot {
    const fn new() -> Self {
        Self {
            state: BlockState::Free,
            refcount: 0,
            generation: 0,
            size_blocks: 0,
            owner_task: 0,

            load_time: 0,
            last_access_time: 0,
            access_count: 0,
            model_id: 0,
            layer_idx: 0,
            gpu_mapped: false,
            is_dirty: false,
            model_priority: 0,
        }
    }

    /// Clear eviction-tracking fields when the slot goes back to the free list.
    fn clear_tracking(&mut self) {
        self.load_time = 0;
        self.last_access_time = 0;
        self.access_count = 0;
        self.model_id = 0;
        self.layer_idx = 0;
        self.gpu_mapped = false;
        self.is_dirty = false;
        self.model_priority = 0;
    }
}

// =============================================================================
// Memory Pool
// =============================================================================

/// A pool of 2MB-aligned memory blocks.
struct MemoryPool {
    base_addr: usize,
    block_count: usize,
    blocks: [BlockSlot; MAX_BLOCKS_PER_POOL],
    free_count: usize,
    peak_usage: usize,
    read_only: bool,
    /// Count of evictions performed on this pool — surfaced through
    /// `PoolStats.evictions_total` for the `eviction` shell command.
    evictions_total: u64,
}

impl MemoryPool {
    const fn new() -> Self {
        Self {
            base_addr: 0,
            block_count: 0,
            blocks: [const { BlockSlot::new() }; MAX_BLOCKS_PER_POOL],
            free_count: 0,
            peak_usage: 0,
            read_only: false,
            evictions_total: 0,
        }
    }

    /// Initialize pool with given base address and block count.
    ///
    /// Caller must ensure `count <= MAX_BLOCKS_PER_POOL`;
    /// `model_mem_init` enforces this and returns
    /// `AllocError::Oversized` otherwise. The `min` here is a
    /// belt-and-braces guard for direct callers (currently none in
    /// production); it keeps the array index sound but masks bugs,
    /// so prefer the upstream check.
    fn init(&mut self, base: usize, count: usize, read_only: bool) {
        self.base_addr = base;
        self.block_count = count.min(MAX_BLOCKS_PER_POOL);
        self.free_count = self.block_count;
        self.peak_usage = 0;
        self.read_only = read_only;

        // Mark all blocks as free
        for i in 0..self.block_count {
            self.blocks[i] = BlockSlot::new();
        }
    }

    /// Find and allocate a free block.
    fn alloc(&mut self, pool_id: u8) -> Result<ModelHandle, AllocError> {
        if self.free_count == 0 {
            return Err(AllocError::OutOfMemory);
        }

        // Linear scan for free block (simple, works for small pools)
        let now = kernel_ffi::get_time_ns();
        for i in 0..self.block_count {
            if self.blocks[i].state == BlockState::Free {
                self.blocks[i].state = BlockState::Allocated;
                self.blocks[i].refcount = 1;
                self.blocks[i].size_blocks = 1;
                self.blocks[i].owner_task = kernel_ffi::task_current().0;

                // Eviction tracking: mark load time, treat alloc as the
                // first access so LRU never sees an apparent zero.
                self.blocks[i].load_time = now;
                self.blocks[i].last_access_time = now;
                self.blocks[i].access_count = 1;

                self.free_count -= 1;
                let used = self.block_count - self.free_count;
                if used > self.peak_usage {
                    self.peak_usage = used;
                }

                return Ok(ModelHandle {
                    block_index: i as u16,
                    pool_id,
                    generation: self.blocks[i].generation,
                    _reserved: 0,
                });
            }
        }

        Err(AllocError::OutOfMemory)
    }

    /// Free a block by handle.
    fn free(&mut self, handle: ModelHandle) -> Result<(), AllocError> {
        let idx = handle.block_index as usize;
        if idx >= self.block_count {
            return Err(AllocError::InvalidHandle);
        }

        let block = &mut self.blocks[idx];
        if block.generation != handle.generation {
            return Err(AllocError::StaleHandle);
        }
        if block.state == BlockState::Free {
            return Err(AllocError::InvalidHandle);
        }

        // Decrement refcount
        if block.refcount > 1 {
            block.refcount -= 1;
            if block.refcount == 1 {
                block.state = BlockState::Allocated;
            }
            return Ok(());
        }

        // Actually free the block
        block.state = BlockState::Free;
        block.refcount = 0;
        block.generation = block.generation.wrapping_add(1);
        block.owner_task = 0;
        block.clear_tracking();
        self.free_count += 1;

        Ok(())
    }

    /// Share a block (increment refcount).
    fn share(&mut self, handle: ModelHandle) -> Result<ModelHandle, AllocError> {
        let idx = handle.block_index as usize;
        if idx >= self.block_count {
            return Err(AllocError::InvalidHandle);
        }

        let block = &mut self.blocks[idx];
        if block.generation != handle.generation {
            return Err(AllocError::StaleHandle);
        }
        if block.state == BlockState::Free {
            return Err(AllocError::InvalidHandle);
        }

        block.refcount = block.refcount.saturating_add(1);
        block.state = BlockState::Shared;

        // Return new handle (same as input for now)
        Ok(handle)
    }

    /// Get pointer for a block.
    fn get_ptr(&self, handle: ModelHandle) -> Option<*mut u8> {
        let idx = handle.block_index as usize;
        if idx >= self.block_count {
            return None;
        }

        let block = &self.blocks[idx];
        if block.generation != handle.generation || block.state == BlockState::Free {
            return None;
        }

        Some((self.base_addr + idx * BLOCK_SIZE) as *mut u8)
    }

    /// Get allocation size.
    fn get_size(&self, handle: ModelHandle) -> Option<usize> {
        let idx = handle.block_index as usize;
        if idx >= self.block_count {
            return None;
        }

        let block = &self.blocks[idx];
        if block.generation != handle.generation || block.state == BlockState::Free {
            return None;
        }

        Some(block.size_blocks as usize * BLOCK_SIZE)
    }

    /// Validate a handle and return a mutable slot reference.
    fn slot_mut(&mut self, handle: ModelHandle) -> Result<&mut BlockSlot, AllocError> {
        let idx = handle.block_index as usize;
        if idx >= self.block_count {
            return Err(AllocError::InvalidHandle);
        }
        let block = &mut self.blocks[idx];
        if block.state == BlockState::Free {
            return Err(AllocError::InvalidHandle);
        }
        if block.generation != handle.generation {
            return Err(AllocError::StaleHandle);
        }
        Ok(block)
    }

    /// Bump access tracking fields. Called by `touch()`.
    fn touch(&mut self, handle: ModelHandle) -> Result<(), AllocError> {
        let now = kernel_ffi::get_time_ns();
        let slot = self.slot_mut(handle)?;
        slot.last_access_time = now;
        slot.access_count = slot.access_count.saturating_add(1);
        Ok(())
    }

    /// Assign identity metadata (model_id / layer_idx / priority) to a slot.
    fn set_metadata(
        &mut self,
        handle: ModelHandle,
        model_id: u8,
        layer_idx: i16,
        model_priority: u8,
    ) -> Result<(), AllocError> {
        let slot = self.slot_mut(handle)?;
        slot.model_id = model_id;
        slot.layer_idx = layer_idx;
        slot.model_priority = model_priority;
        Ok(())
    }

    /// Mark a slot as currently mapped for GPU DMA.
    fn set_gpu_mapped(&mut self, handle: ModelHandle, mapped: bool) -> Result<(), AllocError> {
        let slot = self.slot_mut(handle)?;
        slot.gpu_mapped = mapped;
        Ok(())
    }

    /// Mark a slot as dirty (written since last flush).
    fn set_dirty(&mut self, handle: ModelHandle, dirty: bool) -> Result<(), AllocError> {
        let slot = self.slot_mut(handle)?;
        slot.is_dirty = dirty;
        Ok(())
    }

    /// Snapshot a slot into an eviction-policy `BlockMeta`.
    ///
    /// The `block_id` packs `(pool_id, slot_index)` into a u32 so the
    /// policy can later pass it back through `update_feedback` and the
    /// runtime can route the id to the correct pool.
    #[cfg(feature = "ai_eviction")]
    fn snapshot_at(&self, idx: usize, pool_id: u8) -> Option<super::eviction::BlockMeta> {
        use super::eviction::{BlockMeta, PoolType};
        if idx >= self.block_count {
            return None;
        }
        let slot = &self.blocks[idx];
        if slot.state == BlockState::Free {
            return None;
        }
        let pool_type = match pool_id {
            POOL_WEIGHT => PoolType::Weight,
            POOL_WORKSPACE => PoolType::Workspace,
            _ => return None,
        };
        Some(BlockMeta {
            block_id: ((pool_id as u32) << 24) | (idx as u32 & 0x00FF_FFFF),
            pool_type,
            model_id: slot.model_id,
            layer_idx: slot.layer_idx,
            last_access_time: slot.last_access_time,
            load_time: slot.load_time,
            access_count: slot.access_count,
            ref_count: slot.refcount.min(u8::MAX as u16) as u8,
            gpu_mapped: slot.gpu_mapped,
            is_dirty: slot.is_dirty,
            model_priority: slot.model_priority,
        })
    }

    /// Get pool statistics.
    fn stats(&self) -> PoolStats {
        let mut allocated = 0;
        let mut shared = 0;

        for i in 0..self.block_count {
            match self.blocks[i].state {
                BlockState::Free => {}
                BlockState::Allocated => allocated += 1,
                BlockState::Shared => shared += 1,
            }
        }

        PoolStats {
            total_blocks: self.block_count,
            free_blocks: self.free_count,
            allocated_blocks: allocated,
            shared_blocks: shared,
            peak_usage: self.peak_usage,
            evictions_total: self.evictions_total,
        }
    }
}

// =============================================================================
// Pool Statistics
// =============================================================================

/// Pool usage statistics.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct PoolStats {
    pub total_blocks: usize,
    pub free_blocks: usize,
    pub allocated_blocks: usize,
    pub shared_blocks: usize,
    pub peak_usage: usize,
    /// Total number of blocks the allocator evicted to satisfy an
    /// incoming request. Monotonically increasing; reset only on
    /// `model_mem_init`. M6 wires this counter; earlier phases saw 0.
    pub evictions_total: u64,
}

impl PoolStats {
    /// Total memory in bytes.
    pub fn total_bytes(&self) -> usize {
        self.total_blocks * BLOCK_SIZE
    }

    /// Free memory in bytes.
    pub fn free_bytes(&self) -> usize {
        self.free_blocks * BLOCK_SIZE
    }

    /// Allocated memory in bytes.
    pub fn allocated_bytes(&self) -> usize {
        self.allocated_blocks * BLOCK_SIZE
    }
}

// =============================================================================
// Global Allocator State
// =============================================================================

/// Spinlock for protecting allocator state.
static LOCK: AtomicBool = AtomicBool::new(false);

/// Initialization state.
static INITIALIZED: AtomicU8 = AtomicU8::new(0);

/// Global allocator instance.
/// SAFETY: Only accessed while holding LOCK (enforced by SpinGuard).
static mut WEIGHT_POOL: MemoryPool = MemoryPool::new();
static mut WORKSPACE_POOL: MemoryPool = MemoryPool::new();

// SAFETY: The static-mut pools above are accessed only while holding
// `LOCK` (the SpinGuard pattern). MemoryPool itself contains plain
// data (a free-list array of usizes plus block-state metadata) — no
// interior mutability primitive is needed because the lock provides
// exclusive access. Spelling out the impl makes the soundness
// contract visible to callers and to future maintainers.
unsafe impl Sync for MemoryPool {}

/// Recently-evicted content tracker. `None` until `model_mem_init`
/// constructs it; retained for the life of the runtime. Protected by
/// the allocator's `LOCK` so callers holding the SpinGuard can
/// inspect or mutate it freely.
///
/// The tracker lives behind the `ai_eviction` feature gate; in
/// baseline builds the static is still present (as `None`) for
/// allocator simplicity but is never read. LTO drops it.
#[cfg(feature = "ai_eviction")]
static mut EVICTED_CONTENT_TRACKER: Option<super::eviction::EvictedContentTracker> = None;

/// RAII guard for the allocator spinlock.
///
/// Replaces manual `lock_acquire`/`lock_release` pairs: the lock is
/// released on drop, so panics or early returns can't leak the lock.
/// (We compile with `panic = "abort"`, but RAII still protects against
/// accidental `?` / early-return leaks.)
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

/// Check if initialized.
fn is_initialized() -> bool {
    INITIALIZED.load(Ordering::Acquire) == 1
}

// =============================================================================
// Public API
// =============================================================================

/// Initialize model memory pools.
///
/// Allocates memory from C PMM and sets up weight and workspace pools.
/// Each pool is allocated separately to work efficiently with buddy allocator.
///
/// # Arguments
/// * `weight_mb` - Size of weight pool in megabytes (must be multiple of 2)
/// * `workspace_mb` - Size of workspace pool in megabytes (must be multiple of 2)
///
/// # Errors
/// * `AllocError::AlignmentError` — `weight_mb` or `workspace_mb` is not
///   a multiple of 2 (the pool block size in MB).
/// * `AllocError::Oversized` — either pool would exceed
///   `MAX_BLOCKS_PER_POOL × BLOCK_SIZE`. Surface a loud error rather
///   than silently truncate; callers must keep the C-side
///   `MODEL_MEM_*_MB` knobs in lockstep with `MAX_BLOCKS_PER_POOL`.
/// * `AllocError::PmmFailed` — underlying PMM allocation failed.
pub fn model_mem_init(weight_mb: usize, workspace_mb: usize) -> Result<(), AllocError> {
    // Validate sizes are 2MB aligned
    if weight_mb % 2 != 0 || workspace_mb % 2 != 0 {
        return Err(AllocError::AlignmentError);
    }

    let weight_blocks = weight_mb / 2;
    let workspace_blocks = workspace_mb / 2;

    // Reject oversized requests up front, before touching PMM. The
    // pool's `init` would otherwise silently `count.min(MAX)` and
    // give back a smaller pool than the caller asked for.
    if weight_blocks > MAX_BLOCKS_PER_POOL || workspace_blocks > MAX_BLOCKS_PER_POOL {
        return Err(AllocError::Oversized);
    }

    // Allocate weight pool (power-of-2 pages work well with buddy allocator)
    // 256 MB = 65536 pages = order 16, exactly power of 2
    let weight_pages = weight_mb * 256; // MB to pages (4KB each)
    let weight_base_ptr = kernel_ffi::alloc_pages(weight_pages)
        .map_err(|_| AllocError::PmmFailed)?;
    let weight_base = weight_base_ptr.as_ptr() as usize;

    // Allocate workspace pool separately
    // 128 MB = 32768 pages = order 15, exactly power of 2
    let workspace_pages = workspace_mb * 256; // MB to pages (4KB each)
    let workspace_base_ptr = kernel_ffi::alloc_pages(workspace_pages)
        .map_err(|_| {
            // Free weight pool on failure
            unsafe { kernel_ffi::free_pages(weight_base_ptr, weight_pages); }
            AllocError::PmmFailed
        })?;
    let workspace_base = workspace_base_ptr.as_ptr() as usize;

    {
        let _g = SpinGuard::new();
        // SAFETY: SpinGuard held — exclusive access to the pool statics.
        // `addr_of_mut!` avoids creating a reference to the mutable static.
        unsafe {
            (*addr_of_mut!(WEIGHT_POOL)).init(weight_base, weight_blocks, true);
            (*addr_of_mut!(WORKSPACE_POOL)).init(workspace_base, workspace_blocks, false);

            // Install the eviction-feedback tracker once the heap is up.
            #[cfg(feature = "ai_eviction")]
            {
                *addr_of_mut!(EVICTED_CONTENT_TRACKER) =
                    Some(super::eviction::EvictedContentTracker::new());
                super::eviction::init();
            }
        }
        INITIALIZED.store(1, Ordering::Release);
    }

    Ok(())
}

/// Allocate model memory from the weight pool.
///
/// Weight memory is intended for read-only model parameters.
/// Returns a 2MB-aligned block. On pool exhaustion the active
/// eviction policy (gated on `ai_eviction`) selects a victim and the
/// alloc is retried once; if every block is pinned (`refcount > 1`)
/// the call still returns `AllocError::OutOfMemory`.
pub fn alloc_weights(_size: usize) -> Result<ModelHandle, AllocError> {
    alloc_with_eviction(POOL_WEIGHT)
}

/// Allocate model memory from the workspace pool.
///
/// Workspace memory is for per-inference scratch space.
/// Returns a 2MB-aligned block. Eviction semantics as `alloc_weights`.
pub fn alloc_workspace(_size: usize) -> Result<ModelHandle, AllocError> {
    alloc_with_eviction(POOL_WORKSPACE)
}

/// Fast-path allocation with optional eviction retry.
///
/// Takes the pool lock, tries the pool's `alloc`, and on
/// `OutOfMemory` hands the candidate list to the registered eviction
/// policy (when `ai_eviction` is on). The picked victim is freed
/// inside the same lock so no caller observes an empty pool between
/// eviction and retry.
fn alloc_with_eviction(pool_id: u8) -> Result<ModelHandle, AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }

    // First attempt — the common case, lock held once.
    let first = {
        let _g = SpinGuard::new();
        // SAFETY: SpinGuard held — exclusive access to the pool static.
        unsafe { pool_alloc_raw(pool_id) }
    };
    match first {
        Ok(h) => return Ok(h),
        Err(AllocError::OutOfMemory) => {
            #[cfg(feature = "ai_eviction")]
            {
                return evict_and_retry(pool_id);
            }
            #[cfg(not(feature = "ai_eviction"))]
            {
                return Err(AllocError::OutOfMemory);
            }
        }
        Err(e) => return Err(e),
    }
}

/// Invoke `alloc` on the pool identified by `pool_id`.
/// Caller must hold the allocator SpinGuard.
///
/// # Safety
/// Callers must hold `LOCK` (via SpinGuard) before invoking.
unsafe fn pool_alloc_raw(pool_id: u8) -> Result<ModelHandle, AllocError> {
    // Enforce the SAFETY contract in debug builds. Cheap and catches
    // a future caller that forgets to take the SpinGuard before
    // invoking pool_alloc_raw — which would otherwise be silent UB.
    debug_assert!(
        LOCK.load(Ordering::Relaxed),
        "pool_alloc_raw called without holding LOCK"
    );
    match pool_id {
        POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).alloc(pool_id),
        POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).alloc(pool_id),
        _ => Err(AllocError::InvalidHandle),
    }
}

/// Eviction retry path — only compiled when the `ai_eviction`
/// feature is on. Snapshots the requested pool's non-pinned
/// candidates, consults the active policy, frees the victim, and
/// retries the allocation.
#[cfg(feature = "ai_eviction")]
fn evict_and_retry(pool_id: u8) -> Result<ModelHandle, AllocError> {
    use super::eviction;

    // Flush expired tracker entries up-front. This is a cheap
    // background-cleanup step and produces a stream of
    // `was_fault=false` feedback for the CACHEUS selector. We
    // collect IDs under the allocator lock and drive feedback after
    // releasing it so the registry lock is never nested inside ours.
    let now = kernel_ffi::get_time_ns();
    let expired_ids: alloc::vec::Vec<u32> = {
        let _g = SpinGuard::new();
        // SAFETY: _g held — exclusive access.
        unsafe {
            match &mut *addr_of_mut!(EVICTED_CONTENT_TRACKER) {
                Some(t) => t.drain_expired(now),
                None => alloc::vec::Vec::new(),
            }
        }
    };
    for id in expired_ids {
        eviction::update_feedback(id, false);
    }

    // Build candidate set for this pool. `snapshot_evictable_blocks`
    // returns both pools' candidates — we filter to the requested one.
    let candidates: alloc::vec::Vec<eviction::BlockMeta> =
        snapshot_evictable_blocks()
            .into_iter()
            .filter(|m| {
                let want_pool = match pool_id {
                    POOL_WEIGHT => eviction::PoolType::Weight,
                    POOL_WORKSPACE => eviction::PoolType::Workspace,
                    _ => return false,
                };
                m.pool_type == want_pool
            })
            .collect();

    if candidates.is_empty() {
        // Every block in the pool is free (shouldn't happen here —
        // we're in the OOM branch) or pinned (ref_count > 1). Either
        // way, OOM is the right answer.
        return Err(AllocError::OutOfMemory);
    }

    // Ask the active policy to pick a victim.
    let victim_idx = match eviction::select_victim(&candidates) {
        Some(idx) => idx,
        None => return Err(AllocError::OutOfMemory), // no policy installed
    };
    let victim = candidates[victim_idx];

    // Decode block_id → ModelHandle. block_id encoding is
    // `(pool_id << 24) | slot_idx`. Generation is fetched live so
    // a concurrent free/realloc between snapshot and free is caught
    // as `StaleHandle` — in which case we retry directly.
    let slot_idx = (victim.block_id & 0x00FF_FFFF) as usize;

    // Perform the free under the pool lock. Record the eviction's
    // content key in the tracker so a subsequent alloc with the same
    // (model_id, layer_idx, pool_type) can credit CACHEUS.
    let content_key = eviction::ContentKey {
        pool_type: victim.pool_type,
        model_id: victim.model_id,
        layer_idx: victim.layer_idx,
    };
    {
        let _g = SpinGuard::new();
        // SAFETY: _g held — exclusive access.
        unsafe {
            let pool_ptr = match pool_id {
                POOL_WEIGHT => addr_of_mut!(WEIGHT_POOL),
                POOL_WORKSPACE => addr_of_mut!(WORKSPACE_POOL),
                _ => return Err(AllocError::InvalidHandle),
            };
            let pool = &mut *pool_ptr;
            if slot_idx >= pool.block_count {
                return Err(AllocError::InvalidHandle);
            }
            let generation = pool.blocks[slot_idx].generation;
            let h = ModelHandle {
                block_index: slot_idx as u16,
                pool_id,
                generation,
                _reserved: 0,
            };
            // Free via the pool's free (handles refcount internally,
            // wipes tracking). We accept StaleHandle as a benign race
            // and fall through to the retry. Only count an eviction
            // when the free actually reclaimed a slot — counting
            // StaleHandle losses would skew the metric upward on
            // every concurrent-touch race.
            let freed = pool.free(h).is_ok();
            if freed {
                pool.evictions_total = pool.evictions_total.saturating_add(1);
                if let Some(t) = &mut *addr_of_mut!(EVICTED_CONTENT_TRACKER) {
                    t.record_eviction(content_key, victim.block_id, now);
                }
            }
        }
    }

    // #114: inform the active policy that this block was evicted so
    // ARC can populate its ghost lists proactively. Called outside the
    // pool lock — the registry lock is independent. The pool type is
    // known from pool_id; map it here so the registry dispatches to
    // the correct per-pool policy.
    {
        let pool_type = match pool_id {
            POOL_WEIGHT => eviction::PoolType::Weight,
            POOL_WORKSPACE => eviction::PoolType::Workspace,
            _ => eviction::PoolType::Weight,
        };
        eviction::notify_eviction(victim.block_id, pool_type);
    }

    // Retry allocation. If this still fails, the pool is genuinely
    // broken — return whatever error the retry produces.
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held.
    unsafe { pool_alloc_raw(pool_id) }
}

/// Free model memory.
///
/// If the block is shared (refcount > 1), this decrements the refcount.
/// The block is only actually freed when refcount reaches 0.
pub fn free(handle: ModelHandle) -> Result<(), AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }
    if handle.is_null() {
        return Err(AllocError::InvalidHandle);
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).free(handle),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).free(handle),
            _ => Err(AllocError::InvalidHandle),
        }
    }
}

/// Share model memory with another component.
///
/// Increments reference count, allowing multiple users of the same memory.
/// Returns a new handle (identical to input for now).
pub fn share(handle: ModelHandle) -> Result<ModelHandle, AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }
    if handle.is_null() {
        return Err(AllocError::InvalidHandle);
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).share(handle),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).share(handle),
            _ => Err(AllocError::InvalidHandle),
        }
    }
}

/// Release a shared reference to model memory.
///
/// Alias for `free()` - decrements refcount, frees if it reaches 0.
pub fn unshare(handle: ModelHandle) -> Result<(), AllocError> {
    free(handle)
}

/// Get raw pointer to model memory.
///
/// Returns `None` if handle is invalid or stale.
pub fn get_ptr(handle: ModelHandle) -> Option<*mut u8> {
    if !is_initialized() || handle.is_null() {
        return None;
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).get_ptr(handle),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).get_ptr(handle),
            _ => None,
        }
    }
}

/// Get size of allocation in bytes.
///
/// Returns `None` if handle is invalid or stale.
pub fn get_size(handle: ModelHandle) -> Option<usize> {
    if !is_initialized() || handle.is_null() {
        return None;
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).get_size(handle),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).get_size(handle),
            _ => None,
        }
    }
}

/// Get weight pool statistics.
pub fn weight_pool_stats() -> PoolStats {
    if !is_initialized() {
        return PoolStats {
            total_blocks: 0,
            free_blocks: 0,
            allocated_blocks: 0,
            shared_blocks: 0,
            peak_usage: 0,
            evictions_total: 0,
        };
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to WEIGHT_POOL.
    unsafe { (*addr_of_mut!(WEIGHT_POOL)).stats() }
}

/// Get workspace pool statistics.
pub fn workspace_pool_stats() -> PoolStats {
    if !is_initialized() {
        return PoolStats {
            total_blocks: 0,
            free_blocks: 0,
            allocated_blocks: 0,
            shared_blocks: 0,
            peak_usage: 0,
            evictions_total: 0,
        };
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to WORKSPACE_POOL.
    unsafe { (*addr_of_mut!(WORKSPACE_POOL)).stats() }
}

// =============================================================================
// Eviction-Policy Tracking API
// =============================================================================

/// Update access tracking for a block.
///
/// Bumps `access_count` and `last_access_time` so eviction policies see
/// recent activity. Callers MUST invoke this for the policy to learn —
/// reads/writes through `get_ptr` are opaque to the allocator.
pub fn touch(handle: ModelHandle) -> Result<(), AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }
    if handle.is_null() {
        return Err(AllocError::InvalidHandle);
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).touch(handle),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).touch(handle),
            _ => Err(AllocError::InvalidHandle),
        }
    }
}

/// Attach identity metadata to a block for eviction-policy use.
///
/// `model_id` groups blocks belonging to the same model; `layer_idx`
/// orders them within the model (negative values reserved for
/// non-layered blocks like tokeniser tables); `model_priority` lets
/// policies weight critical models (e.g. a safety monitor) higher.
pub fn set_metadata(
    handle: ModelHandle,
    model_id: u8,
    layer_idx: i16,
    model_priority: u8,
) -> Result<(), AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }
    if handle.is_null() {
        return Err(AllocError::InvalidHandle);
    }

    let result = {
        let _g = SpinGuard::new();
        // SAFETY: SpinGuard held — exclusive access to pool statics.
        unsafe {
            match handle.pool_id {
                POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL))
                    .set_metadata(handle, model_id, layer_idx, model_priority),
                POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL))
                    .set_metadata(handle, model_id, layer_idx, model_priority),
                _ => Err(AllocError::InvalidHandle),
            }
        }
    };
    // After the caller labels the block, probe the content tracker —
    // a hit means this alloc just "re-admitted" a content key that
    // was evicted recently, which is the CACHEUS fault signal. Done
    // with the allocator lock released so the registry's lock isn't
    // nested inside ours.
    #[cfg(feature = "ai_eviction")]
    if result.is_ok() {
        let pool_type = match handle.pool_id {
            POOL_WEIGHT => super::eviction::PoolType::Weight,
            POOL_WORKSPACE => super::eviction::PoolType::Workspace,
            _ => return result,
        };
        let key = super::eviction::ContentKey {
            pool_type, model_id, layer_idx,
        };
        let now = kernel_ffi::get_time_ns();
        let hit_id = {
            let _g = SpinGuard::new();
            // SAFETY: _g held — exclusive access.
            unsafe {
                match &mut *addr_of_mut!(EVICTED_CONTENT_TRACKER) {
                    Some(t) => t.probe_on_alloc(key, now),
                    None => None,
                }
            }
        };
        if let Some(old_id) = hit_id {
            super::eviction::update_feedback(old_id, true);
        }
    }
    result
}

/// Flag or un-flag a block as currently GPU-mapped for DMA.
pub fn set_gpu_mapped(handle: ModelHandle, mapped: bool) -> Result<(), AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }
    if handle.is_null() {
        return Err(AllocError::InvalidHandle);
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).set_gpu_mapped(handle, mapped),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).set_gpu_mapped(handle, mapped),
            _ => Err(AllocError::InvalidHandle),
        }
    }
}

/// Flag or un-flag a block as dirty (written since last flush).
pub fn set_dirty(handle: ModelHandle, dirty: bool) -> Result<(), AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }
    if handle.is_null() {
        return Err(AllocError::InvalidHandle);
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        match handle.pool_id {
            POOL_WEIGHT => (*addr_of_mut!(WEIGHT_POOL)).set_dirty(handle, dirty),
            POOL_WORKSPACE => (*addr_of_mut!(WORKSPACE_POOL)).set_dirty(handle, dirty),
            _ => Err(AllocError::InvalidHandle),
        }
    }
}

/// Read-only accessor for a block's tracking fields.
///
/// `None` on invalid / stale / free handles. Tuple is
/// `(load_time, last_access_time, access_count, model_id, layer_idx,
/// model_priority, gpu_mapped, is_dirty)`. Useful for tests that need
/// to inspect state between operations; hot-path consumers should use
/// `snapshot_evictable_blocks` instead.
pub fn get_tracking(
    handle: ModelHandle,
) -> Option<(u64, u64, u32, u8, i16, u8, bool, bool)> {
    if !is_initialized() || handle.is_null() {
        return None;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        let pool_ptr = match handle.pool_id {
            POOL_WEIGHT => addr_of_mut!(WEIGHT_POOL),
            POOL_WORKSPACE => addr_of_mut!(WORKSPACE_POOL),
            _ => return None,
        };
        let pool = &*pool_ptr;
        let idx = handle.block_index as usize;
        if idx >= pool.block_count {
            return None;
        }
        let slot = &pool.blocks[idx];
        if slot.state == BlockState::Free || slot.generation != handle.generation {
            return None;
        }
        Some((
            slot.load_time,
            slot.last_access_time,
            slot.access_count,
            slot.model_id,
            slot.layer_idx,
            slot.model_priority,
            slot.gpu_mapped,
            slot.is_dirty,
        ))
    }
}

/// Build a snapshot of every allocated, non-pinned block across both
/// pools for the active eviction policy.
///
/// A block is "evictable" when it is allocated and `ref_count == 0` —
/// shared blocks are considered pinned (matches the M6 policy filter).
/// The resulting `Vec` is heap-allocated; callers on the hot path
/// should pre-size / reuse if this becomes a bottleneck.
#[cfg(feature = "ai_eviction")]
pub fn snapshot_evictable_blocks() -> alloc::vec::Vec<super::eviction::BlockMeta> {
    let mut out: alloc::vec::Vec<super::eviction::BlockMeta> = alloc::vec::Vec::new();
    if !is_initialized() {
        return out;
    }
    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to pool statics.
    unsafe {
        for (pool_id, pool_ptr) in [
            (POOL_WEIGHT, addr_of_mut!(WEIGHT_POOL)),
            (POOL_WORKSPACE, addr_of_mut!(WORKSPACE_POOL)),
        ] {
            let pool = &*pool_ptr;
            for i in 0..pool.block_count {
                let slot = &pool.blocks[i];
                if slot.state == BlockState::Free || slot.refcount > 1 {
                    continue;
                }
                if let Some(meta) = pool.snapshot_at(i, pool_id) {
                    out.push(meta);
                }
            }
        }
    }
    out
}

// =============================================================================
// GPU Integration (Stubs)
// =============================================================================

/// Error type for GPU operations.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuError {
    /// Model memory handle is invalid.
    InvalidHandle,
    /// GPU mapping not supported (placeholder driver).
    NotSupported,
    /// Cache flush/invalidate failed.
    CacheError,
}

/// Map model memory for GPU DMA access.
///
/// Flushes CPU caches so the GPU sees the latest data, then returns the
/// physical address (identity-mapped in the kernel). On platforms with
/// unified memory (Jetson Orin), this is sufficient for GPU access.
pub fn gpu_map(handle: ModelHandle) -> Result<u64, GpuError> {
    let ptr = get_ptr(handle).ok_or(GpuError::InvalidHandle)?;
    let size = get_size(handle).ok_or(GpuError::InvalidHandle)?;

    // Flush CPU caches so GPU sees latest data
    unsafe {
        crate::kernel_ffi::slm_gpu_sync_for_device(ptr as *mut u8, size);
    }

    // Flag the block as GPU-mapped for eviction policies; ignore a
    // stale-handle error — the handle was validated by `get_ptr` above.
    let _ = set_gpu_mapped(handle, true);

    // Return physical address (identity mapped in our kernel)
    Ok(ptr as u64)
}

/// Unmap model memory from GPU.
///
/// Invalidates CPU caches so the CPU sees any GPU-written data.
pub fn gpu_unmap(handle: ModelHandle) -> Result<(), GpuError> {
    let ptr = get_ptr(handle).ok_or(GpuError::InvalidHandle)?;
    let size = get_size(handle).ok_or(GpuError::InvalidHandle)?;

    // Invalidate CPU caches so CPU sees GPU-written data
    unsafe {
        crate::kernel_ffi::slm_gpu_sync_for_cpu(ptr as *mut u8, size);
    }

    // Clear the GPU-mapped flag for eviction policies.
    let _ = set_gpu_mapped(handle, false);

    Ok(())
}

#[cfg(test)]
mod tests {
    //! Unit tests for `model_mem_init`'s precondition checks.
    //!
    //! Both `AlignmentError` and `Oversized` short-circuit before any
    //! `kernel_ffi::alloc_pages` call, so they're reachable from
    //! `cargo test` without a stubbed PMM. Coverage for the success
    //! path lives in `kernel/tests/test_model_mem_smoke.c` because it
    //! requires real PMM.
    use super::*;
    // The runtime crate is `no_std`; tests run on the host where the
    // panic + format machinery used by `assert_eq!` lives in `std`.
    extern crate std;

    /// Smallest pool request (in MB) that exceeds the
    /// `MAX_BLOCKS_PER_POOL` cap. `MAX × 2 MB` is exactly the
    /// boundary; one more block (+2 MB) puts us one legal increment
    /// past it. Defined once so the boundary expression has a single
    /// source of truth across the four tests below.
    const FIRST_OVERSIZED_MB: usize = (MAX_BLOCKS_PER_POOL + 1) * 2;

    #[test]
    fn rejects_misaligned_weight_request() {
        // Odd MB violates the 2 MB block alignment.
        assert_eq!(model_mem_init(3, 2), Err(AllocError::AlignmentError));
    }

    #[test]
    fn rejects_misaligned_workspace_request() {
        assert_eq!(model_mem_init(2, 3), Err(AllocError::AlignmentError));
    }

    #[test]
    fn rejects_oversized_weight_request() {
        assert_eq!(
            model_mem_init(FIRST_OVERSIZED_MB, 2),
            Err(AllocError::Oversized)
        );
    }

    #[test]
    fn rejects_oversized_workspace_request() {
        assert_eq!(
            model_mem_init(2, FIRST_OVERSIZED_MB),
            Err(AllocError::Oversized)
        );
    }

    #[test]
    fn alignment_check_fires_before_oversized_check() {
        // A request that's both oversized AND misaligned should
        // surface AlignmentError first, matching the order of the
        // checks in `model_mem_init`. Pinning the order so a future
        // refactor that swaps them surfaces here, not in production.
        // `+ 1` makes the value odd, tripping the alignment check.
        assert_eq!(
            model_mem_init(FIRST_OVERSIZED_MB + 1, 2),
            Err(AllocError::AlignmentError)
        );
    }
}
