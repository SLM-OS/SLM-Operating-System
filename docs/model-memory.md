# Model Memory Management

Design documentation for the SLM-OS model memory allocator (Phase 3, Milestone 1).

**Status:** Complete (December 2025)

---

## Overview

The model memory system provides specialized allocation for AI model weights and inference workspaces. Key characteristics:

- **2MB alignment** — Matches ARM L2 block size for TLB efficiency
- **Two pools** — Separate weight (read-only) and workspace (read-write) regions
- **Pure Rust** — Allocator logic in Rust, calls C PMM only for initial bulk allocation
- **Zero-copy sharing** — Reference counting enables multiple components to share models

---

## Memory Layout

```
┌─────────────────────────────────────────────────────────────────────┐
│  Physical Memory (RAM_BASE + offset)                                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  Weight Pool (Read-Only)                                    │    │
│  │  - Model parameters, embeddings, static data                │    │
│  │  - Long-lived (loaded once, used many times)                │    │
│  │  - Default: 256 MB (128 × 2MB blocks) for 1GB RAM            │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  Workspace Pool (Read-Write)                                │    │
│  │  - Activation tensors, KV cache, inference scratch          │    │
│  │  - Per-inference (allocated, used, freed)                   │    │
│  │  - Default: 128 MB (64 × 2MB blocks) for 1GB RAM             │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Pool Sizing

Default configuration for QEMU with 1GB RAM (scaled for model testing):

| Pool | Size | Blocks | Purpose |
|------|------|--------|---------|
| Weight | 256 MB | 128 | Model weights (read-only after load) |
| Workspace | 128 MB | 64 | Inference scratch space |
| **Total** | **384 MB** | **192** | Reserved for model memory |

On Jetson Orin Nano (8 GB RAM), these can be scaled even larger for production models.

---

## Data Structures

### ModelHandle

Opaque handle returned to callers. Contains:
- Block index within pool
- Pool identifier (weight vs workspace)
- Generation counter (for use-after-free detection)

```rust
#[repr(C)]
pub struct ModelHandle {
    block_index: u16,    // Index within pool (max 65535 blocks)
    pool_id: u8,         // 0 = weight, 1 = workspace
    generation: u8,      // Incremented on free, catches stale handles
    _reserved: u32,      // Future: size class, flags
}
```

### Block Metadata

Per-block tracking:

```rust
struct BlockMeta {
    state: BlockState,      // Free, Allocated, Shared
    refcount: u16,          // Reference count for sharing
    generation: u8,         // Current generation
    size_blocks: u8,        // Contiguous blocks (1 for single 2MB)
    owner_task: u32,        // Task ID that allocated (0 = kernel)
}

enum BlockState {
    Free = 0,
    Allocated = 1,
    Shared = 2,             // refcount > 1
}
```

### Pool Structure

```rust
struct MemoryPool {
    base_addr: *mut u8,     // Start of pool (2MB aligned)
    block_count: usize,     // Number of 2MB blocks
    blocks: [BlockMeta; MAX_BLOCKS],
    free_count: usize,      // Blocks available
    read_only: bool,        // Weight pool = true
}
```

---

## API

### Allocation

```rust
/// Allocate model memory from the weight pool.
/// Returns handle on success, error if pool exhausted.
pub fn alloc_weights(size: usize) -> Result<ModelHandle, AllocError>;

/// Allocate workspace memory for inference.
/// Returns handle on success, error if pool exhausted.
pub fn alloc_workspace(size: usize) -> Result<ModelHandle, AllocError>;

/// Free model memory. No-op if refcount > 1.
pub fn free(handle: ModelHandle) -> Result<(), AllocError>;
```

### Sharing (Zero-Copy)

```rust
/// Share model with another component. Increments refcount.
pub fn share(handle: ModelHandle) -> Result<ModelHandle, AllocError>;

/// Release a shared reference. Frees if refcount reaches 0.
pub fn unshare(handle: ModelHandle) -> Result<(), AllocError>;
```

### Access

```rust
/// Get raw pointer to model memory.
/// Returns None if handle is invalid or freed.
pub fn get_ptr(handle: ModelHandle) -> Option<*mut u8>;

/// Get size of allocation in bytes.
pub fn get_size(handle: ModelHandle) -> Option<usize>;
```

### Statistics

```rust
pub struct PoolStats {
    pub total_blocks: usize,
    pub free_blocks: usize,
    pub allocated_blocks: usize,
    pub shared_blocks: usize,
    pub peak_usage: usize,
}

pub fn weight_pool_stats() -> PoolStats;
pub fn workspace_pool_stats() -> PoolStats;
```

---

## GPU Integration

Model memory can be mapped for GPU access:

```rust
/// Map model memory for GPU DMA access.
/// Flushes CPU caches, returns GPU-visible address.
pub fn gpu_map(handle: ModelHandle) -> Result<u64, GpuError>;

