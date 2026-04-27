//! ONNX model parser.
//!
//! Interprets protobuf wire data according to the ONNX schema (onnx.proto3).
//! Field numbers are hardcoded from the spec.
//!
//! The parser produces a `ParsedOnnx` struct that borrows weight data from
//! the input buffer (zero-copy for raw_data fields). The `build_graph` function
//! converts the borrowed parse result into an owned `OperatorGraph`.
//!
//! # Stack Usage
//!
//! `ParsedOnnx` is carefully sized to fit within a 32KB kernel task stack
//! alongside the call chain. Total struct size is ~6KB with current constants.

use super::protobuf::{self, ProtoIter, ParseError, FieldData};
use super::graph::*;
use crate::mm::model_loader::LoadError;

// =============================================================================
// ONNX protobuf field numbers (from onnx.proto3)
// =============================================================================

// ModelProto
const MODEL_IR_VERSION: u32 = 1;
const MODEL_GRAPH: u32 = 7;

// GraphProto
const GRAPH_NODE: u32 = 1;
const GRAPH_NAME: u32 = 2;
const GRAPH_INITIALIZER: u32 = 5;
const GRAPH_INPUT: u32 = 11;
const GRAPH_OUTPUT: u32 = 12;

// NodeProto
const NODE_INPUT: u32 = 1;
const NODE_OUTPUT: u32 = 2;
const NODE_NAME: u32 = 3;
const NODE_OP_TYPE: u32 = 4;

// TensorProto
const TENSOR_DIMS: u32 = 1;
const TENSOR_DATA_TYPE: u32 = 2;
const TENSOR_FLOAT_DATA: u32 = 4;
const TENSOR_INT64_DATA: u32 = 7;
const TENSOR_NAME: u32 = 8;
const TENSOR_RAW_DATA: u32 = 13;

// ValueInfoProto
const VALUE_INFO_NAME: u32 = 1;
const VALUE_INFO_TYPE: u32 = 2;

// TypeProto
const TYPE_TENSOR_TYPE: u32 = 1;

// TypeProto.Tensor
const TENSOR_TYPE_ELEM_TYPE: u32 = 1;
const TENSOR_TYPE_SHAPE: u32 = 2;

// TensorShapeProto
const SHAPE_DIM: u32 = 1;

// TensorShapeProto.Dimension
const DIM_VALUE: u32 = 1;

// =============================================================================
// Parser constants — kept small to fit in 16KB stack
// =============================================================================

/// Maximum nodes in a parsed graph.
pub const MAX_NODES: usize = 16;

/// Maximum weight tensors (initializers).
pub const MAX_INITIALIZERS: usize = 16;

/// Maximum graph-level inputs/outputs.
pub const MAX_IO: usize = 4;

/// Maximum inputs per node.
pub const MAX_NODE_INPUTS_PARSE: usize = 3;

/// Maximum outputs per node.
pub const MAX_NODE_OUTPUTS_PARSE: usize = 2;

/// Name buffer length — must accommodate ONNX tensor names.
/// MNIST-12 has names up to 38 chars (e.g., "Pooling160_Output_0_reshape0_shape").
pub const NAME_LEN: usize = 40;

// =============================================================================
// Parser types
// =============================================================================

/// Fixed-size name for parser intermediates (24 bytes + length).
#[derive(Clone, Copy)]
pub struct Name {
    pub bytes: [u8; NAME_LEN],
    pub len: u8,
}

impl Name {
    pub const EMPTY: Self = Self {
        bytes: [0; NAME_LEN],
        len: 0,
    };

    pub fn from_bytes(src: &[u8]) -> Self {
        let mut name = Self::EMPTY;
        let copy_len = if src.len() < NAME_LEN { src.len() } else { NAME_LEN - 1 };
        name.bytes[..copy_len].copy_from_slice(&src[..copy_len]);
        // Explicitly NUL-terminate the byte after the copy. Today this
        // is redundant because `EMPTY` zero-fills `bytes`, but if a
        // future change ever shortens `EMPTY` or reuses an existing
        // `Name` instance the trailing byte could carry stale data
        // through C string conversions. NAME_LEN > 0 is statically
        // implied by `bytes: [u8; NAME_LEN]`.
        if copy_len < NAME_LEN {
            name.bytes[copy_len] = 0;
        }
        name.len = copy_len as u8;
        name
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len as usize]
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

impl core::fmt::Debug for Name {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let s = core::str::from_utf8(self.as_bytes()).unwrap_or("<invalid>");
        write!(f, "\"{}\"", s)
    }
}

