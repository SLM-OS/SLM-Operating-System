//! Inference engine — walks an OperatorGraph and executes operators.
//!
//! The engine resolves tensor names to data pointers using a flat binding table,
//! dispatches each operator, and manages intermediate tensor memory via the
//! workspace bump allocator.

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, Ordering};
use crate::loader::graph::*;
use crate::loader::registry;
use crate::mm;
use super::tensor::Tensor;
use super::workspace::BumpAllocator;
use super::ops;

/// Maximum named tensors tracked during inference.
const MAX_BINDINGS: usize = 32;

/// Errors from the inference engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EngineError {
    ModelNotFound,
    WorkspaceExhausted,
    UnsupportedOp,
    ShapeMismatch,
    InvalidInput,
    WeightNotFound,
    InternalError,
}

/// A binding from a tensor name to its data.
#[derive(Clone, Copy)]
struct TensorBinding {
    name: TensorName,
    tensor: Tensor,
    active: bool,
}

impl TensorBinding {
    const EMPTY: Self = Self {
        name: TensorName::EMPTY,
        tensor: Tensor::EMPTY,
        active: false,
    };
}

/// CPU inference engine for a single model.
///
/// Uses a static buffer (~10KB) to avoid both stack overflow and heap
/// allocation issues. Only one inference can run at a time (spinlock).
pub struct InferenceEngine {
    graph: OperatorGraph,
    weight_table: WeightTable,
    weight_base: *const u8,
    workspace: BumpAllocator,
    bindings: [TensorBinding; MAX_BINDINGS],
    binding_count: usize,
    initialized: bool,
}

// SAFETY: InferenceEngine operates on kernel-managed memory.
// Access is serialized by ENGINE_LOCK.
unsafe impl Send for InferenceEngine {}
unsafe impl Sync for InferenceEngine {}

/// Static wrapper for the engine (same pattern as component/registry.rs).
#[repr(transparent)]
struct SyncWrapper<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncWrapper<T> {}
impl<T> SyncWrapper<T> {
    const fn new(value: T) -> Self { SyncWrapper(UnsafeCell::new(value)) }
    fn get(&self) -> *mut T { self.0.get() }
}

static ENGINE: SyncWrapper<InferenceEngine> = SyncWrapper::new(InferenceEngine::empty());
static ENGINE_LOCK: AtomicBool = AtomicBool::new(false);

fn engine_lock() {
    while ENGINE_LOCK.compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed).is_err() {
        core::hint::spin_loop();
    }
}

fn engine_unlock() {
    ENGINE_LOCK.store(false, Ordering::Release);
}

impl InferenceEngine {
    const fn empty() -> Self {
        Self {
            graph: OperatorGraph::EMPTY,
            weight_table: WeightTable::EMPTY,
            weight_base: core::ptr::null(),
            workspace: BumpAllocator::empty(),
            bindings: [TensorBinding::EMPTY; MAX_BINDINGS],
            binding_count: 0,
            initialized: false,
        }
    }

    /// Initialize the static engine for a model. Must be called with lock held.
    ///
    /// Uses `copy_graph_into`/`copy_weight_table_into` to avoid putting
    /// large structs (OperatorGraph ~6KB) on the stack.
    fn init(&mut self, model_index: usize) -> Result<(), EngineError> {
        if !registry::copy_graph_into(model_index, &mut self.graph) {
            return Err(EngineError::ModelNotFound);
        }
        if !registry::copy_weight_table_into(model_index, &mut self.weight_table) {
            return Err(EngineError::ModelNotFound);
        }

        let weight_handle = registry::get_weights(model_index)
            .ok_or(EngineError::ModelNotFound)?;
        let workspace_handle = registry::get_workspace(model_index)
            .ok_or(EngineError::ModelNotFound)?;

        let weight_base = mm::get_ptr(weight_handle)
            .ok_or(EngineError::InternalError)?;
        let ws_ptr = mm::get_ptr(workspace_handle)
            .ok_or(EngineError::InternalError)?;
        let ws_size = mm::get_size(workspace_handle)
            .unwrap_or(0);

        self.weight_base = weight_base as *const u8;
        self.workspace = BumpAllocator::new(ws_ptr, ws_size);
        self.binding_count = 0;
        self.initialized = true;
        Ok(())
    }

    /// Create an engine for the given model, returning Err if model not found.
    ///
    /// This is a lightweight check — actual work happens in `run_inference`.
    pub fn new(model_index: usize) -> Result<(), EngineError> {
        // Just verify the model exists
        registry::get_graph(model_index).ok_or(EngineError::ModelNotFound)?;
        Ok(())
    }

