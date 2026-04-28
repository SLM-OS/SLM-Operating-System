#!/usr/bin/env python3
"""
sched-extract-weights.py — Pull the eight fp32 weight tensors out of
`kernel/sched/ai/ai_weights_mlp.c` and write them as raw little-endian
fp32 binaries the GPU launcher (scripts/gpu-kernel-sched-mlp.c) can
mmap without parsing C.

Also generates a synthetic input vector (`input_synth.bin`) and the
matching CPU reference logits (`expected_logits.bin`) so the launcher
can self-check GPU output against a known-good answer.

The C source declares each W matrix as `[OUT_DIM][IN_DIM]` row-major:

    const float ai_mlp_w0[256 * 108] = { ... }   # 256 rows × 108 cols
    const float ai_mlp_b0[256]       = { ... }
    const float ai_mlp_w1[256 * 256] = { ... }   # 256 × 256
    const float ai_mlp_b1[256]       = { ... }
    const float ai_mlp_w2[128 * 256] = { ... }   # 128 × 256
    const float ai_mlp_b2[128]       = { ... }
    const float ai_mlp_w3[42  * 128] = { ... }   # 42  × 128
    const float ai_mlp_b3[42]        = { ... }

The forward pass walks each layer as
`out[i] = bias[i] + sum_k W[i*IN + k] * in[k]`.

The GPU `gemm_fp32` kernel computes `C[i,j] = sum_k A[i,k] * B[k,j]`
where A is `[M][K]`, B is `[K][N]`, C is `[M][N]`. To reuse it for the
sched MLP with `state` (1×IN) as A, we need B in `[IN][OUT]` form —
that is, the *transpose* of the C-source layout. This script writes
each W as the transposed flat fp32 sequence so the launcher can pass
it straight through to GEMM with `M=1, K=IN_DIM, N=OUT_DIM`.

Run from the repo root:

    python3 scripts/sched-extract-weights.py \\
        kernel/sched/ai/ai_weights_mlp.c \\
        scripts/sched-weights/

Output files (all little-endian float32, no header):

    W0.bin   108 × 256 = 27,648 floats   = 110,592 B  (transposed)
    b0.bin   256       = 256 floats      = 1,024 B
    W1.bin   256 × 256 = 65,536 floats   = 262,144 B  (transposed)
    b1.bin   256       = 256 floats      = 1,024 B
    W2.bin   256 × 128 = 32,768 floats   = 131,072 B  (transposed)
    b2.bin   128       = 128 floats      = 512 B
    W3.bin   128 × 42  = 5,376 floats    = 21,504 B   (transposed)
    b3.bin   42        = 42 floats       = 168 B
    input_synth.bin       108 floats     = 432 B
    expected_logits.bin   42 floats      = 168 B

Total weight bytes: 528,040 (~516 KB), matches the spec arithmetic in
`docs/specs/gpu-policy-models.md` §A.

The synthetic input is deterministic (fixed seed). The launcher's
self-check passes if every output element matches the CPU reference
within ~1e-5 absolute error; ULP-divergence between the kernel's
fma-style accumulation and Python's straight-line product-sum is
expected at this magnitude.
"""

import argparse
import os
import re
import struct
import sys

# Layer shapes match the AI_MLP_LAYER*_{IN,OUT} constants in
# kernel/sched/ai/ai_types.h (LAYER0_IN=AI_STATE_DIM=108,
# LAYER0_OUT=256, LAYER1=256→256, LAYER2=256→128, LAYER3=128→42 on
# Jetson AI_SCHED_N_ACTIONS=42).
LAYERS = [
    # (W_name,        bias_name, OUT,  IN)
    ("ai_mlp_w0", "ai_mlp_b0", 256, 108),
    ("ai_mlp_w1", "ai_mlp_b1", 256, 256),
    ("ai_mlp_w2", "ai_mlp_b2", 128, 256),
    ("ai_mlp_w3", "ai_mlp_b3",  42, 128),
]


