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

/// Maximum blocks per pool (16-bit index in handle)
const MAX_BLOCKS_PER_POOL: usize = 256;

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

/// Per-block metadata.
struct BlockMeta {
    state: BlockState,
    refcount: u16,
    generation: u8,
    size_blocks: u8,    // Contiguous blocks (for future multi-block alloc)
    owner_task: u32,
}

impl BlockMeta {
    const fn new() -> Self {
        Self {
            state: BlockState::Free,
            refcount: 0,
            generation: 0,
            size_blocks: 0,
            owner_task: 0,
        }
    }
}

// =============================================================================
// Memory Pool
// =============================================================================

/// A pool of 2MB-aligned memory blocks.
struct MemoryPool {
    base_addr: usize,
    block_count: usize,
    blocks: [BlockMeta; MAX_BLOCKS_PER_POOL],
    free_count: usize,
    peak_usage: usize,
    read_only: bool,
}

impl MemoryPool {
    const fn new() -> Self {
        Self {
            base_addr: 0,
            block_count: 0,
            blocks: [const { BlockMeta::new() }; MAX_BLOCKS_PER_POOL],
            free_count: 0,
            peak_usage: 0,
            read_only: false,
        }
    }

    /// Initialize pool with given base address and block count.
    fn init(&mut self, base: usize, count: usize, read_only: bool) {
        self.base_addr = base;
        self.block_count = count.min(MAX_BLOCKS_PER_POOL);
        self.free_count = self.block_count;
        self.peak_usage = 0;
        self.read_only = read_only;

        // Mark all blocks as free
        for i in 0..self.block_count {
            self.blocks[i] = BlockMeta::new();
        }
    }

    /// Find and allocate a free block.
    fn alloc(&mut self, pool_id: u8) -> Result<ModelHandle, AllocError> {
        if self.free_count == 0 {
            return Err(AllocError::OutOfMemory);
        }

        // Linear scan for free block (simple, works for small pools)
        for i in 0..self.block_count {
            if self.blocks[i].state == BlockState::Free {
                self.blocks[i].state = BlockState::Allocated;
                self.blocks[i].refcount = 1;
                self.blocks[i].size_blocks = 1;
                self.blocks[i].owner_task = kernel_ffi::task_current().0;

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
/// Returns `AllocError::PmmFailed` if PMM allocation fails.
pub fn model_mem_init(weight_mb: usize, workspace_mb: usize) -> Result<(), AllocError> {
    // Validate sizes are 2MB aligned
    if weight_mb % 2 != 0 || workspace_mb % 2 != 0 {
        return Err(AllocError::AlignmentError);
    }

    let weight_blocks = weight_mb / 2;
    let workspace_blocks = workspace_mb / 2;

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
        }
        INITIALIZED.store(1, Ordering::Release);
    }

    Ok(())
}

/// Allocate model memory from the weight pool.
///
/// Weight memory is intended for read-only model parameters.
/// Returns a 2MB-aligned block.
pub fn alloc_weights(_size: usize) -> Result<ModelHandle, AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }

    // For now, we allocate whole 2MB blocks regardless of size
    // Future: support multi-block allocations for larger models

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to WEIGHT_POOL.
    unsafe { (*addr_of_mut!(WEIGHT_POOL)).alloc(POOL_WEIGHT) }
}

/// Allocate model memory from the workspace pool.
///
/// Workspace memory is for per-inference scratch space.
/// Returns a 2MB-aligned block.
pub fn alloc_workspace(_size: usize) -> Result<ModelHandle, AllocError> {
    if !is_initialized() {
        return Err(AllocError::NotInitialized);
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to WORKSPACE_POOL.
    unsafe { (*addr_of_mut!(WORKSPACE_POOL)).alloc(POOL_WORKSPACE) }
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
        };
    }

    let _g = SpinGuard::new();
    // SAFETY: SpinGuard held — exclusive access to WORKSPACE_POOL.
    unsafe { (*addr_of_mut!(WORKSPACE_POOL)).stats() }
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

    Ok(())
}
