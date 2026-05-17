#!/usr/bin/env python3
"""
eviction-extract-weights.py — Pull the eight fp32 weight tensors out of
`runtime/src/mm/eviction/generated/mlp_policy_f32.rs` and write them as
raw little-endian fp32 binaries the GPU launcher
(scripts/gpu-kernel-eviction-qnet.c) can mmap without parsing Rust.

Also generates a synthetic input vector (`input_synth.bin`) and the
matching CPU reference score (`expected_score.bin`) so the launcher
can self-check GPU output against a known-good answer.

Direct analog of `scripts/sched-extract-weights.py` (which parses
`kernel/sched/ai/ai_weights_mlp.c`). The only meaningful differences
are:

  - input source is Rust, not C, so the literal-extraction regex
    accepts decimal floats (e.g. `-2.58627`, `1.36616e-3`) instead of
    hex floats (e.g. `-0x1.c695240000000p-5`);
  - the network is shallower per layer (27→64→32→16→1) than the
    sched MLP (108→256→256→128→42) so the per-binary byte counts
    are smaller;
  - the final layer applies sigmoid, not "identity for logits", so
    the pipeline grows from 8 ops (4 gemm + 4 add_bias[_relu]) to
    9 ops with a trailing `sigmoid_fp32`;
  - the output is a single fp32 score in [0, 1], not a 42-element
    logit vector, so `expected_score.bin` is 4 bytes total and the
    launcher's agreement check uses `|gpu - cpu| < 1e-4` instead of
    argmax.

The Rust source declares each W matrix as `[[f32; IN]; OUT]` row-major:

    pub const W_L1: [[f32; 27]; 64] = [
        [ ... 27 values for output neuron 0 ... ],
        [ ... 27 values for output neuron 1 ... ],
        ...
    ];
    pub const B_L1: [f32; 64] = [...];
    ... (W_L2 [[f32; 64]; 32], W_L3 [[f32; 32]; 16], W_OUT [[f32; 16]; 1]) ...

The forward pass walks each layer as
`out[i] = bias[i] + sum_k W[i][k] * in[k]` then applies ReLU
(layers 1..3) or sigmoid (output).

The GPU `gemm_fp32` kernel computes `C[i,j] = sum_k A[i,k] * B[k,j]`
where A is `[M][K]`, B is `[K][N]`, C is `[M][N]`. To reuse it for
the eviction MLP with `features` (1×IN) as A, we need B in `[IN][OUT]`
form — the *transpose* of the Rust source layout. This script writes
each W as the transposed flat fp32 sequence so the launcher can pass
it straight through to GEMM with `M=1, K=IN_DIM, N=OUT_DIM`.

Run from the repo root:

    python3 scripts/eviction-extract-weights.py \\
        runtime/src/mm/eviction/generated/mlp_policy_f32.rs \\
        scripts/eviction-weights/

Output files (all little-endian float32, no header):

    W_L1.bin    27 × 64 = 1,728 floats   =  6,912 B  (transposed)
    b_L1.bin    64       = 64 floats     =    256 B
    W_L2.bin    64 × 32 = 2,048 floats   =  8,192 B  (transposed)
    b_L2.bin    32       = 32 floats     =    128 B
    W_L3.bin    32 × 16 =   512 floats   =  2,048 B  (transposed)
    b_L3.bin    16       = 16 floats     =     64 B
    W_OUT.bin   16 × 1  =    16 floats   =     64 B  (transposed)
    b_OUT.bin   1        = 1 float       =      4 B
    input_synth.bin       27 floats      =    108 B
    expected_score.bin    1 float        =      4 B
    pipeline_sentinels.bin  9 (cell,bits) pairs = 72 B

Total weight bytes: 17,668 (~17 KB), matches `PAYLOAD_LEN_V1` minus
the 8-byte header in `runtime/src/mm/eviction/runtime_mlp.rs`.

The synthetic input is deterministic (no PRNG; linear ramp [-1, 1]
across the 27 features so every accumulation path is exercised).
The launcher's self-check passes if the output element matches the
CPU reference within 1e-4 absolute error; ULP-divergence between the
kernel's FMA-style accumulation and Python's straight-line
product-sum is expected at this magnitude.
"""

