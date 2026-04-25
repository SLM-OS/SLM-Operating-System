#!/usr/bin/env python3
"""
mnist-extract-weights.py — Pull the five fp32 weight tensors out of
models/test/mnist.onnx and write them as raw little-endian fp32
binaries the GPU launcher (scripts/gpu-kernel-mnist.c) can mmap
without an ONNX parser.

Also generates a synthetic input image (`mnist_input_synth.bin`) so
the launcher and the CPU NEON reference can be validated against the
same fixed input.

Run from the repo root:
    python3 scripts/mnist-extract-weights.py models/test/mnist.onnx \\
        scripts/mnist-weights/

Inside scripts/mnist-weights/ the script writes (all little-endian
float32, no header):
    W_conv1.bin   8*1*5*5      = 200 floats   = 800 B
    B_conv1.bin   8            = 8 floats     = 32 B
    W_conv2.bin   16*8*5*5     = 3200 floats  = 12800 B
    B_conv2.bin   16           = 16 floats    = 64 B
    W_fc.bin      256*10       = 2560 floats  = 10240 B
    B_fc.bin      10           = 10 floats    = 40 B
    input_synth.bin  1*1*28*28 = 784 floats   = 3136 B (deterministic)
    expected_logits.bin   10   = 10 floats    = 40 B (CPU reference)
"""

import argparse
import os
import struct
import sys
import numpy as np

try:
    import onnx
except ImportError:
    sys.exit(
        "onnx module required. Install in a venv: "
        "python3 -m venv /tmp/onnxenv && "
        "/tmp/onnxenv/bin/pip install onnx numpy && "
        "exec /tmp/onnxenv/bin/python " + " ".join(sys.argv)
    )

# Map ONNX initializer names → output filenames.
WEIGHT_MAP = {
    "Parameter5":   "W_conv1.bin",   # [8,1,5,5]
    "Parameter6":   "B_conv1.bin",   # [8,1,1]
    "Parameter87":  "W_conv2.bin",   # [16,8,5,5]
    "Parameter88":  "B_conv2.bin",   # [16,1,1]
    "Parameter193": "W_fc.bin",      # [16,4,4,10] — reshape to [256,10]
    "Parameter194": "B_fc.bin",      # [1,10]
}


def extract_initializer(model, name):
    for init in model.graph.initializer:
        if init.name == name:
            arr = np.frombuffer(init.raw_data, dtype=np.float32) \
                if init.raw_data else np.array(init.float_data, dtype=np.float32)
            return arr.reshape(list(init.dims))
    raise KeyError(f"initializer '{name}' not found")


