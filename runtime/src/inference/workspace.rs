//! Bump allocator for inference workspace memory.
//!
//! Allocates tensor storage from a contiguous 2MB workspace block.
//! Reset between inference calls for O(1) "free all".

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
    /// Returns null if workspace is exhausted.
    pub fn alloc(&mut self, size: usize, align: usize) -> *mut u8 {
        // Align offset up
        let aligned = (self.offset + align - 1) & !(align - 1);
        if aligned + size > self.capacity {
            return core::ptr::null_mut();
        }
        let ptr = unsafe { self.base.add(aligned) };
        self.offset = aligned + size;
        ptr
    }

    /// Allocate a tensor with the given shape (FP32).
    ///
    /// Returns a Tensor descriptor pointing to the allocated workspace memory.
    pub fn alloc_tensor(&mut self, shape: &[u32]) -> Option<Tensor> {
        let mut n_elem: usize = 1;
        for &d in shape {
            n_elem *= d as usize;
        }
        let size = n_elem * 4; // FP32
        let ptr = self.alloc(size, TENSOR_ALIGN);
        if ptr.is_null() {
            return None;
        }
        Some(Tensor::new(ptr as *const f32, shape))
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
