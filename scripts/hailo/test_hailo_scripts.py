#!/usr/bin/env python3
"""
test_hailo_scripts.py — functional tests for scripts/hailo/ tooling

Covers:
  - export_scheduler_mlp_onnx.py: hex-float parser, numpy reference matches
    hand-computed values, ONNX model validates, full script end-to-end
  - generate_calibration_data.py: shape/dtype/range/zero-fill/determinism
  - compile_hef.sh: argument validation error paths (bash; does not need DFC)

No pytest dependency — run directly:
    python3 scripts/hailo/test_hailo_scripts.py

Returns exit 0 on all-pass, 1 on first failure.
"""
from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
HAILO_DIR = REPO_ROOT / "scripts/hailo"


def _import(name: str):
    """Import a sibling script as a module (filename has hyphens OK since we dotted)."""
    path = HAILO_DIR / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader, f"failed to load {path}"
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class TestRunner:
    def __init__(self):
        self.passes = 0
        self.fails: list[str] = []

    def run(self, name: str, fn):
        try:
            fn()
            print(f"  PASS  {name}")
            self.passes += 1
        except AssertionError as e:
            print(f"  FAIL  {name}: {e}")
            self.fails.append(name)
        except Exception as e:
            print(f"  ERROR {name}: {type(e).__name__}: {e}")
            self.fails.append(name)

    def summary(self) -> int:
        total = self.passes + len(self.fails)
        print(f"\n{self.passes}/{total} passed")
        if self.fails:
            print("Failures:")
            for n in self.fails:
                print(f"  - {n}")
            return 1
        return 0


# ============================================================================
# export_scheduler_mlp_onnx.py
# ============================================================================

def test_hex_float_parser_reads_c99_literals():
    """The regex must extract C99 hex-float literals including exponents + signs."""
    export = _import("export_scheduler_mlp_onnx")

    # A tiny synthetic C file that mimics ai_weights_mlp.c structure.
    # Use tiny shapes by temporarily overriding LAYER_SHAPES, then restoring.
    synthetic = """
    #include "x.h"
    const float ai_mlp_w0[2 * 3] = {
        0x1.0p0, -0x1.8p-1, 0x1.0p-2,
        0x1.0p1, 0x0.0p0, -0x1.0p0,
    };
    const float ai_mlp_b0[2] = { 0x1.0p0, -0x1.0p0 };
    """

    original_shapes = export.LAYER_SHAPES
    try:
        export.LAYER_SHAPES = {"w0": (2, 3), "b0": (2,)}
        with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
            f.write(synthetic)
            path = Path(f.name)
        try:
            parsed = export.parse_c_float_arrays(path, "ai_mlp")
        finally:
            path.unlink()
    finally:
        export.LAYER_SHAPES = original_shapes

    assert parsed["w0"].shape == (2, 3), parsed["w0"].shape
    assert parsed["b0"].shape == (2,), parsed["b0"].shape
    np.testing.assert_array_equal(
        parsed["w0"],
        np.array([[1.0, -0.75, 0.25], [2.0, 0.0, -1.0]], dtype=np.float32),
    )
    np.testing.assert_array_equal(
        parsed["b0"], np.array([1.0, -1.0], dtype=np.float32),
    )


def test_hex_float_parser_raises_on_missing_symbol():
    export = _import("export_scheduler_mlp_onnx")
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write("const float nothing[1] = { 0x1.0p0 };")
        path = Path(f.name)
    try:
        original = export.LAYER_SHAPES
        export.LAYER_SHAPES = {"w0": (1,)}
        try:
            export.parse_c_float_arrays(path, "ai_mlp")
        except RuntimeError as e:
            assert "ai_mlp_w0" in str(e), e
            return
        finally:
            export.LAYER_SHAPES = original
        assert False, "expected RuntimeError"
    finally:
        path.unlink()