/// ONNX data type (parsed from TensorProto.data_type).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum OnnxDataType {
    Float = 1,
    UInt8 = 2,
    Int8 = 3,
    Int32 = 6,
    Int64 = 7,
    Float16 = 10,
    Unknown = 0,
}

impl OnnxDataType {
    pub fn from_u32(v: u32) -> Self {
        match v {
            1 => OnnxDataType::Float,
            2 => OnnxDataType::UInt8,
            3 => OnnxDataType::Int8,
            6 => OnnxDataType::Int32,
            7 => OnnxDataType::Int64,
            10 => OnnxDataType::Float16,
            _ => OnnxDataType::Unknown,
        }
    }

    /// Size of one element in bytes.
    pub fn element_size(&self) -> usize {
        match self {
            OnnxDataType::Float | OnnxDataType::Int32 => 4,
            OnnxDataType::Int64 => 8,
            OnnxDataType::UInt8 | OnnxDataType::Int8 => 1,
            OnnxDataType::Float16 => 2,
            OnnxDataType::Unknown => 0,
        }
    }
}

/// Parsed tensor shape.
#[derive(Debug, Clone, Copy)]
pub struct ParsedShape {
    pub dims: [i64; MAX_DIMS],
    pub ndim: u8,
}

impl ParsedShape {
    pub const EMPTY: Self = Self {
        dims: [0; MAX_DIMS],
        ndim: 0,
    };
}

/// A weight tensor (initializer) with zero-copy reference to data.
/// Size: ~50 bytes (Name=25 + Shape=65 + dt=1 + 2×16 ptrs ≈ 123 bytes)
#[derive(Clone, Copy)]
pub struct TensorInfo<'a> {
    pub name: Name,
    pub shape: ParsedShape,
    pub data_type: OnnxDataType,
    /// Raw weight data (field 13: raw_data). Points into the ONNX buffer.
    pub raw_data: Option<&'a [u8]>,
    /// Packed float data (field 4: float_data). Points into the ONNX buffer.
    pub float_data: Option<&'a [u8]>,
    /// Packed int64 data (field 7: int64_data). Points into the ONNX buffer.
    pub int64_data: Option<&'a [u8]>,
}

impl<'a> TensorInfo<'a> {
    pub const EMPTY: Self = Self {
        name: Name::EMPTY,
        shape: ParsedShape::EMPTY,
        data_type: OnnxDataType::Unknown,
        raw_data: None,
        float_data: None,
        int64_data: None,
    };

    /// Total number of elements.
    pub fn num_elements(&self) -> usize {
        if self.shape.ndim == 0 {
            return 0;
        }
        let mut total: usize = 1;
        for i in 0..self.shape.ndim as usize {
            let d = self.shape.dims[i];
            if d <= 0 {
                return 0;
            }
            total = total.saturating_mul(d as usize);
        }
        total
    }

    /// Total weight data size in bytes.
    pub fn data_size(&self) -> usize {
        if let Some(raw) = self.raw_data {
            return raw.len();
        }
        if let Some(floats) = self.float_data {
            return floats.len();
        }
        if let Some(i64s) = self.int64_data {
            return i64s.len();
        }
        // `num_elements` already saturates; pair with saturating_mul so a
        // malformed tensor (huge shape, no data payload) returns
        // `usize::MAX` rather than wrapping.
        self.num_elements().saturating_mul(self.data_type.element_size())
    }
}

/// A parsed node in the ONNX graph.
/// Size: ~195 bytes (Name×5 + 2 counts = 25*5 + 2 ≈ 127)
/// Actually: 25(op) + 25(name) + 25*3(in) + 1(cnt) + 25*2(out) + 1(cnt) = 177 bytes
#[derive(Clone, Copy)]
pub struct ParsedNode {
    pub op_type: Name,
    pub name: Name,
    pub inputs: [Name; MAX_NODE_INPUTS_PARSE],
    pub input_count: u8,
    pub outputs: [Name; MAX_NODE_OUTPUTS_PARSE],
    pub output_count: u8,
}

impl ParsedNode {
    pub const EMPTY: Self = Self {
        op_type: Name::EMPTY,
        name: Name::EMPTY,
        inputs: [Name::EMPTY; MAX_NODE_INPUTS_PARSE],
        input_count: 0,
        outputs: [Name::EMPTY; MAX_NODE_OUTPUTS_PARSE],
        output_count: 0,
    };
}

/// A graph input or output specification.
#[derive(Clone, Copy)]
pub struct ValueInfo {
    pub name: Name,
    pub shape: ParsedShape,
    pub elem_type: OnnxDataType,
}