import argparse
import os
import re
import struct
import sys

# Layer shapes match the L*_IN / L*_OUT constants in
# runtime/src/mm/eviction/runtime_mlp.rs (L1_IN=27, L1_OUT=64,
# L2_OUT=32, L3_OUT=16, OUT=1).
LAYERS = [
    # (W_name, bias_name, OUT,  IN)
    ("W_L1",  "B_L1",     64,  27),
    ("W_L2",  "B_L2",     32,  64),
    ("W_L3",  "B_L3",     16,  32),
    ("W_OUT", "B_OUT",     1,  16),
]


# Decimal float literal: optional sign, digits, dot, digits, optional
# exponent. Matches everything in mlp_policy_f32.rs we care about; the
# Rust source does NOT mix in hex floats (unlike sched's ai_weights_mlp.c).
# `0.0` and integer-looking values like `0` are also allowed in case the
# importer ever quantises a cell to exactly zero.
_FLOAT_RE = re.compile(r"-?\d+\.\d+(?:[eE][+-]?\d+)?|-?\d+(?:[eE][+-]?\d+)?")


def parse_array(text, name):
    """
    Find `pub const NAME: [...] = [ ... ];` in `text`, parse every
    decimal float literal between the outermost brackets, and return
    the flat list of Python floats (row-major for 2D arrays — i.e.
    the same order the Rust compiler stores them). Length is checked
    by the caller against the declared size.
    """
    # Anchor on `pub const NAME:` to avoid matching shorter names that
    # might appear as substrings (e.g. `W_L1` inside `W_L10` if such a
    # thing existed). The trailing `;` closes the declaration.
    pattern = re.compile(
        r"pub\s+const\s+" + re.escape(name)
        + r"\s*:\s*\[[^=]+=\s*(\[.*?\]);",
        re.DOTALL,
    )
    m = pattern.search(text)
    if not m:
        sys.exit(f"could not find `pub const {name}` in source")

    body = m.group(1)
    tokens = _FLOAT_RE.findall(body)
    return [float(tok) for tok in tokens]


def transpose_weight(flat, out_dim, in_dim):
    """
    Re-order a row-major [out_dim][in_dim] sequence into row-major
    [in_dim][out_dim]. Stdlib-only (no numpy) to match the host's
    minimum Python environment.
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
    so the bit pattern is informational. Mirrors the same helper in
    sched-extract-weights.py.
    """
    for i, v in enumerate(vec):
        bits = struct.unpack("<I", struct.pack("<f", float(v)))[0]
        if bits != 0:
            return (i, bits)
    return (0, 0)


def _sigmoid(x):
    # Match runtime/src/mm/eviction/runtime_mlp.rs:144 verbatim:
    #     1.0 / (1.0 + libm::expf(-out))
    # Python's math.exp on a float matches glibc expf to within ~1 ULP
    # at fp32 precision, comfortably inside the 1e-4 launcher tolerance.
    import math
    return 1.0 / (1.0 + math.exp(-x))


