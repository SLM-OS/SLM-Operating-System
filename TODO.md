# Phase 5: SLM Integration

This document tracks Phase 5 implementation of SLM-OS.

**Status:** In Progress (M1, M2, M3, M4 complete)

**Summary:** Phase 5 brings together all prior work to deliver actual SLM inference capabilities. This includes the ONNX model loader, inference runtime (CPU-based initially, with GPU acceleration path), and example SLM components demonstrating the full AI-first OS vision.

**Goals:**
- ONNX model loader (complete the skeleton from Phase 3)
- Inference runtime with CPU backend
- GPU compute integration (building on Phase 4/4X GPU driver work)
- Component isolation (user/kernel separation)
- 2-3 example SLM components
- End-to-end inference pipeline demo

**Prerequisites (Completed in Earlier Phases):**
- ✅ Model memory allocator (Phase 3) — weight pool, workspace pool, zero-copy sharing
- ✅ Component system (Phase 4) — loader, lifecycle, hot-swap, message routing
- ✅ GPU memory integration (Phase 4) — cache coherency, `nvidia_alloc`/`nvidia_free`
- ✅ GSP firmware documentation (Phase 4X) — patterns for GPU compute
- ✅ AI scheduler (Phase AI-Sched, parallel) — pluggable scheduler with ML inference

**Relationship to Other Phases:**
- **Builds on:** Phase 3 (model memory), Phase 4 (component system, GPU memory), Phase 4X (GPU learnings)
- **Parallel with:** Phase AI-Sched (AI scheduler can use same inference engine patterns)
- **Followed by:** Phase 6 (Demo & Polish)

**Repository:** `CS-496-Capstone-SLM-Operating-System`

---

## Icon Key

| Icon | Meaning |
|------|---------|
| ☐ | Not started |
| ✅ | Complete |
| ⏸️ | Deferred to later phase |
| 🔗 | Has dependency on another milestone |

---

## Milestone 1: ONNX Model Loader

**Depends on:** Phase 3 model memory allocator

### ONNX Format Research
- ✅ Study ONNX file format (protobuf-based)
- ✅ Identify subset needed for SLM inference:
  - ✅ Graph structure (nodes, inputs, outputs)
  - ✅ Operator types (MatMul, Add, Relu, Softmax, LayerNorm, etc.)
  - ✅ Weight tensors (initializers)
  - ✅ Data types (FP32, FP16, INT8)
- ☐ Document supported operators in `docs/onnx-support.md`
- ✅ Evaluate existing minimal ONNX parsers — implemented custom minimal parser

### Protobuf Parser (Minimal)
- ✅ Implement minimal protobuf wire format parser in Rust
  - ✅ Varint decoding
  - ✅ Length-delimited fields
  - ✅ Nested message parsing
- ✅ Create `runtime/src/loader/protobuf.rs`

### Model Loader Implementation
- ✅ Implement model loading pipeline in `runtime/src/loader/registry.rs`:
  - ✅ Parse ONNX protobuf
  - ✅ Extract graph structure
  - ✅ Allocate weight memory from weight pool
  - ✅ Copy weights to allocated memory
  - ✅ Build operator execution graph
- ✅ Implement `OperatorGraph`, `GraphNode`, `TensorShape` in `runtime/src/loader/graph.rs`
- ✅ Model registry with load/unload/find/list (up to 8 models)
- ✅ Handle model metadata (name, format, param count, weight size, node count)

### Weight Management
- ✅ Integrate with Phase 3 model memory allocator
- ✅ Use `alloc_weights()` for read-only weight storage
- ☐ Implement weight sharing (same model loaded once, used by multiple components)
- ✅ Track weight references for safe unloading

### Shell Commands
- ✅ `model load <path>` — load ONNX model from VFS
- ✅ `model list` — list loaded models with memory usage
- ✅ `model info <name>` — show model details (format, params, nodes, I/O)
- ✅ `model unload <name>` — unload model, free memory
- ✅ `model pools` — show pool statistics (backward-compatible)

