#!/usr/bin/env python3
"""
generate_calibration_data.py — synthesize scheduler state vectors for Hailo DFC quantization

Emits build/hailo/calibration_states.npy of shape [N, 108] matching the exact
feature layout produced by kernel/sched/ai/ai_state.c:ai_extract_state:

  [0..35]    6 cores × (util, queue/32, cache_press=0, core_type=1, isolated, prio/7)
  [36..99]   8 tasks × (prio/7, deadline_urg, 4 zero slots, wait_s, 0)
  [100..107] 8 globals (ready/64, miss_rate, avg_lat/1e7, 0, 0, 0, load_imb, 0)

Feature distributions are chosen to match plausible kernel workloads:
- Per-core utilization is drawn from a Beta(2,5) (skewed toward idle)
- Task priorities follow a weighted categorical (most tasks normal, few high/low)
- Deadline urgency is Beta(1,3) (most tasks relaxed)
- The "future" feature slots (cache_pressure, working_set, model_size, etc.) are
  held at the 0.0 values the kernel actually emits today — quantizing with
  realistic zeros matches deployment.
- Core count varies per sample across {4, 6} so DFC sees both Pi 5 (4-core) and
  Jetson (6-core) shapes.

Use --samples to scale sample count (Hailo DFC recommends 32-1024).

Usage:
    python3 scripts/hailo/generate_calibration_data.py
    python3 scripts/hailo/generate_calibration_data.py --samples 256 --seed 42
    python3 scripts/hailo/generate_calibration_data.py --outdir /tmp/hailo

Dependencies: numpy.
"""

import argparse
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTDIR = REPO_ROOT / "build/hailo"


def pretty_path(p: Path) -> str:
    try:
        return str(p.relative_to(REPO_ROOT))
    except ValueError:
        return str(p)

STATE_DIM = 108
NUM_CORES = 6
FEATURES_PER_CORE = 6
NUM_TASKS = 8
FEATURES_PER_TASK = 8
NUM_GLOBAL = 8


def clamp01(x):
    return np.clip(x, 0.0, 1.0)


def sample_core_block(rng: np.random.Generator, active_cores: int) -> np.ndarray:
    """36 floats: 6 cores × 6 features. Inactive cores are zero-filled."""
    block = np.zeros((NUM_CORES, FEATURES_PER_CORE), dtype=np.float32)

    utils = rng.beta(2.0, 5.0, size=active_cores).astype(np.float32)
    queue_depth = rng.poisson(lam=4.0, size=active_cores).astype(np.float32) / 32.0
    isolated_mask = (rng.random(active_cores) < 0.15).astype(np.float32)
    task_prio = (rng.choice([2, 3, 4, 5, 6], size=active_cores,
                            p=[0.1, 0.25, 0.3, 0.25, 0.1]).astype(np.float32)) / 7.0

    block[:active_cores, 0] = utils
    block[:active_cores, 1] = clamp01(queue_depth)
    block[:active_cores, 2] = 0.0                     # cache_pressure — future
    block[:active_cores, 3] = 1.0                     # core_type — homogeneous
    block[:active_cores, 4] = isolated_mask
    block[:active_cores, 5] = task_prio

    return block.reshape(-1)


def sample_task_block(rng: np.random.Generator, active_tasks: int) -> np.ndarray:
    """64 floats: 8 tasks × 8 features. Inactive tasks are zero-filled."""
    block = np.zeros((NUM_TASKS, FEATURES_PER_TASK), dtype=np.float32)

    prio = (rng.choice([1, 2, 3, 4, 5, 6, 7], size=active_tasks,
                       p=[0.05, 0.1, 0.2, 0.3, 0.2, 0.1, 0.05]).astype(np.float32)) / 7.0
    deadline_urg = rng.beta(1.0, 3.0, size=active_tasks).astype(np.float32)
    wait_s = np.abs(rng.normal(loc=0.01, scale=0.05, size=active_tasks)).astype(np.float32)

    block[:active_tasks, 0] = prio
    block[:active_tasks, 1] = deadline_urg
    block[:active_tasks, 2] = 0.0       # working_set — future
    block[:active_tasks, 3] = 0.0       # model_size — future
    block[:active_tasks, 4] = 0.0       # inference_dur — future
    block[:active_tasks, 5] = 0.0       # can_use_gpu — future
    block[:active_tasks, 6] = wait_s
    block[:active_tasks, 7] = 0.0       # component_type — future

    return block.reshape(-1)