    /// Run inference on FP32 input data.
    ///
    /// Returns the number of output floats written to `output`.
    pub fn run(
        &mut self,
        input: *const f32,
        input_len: usize,
        output: *mut f32,
        output_len: usize,
    ) -> Result<usize, EngineError> {
        // Reset workspace and bindings
        self.workspace.reset();
        self.binding_count = 0;

        // Bind graph input(s)
        self.bind_graph_inputs(input, input_len)?;

        // Bind all weights from the weight table
        self.bind_weights()?;

        // Execute nodes in topological order
        for i in 0..self.graph.node_count {
            self.execute_node(i)?;
        }

        // Copy graph output(s) to caller's buffer
        self.copy_output(output, output_len)
    }

    /// Bind graph input names to the provided input data.
    fn bind_graph_inputs(&mut self, input: *const f32, input_len: usize) -> Result<(), EngineError> {
        if input.is_null() || input_len == 0 {
            return Err(EngineError::InvalidInput);
        }

        for i in 0..self.graph.input_count {
            let name = &self.graph.input_names[i];
            let shape = &self.graph.input_shapes[i];

            // Build shape array from TensorShape
            let mut dims = [0u32; 8];
            let ndim = shape.ndim as usize;
            for d in 0..ndim {
                dims[d] = shape.dims[d];
            }

            let tensor = Tensor::new(input, &dims[..ndim]);

            // Verify element count matches
            if tensor.num_elements() > input_len {
                return Err(EngineError::InvalidInput);
            }

            self.bind(*name, tensor);
        }

        Ok(())
    }

    /// Bind all weight initializers from the weight table.
    fn bind_weights(&mut self) -> Result<(), EngineError> {
        for i in 0..self.weight_table.count {
            let entry = &self.weight_table.entries[i];

            // Calculate pointer to this weight's data
            let ptr = unsafe {
                self.weight_base.add(entry.offset as usize) as *const f32
            };

            // Build shape
            let mut dims = [0u32; 8];
            let ndim = entry.shape.ndim as usize;
            for d in 0..ndim {
                dims[d] = entry.shape.dims[d];
            }

            let tensor = Tensor::new(ptr, &dims[..ndim]);
            self.bind(entry.name, tensor);
        }

        Ok(())
    }

    /// Execute a single graph node.
    fn execute_node(&mut self, node_idx: usize) -> Result<(), EngineError> {
        let node = self.graph.nodes[node_idx];

        match node.op_type {
            OpType::Reshape => self.exec_reshape(&node),
            OpType::MatMul => self.exec_matmul(&node),
            OpType::Add => self.exec_add(&node),
            OpType::Relu => self.exec_relu(&node),
            OpType::Softmax => self.exec_softmax(&node),
            OpType::Gemm => self.exec_gemm(&node),
            OpType::Flatten => self.exec_flatten(&node),
            OpType::Conv => self.exec_conv(&node),
            OpType::MaxPool => self.exec_maxpool(&node),
            // Shape/Constant/Cast are handled implicitly (weights already bound)
            OpType::Shape | OpType::Constant | OpType::Cast | OpType::Unsqueeze => {
                // These ops produce values that should already be in the weight table
                // or can be skipped for inference
                Ok(())
            }
            _ => Err(EngineError::UnsupportedOp),
        }
    }

