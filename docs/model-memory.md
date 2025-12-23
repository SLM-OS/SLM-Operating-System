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
│  │  - Default: 16 MB (8 × 2MB blocks)                          │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │  Workspace Pool (Read-Write)                                │    │
│  │  - Activation tensors, KV cache, inference scratch          │    │
│  │  - Per-inference (allocated, used, freed)                   │    │
│  │  - Default: 8 MB (4 × 2MB blocks)                           │    │
│  └─────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Pool Sizing

Default configuration targets QEMU with 128MB RAM:

| Pool | Size | Blocks | Purpose |
|------|------|--------|---------|
| Weight | 16 MB | 8 | Model weights (read-only after load) |
| Workspace | 8 MB | 4 | Inference scratch space |
| **Total** | **24 MB** | **12** | Reserved for model memory |

On Jetson Orin Nano (4-8 GB RAM), these can be scaled significantly larger.

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

A skeleton `ModelLoader` is implemented in `runtime/src/mm/model_loader.rs`:

```rust
/// Supported model formats
pub enum ModelFormat {
    Gguf,       // GGUF format (llama.cpp)
    Onnx,       // ONNX format
    RawTensors, // Raw weight tensors
    Unknown,
}

/// Load a model from memory buffer
let loader = ModelLoader::new();
let model = loader.load_from_buffer(&model_data)?;
```

### ModelLoader API (Skeleton)

| Function | Purpose | Status |
|----------|---------|--------|
| `detect_format(data)` | Detect GGUF/ONNX from magic bytes | Implemented |
| `load_from_buffer(data)` | Parse and load model into memory | Skeleton |
| `estimate_memory(data)` | Estimate weight/workspace requirements | Skeleton |

### LoadedModel

A loaded model holds handles to allocated memory:

```rust
pub struct LoadedModel {
    weights: ModelHandle,    // From weight pool
    workspace: ModelHandle,  // From workspace pool
    metadata: ModelMetadata, // Format, param count, sizes
}
```

Memory is automatically freed when `LoadedModel` is dropped.

---

## Future Extensions

### Demand Paging (Phase 4+)
- Lazy allocation: blocks allocated on first access
- Swap to storage: evict cold model weights
- Prefetch: load next inference batch

### Multi-Model Support
- Model registry: track loaded models by name
- LRU eviction: unload least-recently-used models
- Hot-swap: replace model without stopping inference

---

*Last updated: December 2025*