def sample_global_block(rng: np.random.Generator, core_block: np.ndarray,
                        active_tasks: int) -> np.ndarray:
    """8 floats. Derived partly from the sampled per-core utilizations."""
    block = np.zeros(NUM_GLOBAL, dtype=np.float32)

    ready_count = rng.poisson(lam=float(active_tasks) * 0.8) + active_tasks
    block[0] = min(1.0, ready_count / 64.0)
    block[1] = float(np.abs(rng.normal(loc=0.05, scale=0.1)))   # miss_rate ~5% typical
    block[1] = clamp01(block[1])
    block[2] = clamp01(np.abs(rng.normal(loc=0.1, scale=0.1)))  # avg_latency / 1e7
    block[3] = 0.0                                              # weight_pool_pressure
    block[4] = 0.0                                              # workspace_pool_pressure
    block[5] = 0.0                                              # gpu_queue_depth

    # load_imbalance: coefficient of variation over active core utils
    core_2d = core_block.reshape(NUM_CORES, FEATURES_PER_CORE)
    utils = core_2d[:, 0]
    active_utils = utils[utils > 0.0] if np.any(utils > 0.0) else utils
    if active_utils.size > 1 and active_utils.mean() > 1e-3:
        block[6] = clamp01(float(active_utils.std() / active_utils.mean()))
    else:
        block[6] = 0.0
    block[7] = 0.0                                              # episode_time

    return block


def _resolve_active_cores(cores: str, i: int) -> int:
    """Map --cores argument + sample index to the active-core count.

    'mixed' alternates 4/6 on even/odd indices to cover both Pi 5 and Jetson
    shapes in a single calibration set. '4' or '6' pin every sample.
    """
    if cores == "mixed":
        return 4 if (i % 2 == 0) else 6
    return int(cores)


def generate(n_samples: int, seed: int, cores: str = "mixed") -> np.ndarray:
    rng = np.random.default_rng(seed)
    samples = np.zeros((n_samples, STATE_DIM), dtype=np.float32)

    for i in range(n_samples):
        active_cores = _resolve_active_cores(cores, i)
        active_tasks = int(rng.integers(low=0, high=NUM_TASKS + 1))

        core_block = sample_core_block(rng, active_cores)
        task_block = sample_task_block(rng, active_tasks)
        global_block = sample_global_block(rng, core_block, active_tasks)

        samples[i, :36] = core_block
        samples[i, 36:100] = task_block
        samples[i, 100:108] = global_block

    return samples


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--samples", type=int, default=256,
                    help="Number of calibration samples (32-1024 typical)")
    ap.add_argument("--seed", type=int, default=0xC0DEC0DE)
    ap.add_argument("--outdir", type=Path, default=DEFAULT_OUTDIR)
    ap.add_argument("--cores", default="mixed", choices=["4", "6", "mixed"],
                    help="Active-core count per sample. '4' = Pi 5 only, "
                         "'6' = Jetson only, 'mixed' alternates 4/6 (default)")
    ap.add_argument("--stats", action="store_true",
                    help="Print per-feature min/mean/max across the dataset")
    args = ap.parse_args()

    if args.samples < 1:
        print("ERROR: --samples must be >= 1", file=sys.stderr)
        return 1

    print(f"[calib] generating {args.samples} samples (seed=0x{args.seed:08x}, "
          f"cores={args.cores})")
    data = generate(args.samples, args.seed, cores=args.cores)
    assert data.shape == (args.samples, STATE_DIM)
    assert data.dtype == np.float32

    args.outdir.mkdir(parents=True, exist_ok=True)
    out_npy = args.outdir / "calibration_states.npy"
    np.save(out_npy, data)
    print(f"[calib] wrote {pretty_path(out_npy)}  "
          f"shape={data.shape}  dtype={data.dtype}  "
          f"size={out_npy.stat().st_size} B")

    if args.stats:
        print("[calib] per-feature min/mean/max:")
        for i in range(STATE_DIM):
            col = data[:, i]
            tag = (f"core[{i//6}].f{i%6}" if i < 36
                   else f"task[{(i-36)//8}].f{(i-36)%8}" if i < 100
                   else f"global.f{i-100}")
            print(f"  [{i:3d}] {tag:20s}  "
                  f"min={col.min():+.3f}  mean={col.mean():+.3f}  max={col.max():+.3f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