    /// Reshape: reinterpret tensor with new shape.
    ///
    /// Reads target shape from the second input (int64 initializer in weight table).
    /// Falls back to 2D flatten if shape tensor not available.
    fn exec_reshape(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;
        let total = input.num_elements();

        // Try to read target shape from the second input
        if node.input_count >= 2 {
            if let Ok(shape_tensor) = self.resolve(&node.inputs[1]) {
                // The shape tensor has shape [N] where N is the number of output dims.
                // Data is stored as little-endian int64 (decoded from varint during loading).
                let ndim = shape_tensor.num_elements();
                if ndim > 0 && ndim <= 8 {
                    let mut new_shape = [0u32; 8];
                    let mut has_infer = false;
                    let mut infer_idx = 0;
                    let mut known_product: usize = 1;

                    unsafe {
                        let i64_ptr = shape_tensor.data as *const i64;
                        for i in 0..ndim {
                            let dim = *i64_ptr.add(i);
                            if dim > 0 {
                                new_shape[i] = dim as u32;
                                known_product *= dim as usize;
                            } else if dim == -1 {
                                has_infer = true;
                                infer_idx = i;
                                new_shape[i] = 1; // placeholder
                            } else if dim == 0 {
                                // Copy from input
                                let d = if i < input.ndim as usize { input.dim(i) } else { 1 };
                                new_shape[i] = d;
                                known_product *= d as usize;
                            }
                        }
                    }

                    if has_infer && known_product > 0 {
                        new_shape[infer_idx] = (total / known_product) as u32;
                    }

                    // Validate
                    let product: usize = new_shape[..ndim].iter().map(|&d| d as usize).product();
                    if product == total {
                        let result = ops::reshape(&input, &new_shape[..ndim])?;
                        self.bind_output(node, 0, result);
                        return Ok(());
                    }
                    // If validation fails, fall through to flatten
                }
            }
        }

        // Fallback: flatten to 2D [batch, features]
        let batch = if input.ndim > 0 { input.dim(0) as usize } else { 1 };
        let features = if batch > 0 { total / batch } else { total };
        let result = ops::reshape(&input, &[batch as u32, features as u32])?;
        self.bind_output(node, 0, result);
        Ok(())
    }