def test_numpy_forward_matches_hand_computation():
    """Hand-compute a 2-input 1-output network; numpy_forward must match."""
    export = _import("export_scheduler_mlp_onnx")

    # Tiny network: 2 -> 1 (no hidden layers — degenerate, but exercises the
    # four-layer chain with identity-like weights).
    #
    # Input [1, 0]. Choose all intermediate weights to pass it through:
    # w0 = identity pad-up (2 -> 256), but we'd need real shapes. Instead,
    # build a single-example with known shapes and verify a specific output.
    state = np.zeros(108, dtype=np.float32)
    state[0] = 1.0
    state[107] = -0.5

    # Identity-ish weights: each layer's W is an identity of its output dim
    # over the first few inputs, zero elsewhere; bias = 0. The network then
    # passes state[0] through to out[0] (clamped to ≥0 by ReLU).
    weights = {
        "w0": np.eye(256, 108, dtype=np.float32),
        "b0": np.zeros(256, dtype=np.float32),
        "w1": np.eye(256, 256, dtype=np.float32),
        "b1": np.zeros(256, dtype=np.float32),
        "w2": np.eye(128, 256, dtype=np.float32),
        "b2": np.zeros(128, dtype=np.float32),
        "w3": np.eye(42, 128, dtype=np.float32),
        "b3": np.zeros(42, dtype=np.float32),
    }
    out = export.numpy_forward(state, weights)
    assert out.shape == (42,), out.shape
    # Propagation trace with identity weights + zero biases:
    #   layer 0 out[i] = state[i] for i<108 (eye(256,108) pads upper 148 with 0)
    #   ReLU: state[0]=1.0 kept, state[107]=-0.5 clamped to 0
    #   layers 1,2 are identities that truncate to 256 then 128 dims
    #   layer 3 is eye(42,128): keeps rows 0..41, drops 42..127
    # So out[0] = state[0] = 1.0; all other out[i] are 0 because either the
    # ReLU zeroed them (state[107]) or layer 3 dropped them (rows 42..127).
    assert abs(out[0] - 1.0) < 1e-6, out[0]
    for i in range(1, 42):
        assert abs(out[i]) < 1e-6, f"out[{i}]={out[i]}"


def test_onnx_model_validates_and_matches_reference():
    """End-to-end: build model with random weights, verify ONNX runtime agrees with numpy."""
    export = _import("export_scheduler_mlp_onnx")

    rng = np.random.default_rng(42)
    weights = {
        "w0": rng.standard_normal((256, 108)).astype(np.float32) * 0.1,
        "b0": rng.standard_normal(256).astype(np.float32) * 0.1,
        "w1": rng.standard_normal((256, 256)).astype(np.float32) * 0.1,
        "b1": rng.standard_normal(256).astype(np.float32) * 0.1,
        "w2": rng.standard_normal((128, 256)).astype(np.float32) * 0.1,
        "b2": rng.standard_normal(128).astype(np.float32) * 0.1,
        "w3": rng.standard_normal((42, 128)).astype(np.float32) * 0.1,
        "b3": rng.standard_normal(42).astype(np.float32) * 0.1,
    }

    model = export.build_onnx_model(weights, n_actions=24, opset=11)
    # onnx.checker is called inside build_onnx_model; if it returned, it passed.
    assert len(model.graph.initializer) == 8
    assert [n.op_type for n in model.graph.node] == [
        "Gemm", "Relu", "Gemm", "Relu", "Gemm", "Relu", "Gemm"
    ]

    with tempfile.NamedTemporaryFile(suffix=".onnx", delete=False) as f:
        path = Path(f.name)
    try:
        import onnx
        onnx.save_model(model, path.as_posix())
        max_diff = export.verify(path, weights, n_actions=24)
        assert max_diff < 1e-5, f"max diff {max_diff:.2e} exceeds 1e-5"
    finally:
        path.unlink()


def test_build_onnx_model_rejects_over_sized_n_actions():
    """Slicing w3[:n_actions] silently truncates; the model builder must catch it."""
    export = _import("export_scheduler_mlp_onnx")

    weights = {
        "w0": np.zeros((256, 108), dtype=np.float32),
        "b0": np.zeros(256, dtype=np.float32),
        "w1": np.zeros((256, 256), dtype=np.float32),
        "b1": np.zeros(256, dtype=np.float32),
        "w2": np.zeros((128, 256), dtype=np.float32),
        "b2": np.zeros(128, dtype=np.float32),
        "w3": np.zeros((42, 128), dtype=np.float32),
        "b3": np.zeros(42, dtype=np.float32),
    }

    try:
        export.build_onnx_model(weights, n_actions=50, opset=11)
    except ValueError as e:
        assert "n_actions=50" in str(e), e
        assert "42" in str(e), e
        return
    assert False, "expected ValueError for n_actions > layer-3 row count"


