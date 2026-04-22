#!/usr/bin/env python3
"""Generate a calibration dataset for MNIST HEF compilation.

Produces a `.npy` file with shape [N, 1, 28, 28] dtype float32 suitable
for `hailo optimize --calib-set-path`. Values are normalised to [0, 1]
matching the convention the CNTK-exported `models/test/mnist.onnx`
expects (CNTK MNIST pipelines divide raw pixel intensity by 255).

Prefers real MNIST test samples downloaded from the canonical CDN.
Falls back to synthetic MNIST-like samples if the download is
unavailable (pixel background mostly zero, sparse strokes at ~1.0).
Either source produces a calibration set that's accurate enough for
int8 quantization of this small CNN.
"""

from __future__ import annotations

import argparse
import gzip
import io
import os
import sys
import urllib.error
import urllib.request

import numpy as np

# Ossci mirror of yann.lecun.com; the original is intermittently down.
MNIST_TEST_URL = (
    "https://ossci-datasets.s3.amazonaws.com/mnist/"
    "t10k-images-idx3-ubyte.gz"
)


def download_mnist_test(timeout_s: float = 30.0) -> np.ndarray | None:
    """Fetch the MNIST test images (10000 × 28 × 28, uint8).

    Returns None on any network failure so the caller can fall back
    to synthetic data without a hard dependency on internet access.
    """
    try:
        with urllib.request.urlopen(MNIST_TEST_URL, timeout=timeout_s) as r:
            raw = r.read()
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as e:
        print(f"[calib] MNIST download failed ({e}), using synthetic", file=sys.stderr)
        return None

    with gzip.GzipFile(fileobj=io.BytesIO(raw)) as gz:
        data = gz.read()

    # IDX file format: magic(4) + num(4) + rows(4) + cols(4) + data
    magic = int.from_bytes(data[:4], "big")
    if magic != 2051:
        print(f"[calib] unexpected IDX magic 0x{magic:08x}", file=sys.stderr)
        return None
    num  = int.from_bytes(data[4:8],   "big")
    rows = int.from_bytes(data[8:12],  "big")
    cols = int.from_bytes(data[12:16], "big")
    return np.frombuffer(data[16:], dtype=np.uint8).reshape(num, rows, cols)


def synthetic_mnist(n_samples: int, seed: int = 0) -> np.ndarray:
    """Generate synthetic MNIST-like samples when the real set is unavailable.

    Backgrounds are mostly zero; each sample has a handful of "stroke"
    pixels at high intensity. This isn't a real digit but matches the
    statistical profile quantization calibration needs: per-channel
    scales dominated by sparse high values + a long tail of near-zeros.
    """
    rng = np.random.default_rng(seed)
    samples = np.zeros((n_samples, 28, 28), dtype=np.uint8)
    for i in range(n_samples):
        # Pick ~60 "stroke" pixels out of 784.
        stroke_count = int(rng.integers(40, 120))
        idx = rng.choice(28 * 28, size=stroke_count, replace=False)
        intensity = rng.integers(100, 256, size=stroke_count)
        samples[i].flat[idx] = intensity
    return samples


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--output", required=True,
                    help="Output .npy path (e.g. build/hailo/mnist_calib.npy)")
    ap.add_argument("--samples", type=int, default=128,
                    help="Number of calibration samples (default: 128)")
    ap.add_argument("--synthetic", action="store_true",
                    help="Skip download and use synthetic samples")
    ap.add_argument("--seed", type=int, default=0,
                    help="RNG seed for synthetic generation")
    args = ap.parse_args()

    images: np.ndarray | None
    if args.synthetic:
        images = synthetic_mnist(args.samples, args.seed)
        source = "synthetic"
    else:
        images = download_mnist_test()
        if images is None:
            images = synthetic_mnist(args.samples, args.seed)
            source = "synthetic (download failed)"
        else:
            # Pick `args.samples` indices deterministically.
            rng = np.random.default_rng(args.seed)
            idx = rng.choice(images.shape[0], size=args.samples, replace=False)
            images = images[idx]
            source = f"MNIST test set ({args.samples} of 10000)"

    # Normalise to [0, 1] float32 and shape to NHWC.
    #
    # DFC transposes ONNX NCHW inputs to NHWC internally (the Hailo chip's
    # native layout). Calibration data must match DFC's post-transpose
    # expectation — `hailo optimize` validates the feed shape against the
    # converted network. Supplying NCHW produces
    #   BadInputsShape: Data shape (1, 28, 28) doesn't match network's
    #                   input shape (28, 28, 1)
    nhwc = images.astype(np.float32).reshape(-1, 28, 28, 1) / 255.0

    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    np.save(args.output, nhwc)

    print(f"[calib] source:  {source}")
    print(f"[calib] shape:   {nhwc.shape} (NHWC)")
    print(f"[calib] dtype:   {nhwc.dtype}")
    print(f"[calib] range:   [{nhwc.min():.3f}, {nhwc.max():.3f}]")
    print(f"[calib] mean:    {nhwc.mean():.3f}")
    print(f"[calib] output:  {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