### Testing
- ✅ Unit tests for protobuf parser (varint, field iteration)
- ✅ Load MNIST ONNX model (26KB, real-world model from ONNX Model Zoo)
- ✅ Verify graph structure (nodes, initializers, inputs, outputs)
- ✅ Test model load/unload lifecycle via registry
- ✅ Test find_by_name, count operations
- ✅ All 23 model loader tests pass, 518 total tests pass with 0 failures

---

## Milestone 2: CPU Inference Engine

**Depends on:** M1 (Model Loader)

### Tensor Operations (CPU)
- ✅ Create `runtime/src/inference/` directory and `engine.rs`
- ✅ Implement core operators in Rust:
  - ✅ `MatMul` — matrix multiplication (M×K × K×N → M×N)
  - ✅ `Add` — element-wise addition with broadcasting
  - ✅ `Relu` — element-wise max(0, x)
  - ✅ `Softmax` — exp normalization along axis
  - ✅ `Reshape` — tensor reshape
  - ✅ `Conv2D` — 2D convolution (NCHW layout)
  - ✅ `MaxPool2D` — 2D max pooling
  - ✅ `Gemm` — general matrix multiplication
  - ✅ `Flatten` — flatten tensor to 2D
- ✅ Implement operator dispatch table

### SIMD Optimization
- ☐ Use NEON intrinsics for ARM64:
  ```rust
  #[cfg(target_arch = "aarch64")]
  use core::arch::aarch64::*;
  ```
- ☐ Use SSE/AVX intrinsics for x86-64:
  ```rust
  #[cfg(target_arch = "x86_64")]
  use core::arch::x86_64::*;
  ```
- ☐ Implement SIMD-optimized `matmul_f32()`:
  - ☐ 4-wide accumulation (NEON: `vfmaq_f32`, SSE: `_mm_fmadd_ps`)
  - ☐ Cache-friendly tiling (64×64 or 128×128 blocks)
  - ☐ Prefetching for large matrices
- ☐ Benchmark: target < 1ms for 256×256 × 256×256 MatMul

### Inference Runtime
- ✅ Create `runtime/src/inference/engine.rs`
- ✅ Implement `InferenceEngine` with workspace management
- ✅ Implement operator scheduling:
  - ✅ Topological sort of operator graph
  - ✅ Execute operators in order
  - ✅ Manage intermediate tensor memory in workspace pool
- ☐ Handle dynamic shapes (batch size, sequence length)

### Workspace Management
- ✅ Integrate with Phase 3 workspace pool
- ✅ Allocate workspace on engine creation
- ✅ Reuse workspace across inference calls
- ✅ Free workspace on engine destruction

### Shell Commands
- ✅ `model infer <name|idx>` — run inference with zero input, print output probabilities and predicted class
- ☐ `bench infer <model> [iterations]` — benchmark inference latency

### Testing
- ✅ Unit tests for each operator
- ✅ Integration test: full forward pass on MNIST-12 model
- ☐ Accuracy test: compare outputs to PyTorch reference
- ☐ Benchmark: measure latency for various model sizes

---

## Milestone 3: GPU Compute Integration

**Depends on:** M2 (CPU Inference), Phase 4/4X GPU driver

**Note:** Full GPU compute requires GSP firmware loading (documented in Phase 4X). This milestone implements what's possible without GSP, and documents the GSP path for future work.

### GPU Memory Integration
- ✅ GPU memory integration with model memory (cache coherency via gpu_map_weights/gpu_unmap_weights)
- ✅ Implement cache coherency for CPU-written weights:
  - ✅ `cache_clean_range()` before GPU read (via `slm_gpu_sync_for_device`)
  - ✅ `cache_invalidate_range()` after GPU write (via `slm_gpu_sync_for_cpu`)

### GPU Operator Framework
- ✅ GPU operator framework (`runtime/src/inference/gpu.rs` with GpuCapabilities, select_backend, Backend enum)
- ✅ GPU capability detection (via slm_gpu_available/slm_gpu_get_info FFI)
- ✅ Fall back to CPU for unsupported operators
- ✅ Hybrid CPU/GPU execution operator placement decisions (select_backend heuristic: large MatMul/Gemm >4096 → GPU, element-wise → CPU)