def test_export_script_runs_on_ai_ppo_symbols():
    """PPO weights file has the same shape — script should export against it too."""
    ppo_c = REPO_ROOT / "kernel/sched/ai/ai_weights_ppo.c"
    if not ppo_c.exists():
        # Not strictly required; skip rather than fail.
        print("    (skipping — ai_weights_ppo.c not present)")
        return

    outdir = Path(tempfile.mkdtemp(prefix="hailo-test-ppo-"))
    try:
        result = subprocess.run(
            [sys.executable, str(HAILO_DIR / "export_scheduler_mlp_onnx.py"),
             "--source", "ai_ppo", "--outdir", str(outdir)],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, (
            f"exit {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
        for name in ("scheduler_mlp_pi5.onnx", "scheduler_mlp_jetson.onnx"):
            p = outdir / name
            assert p.exists(), f"{name} missing"
    finally:
        import shutil
        shutil.rmtree(outdir, ignore_errors=True)


def test_end_to_end_export_on_real_weights():
    """Run the real script against the checked-in C weights; check outputs exist + non-empty."""
    outdir = Path(tempfile.mkdtemp(prefix="hailo-test-"))
    try:
        result = subprocess.run(
            [sys.executable, str(HAILO_DIR / "export_scheduler_mlp_onnx.py"),
             "--outdir", str(outdir)],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, (
            f"exit {result.returncode}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
        for name in ("scheduler_mlp_pi5.onnx", "scheduler_mlp_jetson.onnx"):
            p = outdir / name
            assert p.exists(), f"{name} missing"
            assert p.stat().st_size > 100_000, f"{name} suspiciously small"
    finally:
        import shutil
        shutil.rmtree(outdir, ignore_errors=True)


# ============================================================================
# generate_calibration_data.py
# ============================================================================

def test_calibration_shape_and_dtype():
    calib = _import("generate_calibration_data")
    data = calib.generate(n_samples=64, seed=12345)
    assert data.shape == (64, 108)
    assert data.dtype == np.float32


def test_calibration_values_in_zero_one():
    calib = _import("generate_calibration_data")
    data = calib.generate(n_samples=128, seed=98765)
    # All features must be in [0, 1] per the kernel's clamp01 / normalization.
    # "Wait time" (task feature index 6) is the one exception — it's normalized
    # by /1e9 but not clamped — so we allow it a slightly loose upper bound.
    assert data.min() >= 0.0, f"min={data.min()}"
    assert data.max() <= 1.0, f"max={data.max()}"


def test_calibration_deterministic_under_seed():
    calib = _import("generate_calibration_data")
    a = calib.generate(n_samples=32, seed=0xDEADBEEF)
    b = calib.generate(n_samples=32, seed=0xDEADBEEF)
    np.testing.assert_array_equal(a, b)


def test_calibration_zero_fills_inactive_cores():
    """4-core samples (even indices) must have cores 4-5 zero-filled."""
    calib = _import("generate_calibration_data")
    data = calib.generate(n_samples=10, seed=1)
    for i in range(0, 10, 2):  # 4-core shape
        # Cores 4 and 5 are offsets 24..30 and 30..36 in the state vector.
        assert np.all(data[i, 24:36] == 0.0), (
            f"sample {i} cores 4-5 not zero: {data[i, 24:36]}"
        )


def test_calibration_cores_4_pins_every_sample():
    """--cores 4 must zero-fill cores 4-5 on ALL samples, not alternating."""
    calib = _import("generate_calibration_data")
    data = calib.generate(n_samples=16, seed=2, cores="4")
    for i in range(16):
        assert np.all(data[i, 24:36] == 0.0), (
            f"sample {i} cores 4-5 not zero under --cores 4: {data[i, 24:36]}"
        )


def test_calibration_cores_6_populates_every_sample():
    """--cores 6 should populate per-core features for all 6 cores in every sample."""
    calib = _import("generate_calibration_data")
    data = calib.generate(n_samples=32, seed=3, cores="6")
    # core_type (feature index 3) is 1.0 for every active core. Core 5's
    # core_type is at offset 5*6+3 = 33. If it's 1.0 on every sample, core 5
    # is active — proving --cores 6 populates all 6 cores (not just 4).
    for i in range(32):
        assert data[i, 5 * 6 + 3] == 1.0, (
            f"sample {i} core 5 core_type=0 under --cores 6; "
            f"core block: {data[i, :36].reshape(6, 6)}"
        )


def test_calibration_script_end_to_end():
    outdir = Path(tempfile.mkdtemp(prefix="hailo-test-calib-"))
    try:
        result = subprocess.run(
            [sys.executable, str(HAILO_DIR / "generate_calibration_data.py"),
             "--samples", "32", "--outdir", str(outdir)],
            capture_output=True, text=True,
        )
        assert result.returncode == 0, result.stderr
        out = outdir / "calibration_states.npy"
        assert out.exists()
        data = np.load(out)
        assert data.shape == (32, 108)
        assert data.dtype == np.float32
    finally:
        import shutil
        shutil.rmtree(outdir, ignore_errors=True)


# ============================================================================
# compile_hef.sh
# ============================================================================

def test_compile_script_rejects_bad_arch():
    result = subprocess.run(
        ["bash", str(HAILO_DIR / "compile_hef.sh"), "--arch", "hailo9"],
        capture_output=True, text=True,
    )
    assert result.returncode == 2, f"exit {result.returncode}"
    assert "hailo8 or hailo8l" in result.stderr, result.stderr


def test_compile_script_rejects_bad_variant():
    result = subprocess.run(
        ["bash", str(HAILO_DIR / "compile_hef.sh"), "--variant", "nvidia"],
        capture_output=True, text=True,
    )
    assert result.returncode == 2, f"exit {result.returncode}"
    assert "pi5 or jetson" in result.stderr, result.stderr


def test_compile_script_errors_when_hailo_cli_missing():
    """When `hailo` isn't on PATH, the script must exit 3 with a clear message."""
    import os
    env = os.environ.copy()
    # Strip venv bin dirs from PATH so `hailo` is not found.
    stripped = ":".join(p for p in env.get("PATH", "").split(":")
                        if "venv" not in p and "hailo" not in p.lower())
    env["PATH"] = stripped or "/usr/bin:/bin"
    result = subprocess.run(
        ["bash", str(HAILO_DIR / "compile_hef.sh")],
        capture_output=True, text=True, env=env,
    )
    assert result.returncode == 3, f"exit {result.returncode}\nstderr:\n{result.stderr}"
    assert "'hailo' CLI not on PATH" in result.stderr, result.stderr


def test_compile_script_errors_when_onnx_missing():
    """If --outdir points at an empty dir, script must complain about missing ONNX."""
    empty = Path(tempfile.mkdtemp(prefix="hailo-empty-"))
    try:
        import os
        env = os.environ.copy()
        # Create a fake `hailo` on PATH so we get past the CLI check.
        fake_bin = Path(tempfile.mkdtemp(prefix="hailo-fake-bin-"))
        (fake_bin / "hailo").write_text("#!/bin/sh\nexit 0\n")
        (fake_bin / "hailo").chmod(0o755)
        env["PATH"] = f"{fake_bin}:{env.get('PATH', '')}"
        try:
            result = subprocess.run(
                ["bash", str(HAILO_DIR / "compile_hef.sh"),
                 "--outdir", str(empty)],
                capture_output=True, text=True, env=env,
            )
            assert result.returncode == 4, (
                f"exit {result.returncode}\nstderr:\n{result.stderr}"
            )
            assert ".onnx not found" in result.stderr, result.stderr
        finally:
            import shutil
            shutil.rmtree(fake_bin, ignore_errors=True)
    finally:
        import shutil
        shutil.rmtree(empty, ignore_errors=True)


# ============================================================================
# Entry point
# ============================================================================

def main() -> int:
    runner = TestRunner()

    print("== export_scheduler_mlp_onnx.py ==")
    runner.run("hex_float_parser_reads_c99_literals", test_hex_float_parser_reads_c99_literals)
    runner.run("hex_float_parser_raises_on_missing_symbol", test_hex_float_parser_raises_on_missing_symbol)
    runner.run("numpy_forward_matches_hand_computation", test_numpy_forward_matches_hand_computation)
    runner.run("onnx_model_validates_and_matches_reference", test_onnx_model_validates_and_matches_reference)
    runner.run("build_onnx_model_rejects_over_sized_n_actions", test_build_onnx_model_rejects_over_sized_n_actions)
    runner.run("export_script_runs_on_ai_ppo_symbols", test_export_script_runs_on_ai_ppo_symbols)
    runner.run("end_to_end_export_on_real_weights", test_end_to_end_export_on_real_weights)

    print("\n== generate_calibration_data.py ==")
    runner.run("calibration_shape_and_dtype", test_calibration_shape_and_dtype)
    runner.run("calibration_values_in_zero_one", test_calibration_values_in_zero_one)
    runner.run("calibration_deterministic_under_seed", test_calibration_deterministic_under_seed)
    runner.run("calibration_zero_fills_inactive_cores", test_calibration_zero_fills_inactive_cores)
    runner.run("calibration_cores_4_pins_every_sample", test_calibration_cores_4_pins_every_sample)
    runner.run("calibration_cores_6_populates_every_sample", test_calibration_cores_6_populates_every_sample)
    runner.run("calibration_script_end_to_end", test_calibration_script_end_to_end)

    print("\n== compile_hef.sh ==")
    runner.run("compile_script_rejects_bad_arch", test_compile_script_rejects_bad_arch)
    runner.run("compile_script_rejects_bad_variant", test_compile_script_rejects_bad_variant)
    runner.run("compile_script_errors_when_hailo_cli_missing", test_compile_script_errors_when_hailo_cli_missing)
    runner.run("compile_script_errors_when_onnx_missing", test_compile_script_errors_when_onnx_missing)

    return runner.summary()


if __name__ == "__main__":
    sys.exit(main())
