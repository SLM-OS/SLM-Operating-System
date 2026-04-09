//! Operator graph representation for loaded models.
//!
//! These types are the owned, lifetime-free representation of a parsed model's
//! computation graph. They persist in the model registry after the ONNX parse
//! buffer has been freed.

/// Maximum nodes in the operator graph.
pub const MAX_GRAPH_NODES: usize = 12;

/// Maximum inputs per graph node.
pub const MAX_NODE_INPUTS: usize = 4;

/// Maximum outputs per graph node.
pub const MAX_NODE_OUTPUTS: usize = 2;

/// Maximum graph-level inputs/outputs.
pub const MAX_GRAPH_IO: usize = 4;

/// Maximum tensor name length — must accommodate ONNX tensor names.
pub const TENSOR_NAME_LEN: usize = 40;

/// Maximum tensor dimensions.
pub const MAX_DIMS: usize = 8;

/// Supported ONNX operators.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OpType {
    MatMul = 0,
    Add = 1,
    Relu = 2,
    Softmax = 3,
    LayerNorm = 4,
    Reshape = 5,
    Transpose = 6,
    Gather = 7,
    Concat = 8,
    Unsqueeze = 9,
    Gemm = 10,
    Flatten = 11,
    Shape = 12,
    Constant = 13,
    Cast = 14,
    Conv = 15,
    MaxPool = 16,
    /// Operator not in the supported set.
    Unknown = 255,
}

impl OpType {
    /// Convert an ONNX op_type name to our enum.
    pub fn from_name(name: &[u8]) -> Self {
        match name {
            b"MatMul" => OpType::MatMul,
            b"Add" => OpType::Add,
            b"Relu" => OpType::Relu,
            b"Softmax" => OpType::Softmax,
            b"LayerNorm" => OpType::LayerNorm,
            b"Reshape" => OpType::Reshape,
            b"Transpose" => OpType::Transpose,
            b"Gather" => OpType::Gather,
            b"Concat" => OpType::Concat,
            b"Unsqueeze" => OpType::Unsqueeze,
            b"Gemm" => OpType::Gemm,
            b"Flatten" => OpType::Flatten,
            b"Shape" => OpType::Shape,
            b"Constant" => OpType::Constant,
            b"Cast" => OpType::Cast,
            b"Conv" => OpType::Conv,
            b"MaxPool" => OpType::MaxPool,
            _ => OpType::Unknown,
        }
    }

    /// Get the display name of this operator.
    pub fn as_str(&self) -> &'static str {
        match self {
            OpType::MatMul => "MatMul",
            OpType::Add => "Add",
            OpType::Relu => "Relu",
            OpType::Softmax => "Softmax",
            OpType::LayerNorm => "LayerNorm",
            OpType::Reshape => "Reshape",
            OpType::Transpose => "Transpose",
            OpType::Gather => "Gather",
            OpType::Concat => "Concat",
            OpType::Unsqueeze => "Unsqueeze",
            OpType::Gemm => "Gemm",
            OpType::Flatten => "Flatten",
            OpType::Shape => "Shape",
            OpType::Constant => "Constant",
            OpType::Cast => "Cast",
            OpType::Conv => "Conv",
            OpType::MaxPool => "MaxPool",
            OpType::Unknown => "Unknown",
        }
    }

    /// Whether this operator is supported for inference (M2).
    pub fn is_supported(&self) -> bool {
        !matches!(self, OpType::Unknown)
    }
}

/// A tensor name stored as a fixed-size byte array.
#[derive(Clone, Copy)]
pub struct TensorName {
    pub bytes: [u8; TENSOR_NAME_LEN],
    pub len: u8,
}

impl TensorName {
    pub const EMPTY: Self = Self {
        bytes: [0; TENSOR_NAME_LEN],
        len: 0,
    };

    /// Create a TensorName from a byte slice (truncates if too long).
    pub fn from_bytes(src: &[u8]) -> Self {
        let mut name = Self::EMPTY;
        let copy_len = if src.len() < TENSOR_NAME_LEN {
            src.len()
        } else {
            TENSOR_NAME_LEN - 1
        };
        name.bytes[..copy_len].copy_from_slice(&src[..copy_len]);
        name.len = copy_len as u8;
        name
    }

    /// Get the name as a byte slice.
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len as usize]
    }

    /// Check if this name matches a byte slice.
    pub fn eq_bytes(&self, other: &[u8]) -> bool {
        self.as_bytes() == other
    }

    /// Check if the name is empty.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl core::fmt::Debug for TensorName {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let s = core::str::from_utf8(self.as_bytes()).unwrap_or("<invalid utf8>");
        write!(f, "TensorName(\"{}\")", s)
    }
}

/// ONNX element data types.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum ElemType {
    Float = 1,
    UInt8 = 2,
    Int8 = 3,
    Int32 = 6,
    Int64 = 7,
    Float16 = 10,
    Unknown = 0,
}