def parse_array(text, name):
    """
    Find `const float NAME[...] = { ... };` in `text`, parse every
    hex-float literal between the braces, and return the flat list of
    Python floats. Length is checked by the caller against the
    declared size.
    """
    pattern = re.compile(
        r"const\s+float\s+" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{([^}]+)\}",
        re.DOTALL,
    )
    m = pattern.search(text)
    if not m:
        sys.exit(f"could not find array {name!r} in source")

    body = m.group(1)
    # The C source uses hex-float literals like `-0x1.c695240000000p-5`.
    # Python's float.fromhex accepts that format directly.
    tokens = re.findall(r"-?0x1\.[0-9a-f]+p[+-]?\d+|-?0x0p\+0|-?0x0p0", body)
    return [float.fromhex(tok) for tok in tokens]


def transpose_weight(flat, out_dim, in_dim):
    """
    Re-order a row-major [out_dim][in_dim] sequence into row-major
    [in_dim][out_dim]. Written as nested loops rather than numpy so
    the script has zero non-stdlib dependencies (numpy isn't always
    available in the build host's default Python).
    """
    if len(flat) != out_dim * in_dim:
        sys.exit(f"weight length {len(flat)} != {out_dim} * {in_dim}")
    out = [0.0] * (in_dim * out_dim)
    for i in range(out_dim):
        for k in range(in_dim):
            out[k * out_dim + i] = flat[i * in_dim + k]
    return out


def write_floats(path, floats):
    with open(path, "wb") as f:
        f.write(struct.pack("<" + "f" * len(floats), *floats))


def _first_nonzero_sentinel(vec):
    """
    Return (cell_idx, bits) of the first cell in `vec` whose fp32
    bit-pattern is non-zero. `bits` is the little-endian uint32
    representation of the float, captured for any future exact-match
    poller; the launcher uses any-non-zero mode (expected_payload=0)
    so the bit pattern is informational. If every cell rounds to 0.0
    (degenerate case — should never happen for a well-trained
    network's intermediate output), fall back to cell 0 with bits=0;
    the launcher will then time out, which is the right diagnostic.
    """
    for i, v in enumerate(vec):
        bits = struct.unpack("<I", struct.pack("<f", float(v)))[0]
        if bits != 0:
            return (i, bits)
    return (0, 0)