### GSP Firmware Path (Future)
- ⏸️ Load GSP firmware from filesystem
- ⏸️ Initialize GSP RISC-V core
- ⏸️ Implement GSP mailbox communication
- ⏸️ Submit compute commands via GSP
- ✅ Document GSP integration plan in `docs/gpu-compute.md`

### Testing
- ☐ Test GPU memory allocation on Jetson hardware
- ☐ Benchmark CPU vs GPU performance

---

## Milestone 4: Component Isolation

**Depends on:** Phase 4 component system

**Note:** This implements user/kernel separation (EL0/EL1 on ARM64, Ring 3/Ring 0 on x86-64) to provide real isolation between components. See `docs/component-isolation.md` for design details.

### Privilege Separation Design
- ✅ Document isolation architecture in `docs/component-isolation.md`
- ✅ Define syscall interface (numbers and calling convention)
- ☐ Design capability-based access control

### ARM64 User Mode (EL0)
- ☐ Implement EL1 → EL0 transition for component execution — blocked: VMM_FLAG_USER investigation
- ☐ Configure TTBR0_EL1 for per-component page tables
- ✅ Implement syscall entry via `SVC` instruction
- ✅ Handle `SVC` exception and dispatch to kernel
- ✅ Implement syscall return via `ERET`

### x86-64 User Mode (Ring 3)
- ☐ Implement Ring 0 → Ring 3 transition via `IRET`
- ☐ Configure per-component page tables in CR3
- ☐ Implement syscall entry via `SYSCALL` instruction
- ☐ Set up STAR, LSTAR, SFMASK MSRs for syscall handling
- ☐ Implement syscall return via `SYSRET`

### Syscall Interface
- ✅ Define syscall numbers and calling convention
- ✅ Core syscalls (via `user_syscall.h`):
  - ✅ `sys_exit(code)` — terminate component
  - ✅ `sys_yield()` — yield CPU
  - ✅ `sys_sleep(ms)` — sleep for duration
  - ✅ `sys_send(topic, msg, len)` — send message
  - ✅ `sys_recv(topic, buf, len, timeout)` — receive message
  - ✅ `sys_model_infer(handle, input, output)` — run inference
  - ✅ `sys_log(str, len)` — log to kernel UART
- ✅ Validate user pointers before kernel access

### Per-Component Address Spaces
- ☐ Create separate page tables per component
- ☐ Map component code/data into user space
- ☐ Map shared kernel services (syscall entry)
- ☐ Map shared model weights (read-only)
- ☐ Prevent access to other components' memory

### Resource Limits
- ☐ Implement memory limit per component
- ☐ Track memory usage in component struct
- ☐ Fail allocation if limit exceeded
- ☐ Implement CPU time accounting
- ⏸️ CPU time limit enforcement — requires preemption improvements

### Fault Isolation
- ✅ Component crash doesn't crash kernel (`handle_user_fault` terminates component)
- ✅ Catch page faults, illegal instructions in component (`el0_sync_handler`)
- ✅ Terminate faulting component cleanly
- ☐ Notify parent/supervisor of component failure
- ☐ Automatic component restart (optional, configurable)

### Testing
- ☐ Test EL0/Ring3 transition and return
- ☐ Test all syscalls from user mode
- ☐ Test component crash handling
- ☐ Test memory isolation between components
- ☐ Stress test with many components

---

## Milestone 5: Example SLM Components

**Depends on:** M1 (Model Loader), M2 (Inference Engine), M4 (Isolation)

### Component Development Framework
- ☐ Create component template in `components/template/`
- ☐ Define component manifest schema (expand from Phase 4):
  ```yaml
  name: example-component
  version: 1.0.0
  type: slm-component
  
  models:
    - name: my-model
      path: /models/my-model.onnx
      preload: true
  
  resources:
    memory_mb: 64
    workspace_mb: 16
  
  interfaces:
    subscribes:
      - topic: /input/data
    publishes:
      - topic: /output/result
  ```
- ☐ Document component API in `docs/component-development.md`

### Anomaly Detector Component
- ☐ Create `components/anomaly-detector/`
- ☐ Implement simple anomaly detection model:
  - ☐ Input: sensor readings (float array)
  - ☐ Output: anomaly score + classification
  - ☐ Model: Small MLP or autoencoder (< 1MB)
