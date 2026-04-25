#!/usr/bin/env python3
"""
mnist-extract-test-digits.py — Pull a few digit images from the
MNIST test set and write them as raw fp32 binaries the SLM-OS
GPU pipeline can mmap directly.

Each output file is 1×1×28×28 = 784 fp32 values = 3,136 bytes,
little-endian. Pixel values are normalized to [0, 1] (= raw / 255).

Run from the repo root:
    /tmp/mnistenv/bin/python3 scripts/mnist-extract-test-digits.py \\
        scripts/mnist-weights/digits/

The output directory will contain:
    digit_<class>_idx<N>.bin   — one digit per requested target class

The script picks the first matching test-set sample for each class
0..9. Optional --indices override picks specific test-set indices
instead.

Network access is required for the first run to download the MNIST
test set from the canonical Yann LeCun mirror; subsequent runs reuse
the cache at ~/.cache/slmos-mnist/.
"""

import argparse
import gzip
import os
import struct
import sys
import urllib.request
from pathlib import Path

import numpy as np

CACHE_DIR = Path.home() / ".cache" / "slmos-mnist"
TEST_IMAGES_URL = "https://ossci-datasets.s3.amazonaws.com/mnist/t10k-images-idx3-ubyte.gz"
TEST_LABELS_URL = "https://ossci-datasets.s3.amazonaws.com/mnist/t10k-labels-idx1-ubyte.gz"


def fetch(url: str, dest: Path) -> None:
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[fetch] {url} -> {dest}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=30) as resp, dest.open("wb") as out:
        out.write(resp.read())


def load_test_set() -> tuple[np.ndarray, np.ndarray]:
    img_gz = CACHE_DIR / "t10k-images-idx3-ubyte.gz"
    lbl_gz = CACHE_DIR / "t10k-labels-idx1-ubyte.gz"
    fetch(TEST_IMAGES_URL, img_gz)
    fetch(TEST_LABELS_URL, lbl_gz)

    with gzip.open(img_gz, "rb") as f:
        magic, count, rows, cols = struct.unpack(">IIII", f.read(16))
        if magic != 2051 or rows != 28 or cols != 28:
            raise RuntimeError(f"unexpected images header: {magic} {rows}x{cols}")
        images = np.frombuffer(f.read(), dtype=np.uint8).reshape(count, rows, cols)

    with gzip.open(lbl_gz, "rb") as f:
        magic, count2 = struct.unpack(">II", f.read(8))
        if magic != 2049 or count2 != count:
            raise RuntimeError(f"label/image count mismatch: {count} vs {count2}")
        labels = np.frombuffer(f.read(), dtype=np.uint8)

    return images, labels


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("out_dir", type=Path,
                   help="Output directory for digit_*.bin files")
    p.add_argument("--indices", nargs="+", type=int, default=None,
                   help="Specific test-set indices to extract (overrides default)")
    args = p.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)

    images, labels = load_test_set()

    if args.indices is not None:
        targets = [(int(i), int(labels[i])) for i in args.indices]
    else:
        # First sample per class 0..9.
        targets = []
        for cls in range(10):
            idx = int(np.argmax(labels == cls))
            targets.append((idx, cls))

    for idx, cls in targets:
        img = images[idx].astype(np.float32) / 255.0  # [28, 28], values in [0, 1]
        # Match MNIST ONNX input layout: [1, 1, 28, 28].
        out_path = args.out_dir / f"digit_{cls}_idx{idx}.bin"
        img.tofile(out_path)
        print(f"[wrote] {out_path}  class={cls}  shape={img.shape}  "
              f"size={out_path.stat().st_size}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
