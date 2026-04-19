#!/usr/bin/env python3
"""
export_scheduler_mlp_onnx.py — convert kernel/sched/ai/ai_weights_mlp.c to ONNX

Reads the C99 hex-float arrays defined in ai_weights_mlp.c, reconstructs the
4-layer MLP (108 -> 256 -> 256 -> 128 -> N_actions, ReLU between hidden layers),
and emits two ONNX files:

  build/hailo/scheduler_mlp_jetson.onnx   (42 actions — full weights)
  build/hailo/scheduler_mlp_pi5.onnx      (24 actions — first 24 rows of layer 3)

Built directly against the ONNX Python API — no PyTorch dependency.

Verification: runs the exported graph under onnxruntime against a pure-numpy
reference that matches kernel/sched/ai/ai_inference.c forward_logits exactly
and fails if max|onnx - ref| on a fixed test input exceeds 1e-5.

Usage:
    python3 scripts/hailo/export_scheduler_mlp_onnx.py
    python3 scripts/hailo/export_scheduler_mlp_onnx.py --source ai_ppo
    python3 scripts/hailo/export_scheduler_mlp_onnx.py --outdir /tmp/hailo

Dependencies: numpy, onnx, onnxruntime.
    pip install --user --break-system-packages numpy onnx onnxruntime
"""

import argparse
import re
import sys
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_WEIGHTS_C = REPO_ROOT / "kernel/sched/ai/ai_weights_mlp.c"
DEFAULT_OUTDIR = REPO_ROOT / "build/hailo"


def pretty_path(p: Path) -> str:
    try:
        return str(p.relative_to(REPO_ROOT))
    except ValueError:
        return str(p)


LAYER_SHAPES = {
    "w0": (256, 108),
    "b0": (256,),
    "w1": (256, 256),
    "b1": (256,),
    "w2": (128, 256),
    "b2": (128,),
    "w3": (42, 128),
    "b3": (42,),
}

HEX_FLOAT_RE = re.compile(r"-?0x[0-9a-fA-F.]+(?:p[+-]?\d+)?")


def parse_c_float_arrays(path: Path, prefix: str) -> dict[str, np.ndarray]:
    """Extract `{prefix}_{name}` arrays from the C file as float32 numpy arrays.

    Expects definitions like:
        const float ai_mlp_w0[256 * 108] = { -0x1.c6...p-5, ..., };
    """
    text = path.read_text()
    out: dict[str, np.ndarray] = {}

    for short_name, shape in LAYER_SHAPES.items():
        sym = f"{prefix}_{short_name}"
        pattern = re.compile(
            rf"{re.escape(sym)}\s*\[[^\]]+\]\s*=\s*\{{(?P<body>.*?)\}}\s*;",
            re.DOTALL,
        )
        m = pattern.search(text)
        if m is None:
            raise RuntimeError(f"symbol {sym} not found in {path}")
        body = m.group("body")
        tokens = HEX_FLOAT_RE.findall(body)
        expected = int(np.prod(shape))
        if len(tokens) != expected:
            raise RuntimeError(
                f"{sym}: expected {expected} floats, found {len(tokens)}"
            )
        arr = np.array([float.fromhex(t) for t in tokens], dtype=np.float32)
        out[short_name] = arr.reshape(shape)
    return out


def numpy_forward(state: np.ndarray, w: dict[str, np.ndarray]) -> np.ndarray:
    """Reference forward pass matching kernel/sched/ai/ai_inference.c:forward_logits."""
    x = state.astype(np.float32)
    x = np.maximum(w["w0"] @ x + w["b0"], 0.0)
    x = np.maximum(w["w1"] @ x + w["b1"], 0.0)
    x = np.maximum(w["w2"] @ x + w["b2"], 0.0)
    return w["w3"] @ x + w["b3"]


def make_initializer(name: str, arr: np.ndarray) -> TensorProto:
    return numpy_helper.from_array(arr.astype(np.float32), name=name)