- ☐ Subscribe to `/sensors/data` topic
- ☐ Publish to `/alerts/anomaly` topic
- ☐ Implement threshold-based alerting

### Text Classifier Component
- ☐ Create `components/text-classifier/`
- ☐ Implement simple text classification:
  - ☐ Input: text string (tokenized)
  - ☐ Output: category + confidence
  - ☐ Model: Small transformer or LSTM (< 10MB)
- ☐ Subscribe to `/input/text` topic
- ☐ Publish to `/output/classification` topic

### Sensor Monitor Component (No Model)
- ☐ Create `components/sensor-monitor/`
- ☐ Implement rule-based monitoring (no ML):
  - ☐ Input: sensor readings
  - ☐ Output: threshold alerts
- ☐ Demonstrates component system without model loading
- ☐ Useful as baseline for comparison

### Component Integration Testing
- ☐ Load all three components simultaneously
- ☐ Verify message routing between components
- ☐ Test hot-swap of anomaly detector
- ☐ Measure end-to-end latency (input → inference → output)
- ☐ Stress test with high message rates

### Demo Script
- ☐ Create `scripts/demo.lua` for scripted demo
- ☐ Load components, inject test data, show results
- ☐ Document demo in `docs/demo.md`

---

## Milestone 6: Inference Pipeline Integration

**Depends on:** M1-M5

### End-to-End Pipeline
- ☐ Define inference request/response protocol
- ☐ Implement pipeline stages:
  1. ☐ Component receives input message
  2. ☐ Preprocess input (tokenization, normalization)
  3. ☐ Load/reuse model
  4. ☐ Run inference
  5. ☐ Postprocess output
  6. ☐ Send response message
- ☐ Measure and log each stage's latency

### Batching Support
- ☐ Implement request batching in inference engine:
  - ☐ Collect requests up to max batch size or timeout
  - ☐ Execute batched inference
  - ☐ Distribute results to requesters
- ☐ Configure batching per model (batch size, timeout)
- ☐ Measure throughput improvement vs single requests

### Model Caching
- ☐ Implement model cache with LRU eviction
- ☐ Configure cache size limit
- ☐ Track model usage statistics
- ☐ Preload frequently used models

### Deadline-Aware Inference
- ☐ Integrate with deadline-aware scheduler:
  - ☐ Set task deadline based on inference SLA
  - ☐ Prioritize urgent inference requests
  - ☐ Log deadline misses
- ☐ If AI scheduler available (Phase AI-Sched), use ML-based scheduling

### Performance Monitoring
- ☐ Track inference latency (p50, p95, p99)
- ☐ Track throughput (inferences/second)
- ☐ Track memory usage (weights, workspace)
- ☐ Expose metrics via shell command: `infer stats`
- ☐ Optional: Expose metrics via network API

### Testing
- ☐ End-to-end latency test with real model
- ☐ Throughput test under load
- ☐ Deadline miss rate test
- ☐ Memory pressure test (load many models)

---

## Milestone 7: Documentation & Polish

### API Documentation
- ☐ Document ONNX loader API (`docs/api/model-loader.md`)
- ☐ Document inference engine API (`docs/api/inference.md`)
- ☐ Document component syscall API (`docs/api/syscalls.md`)
- ☐ Generate rustdoc for runtime crate

### User Guides
- ☐ Component development tutorial (`docs/tutorials/component.md`)
- ☐ Model preparation guide (`docs/tutorials/models.md`)
- ☐ Performance tuning guide (`docs/tutorials/performance.md`)
- ☐ Troubleshooting guide updates

### Architecture Documentation
- ☐ Update architecture doc with Phase 5 additions
- ☐ Document inference pipeline architecture
- ☐ Document isolation architecture
- ☐ Add sequence diagrams for key flows

### Performance Documentation
- ☐ Benchmark results for all target platforms
- ☐ Comparison with baseline (Linux + Python)
- ☐ Memory usage analysis
- ☐ Power consumption analysis (if measurable)

---

## Phase 5 Completion Checklist

