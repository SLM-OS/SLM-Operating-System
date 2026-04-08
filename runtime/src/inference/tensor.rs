//! Lightweight tensor descriptor for inference.
//!
//! A `Tensor` is a non-owning view of FP32 data in workspace or weight memory.
//! It does not own or free its data.

/// Maximum tensor dimensions.
pub const MAX_DIMS: usize = 8;

/// A non-owning tensor descriptor.
///
/// Points to FP32 data stored in workspace or weight memory blocks.
/// ~44 bytes on 64-bit (pointer + 8×u32 + u8).
#[derive(Clone, Copy)]
pub struct Tensor {
    pub data: *const f32,
    pub shape: [u32; MAX_DIMS],
    pub ndim: u8,
}

// SAFETY: Tensor is just a pointer + shape metadata. The underlying data
// lives in kernel-managed memory (weight pool or workspace) which is
// accessible from any CPU.
unsafe impl Send for Tensor {}
unsafe impl Sync for Tensor {}

impl Tensor {
    pub const EMPTY: Self = Self {
        data: core::ptr::null(),
        shape: [0; MAX_DIMS],
        ndim: 0,
    };

    /// Create a tensor from a data pointer and shape.
    pub fn new(data: *const f32, shape: &[u32]) -> Self {
        let mut t = Self::EMPTY;
        t.data = data;
        let ndim = core::cmp::min(shape.len(), MAX_DIMS);
        for i in 0..ndim {
            t.shape[i] = shape[i];
        }
        t.ndim = ndim as u8;
        t
    }

    /// Total number of elements.
    pub fn num_elements(&self) -> usize {
        if self.ndim == 0 {
            return 0;
        }
        let mut total: usize = 1;
        for i in 0..self.ndim as usize {
            total *= self.shape[i] as usize;
        }
        total
    }

    /// Size in bytes (FP32).
    pub fn size_bytes(&self) -> usize {
        self.num_elements() * 4
    }

    /// Get a mutable pointer to the data (for writing output tensors).
    pub fn data_mut(&self) -> *mut f32 {
        self.data as *mut f32
    }

    /// Get dimension at index.
    pub fn dim(&self, i: usize) -> u32 {
        if i < self.ndim as usize {
            self.shape[i]
        } else {
            0
        }
    }

    /// Whether this is a 2D matrix.
    pub fn is_matrix(&self) -> bool {
        self.ndim == 2
    }

    /// Number of rows (second-to-last dimension, or 1 if 1D).
    pub fn rows(&self) -> u32 {
        if self.ndim >= 2 {
            self.shape[self.ndim as usize - 2]
        } else if self.ndim == 1 {
            1
        } else {
            0
        }
    }

    /// Number of columns (last dimension).
    pub fn cols(&self) -> u32 {
        if self.ndim >= 1 {
            self.shape[self.ndim as usize - 1]
        } else {
            0
        }
    }

    /// Read an element at a flat index.
    ///
    /// # Safety
    /// Caller must ensure `index < num_elements()`.
    pub unsafe fn get(&self, index: usize) -> f32 {
        *self.data.add(index)
    }

    /// Write an element at a flat index.
    ///
    /// # Safety
    /// Caller must ensure `index < num_elements()` and data is writable.
    pub unsafe fn set(&self, index: usize, value: f32) {
        *self.data_mut().add(index) = value;
    }
}

impl core::fmt::Debug for Tensor {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "Tensor([")?;
        for i in 0..self.ndim as usize {
            if i > 0 {
                write!(f, ",")?;
            }
            write!(f, "{}", self.shape[i])?;
        }
        write!(f, "] {}B)", self.size_bytes())
    }
}