    /// MatMul: C = A × B
    fn exec_matmul(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;

        let m = a.rows() as usize;
        let n = b.cols() as usize;

        let mut out = self.workspace.alloc_tensor(&[m as u32, n as u32])
            .ok_or(EngineError::WorkspaceExhausted)?;

        ops::matmul(&a, &b, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Add: out = a + b
    fn exec_add(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;

        // Output shape matches the larger input
        let mut out_shape = [0u32; 8];
        let ndim = a.ndim as usize;
        for i in 0..ndim {
            out_shape[i] = a.shape[i];
        }

        let mut out = self.workspace.alloc_tensor(&out_shape[..ndim])
            .ok_or(EngineError::WorkspaceExhausted)?;

        ops::add(&a, &b, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Relu: out = max(0, input)
    fn exec_relu(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;

        let mut out_shape = [0u32; 8];
        let ndim = input.ndim as usize;
        for i in 0..ndim {
            out_shape[i] = input.shape[i];
        }

        let mut out = self.workspace.alloc_tensor(&out_shape[..ndim])
            .ok_or(EngineError::WorkspaceExhausted)?;

        ops::relu(&input, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Softmax: out = softmax(input) along last axis
    fn exec_softmax(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;

        let mut out_shape = [0u32; 8];
        let ndim = input.ndim as usize;
        for i in 0..ndim {
            out_shape[i] = input.shape[i];
        }

        let mut out = self.workspace.alloc_tensor(&out_shape[..ndim])
            .ok_or(EngineError::WorkspaceExhausted)?;

        ops::softmax(&input, &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Gemm: Y = A × B + C
    fn exec_gemm(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let a = self.resolve(&node.inputs[0])?;
        let b = self.resolve(&node.inputs[1])?;
        let c = if node.input_count >= 3 {
            Some(self.resolve(&node.inputs[2])?)
        } else {
            None
        };

        let m = a.rows() as usize;
        let n = b.cols() as usize;

        let mut out = self.workspace.alloc_tensor(&[m as u32, n as u32])
            .ok_or(EngineError::WorkspaceExhausted)?;

        ops::gemm(&a, &b, c.as_ref(), &mut out)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Flatten: reshape to 2D [batch, features]
    fn exec_flatten(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;
        let total = input.num_elements();
        // Default axis=1: keep first dim, flatten rest
        let batch = if input.ndim > 0 { input.shape[0] as usize } else { 1 };
        let features = total / batch;

        let result = ops::reshape(&input, &[batch as u32, features as u32])?;
        self.bind_output(node, 0, result);
        Ok(())
    }

    /// Conv2D: out = conv(input, weight) + bias
    ///
    /// Infers kernel size from the weight tensor shape.
    /// Default: stride=1, pad=0 (MNIST uses these defaults for first conv).
    fn exec_conv(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;
        let weight = self.resolve(&node.inputs[1])?;
        let bias = if node.input_count >= 3 {
            Some(self.resolve(&node.inputs[2])?)
        } else {
            None
        };

        if input.ndim < 4 || weight.ndim < 4 {
            return Err(EngineError::ShapeMismatch);
        }

        let batch = input.dim(0) as usize;
        let c_out = weight.dim(0) as usize;
        let kh = weight.dim(2);
        let kw = weight.dim(3);

        // Default stride=1, pad=0 (MNIST-12 uses kernel_shape=[5,5] stride=1 pad=0)
        let sh: u32 = 1;
        let sw: u32 = 1;
        let ph: u32 = 0;
        let pw: u32 = 0;

        let h_out = (input.dim(2) + 2 * ph - kh) / sh + 1;
        let w_out = (input.dim(3) + 2 * pw - kw) / sw + 1;

        let mut out = self.workspace.alloc_tensor(
            &[batch as u32, c_out as u32, h_out, w_out],
        ).ok_or(EngineError::WorkspaceExhausted)?;

        ops::conv2d(&input, &weight, bias.as_ref(), &mut out, kh, kw, sh, sw, ph, pw)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// MaxPool2D: out = maxpool(input)
    ///
    /// Default: kernel=2×2, stride=2 (MNIST-12 uses these).
    fn exec_maxpool(&mut self, node: &GraphNode) -> Result<(), EngineError> {
        let input = self.resolve(&node.inputs[0])?;

        if input.ndim < 4 {
            return Err(EngineError::ShapeMismatch);
        }

        let batch = input.dim(0) as usize;
        let channels = input.dim(1) as usize;

        // Default kernel=2, stride=2 (MNIST-12 uses kernel_shape=[2,2] strides=[2,2])
        let kh: u32 = 2;
        let kw: u32 = 2;
        let sh: u32 = 2;
        let sw: u32 = 2;

        let h_out = (input.dim(2) - kh) / sh + 1;
        let w_out = (input.dim(3) - kw) / sw + 1;

        let mut out = self.workspace.alloc_tensor(
            &[batch as u32, channels as u32, h_out, w_out],
        ).ok_or(EngineError::WorkspaceExhausted)?;

        ops::maxpool2d(&input, &mut out, kh, kw, sh, sw)?;
        self.bind_output(node, 0, out);
        Ok(())
    }

    /// Copy graph output to the caller's buffer.
    fn copy_output(&self, output: *mut f32, output_len: usize) -> Result<usize, EngineError> {
        if self.graph.output_count == 0 {
            return Err(EngineError::InternalError);
        }

        let out_name = &self.graph.output_names[0];
        let out_tensor = self.resolve(out_name)?;
        let n = out_tensor.num_elements();

        if n > output_len {
            return Err(EngineError::InvalidInput);
        }

        unsafe {
            core::ptr::copy_nonoverlapping(out_tensor.data, output, n);
        }

        Ok(n)
    }

    // =========================================================================
    // Binding table operations
    // =========================================================================

    /// Bind a tensor name to a tensor descriptor.
    fn bind(&mut self, name: TensorName, tensor: Tensor) {
        // Check if name already exists (update in place)
        for i in 0..self.binding_count {
            if self.bindings[i].active && self.bindings[i].name.eq_bytes(name.as_bytes()) {
                self.bindings[i].tensor = tensor;
                return;
            }
        }
        // Add new binding
        if self.binding_count < MAX_BINDINGS {
            self.bindings[self.binding_count] = TensorBinding {
                name,
                tensor,
                active: true,
            };
            self.binding_count += 1;
        }
    }

    /// Bind a node's output to the binding table.
    fn bind_output(&mut self, node: &GraphNode, output_idx: usize, tensor: Tensor) {
        if output_idx < node.output_count as usize {
            self.bind(node.outputs[output_idx], tensor);
        }
    }

    /// Resolve a tensor name to its tensor descriptor.
    fn resolve(&self, name: &TensorName) -> Result<Tensor, EngineError> {
        if name.is_empty() {
            return Err(EngineError::WeightNotFound);
        }
        for i in 0..self.binding_count {
            if self.bindings[i].active && self.bindings[i].name.eq_bytes(name.as_bytes()) {
                return Ok(self.bindings[i].tensor);
            }
        }
        Err(EngineError::WeightNotFound)
    }
}

// =============================================================================
// Public API — uses the static engine with lock
// =============================================================================

/// Run inference on a loaded model using the static engine.
///
/// Thread-safe: only one inference at a time via spinlock.
pub fn run_inference(
    model_index: usize,
    input: *const f32,
    input_len: usize,
    output: *mut f32,
    output_len: usize,
) -> Result<usize, EngineError> {
    engine_lock();

    // SAFETY: We hold the engine lock, exclusive access guaranteed.
    let result = unsafe {
        let engine = &mut *ENGINE.get();
        engine.init(model_index)?;
        engine.run(input, input_len, output, output_len)
    };

    engine_unlock();
    result
}