### Deliverables
- ☐ ONNX model loader working (at least subset of operators)
- ☐ CPU inference engine running real models
- ☐ GPU memory integration working (compute if GSP available)
- ☐ Component isolation implemented (EL0/Ring3)
- ☐ At least 2 example components running
- ☐ End-to-end inference pipeline demonstrated
- ☐ All tests pass on QEMU, Jetson, Pi 5, x86-64 PC

### Demo
- ☐ Boot SLM-OS on target hardware
- ☐ Load ONNX model via shell
- ☐ Run inference on test input
- ☐ Show component receiving input, running inference, sending output
- ☐ Demonstrate hot-swap of component with model
- ☐ Show performance metrics

### Performance Targets
- ☐ Model load time: < 100ms for 50MB model
- ☐ Inference latency: < 10ms for small model (< 1M params)
- ☐ Inference throughput: > 100 inferences/second (small model)
- ☐ Memory overhead: < 20% of model size

---

## Outstanding Decisions

### Milestone 1 — Model Loader

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **ONNX parsing** | Full protobuf vs simplified format | **Simplified** — pre-process ONNX at build time, load flat format at runtime |
| **Operator subset** | All ONNX ops vs minimal | **Minimal** — start with transformer ops (MatMul, Add, LayerNorm, Softmax, Relu) |
| **Weight format** | FP32 vs FP16 vs mixed | **FP32** initially — add FP16/INT8 later for optimization |

### Milestone 2 — Inference Engine

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Operator implementation** | Hand-written vs codegen | **Hand-written** — easier to debug, optimize incrementally |
| **Memory management** | Arena vs per-tensor | **Arena (workspace pool)** — less fragmentation, faster |
| **Threading** | Single-threaded vs parallel ops | **Single-threaded** initially — add parallelism if needed |

### Milestone 3 — GPU Compute

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **GSP loading** | Implement vs defer | **Defer** — focus on CPU, document GSP path for post-capstone |
| **GPU operators** | All vs MatMul only | **MatMul only** if GSP works — largest performance impact |

### Milestone 4 — Isolation

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Syscall ABI** | Linux-compatible vs custom | **Custom** — simpler, no compatibility burden |
| **Address space** | Full separation vs shared kernel | **Full separation** — proper isolation |
| **Restart policy** | Always vs never vs configurable | **Configurable** — in component manifest |

### Milestone 5 — Components

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **First model** | Anomaly detector vs text classifier | **Anomaly detector** — simpler, smaller model |
| **Model source** | Train custom vs use pretrained | **Pretrained + fine-tuned** — faster development |

---

## Risk Mitigation

### Identified Risks

1. **ONNX Complexity**
   - Risk: Full ONNX support is huge; protobuf parsing is complex
   - Mitigation: Support minimal operator subset
   - Mitigation: Pre-process ONNX to simpler format
   - Fallback: Hardcoded model structure (no dynamic loading)

2. **Inference Performance**
   - Risk: Pure Rust/C inference too slow without GPU
   - Mitigation: SIMD optimization (NEON/SSE/AVX)
   - Mitigation: Cache-friendly memory access patterns
   - Mitigation: Target small models (< 10M params)
   - Fallback: Acceptable for demo even if slower than TensorRT

3. **GSP Firmware Complexity**
   - Risk: GSP loading is complex, may not complete in time
   - Mitigation: CPU-only inference is functional fallback
   - Mitigation: GPU memory allocation works without GSP
   - Fallback: Document GSP path, implement post-capstone

4. **User/Kernel Separation Complexity**
   - Risk: EL0/Ring3 transition is significant work
   - Mitigation: Start with ARM64 (better understood from kernel work)
   - Mitigation: Minimal syscall set
   - Fallback: Run components in kernel mode (less isolated but functional)

5. **Model Accuracy**
   - Risk: Inference output doesn't match PyTorch reference
   - Mitigation: Test each operator against reference
   - Mitigation: Use simple models with known outputs
   - Mitigation: Accept small numerical differences (FP ordering)
   - Fallback: Debug with intermediate tensor dumps