impl ValueInfo {
    pub const EMPTY: Self = Self {
        name: Name::EMPTY,
        shape: ParsedShape::EMPTY,
        elem_type: OnnxDataType::Unknown,
    };
}

/// Complete parsed ONNX model.
///
/// Lifetime `'a` borrows from the input ONNX data buffer (for weight data).
///
/// Stack size: ~6KB total
///   ParsedNode × 16 = ~2,832
///   TensorInfo × 16 = ~1,968
///   ValueInfo × 4 × 2 = ~768
///   Overhead = ~100
///   Total ≈ 5,668 bytes
pub struct ParsedOnnx<'a> {
    pub ir_version: i64,
    pub graph_name: Name,
    pub nodes: [ParsedNode; MAX_NODES],
    pub node_count: usize,
    pub initializers: [TensorInfo<'a>; MAX_INITIALIZERS],
    pub initializer_count: usize,
    pub inputs: [ValueInfo; MAX_IO],
    pub input_count: usize,
    pub outputs: [ValueInfo; MAX_IO],
    pub output_count: usize,
}

impl<'a> ParsedOnnx<'a> {
    pub fn new() -> Self {
        Self {
            ir_version: 0,
            graph_name: Name::EMPTY,
            nodes: [ParsedNode::EMPTY; MAX_NODES],
            node_count: 0,
            initializers: [TensorInfo::EMPTY; MAX_INITIALIZERS],
            initializer_count: 0,
            inputs: [ValueInfo::EMPTY; MAX_IO],
            input_count: 0,
            outputs: [ValueInfo::EMPTY; MAX_IO],
            output_count: 0,
        }
    }

    /// Total weight data size across all initializers.
    pub fn total_weight_size(&self) -> usize {
        let mut total = 0usize;
        for i in 0..self.initializer_count {
            total = total.saturating_add(self.initializers[i].data_size());
        }
        total
    }

    /// Total weight size after FP16→FP32 expansion.
    ///
    /// FP16 tensors are converted to FP32 at load time, so they need
    /// twice the storage of their on-disk representation.
    pub fn total_weight_size_expanded(&self) -> usize {
        let mut total = 0usize;
        for i in 0..self.initializer_count {
            let t = &self.initializers[i];
            if t.data_type == OnnxDataType::Float16 {
                // FP16 weights will be expanded to FP32 (2 bytes → 4 bytes)
                total = total.saturating_add(t.num_elements() * 4);
            } else {
                total = total.saturating_add(t.data_size());
            }
        }
        total
    }
}

// =============================================================================
// Parsing functions
// =============================================================================

/// Parse an ONNX model from a protobuf-encoded buffer.
///
/// The returned `ParsedOnnx` borrows weight data from `data`.
/// Struct is ~6KB — fits in the 32KB kernel task stack.
pub fn parse_onnx(data: &[u8]) -> Result<ParsedOnnx<'_>, ParseError> {
    let mut model = ParsedOnnx::new();

    // Iterate ModelProto fields
    for field in ProtoIter::new(data) {
        let field = field?;
        match field.field_number {
            MODEL_IR_VERSION => {
                model.ir_version = protobuf::field_as_i64(&field);
            }
            MODEL_GRAPH => {
                if let FieldData::Bytes(graph_data) = field.data {
                    parse_graph(graph_data, &mut model)?;
                }
            }
            _ => {} // Skip opset_import and other fields
        }
    }

    Ok(model)
}

/// Parse a GraphProto message.
fn parse_graph<'a>(data: &'a [u8], model: &mut ParsedOnnx<'a>) -> Result<(), ParseError> {
    for field in ProtoIter::new(data) {
        let field = field?;
        match field.field_number {
            GRAPH_NAME => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    model.graph_name = Name::from_bytes(name);
                }
            }
            GRAPH_NODE => {
                if let FieldData::Bytes(node_data) = field.data {
                    if model.node_count < MAX_NODES {
                        model.nodes[model.node_count] = parse_node(node_data)?;
                        model.node_count += 1;
                    }
                }
            }
            GRAPH_INITIALIZER => {
                if let FieldData::Bytes(tensor_data) = field.data {
                    if model.initializer_count < MAX_INITIALIZERS {
                        model.initializers[model.initializer_count] =
                            parse_tensor_proto(tensor_data)?;
                        model.initializer_count += 1;
                    }
                }
            }
            GRAPH_INPUT => {
                if let FieldData::Bytes(vi_data) = field.data {
                    if model.input_count < MAX_IO {
                        model.inputs[model.input_count] = parse_value_info(vi_data)?;
                        model.input_count += 1;
                    }
                }
            }
            GRAPH_OUTPUT => {
                if let FieldData::Bytes(vi_data) = field.data {
                    if model.output_count < MAX_IO {
                        model.outputs[model.output_count] = parse_value_info(vi_data)?;
                        model.output_count += 1;
                    }
                }
            }
            _ => {}
        }
    }
    Ok(())
}