impl ElemType {
    pub fn from_onnx(v: u32) -> Self {
        match v {
            1 => ElemType::Float,
            2 => ElemType::UInt8,
            3 => ElemType::Int8,
            6 => ElemType::Int32,
            7 => ElemType::Int64,
            10 => ElemType::Float16,
            _ => ElemType::Unknown,
        }
    }

    /// Size in bytes of one element.
    pub fn size(&self) -> usize {
        match self {
            ElemType::Float => 4,
            ElemType::UInt8 | ElemType::Int8 => 1,
            ElemType::Int32 => 4,
            ElemType::Int64 => 8,
            ElemType::Float16 => 2,
            ElemType::Unknown => 0,
        }
    }
}

/// Tensor shape descriptor.
#[derive(Debug, Clone, Copy)]
pub struct TensorShape {
    pub dims: [u32; MAX_DIMS],
    pub ndim: u8,
    pub elem_type: ElemType,
}

impl TensorShape {
    pub const EMPTY: Self = Self {
        dims: [0; MAX_DIMS],
        ndim: 0,
        elem_type: ElemType::Unknown,
    };

    /// Total number of elements in this tensor.
    pub fn num_elements(&self) -> usize {
        if self.ndim == 0 {
            return 0;
        }
        let mut total: usize = 1;
        for i in 0..self.ndim as usize {
            total = total.saturating_mul(self.dims[i] as usize);
        }
        total
    }

    /// Total size in bytes.
    pub fn size_bytes(&self) -> usize {
        self.num_elements() * self.elem_type.size()
    }
}

/// A node in the runtime operator graph.
#[derive(Clone, Copy)]
pub struct GraphNode {
    pub op_type: OpType,
    pub name: TensorName,
    pub inputs: [TensorName; MAX_NODE_INPUTS],
    pub input_count: u8,
    pub outputs: [TensorName; MAX_NODE_OUTPUTS],
    pub output_count: u8,
}

impl GraphNode {
    pub const EMPTY: Self = Self {
        op_type: OpType::Unknown,
        name: TensorName::EMPTY,
        inputs: [TensorName::EMPTY; MAX_NODE_INPUTS],
        input_count: 0,
        outputs: [TensorName::EMPTY; MAX_NODE_OUTPUTS],
        output_count: 0,
    };
}

impl core::fmt::Debug for GraphNode {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "GraphNode({} inputs={} outputs={})",
            self.op_type.as_str(), self.input_count, self.output_count)
    }
}

/// The complete runtime operator graph.
///
/// Nodes are stored in topological order (ONNX spec guarantees this).
#[derive(Clone)]
pub struct OperatorGraph {
    pub nodes: [GraphNode; MAX_GRAPH_NODES],
    pub node_count: usize,
    pub input_shapes: [TensorShape; MAX_GRAPH_IO],
    pub input_names: [TensorName; MAX_GRAPH_IO],
    pub input_count: usize,
    pub output_shapes: [TensorShape; MAX_GRAPH_IO],
    pub output_names: [TensorName; MAX_GRAPH_IO],
    pub output_count: usize,
}

impl OperatorGraph {
    pub const EMPTY: Self = Self {
        nodes: [GraphNode::EMPTY; MAX_GRAPH_NODES],
        node_count: 0,
        input_shapes: [TensorShape::EMPTY; MAX_GRAPH_IO],
        input_names: [TensorName::EMPTY; MAX_GRAPH_IO],
        input_count: 0,
        output_shapes: [TensorShape::EMPTY; MAX_GRAPH_IO],
        output_names: [TensorName::EMPTY; MAX_GRAPH_IO],
        output_count: 0,
    };
}

impl core::fmt::Debug for OperatorGraph {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "OperatorGraph(nodes={} inputs={} outputs={})",
            self.node_count, self.input_count, self.output_count)
    }
}

// =============================================================================
// Weight table (maps initializer names to offsets in weight memory block)
// =============================================================================

/// Maximum weight entries (matches parser MAX_INITIALIZERS).
pub const MAX_WEIGHT_ENTRIES: usize = 16;

/// An entry mapping an initializer name to its offset within the weight block.
#[derive(Clone, Copy)]
pub struct WeightEntry {
    pub name: TensorName,
    pub offset: u32,
    pub size: u32,
    pub shape: TensorShape,
}

impl WeightEntry {
    pub const EMPTY: Self = Self {
        name: TensorName::EMPTY,
        offset: 0,
        size: 0,
        shape: TensorShape::EMPTY,
    };
}

/// Table of weight name → offset mappings for a loaded model.
#[derive(Clone)]
pub struct WeightTable {
    pub entries: [WeightEntry; MAX_WEIGHT_ENTRIES],
    pub count: usize,
}

impl WeightTable {
    pub const EMPTY: Self = Self {
        entries: [WeightEntry::EMPTY; MAX_WEIGHT_ENTRIES],
        count: 0,
    };

    /// Find a weight entry by name.
    pub fn find(&self, name: &[u8]) -> Option<&WeightEntry> {
        for i in 0..self.count {
            if self.entries[i].name.eq_bytes(name) {
                return Some(&self.entries[i]);
            }
        }
        None
    }
}
