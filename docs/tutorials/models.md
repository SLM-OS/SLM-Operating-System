# Tutorial: Preparing and Loading ONNX Models

Guide for preparing, loading, and verifying ONNX models in SLM-OS.

**Prerequisites:** A running SLM-OS instance (QEMU or hardware). The LittleFS filesystem must be mounted at `/mnt/files`.

---

## Overview

SLM-OS includes a minimal ONNX model loader implemented in Rust that parses ONNX protobuf files, constructs an operator graph, and stores model weights in dedicated memory pools. The loader is designed for a `no_std` bare-metal environment with strict size constraints, so only a subset of ONNX features is supported.

The reference test model is **MNIST-12** from the [ONNX Model Zoo](https://github.com/onnx/models), a 26 KB handwritten digit classifier.

---

## Supported Model Format

### Requirements

- **Format:** ONNX (Open Neural Network Exchange)
- **Opset version:** 7 or higher
- **Weight data type:** FP32 (32-bit floating point)
- **Weight storage:** Raw bytes in ONNX protobuf (`raw_data` field, field number 13)

### Supported Operators

The ONNX parser recognizes the following operators during graph construction:

| Operator | Description | Notes |
|----------|-------------|-------|
| MatMul | Matrix multiplication | M x K times K x N |
| Add | Element-wise addition | With broadcasting |
| Relu | Rectified linear unit | max(0, x) |
| Softmax | Softmax normalization | Along last axis |
| Reshape | Tensor reshape | Supports -1 dimension inference |
| Conv | 2D convolution | NCHW layout, default stride=1, pad=0 |
| MaxPool | 2D max pooling | Default kernel=2x2, stride=2 |
| Gemm | General matrix multiply | Y = A x B + C |
| Flatten | Flatten to 2D | Default axis=1 |
| LayerNorm | Layer normalization | Recognized but not yet executed |
| Transpose | Tensor transpose | Recognized during parsing |
| Gather | Gather elements | Recognized during parsing |
| Concat | Concatenate tensors | Recognized during parsing |
| Unsqueeze | Insert dimensions | Skipped during execution |
| Shape | Return tensor shape | Skipped during execution |
| Constant | Constant value | Handled via weight table |
| Cast | Type conversion | Skipped during execution |

Operators not in this set are parsed as `Unknown`. An `Unknown` operator does not prevent loading but will block inference when the execution engine encounters it.

---

## Size Constraints

The loader operates within fixed-size buffers to avoid heap allocation. These constraints determine the maximum model complexity:

| Constraint | Limit | Notes |
|-----------|-------|-------|
| Maximum graph nodes | 16 (parse) / 32 (graph) | Sufficient for MNIST-12 (12 nodes) |
| Maximum initializers | 16 | Weight tensors per model |
| Maximum inputs per node | 3 (parse) / 4 (graph) | Covers standard operators |
| Maximum outputs per node | 2 | |
| Maximum graph-level I/O | 4 | Graph inputs and outputs |
| Tensor name length | 24 bytes (parse) / 32 bytes (graph) | Truncated if longer |
| Maximum loaded models | 8 | Simultaneous models in registry |
| Maximum tensor dimensions | 8 | Per-tensor dimension limit |
| Weight block alignment | 2 MB | Pool allocates in 2 MB blocks |

### Memory Pool Sizes

The model memory system uses two separate pools:

- **Weight pool:** Stores read-only model weights. Allocated from physical memory in 2 MB blocks.
- **Workspace pool:** Temporary storage for intermediate tensors during inference. Reused across inference calls via a bump allocator.

Pool statistics can be viewed with `model pools` in the shell.

---

## How to Load a Model

### Step 1: Place the Model on the Filesystem

The ONNX model file must be accessible through the VFS. For QEMU, the LittleFS filesystem is pre-populated during boot. The MNIST test model is available at `/mnt/files/mnist.onnx`.

For custom models, the file must be copied into `/mnt/files` or loaded through another VFS-accessible path before `model load`.

### Step 2: Load the Model

From the SLM-OS shell:

```
SLM-OS> model load /mnt/files/mnist.onnx
```

The loader performs the following steps:

1. Reads the file from VFS into a temporary buffer
2. Parses the ONNX protobuf wire format
3. Extracts the computation graph (nodes, inputs, outputs)
4. Copies weight tensors into the weight memory pool
5. Registers the model in the model registry

On success, the shell displays the model name, parameter count, and number of nodes.

### Step 3: Verify the Model

Use `model info` to inspect the loaded model:

```
SLM-OS> model info mnist
```

This displays:

- Model name and format (ONNX)
- Parameter count and weight size
- Number of graph nodes
- Number of inputs and outputs

Use `model list` to see all loaded models:

```
SLM-OS> model list
```

### Step 4: Run Inference

Test the model with a zero-input inference:

```
SLM-OS> model infer mnist
```

This executes the full forward pass through the operator graph with a zero-valued input tensor and prints:

- Output probabilities for each class
- The predicted class (argmax)
- Inference time

---

## Verification Commands

### model info

Displays detailed information about a loaded model:

```
SLM-OS> model info mnist
Model: mnist
  Format: ONNX
  Parameters: ~26,000
  Weight size: 26 KB
  Nodes: 12
  Inputs: 1
  Outputs: 1
```

### model infer

Runs inference with zero input and prints the full output:

```
SLM-OS> model infer mnist
```

The output includes per-class probabilities and the predicted class index. With zero input, the predicted class depends on the model's bias terms.

### model bench

Runs multiple inference iterations and reports timing statistics:

```
SLM-OS> model bench mnist 10
```

This runs 10 inference iterations and reports min, max, and average latency. See `docs/tutorials/performance.md` for details on benchmarking.

### model pools

Shows memory pool statistics:

```
SLM-OS> model pools
```

Displays total blocks, free blocks, allocated blocks, shared blocks, and peak usage for both the weight pool and workspace pool.

---

## MNIST-12 Reference Model

The MNIST-12 model from the ONNX Model Zoo is the primary test model for SLM-OS:

| Property | Value |
|----------|-------|
| Task | Handwritten digit classification (28x28 grayscale to 10 classes) |
| File size | ~26 KB ONNX protobuf |
| Graph nodes | 12 (Conv, MaxPool, Reshape, MatMul, Add, Relu, Softmax) |
| Weight tensors | 8 initializers |
| Weight data | ~26 KB (FP32) |
| Parameters | ~26,000 |

The model uses two convolutional layers with max pooling, followed by fully-connected layers with ReLU activation and a softmax output.

### Operator Flow

```
Input [1,1,28,28]
    |
    v
Conv (8 filters, 5x5) --> Relu --> MaxPool (2x2)
    |
    v
Conv (16 filters, 5x5) --> MaxPool (2x2)
    |
    v
Reshape --> MatMul + Add --> Relu
    |
    v
MatMul + Add --> Softmax
    |
    v
Output [1,10] (digit probabilities)
```

---

## Troubleshooting

### "Model not found" after loading

Verify the file path exists in VFS:

```
SLM-OS> ls /mnt/files/
```

If the file is listed but loading fails, the ONNX file may exceed the parser's size constraints (see "Size Constraints" above).

### "Unknown operator" warning during load

The model uses an operator not in the supported set. Loading succeeds, but inference will fail when the engine reaches the unsupported operator. Check the supported operators list and consider simplifying the model.

### Inference returns unexpected results

With zero input, the predicted class is determined entirely by the model's bias terms. This is expected behavior and validates that the inference pipeline executes correctly end-to-end. For meaningful predictions, real input data must be provided via the `rust_infer()` FFI function from a component.

### Weight pool exhaustion

If `model load` fails with a memory error, the weight pool may be full. Use `model pools` to check pool utilization and `model unload <name>` to free memory from unused models.

---

## Source Files

| File | Purpose |
|------|---------|
| `runtime/src/loader/protobuf.rs` | Protobuf wire format parser |
| `runtime/src/loader/onnx_parser.rs` | ONNX schema parser and graph builder |
| `runtime/src/loader/graph.rs` | Operator graph types (OpType, GraphNode, etc.) |
| `runtime/src/loader/registry.rs` | Model registry (load, unload, find, list) |
| `runtime/src/inference/engine.rs` | Inference engine (operator dispatch, workspace) |
| `runtime/src/inference/ops.rs` | Individual operator implementations |
| `docs/onnx-support.md` | Detailed ONNX format documentation |

---

*Last updated: April 2026*