/// Parse a NodeProto message.
fn parse_node(data: &[u8]) -> Result<ParsedNode, ParseError> {
    let mut node = ParsedNode::EMPTY;

    for field in ProtoIter::new(data) {
        let field = field?;
        match field.field_number {
            NODE_INPUT => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    if (node.input_count as usize) < MAX_NODE_INPUTS_PARSE {
                        node.inputs[node.input_count as usize] = Name::from_bytes(name);
                        node.input_count += 1;
                    }
                }
            }
            NODE_OUTPUT => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    if (node.output_count as usize) < MAX_NODE_OUTPUTS_PARSE {
                        node.outputs[node.output_count as usize] = Name::from_bytes(name);
                        node.output_count += 1;
                    }
                }
            }
            NODE_NAME => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    node.name = Name::from_bytes(name);
                }
            }
            NODE_OP_TYPE => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    node.op_type = Name::from_bytes(name);
                }
            }
            _ => {} // Skip attributes, domain, etc.
        }
    }

    Ok(node)
}

/// Parse a TensorProto message (initializer / weight tensor).
fn parse_tensor_proto<'a>(data: &'a [u8]) -> Result<TensorInfo<'a>, ParseError> {
    let mut tensor = TensorInfo::EMPTY;

    for field in ProtoIter::new(data) {
        let field = field?;
        match field.field_number {
            TENSOR_DIMS => {
                match field.data {
                    FieldData::Bytes(packed) => {
                        for val in protobuf::packed_varint_i64(packed) {
                            let val = val?;
                            if (tensor.shape.ndim as usize) < MAX_DIMS {
                                tensor.shape.dims[tensor.shape.ndim as usize] = val;
                                tensor.shape.ndim += 1;
                            }
                        }
                    }
                    FieldData::Varint(v) => {
                        if (tensor.shape.ndim as usize) < MAX_DIMS {
                            tensor.shape.dims[tensor.shape.ndim as usize] = v as i64;
                            tensor.shape.ndim += 1;
                        }
                    }
                    _ => {}
                }
            }
            TENSOR_DATA_TYPE => {
                let dt = protobuf::field_as_u64(&field) as u32;
                tensor.data_type = OnnxDataType::from_u32(dt);
            }
            TENSOR_FLOAT_DATA => {
                if let FieldData::Bytes(packed) = field.data {
                    tensor.float_data = Some(packed);
                }
            }
            TENSOR_INT64_DATA => {
                if let FieldData::Bytes(packed) = field.data {
                    tensor.int64_data = Some(packed);
                }
            }
            TENSOR_NAME => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    tensor.name = Name::from_bytes(name);
                }
            }
            TENSOR_RAW_DATA => {
                if let FieldData::Bytes(raw) = field.data {
                    tensor.raw_data = Some(raw);
                }
            }
            _ => {}
        }
    }

    Ok(tensor)
}

/// Parse a ValueInfoProto message (graph input/output).
fn parse_value_info(data: &[u8]) -> Result<ValueInfo, ParseError> {
    let mut vi = ValueInfo::EMPTY;

    for field in ProtoIter::new(data) {
        let field = field?;
        match field.field_number {
            VALUE_INFO_NAME => {
                if let Some(name) = protobuf::field_as_str(&field) {
                    vi.name = Name::from_bytes(name);
                }
            }
            VALUE_INFO_TYPE => {
                if let FieldData::Bytes(type_data) = field.data {
                    parse_type_proto(type_data, &mut vi)?;
                }
            }
            _ => {}
        }
    }

    Ok(vi)
}

/// Parse a TypeProto message.
fn parse_type_proto(data: &[u8], vi: &mut ValueInfo) -> Result<(), ParseError> {
    for field in ProtoIter::new(data) {
        let field = field?;
        if field.field_number == TYPE_TENSOR_TYPE {
            if let FieldData::Bytes(tensor_type_data) = field.data {
                parse_tensor_type(tensor_type_data, vi)?;
            }
        }
    }
    Ok(())
}

/// Parse a TypeProto.Tensor message.
fn parse_tensor_type(data: &[u8], vi: &mut ValueInfo) -> Result<(), ParseError> {
    for field in ProtoIter::new(data) {
        let field = field?;
        match field.field_number {
            TENSOR_TYPE_ELEM_TYPE => {
                let et = protobuf::field_as_u64(&field) as u32;
                vi.elem_type = OnnxDataType::from_u32(et);
            }
            TENSOR_TYPE_SHAPE => {
                if let FieldData::Bytes(shape_data) = field.data {
                    parse_tensor_shape(shape_data, &mut vi.shape)?;
                }
            }
            _ => {}
        }
    }
    Ok(())
}