/// Unmap from GPU, invalidate CPU caches.
pub fn gpu_unmap(handle: ModelHandle) -> Result<(), GpuError>;
```

Cache coherency is handled automatically:
1. **Before GPU access:** Flush D-cache for region
2. **After GPU writes:** Invalidate D-cache for region

For Jetson, the GPU and CPU share physical memory, so mapping is conceptually a no-op but cache maintenance is still required.

---

## Initialization

Called during Rust runtime init:

```rust
/// Initialize model memory pools.
/// Requests memory from C PMM, sets up internal structures.
pub fn model_mem_init(weight_mb: usize, workspace_mb: usize) -> Result<(), InitError>;
```

Sequence:
1. Calculate total pages needed: `(weight_mb + workspace_mb) * 512`
2. Call `slm_alloc_pages()` to get contiguous physical memory
3. Initialize weight pool at base
4. Initialize workspace pool after weight pool
5. Mark all blocks as free

---

## Error Handling

```rust
#[derive(Debug, Clone, Copy)]
pub enum AllocError {
    OutOfMemory,        // Pool exhausted
    InvalidHandle,      // Handle doesn't match allocated block
    StaleHandle,        // Generation mismatch (use-after-free)
    AlignmentError,     // Size not 2MB aligned (internal error)
    NotInitialized,     // model_mem_init() not called
}
```

---

## Thread Safety

The allocator uses a spinlock for thread safety:
- Single lock protects both pools
- Lock held only during metadata updates (fast path)
- Actual memory access is lock-free

---

## Model Loader (Phase 5)

**Status:** Implemented (April 2026)

The model loader parses ONNX model files, builds an operator graph, allocates weight and workspace memory from the model memory pools, and stores entries in a fixed-size registry. The implementation is split across four Rust modules under `runtime/src/loader/`:

| Module | File | Purpose |
|--------|------|---------|
| `protobuf` | `runtime/src/loader/protobuf.rs` | Minimal protobuf wire format parser (varint, length-delimited, fixed32/64) |
| `onnx_parser` | `runtime/src/loader/onnx_parser.rs` | ONNX schema interpreter with hardcoded field numbers from `onnx.proto3` |
| `graph` | `runtime/src/loader/graph.rs` | Owned operator graph representation (lifetime-free, stored in registry) |
| `registry` | `runtime/src/loader/registry.rs` | Fixed-size model registry (max 8 models), spinlock-protected |

### Parse Pipeline

```
ONNX bytes --> protobuf parser --> ONNX parser --> ParsedOnnx (borrowed)
                                                        |
                                             build_graph + copy weights
                                                        |
                                             OperatorGraph + ModelHandle (owned)
                                                        |
                                                   registry entry
```

The `ParsedOnnx` struct borrows weight data from the input buffer (zero-copy for `raw_data` fields). The `build_graph` step converts borrowed parse results into an owned `OperatorGraph` and copies weight data into allocated pool memory.

### Model Registry

The registry stores up to 8 simultaneously loaded models. Each entry contains:
- Weight memory handle (from weight pool)
- Workspace memory handle (from workspace pool)
- Operator graph (owned, lifetime-free)
- FFI-safe metadata (`ModelInfoC` struct)

### FFI Functions

| Function | Signature | Purpose |
|----------|-----------|---------|
| `rust_model_loader_init` | `() -> i32` | Initialize the registry |
| `rust_model_load` | `(name, data, len) -> i32` | Parse ONNX, allocate memory, store in registry; returns index or -1 |
| `rust_model_unload` | `(index) -> i32` | Free weight/workspace memory, clear registry slot |
| `rust_model_get_info` | `(index, info*) -> i32` | Fill `RustModelInfo` struct with model metadata |
| `rust_model_count` | `() -> u32` | Number of currently loaded models |
| `rust_model_find` | `(name) -> i32` | Find model by name; returns index or -1 |
| `rust_model_loader_test` | `() -> i32` | Run loader self-tests; returns failure count |

### RustModelInfo Structure

```c
typedef struct {
    uint8_t  name[32];        // Model name (null-terminated)
    uint8_t  format;          // 0=GGUF, 1=ONNX, 2=Raw
    uint8_t  _pad[3];
    uint64_t param_count;     // Total parameters across all tensors
    uint64_t weight_size;     // Weight data size in bytes
    uint64_t workspace_size;  // Workspace allocation in bytes
    uint32_t node_count;      // Operator nodes in graph
    uint32_t input_count;     // Graph-level inputs (excluding initializers)
    uint32_t output_count;    // Graph-level outputs
    uint32_t _reserved;
} RustModelInfo;
```

### Stack Usage

The `ParsedOnnx` struct is ~6KB, designed to fit within the 32KB kernel task stack alongside the call chain. Constants are intentionally small (max 16 nodes, max 16 initializers, 24-char names) to keep stack usage bounded. The stack was increased from 16KB to 32KB in Phase 5 to accommodate ONNX parsing plus operator graph construction in the same call chain.

See `docs/onnx-support.md` for supported operators and ONNX format details.

### Inference Engine

The inference engine (`runtime/src/inference/engine.rs`) executes operator graphs on loaded models using the following memory strategy:

- **Static 10KB workspace buffer** -- The engine uses a fixed-size 10KB buffer for intermediate tensors, avoiding heap allocation entirely. This keeps inference within the kernel task stack budget.
- **Bump allocator with per-call reset** -- Intermediate tensors are allocated sequentially from the workspace buffer during a forward pass. The bump pointer resets to zero at the start of each inference call, so no explicit free operations are needed.
- **WeightTable** -- Maps initializer names to byte offsets within the weight memory block allocated from the weight pool. During inference, operator inputs that correspond to model weights are resolved through this table rather than copied.

---

## Future Extensions

### Demand Paging
- Lazy allocation: blocks allocated on first access
- Swap to storage: evict cold model weights
- Prefetch: load next inference batch

### Multi-Model Enhancements
- LRU eviction: unload least-recently-used models when registry is full
- Hot-swap: replace model without stopping inference
- FP16/INT8 quantized weight support

---

*Last updated: April 2026*
