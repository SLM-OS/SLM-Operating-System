# ONNX Model Support

Documentation for ONNX model parsing and loading in SLM-OS (Phase 5, Milestone 1).

**Status:** Implemented (April 2026)

---

## Overview

SLM-OS includes a minimal ONNX model loader that parses ONNX protobuf files, constructs an operator graph, and stores model weights in the model memory pools. The implementation is designed for a `no_std` bare-metal environment with strict stack size constraints (32KB kernel task stack).

The loader does not perform inference -- it prepares models for a future inference engine (Phase 5, Milestone 2).

---

## Supported Operators

The following ONNX operators are recognized during graph construction:

| Operator | OpType Enum | Description |
|----------|-------------|-------------|
| MatMul | `MatMul` | Matrix multiplication |
| Add | `Add` | Element-wise addition |
| Relu | `Relu` | Rectified linear unit activation |
| Softmax | `Softmax` | Softmax normalization |
| LayerNorm | `LayerNorm` | Layer normalization |
| Reshape | `Reshape` | Tensor reshape |
| Transpose | `Transpose` | Tensor transpose |
| Gather | `Gather` | Gather elements along axis |
| Concat | `Concat` | Concatenate tensors |
| Unsqueeze | `Unsqueeze` | Insert dimensions |
| Gemm | `Gemm` | General matrix multiplication |
| Flatten | `Flatten` | Flatten tensor to 2D |
| Shape | `Shape` | Return tensor shape |
| Constant | `Constant` | Constant tensor value |
| Cast | `Cast` | Data type conversion |

Operators not in this set are parsed as `Unknown`. An `Unknown` operator does not prevent loading but would block inference once the execution engine is implemented.

---

## Data Types

| ONNX Type | Enum Value | Element Size | Status |
|-----------|------------|--------------|--------|
| FLOAT (FP32) | 1 | 4 bytes | Primary -- weights parsed and stored |
| INT64 | 7 | 8 bytes | Supported for shape dimensions |
| FLOAT16 (FP16) | 10 | 2 bytes | Defined, not yet used for inference |
| INT8 | 3 | 1 byte | Defined, not yet used for inference |
| UINT8 | 2 | 1 byte | Defined, not yet used for inference |
| INT32 | 6 | 4 bytes | Defined, not yet used for inference |

FP32 is the primary data type. Weight tensors are loaded as raw bytes regardless of declared type.

---

## Parsing Approach

### Protobuf Wire Format Parser

The protobuf parser (`runtime/src/loader/protobuf.rs`) implements only the wire format features needed for ONNX:

- **Varint** (wire type 0) -- for integers, enum values, field tags
- **Fixed32** (wire type 5) -- for float values
- **Fixed64** (wire type 1) -- for double/int64 values
- **Length-delimited** (wire type 2) -- for strings, bytes, nested messages, packed repeated fields

The parser operates on borrowed `&[u8]` slices with zero allocation. A `ProtoIter` iterator yields `ProtoField` values containing zero-copy references into the original buffer.

### ONNX Schema Interpretation

The ONNX parser (`runtime/src/loader/onnx_parser.rs`) uses hardcoded field numbers from the ONNX protobuf schema (`onnx.proto3`). This avoids the need for a protobuf code generator or schema compiler. Key field numbers include:

- `ModelProto.graph` (field 7) -- the computation graph
- `GraphProto.node` (field 1), `initializer` (field 5), `input` (field 11), `output` (field 12)
- `NodeProto.input` (field 1), `output` (field 2), `op_type` (field 4)
- `TensorProto.dims` (field 1), `data_type` (field 2), `raw_data` (field 13)

### Graph Construction

The `build_graph` function converts the borrowed `ParsedOnnx` into an owned `OperatorGraph`:

1. Each `ParsedNode` is mapped to a `GraphNode` with an `OpType` enum
2. Graph inputs are filtered to exclude initializers (ONNX lists initializers as inputs)
3. Tensor names are copied into fixed-size `TensorName` buffers (32 bytes)
4. Nodes are stored in topological order (as guaranteed by the ONNX specification)

---

## Limitations

| Constraint | Value | Notes |
|-----------|-------|-------|
| Maximum graph nodes | 16 (parse), 32 (graph) | Sufficient for small models like MNIST-12 |
| Maximum initializers | 16 | Weight tensors per model |
| Maximum inputs per node | 3 (parse), 4 (graph) | Covers standard ONNX operators |
| Maximum outputs per node | 2 | |
| Maximum graph I/O | 4 | Graph-level inputs and outputs |
| Name length | 24 bytes (parse), 32 bytes (graph) | Truncated if longer |
| Maximum loaded models | 8 | Simultaneous models in registry |
| Shape support | Static only | Dynamic dimensions not supported |
| Tensor dimensions | 8 max | Per-tensor dimension limit |

### Design Rationale

These limits are intentionally small to ensure the `ParsedOnnx` struct (~6KB) fits within the 16KB kernel task stack alongside the full call chain. In a bare-metal environment without heap-allocated containers, fixed-size arrays are the only option.

---

## Test Model

**MNIST-12** from the [ONNX Model Zoo](https://github.com/onnx/models) is used as the primary test model:

- **Size:** ~26KB ONNX protobuf
- **Task:** Handwritten digit classification (28x28 grayscale -> 10 classes)
- **Graph:** 12 operator nodes (Reshape, MatMul, Add, Relu, Softmax)
- **Weights:** 8 initializer tensors, ~26KB total weight data
- **Parameters:** ~26,000

The model file is stored in VFS at `/mnt/files/mnist.onnx` and can be loaded via the shell:

```
SLM-OS> model load /mnt/files/mnist.onnx
```

---

## Source Files

| File | Purpose |
|------|---------|
| `runtime/src/loader/mod.rs` | Module root, re-exports |
| `runtime/src/loader/protobuf.rs` | Protobuf wire format parser |
| `runtime/src/loader/onnx_parser.rs` | ONNX schema parser and graph builder |
| `runtime/src/loader/graph.rs` | Operator graph types (`OpType`, `OperatorGraph`, `GraphNode`) |
| `runtime/src/loader/registry.rs` | Model registry (max 8 models, spinlock-protected) |
| `kernel/include/slm_ffi.h` | C declarations for model loader FFI |

---

## Future Work

- **FP16/INT8 quantization** -- Load and store quantized weights for reduced memory usage
- **Larger models** -- Increase node/initializer limits for production models (requires heap allocation or larger stack)
- **Dynamic shapes** -- Support batch dimension and variable-length sequences
- **GGUF format** -- Parse llama.cpp GGUF models (format detection is implemented but loading is not)
- **Inference engine** -- Execute the operator graph on loaded weights (Phase 5, Milestone 2)

---

*Last updated: April 2026*