def cpu_forward(weights, x):
    """
    Numpy-free reference forward pass. Matches `predict` in
    `runtime/src/mm/eviction/runtime_mlp.rs`: `out = bias + W·in`,
    ReLU on layers 0..N-2, sigmoid on the final layer. Used to
    generate `expected_score.bin` so the launcher can self-check
    against a fixed answer.
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
        else:
            nxt = [_sigmoid(v) for v in nxt]
        cur = nxt
    return cur


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source",
                    help="path to mlp_policy_f32.rs")
    ap.add_argument("out_dir",
                    help="output directory for binaries")
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
        write_floats(os.path.join(args.out_dir, f"{w_name}.bin"), w_t)
        write_floats(os.path.join(args.out_dir, f"{b_name}.bin"), b_flat)
        print(f"  L{li}: {w_name}.bin  ({in_dim}×{out_dim} = {len(w_t)} floats)"
              f"  {b_name}.bin  ({len(b_flat)} floats)")

        # Keep the *original* (non-transposed) W for the CPU reference
        # so the math directly matches `predict` in runtime_mlp.rs.
        # The transpose is only to feed gemm_fp32.
        layers_for_cpu.append((w_flat, b_flat, out_dim, in_dim))

    # Synthetic input: deterministic 27-element vector. Linear ramp
    # `(i / 130.0) - 0.1` spans roughly [-0.1, 0.1] across the 27
    # features — small enough that the network's strongly-trained
    # weights (some rows of W_L1 carry coefficients around ±6) don't
    # push the L1 hidden activations into ReLU saturation, which in
    # turn keeps the final sigmoid output away from the [0, 1]
    # asymptotes. Mid-range output is a more discriminating
    # GPU/CPU agreement test than a saturated one, since both paths
    # round to 1.0 at saturation and would silently agree on any
    # mid-pipeline divergence the sigmoid swamps out.
    in_vec = [(i / 130.0) - 0.1 for i in range(27)]
    write_floats(os.path.join(args.out_dir, "input_synth.bin"), in_vec)
    print(f"  input_synth.bin     ({len(in_vec)} floats, deterministic)")

    expected = cpu_forward(layers_for_cpu, in_vec)
    write_floats(os.path.join(args.out_dir, "expected_score.bin"), expected)
    print(f"  expected_score.bin  ({len(expected)} float, CPU reference)")
    print(f"  expected_score      = {expected[0]:.9f}")

    # Per-op sentinels: (cell_idx, fp32_bits) for each of the 9 pipeline
    # ops. Format matches MNIST's `pipeline_sentinels.bin` / sched's
    # equivalent so the launcher (gpu-kernel-eviction-qnet.c) can read
    # it the same way.
    #
    # The 9-op pipeline is gemm + add_bias_relu × 3, gemm + add_bias
    # (no relu), then sigmoid:
    #   op 0: gemm(L1)           out[64]   pre-bias
    #   op 1: add_bias_relu(L1)  out[64]   bias + ReLU
    #   op 2: gemm(L2)           out[32]   pre-bias
    #   op 3: add_bias_relu(L2)  out[32]   bias + ReLU
    #   op 4: gemm(L3)           out[16]   pre-bias
    #   op 5: add_bias_relu(L3)  out[16]   bias + ReLU
    #   op 6: gemm(OUT)          out[1]    pre-bias
    #   op 7: add_bias(OUT)      out[1]    bias only, no ReLU
    #   op 8: sigmoid(OUT)       out[1]    final eviction score in [0, 1]
    #
    # IMPORTANT split with how `predict` is structured: the Rust code
    # does fused matvec+bias in one step, but the GPU pipeline splits
    # gemm and add_bias_relu (and the trailing sigmoid) into separate
    # ops. So the gemm output here is (W·x) without bias; the
    # add_bias_relu output adds bias and applies ReLU on hidden
    # layers; the add_bias-only output adds the OUT bias without
    # ReLU; sigmoid produces the final score.
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

        # add_bias[_relu] op: output[i] = gemm[i] + b[i]; ReLU on
        # hidden layers (0..N-2), no activation on the OUT layer.
        post = [gemm_out[i] + b[i] for i in range(out_dim)]
        if li < len(layers_for_cpu) - 1:
            post = [v if v > 0.0 else 0.0 for v in post]
        op_sentinels.append(_first_nonzero_sentinel(post))

        cur = post

    # Final sigmoid op operates on the (single-cell) post-add output.
    sig_out = [_sigmoid(v) for v in cur]
    op_sentinels.append(_first_nonzero_sentinel(sig_out))

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
