#!/usr/bin/env python3
"""
mnist-extract-weights.py — Pull the weight tensors out of
models/test/mnist.onnx and write them as raw little-endian binaries
the GPU launcher (scripts/gpu-kernel-mnist.c) can mmap without an
ONNX parser.

Also generates a synthetic input image (`input_synth.bin`) so the
launcher and the CPU NEON reference can be validated against the
same fixed input.

Run from the repo root:
    python3 scripts/mnist-extract-weights.py models/test/mnist.onnx \\
        scripts/mnist-weights/                       # default: fp32

    python3 scripts/mnist-extract-weights.py models/test/mnist.onnx \\
        scripts/mnist-weights-fp16/ --dtype fp16     # tensor-core path

Inside the output directory the script writes (all little-endian, no
header). Sizes shown for fp32; fp16 halves every weight/bias file:
    W_conv1.bin   8*1*5*5      = 200 elem    = 800 B  fp32 / 400 B fp16
    B_conv1.bin   8            = 8 elem      = 32 B   fp32 / 16 B  fp16
    W_conv2.bin   16*8*5*5     = 3200 elem   = 12800 B / 6400 B
    B_conv2.bin   16           = 16 elem     = 64 B   / 32 B
    W_fc.bin      256*10       = 2560 elem   = 10240 B / 5120 B
    B_fc.bin      10           = 10 elem     = 40 B   / 20 B
    input_synth.bin  1*1*28*28 = 784 floats  = 3136 B (always fp32 —
                                 input to the pipeline is always fp32;
                                 only weights change format)
    expected_logits.bin   10   = 10 floats   = 40 B (always fp32; in
                                 the fp16 directory these are computed
                                 with FP16-roundtripped weights, which
                                 emulates the precision of the future
                                 HMMA GEMM kernel)

Per #660's alignment note: the fp16 output emulates a future Q4_K /
INT8 / etc. weight conversion path. The on-disk format is "raw FP16
weight bytes"; the launcher (in #661) loads them as-is for HMMA
kernels and casts back to FP32 at load time for FP32 kernels until
HMMA Conv2D (#662) lands. Roundtrip precision in this script matches
that "all kernels see FP16-rounded weights" pessimistic case.
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


def fp16_roundtrip(arr):
    """Cast FP32 → FP16 → FP32 to emulate the precision the GPU will
    see when weights are stored as FP16 and an HMMA-style kernel
    casts activations to FP16 on the load path. The accumulator is
    still FP32 (matches mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32)
    so the matmul itself accumulates in FP32 even though both
    operands are FP16. This is the closest numpy can get without
    re-implementing the MMA fragment ordering."""
    return arr.astype(np.float16).astype(np.float32)


def cpu_reference_inference(model, input_img, op_outputs=None,
                            weight_dtype=np.float32):
    """Run the model in numpy to produce reference logits.

    If `op_outputs` is a list, append each op's output to it (in
    pipeline order, matching the ordering used by the launcher).

    `weight_dtype` controls the precision the CPU reference uses for
    weights:
      np.float32  — bit-exact today's behavior (FP32 throughout)
      np.float16  — emulate the FP16 weight path: round-trip every
                    weight through FP16 once, then run the rest of
                    the pipeline in FP32 with the rounded weights.
                    Activations are FP32 (matching the HMMA kernel's
                    cvt-on-load model). The output reflects the
                    precision the GPU will produce.
    """
    W_conv1 = extract_initializer(model, "Parameter5")  # [8,1,5,5]
    B_conv1 = extract_initializer(model, "Parameter6")  # [8,1,1]
    W_conv2 = extract_initializer(model, "Parameter87") # [16,8,5,5]
    B_conv2 = extract_initializer(model, "Parameter88") # [16,1,1]
    W_fc    = extract_initializer(model, "Parameter193")# [16,4,4,10]
    B_fc    = extract_initializer(model, "Parameter194")# [1,10]

    if weight_dtype == np.float16:
        W_conv1 = fp16_roundtrip(W_conv1)
        B_conv1 = fp16_roundtrip(B_conv1)
        W_conv2 = fp16_roundtrip(W_conv2)
        B_conv2 = fp16_roundtrip(B_conv2)
        W_fc    = fp16_roundtrip(W_fc)
        B_fc    = fp16_roundtrip(B_fc)

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
    ap.add_argument("--dtype", choices=("fp32", "fp16"), default="fp32",
                    help="Weight precision on disk. "
                         "fp32 (default): today's 4-byte little-endian floats. "
                         "fp16: 2-byte IEEE-754 binary16 for tensor-core paths "
                         "(#659/#661). Synthetic input + expected_logits stay "
                         "fp32; only weights/biases change format.")
    args = ap.parse_args()

    weight_np_dtype = np.float16 if args.dtype == "fp16" else np.float32
    weight_bytes = 2 if args.dtype == "fp16" else 4

    os.makedirs(args.out_dir, exist_ok=True)
    model = onnx.load(args.onnx_path)

    print(f"[extract] reading {args.onnx_path} (dtype={args.dtype})")
    for name, fname in WEIGHT_MAP.items():
        arr = extract_initializer(model, name)
        # Cast to the requested wire format. For fp16, the FP32→FP16
        # round-down here is the same loss the GPU will see when
        # reading the weight buffer; the CPU reference below
        # reproduces this loss via fp16_roundtrip() so the
        # expected_logits and sentinels match what the kernel will
        # actually produce.
        cast = arr.astype(weight_np_dtype)
        path = os.path.join(args.out_dir, fname)
        with open(path, "wb") as f:
            f.write(cast.tobytes())
        print(f"  {name} -> {fname}: shape={list(arr.shape)} "
              f"({arr.size} elem, {arr.size*weight_bytes} B {args.dtype})")

    # Synthetic input — deterministic, non-trivial so all conv
    # filters see varied data. Always FP32: input to the pipeline is
    # FP32 regardless of weight dtype; the HMMA kernel casts
    # activations on the load path internally.
    rng = np.random.default_rng(42)
    inp = rng.standard_normal((1, 1, 28, 28)).astype(np.float32) * 0.3
    inp_path = os.path.join(args.out_dir, "input_synth.bin")
    with open(inp_path, "wb") as f:
        f.write(inp.tobytes())
    print(f"  synthetic input -> input_synth.bin: 1x1x28x28 "
          f"({inp.size} elem fp32)")

    # CPU reference logits + per-op intermediate activations. The
    # weight_dtype here drives whether the reference path emulates
    # FP16-rounded weights or stays bit-exact FP32.
    print(f"[extract] computing CPU reference inference "
          f"(weight precision={args.dtype})...")
    op_outputs = []
    logits = cpu_reference_inference(model, inp, op_outputs,
                                     weight_dtype=weight_np_dtype)
    log_path = os.path.join(args.out_dir, "expected_logits.bin")
    with open(log_path, "wb") as f:
        f.write(logits.astype(np.float32).tobytes())
    print(f"  expected logits -> expected_logits.bin: {logits.size} elem fp32")
    print(f"  argmax = {int(np.argmax(logits))}, "
          f"logits = {logits.tolist()}")

    # If we're running fp16, also report the FP32 reference for
    # comparison so a regression in the round-trip is visible at
    # extraction time. Argmax must match (one synthetic input is a
    # weak signal but a useful smoke check; full validation is in
    # mnist-verify-fp16-roundtrip.py).
    if args.dtype == "fp16":
        ref_logits = cpu_reference_inference(model, inp,
                                             weight_dtype=np.float32)
        ref_argmax = int(np.argmax(ref_logits))
        fp16_argmax = int(np.argmax(logits))
        l2 = float(np.linalg.norm(logits - ref_logits))
        print(f"[fp16-check] fp32 ref argmax = {ref_argmax}, "
              f"fp16 argmax = {fp16_argmax}, L2(diff) = {l2:.6f}")
        if ref_argmax != fp16_argmax:
            print(f"[fp16-check] WARNING: argmax differs on synthetic "
                  f"input. Run mnist-verify-fp16-roundtrip.py against "
                  f"the test set to assess severity.")

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