def cpu_forward(weights, x):
    """
    NumPy-free reference forward pass. Matches `forward_logits` in
    `kernel/sched/ai/ai_inference.c`: `out = bias + W·in`, ReLU on
    layers 0..N-2, no activation on the final layer. Used to generate
    `expected_logits.bin` so the launcher can self-check against a
    fixed answer.
    """
    cur = list(x)
    for li, (W_flat, b, out_dim, in_dim) in enumerate(weights):
        nxt = list(b)  # start from bias
        for i in range(out_dim):
            row_off = i * in_dim
            for k in range(in_dim):
                nxt[i] += W_flat[row_off + k] * cur[k]
        if li < len(weights) - 1:
            nxt = [v if v > 0.0 else 0.0 for v in nxt]
        cur = nxt
    return cur


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="path to ai_weights_mlp.c")
    ap.add_argument("out_dir", help="output directory for binaries")
    args = ap.parse_args()

    if not os.path.isfile(args.source):
        sys.exit(f"source file not found: {args.source}")
    os.makedirs(args.out_dir, exist_ok=True)

    with open(args.source, "r") as f:
        text = f.read()

    # Extract + transpose each W; extract bias as-is (it's a vector).
    layers_for_cpu = []
    print(f"Extracting from {args.source} → {args.out_dir}/")
    for li, (w_name, b_name, out_dim, in_dim) in enumerate(LAYERS):
        w_flat = parse_array(text, w_name)
        b_flat = parse_array(text, b_name)
        if len(w_flat) != out_dim * in_dim:
            sys.exit(f"{w_name}: expected {out_dim*in_dim} floats, got {len(w_flat)}")
        if len(b_flat) != out_dim:
            sys.exit(f"{b_name}: expected {out_dim} floats, got {len(b_flat)}")

        w_t = transpose_weight(w_flat, out_dim, in_dim)
        write_floats(os.path.join(args.out_dir, f"W{li}.bin"), w_t)
        write_floats(os.path.join(args.out_dir, f"b{li}.bin"), b_flat)
        print(f"  L{li}: W{li}.bin  ({in_dim}×{out_dim} = {len(w_t)} floats)"
              f"  b{li}.bin  ({len(b_flat)} floats)")

        # Keep the *original* (non-transposed) W for the CPU reference
        # so the math directly matches `forward_logits` in
        # ai_inference.c. The transpose is only to feed gemm_fp32.
        layers_for_cpu.append((w_flat, b_flat, out_dim, in_dim))

    # Synthetic input: deterministic 108-element vector. The values
    # are picked to span [-1, 1] without being trivially zero or
    # one-hot, so the GPU/CPU agreement check exercises the full
    # accumulation path. Linear sequence (i / 53.5 - 1.0) for the
    # first 108 entries is enough to expose any per-row aliasing
    # bug in the gemm tile layout.
    in_vec = [(i / 53.5) - 1.0 for i in range(108)]
    write_floats(os.path.join(args.out_dir, "input_synth.bin"), in_vec)
    print(f"  input_synth.bin    ({len(in_vec)} floats, deterministic)")

    expected = cpu_forward(layers_for_cpu, in_vec)
    write_floats(os.path.join(args.out_dir, "expected_logits.bin"), expected)
    print(f"  expected_logits.bin ({len(expected)} floats, CPU reference)")
    print(f"  argmax(expected) = {expected.index(max(expected))}")

    # Per-op sentinels: (cell_idx, fp32_bits) for each of the 8 pipeline
    # ops. Format matches MNIST's `pipeline_sentinels.bin` so the
    # launcher (gpu-kernel-sched-mlp.c) can read it the same way.
    #
    # The 8-op pipeline is gemm + addrelu × 3, then gemm + addbias:
    #   op 0: gemm(L0)        out[256]   pre-relu
    #   op 1: addrelu(L0+b0)  out[256]   post-relu
    #   op 2: gemm(L1 of post-relu0)     out[256] pre-relu
    #   op 3: addrelu(L1+b1)  out[256]
    #   op 4: gemm(L2)        out[128]
    #   op 5: addrelu(L2+b2)  out[128]
    #   op 6: gemm(L3)        out[42]
    #   op 7: addbias(L3+b3)  out[42]   final logits
    #
    # IMPORTANT split with how `forward_logits` is structured: the
    # original C does fused matvec+bias in one step, but the GPU
    # pipeline splits gemm and add_bias_relu into separate ops. So
    # the gemm output here is (W·x) without bias; the addrelu output
    # adds bias and applies ReLU (or just bias for the final).
    #
    # For each op we find the first cell whose value is non-zero
    # (strictly: bit-pattern != 0). The launcher polls that cell in
    # any-non-zero mode; the kernel writes that bit pattern (or
    # something ULP-divergent), the poll fires, the next op
    # dispatches.
    op_sentinels = []  # list of (cell_idx, bits)
    cur = list(in_vec)
    for li, (w_flat, b, out_dim, in_dim) in enumerate(layers_for_cpu):
        # gemm op: output[i] = sum_k W[i*IN+k] * cur[k]   (no bias)
        gemm_out = [0.0] * out_dim
        for i in range(out_dim):
            row_off = i * in_dim
            for k in range(in_dim):
                gemm_out[i] += w_flat[row_off + k] * cur[k]
        op_sentinels.append(_first_nonzero_sentinel(gemm_out))

        # addrelu / addbias op: output[i] = gemm[i] + b[i]; ReLU on
        # all but the final layer.
        post = [gemm_out[i] + b[i] for i in range(out_dim)]
        if li < len(layers_for_cpu) - 1:
            post = [v if v > 0.0 else 0.0 for v in post]
        op_sentinels.append(_first_nonzero_sentinel(post))

        # Layer's "output" feeds the next layer (after ReLU); the
        # final layer produces the logits and there's no next layer.
        cur = post

    sentinel_path = os.path.join(args.out_dir, "pipeline_sentinels.bin")
    with open(sentinel_path, "wb") as f:
        for (cell, bits) in op_sentinels:
            f.write(struct.pack("<II", cell, bits))
    print(f"  pipeline_sentinels.bin ({len(op_sentinels)} (cell,bits) pairs, "
          f"first-non-zero per op)")
    for op_idx, (cell, bits) in enumerate(op_sentinels):
        print(f"    op[{op_idx}] cell={cell} bits=0x{bits:08x}")


if __name__ == "__main__":
    main()
