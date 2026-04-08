# Model Loader API Reference

API reference for the SLM-OS ONNX model loader. The loader parses ONNX protobuf files,
constructs operator graphs, allocates weight and workspace memory from dedicated pools,
and stores loaded models in a fixed-size registry.

**Source files:**
- `runtime/src/loader/registry.rs` -- Model registry
- `runtime/src/loader/graph.rs` -- Operator graph types
- `runtime/src/loader/onnx_parser.rs` -- ONNX protobuf parser
- `kernel/include/slm_ffi.h` -- C FFI declarations

---

## C FFI Functions

These functions are exported by the Rust runtime with `#[no_mangle] pub extern "C"` linkage
and declared in `kernel/include/slm_ffi.h`. The kernel and shell invoke model operations
through this interface.

### rust_model_loader_init

```c
extern int rust_model_loader_init(void);
```

Initialize the model loader registry. Must be called once during boot before any
model operations. Marks all 8 registry slots as empty.

**Returns:** `0` on success.

---

### rust_model_load

```c
extern int rust_model_load(const char *name, const uint8_t *data, size_t data_len);
```

Load an ONNX model from an in-memory buffer. The loader performs these steps:

1. Parse the ONNX protobuf to extract the computation graph and weight tensors
2. Build an `OperatorGraph` from the parsed nodes
3. Allocate a weight memory block from the weight pool and copy initializer data
4. Allocate a workspace memory block (25% of weight size, minimum 64 KB)
5. Store the model in the first available registry slot

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `name` | `const char *` | Null-terminated model name (max 31 characters) |
| `data` | `const uint8_t *` | Pointer to raw ONNX protobuf bytes |
| `data_len` | `size_t` | Length of data buffer in bytes |

**Returns:** Registry index (>= 0) on success, `-1` on error.

**Errors:** Returns `-1` if any pointer is null, `data_len` is zero, the ONNX data is
malformed, memory allocation fails, or no free registry slot exists.

---

### rust_model_unload

```c
extern int rust_model_unload(uint32_t index);
```

Unload a model by its registry index. Frees the associated weight and workspace
memory blocks and marks the slot as inactive.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `index` | `uint32_t` | Registry slot index (from `rust_model_load`) |

**Returns:** `0` on success, `-1` if the index is invalid or the slot is inactive.

---

### rust_model_get_info

```c
extern int rust_model_get_info(uint32_t index, RustModelInfo *info);
```

Retrieve metadata for a loaded model.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `index` | `uint32_t` | Registry slot index |
| `info` | `RustModelInfo *` | Output pointer for model metadata |

**Returns:** `0` on success, `-1` if the index is invalid or the pointer is null.

---

### rust_model_count

```c
extern uint32_t rust_model_count(void);
```

Return the number of currently loaded models (active registry slots).

**Returns:** Count of loaded models (0--8).

---

### rust_model_find

```c
extern int rust_model_find(const char *name);
```

Search the registry for a model by name.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `name` | `const char *` | Null-terminated model name to search for |

**Returns:** Registry index (>= 0) if found, `-1` if not found or name is null.

---

### rust_model_loader_test

```c
extern int rust_model_loader_test(void);
```

Run the model loader test suite. Tests include protobuf varint decoding, length-delimited
field parsing, ONNX model parsing, graph construction, weight table building, and
registry load/unload operations.

**Returns:** Number of test failures (`0` = all passed).

---

## RustModelInfo Struct

Defined in `kernel/include/slm_ffi.h` (C) and `runtime/src/loader/registry.rs` (Rust as `ModelInfoC`).
Layout is `#[repr(C)]` for FFI compatibility.

```c
typedef struct {
    uint8_t  name[32];        /* Null-terminated model name              */
    uint8_t  format;          /* 0=GGUF, 1=ONNX, 2=Raw                  */
    uint8_t  _pad[3];         /* Padding for alignment                   */
    uint64_t param_count;     /* Total parameters (sum of initializers)   */
    uint64_t weight_size;     /* Total weight data in bytes               */
    uint64_t workspace_size;  /* Workspace allocation in bytes            */
    uint32_t node_count;      /* Operator nodes in the graph              */
    uint32_t input_count;     /* Graph-level inputs (excluding weights)   */
    uint32_t output_count;    /* Graph-level outputs                      */
    uint32_t _reserved;       /* Reserved for future use                  */
} RustModelInfo;
```