/// Parse a TensorShapeProto message.
fn parse_tensor_shape(data: &[u8], shape: &mut ParsedShape) -> Result<(), ParseError> {
    for field in ProtoIter::new(data) {
        let field = field?;
        if field.field_number == SHAPE_DIM {
            if let FieldData::Bytes(dim_data) = field.data {
                parse_shape_dim(dim_data, shape)?;
            }
        }
    }
    Ok(())
}

/// Parse a TensorShapeProto.Dimension message.
fn parse_shape_dim(data: &[u8], shape: &mut ParsedShape) -> Result<(), ParseError> {
    for field in ProtoIter::new(data) {
        let field = field?;
        if field.field_number == DIM_VALUE {
            if (shape.ndim as usize) < MAX_DIMS {
                shape.dims[shape.ndim as usize] = protobuf::field_as_i64(&field);
                shape.ndim += 1;
            }
        }
    }
    Ok(())
}

// =============================================================================
// Graph building (borrowed parse result -> owned graph)
// =============================================================================

/// Convert a `ParsedOnnx` into an owned `OperatorGraph`.
pub fn build_graph(parsed: &ParsedOnnx) -> Result<OperatorGraph, LoadError> {
    let mut graph = OperatorGraph::EMPTY;

    for i in 0..parsed.node_count {
        if i >= MAX_GRAPH_NODES {
            break;
        }
        let pnode = &parsed.nodes[i];
        let op_type = OpType::from_name(pnode.op_type.as_bytes());

        let mut gnode = GraphNode::EMPTY;
        gnode.op_type = op_type;
        gnode.name = TensorName::from_bytes(pnode.name.as_bytes());

        let in_count = core::cmp::min(
            core::cmp::min(pnode.input_count as usize, MAX_NODE_INPUTS_PARSE),
            MAX_NODE_INPUTS,
        );
        for j in 0..in_count {
            gnode.inputs[j] = TensorName::from_bytes(pnode.inputs[j].as_bytes());
        }
        gnode.input_count = in_count as u8;

        let out_count = core::cmp::min(
            core::cmp::min(pnode.output_count as usize, MAX_NODE_OUTPUTS_PARSE),
            MAX_NODE_OUTPUTS,
        );
        for j in 0..out_count {
            gnode.outputs[j] = TensorName::from_bytes(pnode.outputs[j].as_bytes());
        }
        gnode.output_count = out_count as u8;

        graph.nodes[graph.node_count] = gnode;
        graph.node_count += 1;
    }

    // Convert graph inputs (skip initializers — they appear as inputs too in ONNX)
    let mut real_input_count = 0;
    for i in 0..parsed.input_count {
        let vi = &parsed.inputs[i];
        let is_initializer = (0..parsed.initializer_count)
            .any(|j| parsed.initializers[j].name.as_bytes() == vi.name.as_bytes());
        if is_initializer {
            continue;
        }
        if real_input_count >= MAX_GRAPH_IO {
            break;
        }
        graph.input_names[real_input_count] = TensorName::from_bytes(vi.name.as_bytes());
        graph.input_shapes[real_input_count] = parsed_shape_to_tensor_shape(&vi.shape, vi.elem_type);
        real_input_count += 1;
    }
    graph.input_count = real_input_count;

    for i in 0..parsed.output_count {
        if i >= MAX_GRAPH_IO {
            break;
        }
        let vi = &parsed.outputs[i];
        graph.output_names[i] = TensorName::from_bytes(vi.name.as_bytes());
        graph.output_shapes[i] = parsed_shape_to_tensor_shape(&vi.shape, vi.elem_type);
    }
    graph.output_count = parsed.output_count;

    Ok(graph)
}

/// Convert a ParsedShape + OnnxDataType to a TensorShape.
fn parsed_shape_to_tensor_shape(ps: &ParsedShape, dt: OnnxDataType) -> TensorShape {
    let mut ts = TensorShape::EMPTY;
    ts.elem_type = ElemType::from_onnx(dt as u32);
    let ndim = core::cmp::min(ps.ndim as usize, MAX_DIMS);
    for i in 0..ndim {
        ts.dims[i] = if ps.dims[i] > 0 { ps.dims[i] as u32 } else { 1 };
    }
    ts.ndim = ndim as u8;
    ts
}