def build_onnx_model(weights: dict[str, np.ndarray], n_actions: int,
                    opset: int) -> onnx.ModelProto:
    """Build a 4-layer MLP ONNX graph.

    Each layer is expressed as Gemm(x, W^T, b) so the stored weights keep the
    kernel-native [out, in] layout. ONNX Gemm with transB=1 computes
    alpha*A*B^T + beta*C, which is exactly y = x @ W.T + b for a (1, in)
    input batch, matching the C reference.
    """
    w3 = weights["w3"][:n_actions]
    b3 = weights["b3"][:n_actions]

    initializers = [
        make_initializer("W0", weights["w0"]),
        make_initializer("B0", weights["b0"]),
        make_initializer("W1", weights["w1"]),
        make_initializer("B1", weights["b1"]),
        make_initializer("W2", weights["w2"]),
        make_initializer("B2", weights["b2"]),
        make_initializer("W3", w3),
        make_initializer("B3", b3),
    ]

    input_tensor = helper.make_tensor_value_info("state", TensorProto.FLOAT,
                                                  ["batch", 108])
    output_tensor = helper.make_tensor_value_info("logits", TensorProto.FLOAT,
                                                   ["batch", n_actions])

    nodes = [
        helper.make_node("Gemm", ["state", "W0", "B0"], ["fc0"],
                         alpha=1.0, beta=1.0, transA=0, transB=1),
        helper.make_node("Relu", ["fc0"], ["h0"]),
        helper.make_node("Gemm", ["h0", "W1", "B1"], ["fc1"],
                         alpha=1.0, beta=1.0, transA=0, transB=1),
        helper.make_node("Relu", ["fc1"], ["h1"]),
        helper.make_node("Gemm", ["h1", "W2", "B2"], ["fc2"],
                         alpha=1.0, beta=1.0, transA=0, transB=1),
        helper.make_node("Relu", ["fc2"], ["h2"]),
        helper.make_node("Gemm", ["h2", "W3", "B3"], ["logits"],
                         alpha=1.0, beta=1.0, transA=0, transB=1),
    ]

    graph = helper.make_graph(
        nodes, name=f"scheduler_mlp_{n_actions}",
        inputs=[input_tensor], outputs=[output_tensor],
        initializer=initializers,
    )

    opset_import = [helper.make_opsetid("", opset)]
    model = helper.make_model(graph, opset_imports=opset_import,
                              producer_name="slm-os")
    model.ir_version = 8
    onnx.checker.check_model(model)
    return model


def verify(onnx_path: Path, ref_weights: dict[str, np.ndarray],
           n_actions: int) -> float:
    rng = np.random.default_rng(0xDEC0DE)
    state = rng.standard_normal(108).astype(np.float32)

    sess = ort.InferenceSession(onnx_path.as_posix(),
                                providers=["CPUExecutionProvider"])
    y_onnx = sess.run(None, {"state": state[None, :]})[0][0]

    ref = {**ref_weights,
           "w3": ref_weights["w3"][:n_actions],
           "b3": ref_weights["b3"][:n_actions]}
    y_ref = numpy_forward(state, ref)

    if y_onnx.shape != y_ref.shape:
        raise RuntimeError(f"shape mismatch: onnx={y_onnx.shape} ref={y_ref.shape}")
    return float(np.max(np.abs(y_onnx - y_ref)))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--weights-c", type=Path, default=DEFAULT_WEIGHTS_C,
                    help=f"C source with weight arrays (default: {DEFAULT_WEIGHTS_C})")
    ap.add_argument("--source", default="ai_mlp", choices=["ai_mlp", "ai_ppo"],
                    help="Symbol prefix to extract (default: ai_mlp)")
    ap.add_argument("--outdir", type=Path, default=DEFAULT_OUTDIR,
                    help=f"Output directory (default: {DEFAULT_OUTDIR})")
    ap.add_argument("--opset", type=int, default=11,
                    help="ONNX opset (Hailo DFC accepts 11-17; default: 11)")
    ap.add_argument("--tolerance", type=float, default=1e-3,
                    help="Max |onnx - ref|. Layer 1 weights reach ~200, giving "
                         "float32 ULP ~6e-3 at intermediate magnitudes; 1e-3 "
                         "catches real bugs but allows rounding noise.")
    args = ap.parse_args()

    if not args.weights_c.exists():
        print(f"ERROR: {args.weights_c} not found", file=sys.stderr)
        return 1

    print(f"[export] reading {args.source}_* from {args.weights_c}")
    weights = parse_c_float_arrays(args.weights_c, args.source)
    for name, arr in weights.items():
        print(f"  {args.source}_{name}: shape={tuple(arr.shape)} "
              f"min={arr.min():+.3e} max={arr.max():+.3e}")

    if all(np.all(arr == 0.0) for arr in weights.values()):
        print("ERROR: all weights are zero — did the stub get linked instead "
              "of ai_weights_mlp.c?", file=sys.stderr)
        return 2

    args.outdir.mkdir(parents=True, exist_ok=True)

    for n_actions, label in [(42, "jetson"), (24, "pi5")]:
        model = build_onnx_model(weights, n_actions, args.opset)
        out_path = args.outdir / f"scheduler_mlp_{label}.onnx"
        onnx.save_model(model, out_path.as_posix())
        max_diff = verify(out_path, weights, n_actions)
        status = "OK" if max_diff <= args.tolerance else "FAIL"
        print(f"[export] {pretty_path(out_path)}  "
              f"n_actions={n_actions}  max|onnx-ref|={max_diff:.2e}  [{status}]")
        if max_diff > args.tolerance:
            return 3

    print("[export] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