6. **Time Constraints**
   - Risk: Phase 5 scope is large
   - Mitigation: Prioritize M1-M2 (model loader + inference) over M4 (isolation)
   - Mitigation: Single working component sufficient for demo
   - Fallback: Defer GPU compute and isolation to Phase 6

---

## Dependencies

### External Dependencies
- **ONNX models:** Need test models for development
  - TinyBERT, DistilBERT, or similar small transformer
  - Simple MLP for anomaly detection
  - Source: Hugging Face, ONNX Model Zoo
- **Phase AI-Sched:** Inference engine patterns may be shared
  - Coordinate on SIMD optimization code
  - Share operator implementations if applicable

### Internal Dependencies

```
M1 (Model Loader) ─────> M2 (Inference) ─────> M5 (Components)
                               │                     │
                               ├─────> M3 (GPU) ─────┤
                               │                     │
                               └─────> M4 (Isolation)┘
                                                     │
M6 (Pipeline) ────────────────────────────────────────┘
                                                     
M7 (Documentation) ────> Throughout
```

**Recommended Order:**
1. **M1 (Model Loader)** — foundation for everything
2. **M2 (Inference Engine)** — core functionality
3. **M5 (Components)** — demonstrates value
4. **M4 (Isolation)** — proper security
5. **M3 (GPU)** — performance optimization
6. **M6 (Pipeline)** — production-ready integration
7. **M7 (Documentation)** — throughout

**Critical Path:** M1 → M2 → M5 (minimum viable demo)

---

## Resources

### ONNX
- [ONNX Specification](https://onnx.ai/onnx/)
- [ONNX Operators](https://onnx.ai/onnx/operators/)
- [ONNX Model Zoo](https://github.com/onnx/models)
- [Protobuf Wire Format](https://protobuf.dev/programming-guides/encoding/)

### Inference Optimization
- [BLAS-like Library Instantiation Software (BLIS)](https://github.com/flame/blis)
- [XNNPACK](https://github.com/google/XNNPACK) — mobile inference library
- [ARM Compute Library](https://github.com/ARM-software/ComputeLibrary)

### User Mode / Syscalls
- ARM Architecture Reference Manual — Exception Handling, EL0/EL1
- Intel SDM Vol. 3 — SYSCALL/SYSRET
- [OSDev Wiki — System Calls](https://wiki.osdev.org/System_Calls)
- seL4 syscall design (minimal microkernel)

### Example Models
- [TinyBERT](https://huggingface.co/huawei-noah/TinyBERT_General_4L_312D)
- [DistilBERT](https://huggingface.co/distilbert-base-uncased)
- [MobileNetV2](https://github.com/onnx/models/tree/main/vision/classification/mobilenet)

---

## Lessons from Prior Phases

### From Phase 3 (Model Memory)
- 2MB blocks work well for model weights
- Weight/workspace pool separation is clean
- Reference counting enables safe sharing
- Statistics are valuable for debugging

### From Phase 4 (Components)
- Component manifest YAML is flexible
- Hot-swap with subscription preservation works
- Topic-based routing is sufficient
- Stateless hot-swap is much simpler than stateful

### From Phase 4X (GPU)
- GSP is mandatory for Ampere compute
- Register probing and VRAM access work without GSP
- GPU memory allocation via PMM is straightforward
- Cache coherency patterns are well-understood

### From Phase AI-Sched (Inference Patterns)
- Stack-allocated scratch buffers work well
- SIMD auto-vectorization may be sufficient
- FP state save/restore needed in IRQ context
- Fallback to heuristic is essential

---

## Deferred to Phase 6

| Item | Notes |
|------|-------|
| GSP firmware loading | Requires reverse-engineering nouveau GSP boot |
| GPU compute kernels | Requires GSP |
| FP16/INT8 quantization | Optimization after FP32 works |
| Advanced batching | Dynamic batching based on load |
| Model caching | LRU eviction when memory pressure |
| Distributed inference | Multi-board pipeline (network required) |
| Power management | DVFS, thermal throttling integration |

---

*Created: April 2026*
*Target: Complete SLM inference pipeline with example components*
*Capstone Focus: M1 (Model Loader) + M2 (Inference) + M5 (Components) = minimum viable demo*