---

## Rust Internal API

The following functions are internal to the Rust runtime (`runtime/src/loader/registry.rs`).
They are called by the FFI wrappers in `lib.rs` and by the inference engine.

### registry::init

```rust
pub fn init()
```

Initialize the model registry. Sets all 8 slots to empty. Idempotent (subsequent calls are no-ops). Protected by a spinlock.

---

### registry::load_model

```rust
pub fn load_model(name: &[u8], data: &[u8]) -> Result<usize, LoadError>
```

Parse an ONNX model from `data`, allocate weight and workspace memory, and store the model in the registry. Returns the slot index on success.

**Steps performed:**

1. `onnx_parser::parse_onnx(data)` -- parse protobuf into `ParsedOnnx`
2. `onnx_parser::build_graph(&parsed)` -- convert to owned `OperatorGraph`
3. `mm::alloc_weights(total_weight_size)` -- allocate from weight pool
4. Copy each initializer's raw data into the weight block
5. Build a `WeightTable` mapping initializer names to offsets
6. `mm::alloc_workspace(workspace_size)` -- allocate from workspace pool
7. Store the entry in a free registry slot

---

### registry::unload_model

```rust
pub fn unload_model(index: usize) -> Result<(), LoadError>
```

Remove a model from the registry by slot index. Frees the weight and workspace
memory handles.

---

### registry::get_info

```rust
pub fn get_info(index: usize) -> Option<ModelInfoC>
```

Return a copy of the `ModelInfoC` metadata for the model at `index`, or `None` if the
slot is inactive.

---

### registry::get_graph

```rust
pub fn get_graph(index: usize) -> Option<OperatorGraph>
```

Return a clone of the `OperatorGraph` for the model at `index`. The graph is ~6 KB
and is returned by value. For stack-constrained contexts, prefer `copy_graph_into`.

---

### registry::copy_graph_into

```rust
pub fn copy_graph_into(index: usize, dest: &mut OperatorGraph) -> bool
```

Copy the operator graph directly into a caller-provided buffer, avoiding a 6 KB
stack-allocated intermediate. Used by the inference engine.

---

### registry::get_weight_table

```rust
pub fn get_weight_table(index: usize) -> Option<WeightTable>
```

Return a clone of the `WeightTable` for the model at `index`.

---

### registry::copy_weight_table_into

```rust
pub fn copy_weight_table_into(index: usize, dest: &mut WeightTable) -> bool
```

Copy the weight table directly into a caller-provided buffer.

---

### registry::get_weights

```rust
pub fn get_weights(index: usize) -> Option<ModelHandle>
```

Return the `ModelHandle` for the weight memory block of the model at `index`.

---

### registry::get_workspace

```rust
pub fn get_workspace(index: usize) -> Option<ModelHandle>
```

Return the `ModelHandle` for the workspace memory block of the model at `index`.

---

### registry::find_by_name

```rust
pub fn find_by_name(name: &[u8]) -> Option<usize>
```

Search the registry for a model whose name matches `name`. Trailing null bytes are
trimmed before comparison. Returns the slot index if found.

---

### registry::count

```rust
pub fn count() -> usize
```

Return the number of active (loaded) models in the registry.

---

## Operator Graph Types

Defined in `runtime/src/loader/graph.rs`.

### OpType Enum

Enumerates the supported ONNX operator types. Each variant maps directly to an ONNX
`op_type` string.

```rust
#[repr(u8)]
pub enum OpType {
    MatMul    = 0,
    Add       = 1,
    Relu      = 2,
    Softmax   = 3,
    LayerNorm = 4,
    Reshape   = 5,
    Transpose = 6,
    Gather    = 7,
    Concat    = 8,
    Unsqueeze = 9,
    Gemm      = 10,
    Flatten   = 11,
    Shape     = 12,
    Constant  = 13,
    Cast      = 14,
    Conv      = 15,
    MaxPool   = 16,
    Unknown   = 255,
}
```

All variants except `Unknown` are considered supported for inference
(`OpType::is_supported()` returns `true`).

### OperatorGraph

