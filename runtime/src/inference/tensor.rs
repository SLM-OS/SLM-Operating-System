//! Lightweight tensor descriptor for inference.
//!
//! A `Tensor` is a non-owning view of FP32 data in workspace or weight memory.
//! It does not own or free its data.

/// Maximum tensor dimensions.
pub const MAX_DIMS: usize = 8;

/// Element type for tensor data.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum TensorElemType {
    Float32 = 0,
    Float16 = 1,
    Int8 = 2,
}

/// Quantization parameters for INT8 tensors.
///
/// Maps between INT8 and FP32: `real_value = scale * (int8_value - zero_point)`
#[derive(Clone, Copy, Debug)]
pub struct QuantParams {
    pub scale: f32,
    pub zero_point: i8,
}

/// A non-owning tensor descriptor.
///
/// Points to data stored in workspace or weight memory blocks.
/// Supports FP32 (default) and FP16 (weights loaded from FP16 ONNX models
/// when `skip_fp16_conversion` is enabled in the loader).
#[derive(Clone, Copy)]
pub struct Tensor {
    pub data: *const f32,
    pub shape: [u32; MAX_DIMS],
    pub ndim: u8,
    pub elem_type: TensorElemType,
    pub quant: QuantParams,
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
        elem_type: TensorElemType::Float32,
        quant: QuantParams { scale: 1.0, zero_point: 0 },
    };

    /// Create a tensor from a data pointer and shape (FP32).
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

    /// Create an FP16 tensor. Data pointer is cast from *const u16.
    pub fn new_fp16(data: *const u16, shape: &[u32]) -> Self {
        let mut t = Self::EMPTY;
        t.data = data as *const f32; // stored as raw pointer, interpreted based on elem_type
        t.elem_type = TensorElemType::Float16;
        let ndim = core::cmp::min(shape.len(), MAX_DIMS);
        for i in 0..ndim {
            t.shape[i] = shape[i];
        }
        t.ndim = ndim as u8;
        t
    }

    /// Create an INT8 quantized tensor.
    pub fn new_int8(data: *const i8, shape: &[u32], scale: f32, zero_point: i8) -> Self {
        let mut t = Self::EMPTY;
        t.data = data as *const f32;
        t.elem_type = TensorElemType::Int8;
        t.quant = QuantParams { scale, zero_point };
        let ndim = core::cmp::min(shape.len(), MAX_DIMS);
        for i in 0..ndim {
            t.shape[i] = shape[i];
        }
        t.ndim = ndim as u8;
        t
    }

    /// Whether this tensor holds FP16 data.
    pub fn is_fp16(&self) -> bool {
        self.elem_type == TensorElemType::Float16
    }

    /// Whether this tensor holds INT8 quantized data.
    pub fn is_int8(&self) -> bool {
        self.elem_type == TensorElemType::Int8
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

    /// Size in bytes.
    pub fn size_bytes(&self) -> usize {
        let elem_size = match self.elem_type {
            TensorElemType::Float32 => 4,
            TensorElemType::Float16 => 2,
            TensorElemType::Int8 => 1,
        };
        self.num_elements() * elem_size
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