def cpu_reference_inference(model, input_img, op_outputs=None):
    """Run the model in numpy to produce reference logits.

    If `op_outputs` is a list, append each op's output to it (in
    pipeline order, matching the ordering used by the launcher).
    """
    W_conv1 = extract_initializer(model, "Parameter5")  # [8,1,5,5]
    B_conv1 = extract_initializer(model, "Parameter6")  # [8,1,1]
    W_conv2 = extract_initializer(model, "Parameter87") # [16,8,5,5]
    B_conv2 = extract_initializer(model, "Parameter88") # [16,1,1]
    W_fc    = extract_initializer(model, "Parameter193")# [16,4,4,10]
    B_fc    = extract_initializer(model, "Parameter194")# [1,10]

    def conv2d_same(x, w, pad):
        N, C_in, H, W = x.shape
        C_out, _, kH, kW = w.shape
        out = np.zeros((N, C_out, H, W), dtype=np.float32)
        for n in range(N):
            for co in range(C_out):
                for oh in range(H):
                    for ow in range(W):
                        s = 0.0
                        for ci in range(C_in):
                            for kh in range(kH):
                                ih = oh + kh - pad
                                if ih < 0 or ih >= H: continue
                                for kw in range(kW):
                                    iw = ow + kw - pad
                                    if iw < 0 or iw >= W: continue
                                    s += x[n,ci,ih,iw] * w[co,ci,kh,kw]
                        out[n,co,oh,ow] = s
        return out

    def maxpool(x, k, s):
        N, C, H, W = x.shape
        H_out = (H - k) // s + 1
        W_out = (W - k) // s + 1
        out = np.zeros((N, C, H_out, W_out), dtype=np.float32)
        for n in range(N):
            for c in range(C):
                for oh in range(H_out):
                    for ow in range(W_out):
                        m = -np.inf
                        for dh in range(k):
                            for dw in range(k):
                                v = x[n, c, oh*s+dh, ow*s+dw]
                                if v > m: m = v
                        out[n,c,oh,ow] = m
        return out

    def maybe_capture(arr):
        if op_outputs is not None:
            op_outputs.append(arr.copy())

    # Op 0: Conv1
    x = input_img  # [1,1,28,28]
    x = conv2d_same(x, W_conv1, pad=2)         # [1,8,28,28]
    maybe_capture(x)

    # Op 1: Add+ReLU after Conv1
    x = x + B_conv1[None, :, :, :]
    x = np.maximum(0, x)
    maybe_capture(x)

    # Op 2: MaxPool1
    x = maxpool(x, k=2, s=2)                   # [1,8,14,14]
    maybe_capture(x)

    # Op 3: Conv2
    x = conv2d_same(x, W_conv2, pad=2)         # [1,16,14,14]
    maybe_capture(x)

    # Op 4: Add+ReLU after Conv2
    x = x + B_conv2[None, :, :, :]
    x = np.maximum(0, x)
    maybe_capture(x)

    # Op 5: MaxPool2
    x = maxpool(x, k=3, s=3)                   # [1,16,4,4]
    maybe_capture(x)

    # Op 6: MatMul (Reshape is free — pointer aliasing)
    x_flat = x.reshape(1, 16*4*4)              # [1,256]
    W_fc_flat = W_fc.reshape(256, 10)          # [256,10]
    x = x_flat @ W_fc_flat                     # [1,10]
    maybe_capture(x)

    # Op 7: AddBias (no ReLU)
    x = x + B_fc                               # [1,10]
    maybe_capture(x)

    return x.flatten()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("onnx_path")
    ap.add_argument("out_dir")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    model = onnx.load(args.onnx_path)

    print(f"[extract] reading {args.onnx_path}")
    for name, fname in WEIGHT_MAP.items():
        arr = extract_initializer(model, name)
        path = os.path.join(args.out_dir, fname)
        with open(path, "wb") as f:
            f.write(arr.astype(np.float32).tobytes())
        print(f"  {name} -> {fname}: shape={list(arr.shape)} "
              f"({arr.size} elem, {arr.size*4} B)")

    # Synthetic input — deterministic, non-trivial so all conv
    # filters see varied data. Picks a 28x28 ramp.
    rng = np.random.default_rng(42)
    inp = rng.standard_normal((1, 1, 28, 28)).astype(np.float32) * 0.3
    inp_path = os.path.join(args.out_dir, "input_synth.bin")
    with open(inp_path, "wb") as f:
        f.write(inp.tobytes())
    print(f"  synthetic input -> input_synth.bin: 1x1x28x28 "
          f"({inp.size} elem)")

    # CPU reference logits + per-op intermediate activations.
    print(f"[extract] computing CPU reference inference...")
    op_outputs = []
    logits = cpu_reference_inference(model, inp, op_outputs)
    log_path = os.path.join(args.out_dir, "expected_logits.bin")
    with open(log_path, "wb") as f:
        f.write(logits.astype(np.float32).tobytes())
    print(f"  expected logits -> expected_logits.bin: {logits.size} elem")
    print(f"  argmax = {int(np.argmax(logits))}, "
          f"logits = {logits.tolist()}")

    # Per-op sentinel values. The launcher uses these to populate
    # the v5 pipeline ops array's `expected_payload` field so SLM-OS
    # can poll for completion of each op without computing them
    # itself.
    #
    # Sentinel cell choice: we'd like to use the first cell of each
    # op's output, but ReLU and MaxPool can produce 0.0 at the top-
    # left corner — which would match the pre-cleared poll buffer
    # immediately and report "done" before the GPU even ran. Pick
    # the first non-zero cell instead, recording its byte offset so
    # the launcher knows where to point the poll target.
    op_names = [
        "00_conv1", "01_addrelu1", "02_pool1",
        "03_conv2", "04_addrelu2", "05_pool2",
        "06_matmul", "07_addbias",
    ]
    sentinels = []
    for name, arr in zip(op_names, op_outputs):
        flat = arr.flatten()
        idx = 0
        for j in range(flat.size):
            if flat[j] != 0.0:
                idx = j
                break
        else:
            raise RuntimeError(f"op {name} output is entirely zero — no usable sentinel")
        val = float(flat[idx])
        bits = struct.unpack("<I", struct.pack("<f", val))[0]
        sentinels.append((name, idx, val, bits, list(arr.shape)))
        print(f"  op[{name}] shape={list(arr.shape)} "
              f"sentinel cell={idx} val={val:.6f} bits=0x{bits:08x}")
    # Wire format: pairs of (uint32 cell_index, uint32 expected_bits),
    # one per op. Launcher reads in order.
    sent_path = os.path.join(args.out_dir, "pipeline_sentinels.bin")
    with open(sent_path, "wb") as f:
        for _, idx, _, bits, _ in sentinels:
            f.write(struct.pack("<II", idx, bits))
    print(f"  pipeline sentinels -> pipeline_sentinels.bin "
          f"({len(sentinels)} pairs)")


if __name__ == "__main__":
    main()