```rust
pub struct OperatorGraph {
    pub nodes: [GraphNode; 12],     // MAX_GRAPH_NODES
    pub node_count: usize,
    pub input_shapes: [TensorShape; 4],   // MAX_GRAPH_IO
    pub input_names: [TensorName; 4],
    pub input_count: usize,
    pub output_shapes: [TensorShape; 4],
    pub output_names: [TensorName; 4],
    pub output_count: usize,
}
```

Nodes are stored in topological order as guaranteed by the ONNX specification.

### GraphNode

```rust
pub struct GraphNode {
    pub op_type: OpType,
    pub name: TensorName,
    pub inputs: [TensorName; 4],    // MAX_NODE_INPUTS
    pub input_count: u8,
    pub outputs: [TensorName; 2],   // MAX_NODE_OUTPUTS
    pub output_count: u8,
}
```

### WeightTable / WeightEntry

```rust
pub struct WeightTable {
    pub entries: [WeightEntry; 16],   // MAX_WEIGHT_ENTRIES
    pub count: usize,
}

pub struct WeightEntry {
    pub name: TensorName,
    pub offset: u32,    // Byte offset within weight memory block
    pub size: u32,      // Size in bytes
    pub shape: TensorShape,
}
```

The `WeightTable::find(name: &[u8])` method looks up an entry by tensor name.

### ElemType

```rust
#[repr(u8)]
pub enum ElemType {
    Float   = 1,   // 4 bytes
    UInt8   = 2,   // 1 byte
    Int8    = 3,   // 1 byte
    Int32   = 6,   // 4 bytes
    Int64   = 7,   // 8 bytes
    Float16 = 10,  // 2 bytes
    Unknown = 0,
}
```

---

## Constants and Limits

| Constant | Value | Description |
|----------|-------|-------------|
| `MAX_MODELS` | 8 | Maximum simultaneously loaded models |
| `MODEL_NAME_LEN` | 32 | Maximum model name length (including null) |
| `MAX_GRAPH_NODES` | 12 | Maximum operator nodes per graph |
| `MAX_NODE_INPUTS` | 4 | Maximum inputs per graph node |
| `MAX_NODE_OUTPUTS` | 2 | Maximum outputs per graph node |
| `MAX_GRAPH_IO` | 4 | Maximum graph-level inputs/outputs |
| `MAX_WEIGHT_ENTRIES` | 16 | Maximum weight tensors per model |
| `TENSOR_NAME_LEN` | 40 | Maximum tensor name length |
| `MAX_DIMS` | 8 | Maximum tensor dimensions |

---

## Error Codes

FFI functions return integer error codes:

| Value | Meaning |
|-------|---------|
| >= 0 | Success (for `rust_model_load` and `rust_model_find`, the registry index) |
| 0 | Success (for `rust_model_unload`, `rust_model_get_info`, `rust_model_loader_init`) |
| -1 | Error (invalid argument, model not found, allocation failure, or registry full) |

The Rust-internal `LoadError` enum covers:
- `CorruptedData` -- ONNX protobuf parse failure
- `InvalidFormat` -- Missing or empty weight data, or invalid registry index
- `ModelTooLarge` -- No free registry slots
- `AllocFailed` -- Weight or workspace memory allocation failure

---

## Shell Commands

The kernel shell exposes model loader operations through the `model` command:

| Command | Description |
|---------|-------------|
| `model load <path>` | Load an ONNX model from VFS (e.g., `/mnt/files/mnist.onnx`) |
| `model list` | List all loaded models with index, name, format, parameters, and weight size |
| `model info <index>` | Display detailed metadata for a loaded model |
| `model unload <index>` | Unload a model by registry index |

Example session:

```
SLM-OS> model load /mnt/files/mnist.onnx
Model loaded: index=0 name=mnist nodes=12 params=26506 weights=102KB

SLM-OS> model list
Loaded models (1/8):
  [0] mnist  ONNX  26506 params  102KB weights  12 nodes

SLM-OS> model info 0
Model: mnist
  Format:     ONNX
  Parameters: 26506
  Weights:    102 KB
  Workspace:  64 KB
  Nodes:      12
  Inputs:     1
  Outputs:    1

SLM-OS> model unload 0
Model 0 unloaded
```

---

*Last updated: April 2026*
