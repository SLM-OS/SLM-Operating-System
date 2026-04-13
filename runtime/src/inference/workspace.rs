//! Bump allocator for inference workspace memory.
//!
//! Allocates tensor storage from a contiguous 2MB workspace block.
//! Reset between inference calls for O(1) "free all".

use super::engine::EngineError;
use super::tensor::Tensor;

/// Alignment for tensor data (16 bytes for NEON/SSE compatibility).
const TENSOR_ALIGN: usize = 16;

/// Bump allocator over a workspace memory block.
pub struct BumpAllocator {
    base: *mut u8,
    capacity: usize,
    offset: usize,
}

// SAFETY: BumpAllocator operates on kernel-managed workspace memory.
unsafe impl Send for BumpAllocator {}

impl BumpAllocator {
    /// Create an empty (uninitialized) allocator for static storage.
    pub const fn empty() -> Self {
        Self {
            base: core::ptr::null_mut(),
            capacity: 0,
            offset: 0,
        }
    }

    /// Create a new bump allocator over a workspace block.
    pub fn new(base: *mut u8, capacity: usize) -> Self {
        Self {
            base,
            capacity,
            offset: 0,
        }
    }

    /// Allocate `size` bytes with the given alignment.
    ///
    /// Returns null if workspace is exhausted or the alignment arithmetic
    /// would wrap `usize`. Caller must pass `align` as a non-zero power of
    /// two.
    pub fn alloc(&mut self, size: usize, align: usize) -> *mut u8 {
        // Align offset up with explicit overflow check — the naive
        // `(offset + align - 1) & !(align - 1)` can wrap near `usize::MAX`
        // and yield a bogus pointer that still passes a wrap-unaware
        // capacity check.
        let aligned = match self.offset.checked_add(align - 1) {
            Some(a) => a & !(align - 1),
            None => return core::ptr::null_mut(),
        };
        // `capacity - aligned` without underflow; also fails if `size`
        // won't fit in the remainder.
        if self.capacity.checked_sub(aligned).map_or(true, |rem| rem < size) {
            return core::ptr::null_mut();
        }
        // SAFETY: `aligned + size <= capacity` verified above, and
        // `base..base + capacity` is a valid kernel-owned workspace region.
        let ptr = unsafe { self.base.add(aligned) };
        self.offset = aligned + size;
        ptr
    }

    /// Allocate a tensor with the given shape (FP32).
    ///
    /// Returns `ShapeOverflow` if the shape dimensions multiplied beyond
    /// `usize::MAX` (malformed model), `WorkspaceExhausted` if the bump
    /// allocator is full.
    pub fn alloc_tensor(&mut self, shape: &[u32]) -> Result<Tensor, EngineError> {
        let mut n_elem: usize = 1;
        for &d in shape {
            n_elem = n_elem
                .checked_mul(d as usize)
                .ok_or(EngineError::ShapeOverflow)?;
        }
        let size = n_elem
            .checked_mul(core::mem::size_of::<f32>())
            .ok_or(EngineError::ShapeOverflow)?;
        let ptr = self.alloc(size, TENSOR_ALIGN);
        if ptr.is_null() {
            return Err(EngineError::WorkspaceExhausted);
        }
        Ok(Tensor::new(ptr as *const f32, shape))
    }

    /// Reset the allocator — frees all workspace allocations in O(1).
    pub fn reset(&mut self) {
        self.offset = 0;
    }

    /// Bytes remaining in workspace.
    pub fn remaining(&self) -> usize {
        self.capacity.saturating_sub(self.offset)
    }

    /// Bytes used.
    pub fn used(&self) -> usize {
        self.offset
    }
}
