#!/usr/bin/env python3
"""
mnist-verify-fp16-roundtrip.py — Verify FP32 vs FP16 weight precision
produces identical argmax across a batch of real MNIST test digits.

Companion to mnist-extract-weights.py's --dtype fp16 mode (#660). The
script is a host-side correctness check — it doesn't touch the GPU
or SLM-OS at all. The argument is: if numpy with FP16-roundtripped
weights produces the same argmax as numpy with FP32 weights on N
real test images, then the future HMMA GEMM kernel (#659/#661) using
the same FP16 weights will also classify correctly. The kernel may
introduce additional accumulator-ordering differences, but those
are bounded by the same FP16-precision envelope.

Run from the repo root with a Python 3 venv that has `numpy` and
`onnx` installed:
    python3 scripts/mnist-verify-fp16-roundtrip.py \\
        models/test/mnist.onnx --count 100

Acceptance for #660: at least 99% argmax agreement on N=100 random
test-set indices. The remaining ~1% are MNIST cases where the FP32
top-1 is itself within 1-2% of FP32 top-2; FP16 rounding can flip
those without indicating a real classifier-quality regression. Run
N=1000 or N=10000 if the user wants tighter bounds.

Performance note: each compared image runs the full Conv1 + Conv2 +
2× MaxPool + GEMM + AddBias pipeline twice (FP32 reference + FP16
emulated reference) in pure numpy. N=100 finishes in a few seconds;
N=10000 takes minutes. Bump `--count` deliberately.

Caches the test set under ~/.cache/slmos-mnist/ via the same path
mnist-extract-test-digits.py uses.
"""

import argparse
import gzip
import struct
import sys
import urllib.request
from pathlib import Path

import numpy as np

try:
    import onnx
except ImportError:
    sys.exit(
        "onnx module required. Activate or run from a Python venv "
        "with `pip install onnx numpy`."
    )

# Reuse the reference inference + roundtrip helper from the extractor.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from importlib import import_module
extract = import_module("mnist-extract-weights")

CACHE_DIR = Path.home() / ".cache" / "slmos-mnist"
TEST_IMAGES_URL = ("https://ossci-datasets.s3.amazonaws.com/mnist/"
                   "t10k-images-idx3-ubyte.gz")
TEST_LABELS_URL = ("https://ossci-datasets.s3.amazonaws.com/mnist/"
                   "t10k-labels-idx1-ubyte.gz")

# IDX file-format magics. The IDX format prefixes every file with a
# 32-bit big-endian magic that encodes the dimensionality and dtype.
# See http://yann.lecun.com/exdb/mnist/ § "FILE FORMATS".
IDX3_IMAGES_MAGIC = 0x00000803  # 2051: 3-D, ubyte (image stack)
IDX1_LABELS_MAGIC = 0x00000801  # 2049: 1-D, ubyte (label vector)


def fetch(url: str, dest: Path) -> None:
    if dest.exists():
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[fetch] {url} -> {dest}", file=sys.stderr)
    with urllib.request.urlopen(url, timeout=30) as resp, \
         dest.open("wb") as out:
        out.write(resp.read())


def load_test_set():
    img_gz = CACHE_DIR / "t10k-images-idx3-ubyte.gz"
    lbl_gz = CACHE_DIR / "t10k-labels-idx1-ubyte.gz"
    fetch(TEST_IMAGES_URL, img_gz)
    fetch(TEST_LABELS_URL, lbl_gz)

    with gzip.open(img_gz, "rb") as f:
        magic, count, rows, cols = struct.unpack(">IIII", f.read(16))
        if magic != IDX3_IMAGES_MAGIC:
            raise RuntimeError(f"unexpected images magic: {magic:#x} "
                               f"(want {IDX3_IMAGES_MAGIC:#x})")
        images = np.frombuffer(f.read(),
                               dtype=np.uint8).reshape(count, rows, cols)
    with gzip.open(lbl_gz, "rb") as f:
        magic, _ = struct.unpack(">II", f.read(8))
        if magic != IDX1_LABELS_MAGIC:
            raise RuntimeError(f"unexpected labels magic: {magic:#x} "
                               f"(want {IDX1_LABELS_MAGIC:#x})")
        labels = np.frombuffer(f.read(), dtype=np.uint8)
    return images, labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("onnx_path")
    ap.add_argument("--count", type=int, default=100,
                    help="Number of test digits to compare (default 100)")
    ap.add_argument("--seed", type=int, default=42,
                    help="RNG seed for sample selection (default 42)")
    ap.add_argument("--threshold", type=float, default=99.0,
                    help="Pass threshold: minimum FP16-vs-FP32 argmax "
                         "agreement percentage required (default 99.0)")
    args = ap.parse_args()

    print(f"[verify] loading {args.onnx_path}")
    model = onnx.load(args.onnx_path)
    images, labels = load_test_set()
    print(f"[verify] test set: {len(images)} images")

    rng = np.random.default_rng(args.seed)
    indices = rng.choice(len(images), size=args.count, replace=False)
    indices.sort()

    fp32_correct = 0
    fp16_correct = 0
    argmax_matches = 0
    mismatches = []
    for n, idx in enumerate(indices):
        img = (images[idx].astype(np.float32) / 255.0)
        img = img.reshape(1, 1, 28, 28)
        true_label = int(labels[idx])

        fp32_logits = extract.cpu_reference_inference(
            model, img, weight_dtype=np.float32)
        fp16_logits = extract.cpu_reference_inference(
            model, img, weight_dtype=np.float16)

        fp32_argmax = int(np.argmax(fp32_logits))
        fp16_argmax = int(np.argmax(fp16_logits))

        if fp32_argmax == true_label:
            fp32_correct += 1
        if fp16_argmax == true_label:
            fp16_correct += 1
        if fp32_argmax == fp16_argmax:
            argmax_matches += 1
        else:
            mismatches.append((idx, true_label, fp32_argmax, fp16_argmax,
                               fp32_logits.tolist(), fp16_logits.tolist()))

        if (n + 1) % 10 == 0 or n + 1 == args.count:
            print(f"  [{n+1}/{args.count}] "
                  f"fp32 acc={100*fp32_correct/(n+1):.1f}% "
                  f"fp16 acc={100*fp16_correct/(n+1):.1f}% "
                  f"argmax-match={100*argmax_matches/(n+1):.1f}%",
                  file=sys.stderr)

    agreement_pct = 100.0 * argmax_matches / args.count
    print(f"\n[verify] summary over {args.count} images "
          f"(seed={args.seed}):")
    print(f"  FP32 accuracy        : {100*fp32_correct/args.count:.2f}%  "
          f"({fp32_correct}/{args.count})")
    print(f"  FP16 accuracy        : {100*fp16_correct/args.count:.2f}%  "
          f"({fp16_correct}/{args.count})")
    print(f"  FP16 vs FP32 agree   : {agreement_pct:.2f}%  "
          f"({argmax_matches}/{args.count})")

    if mismatches:
        print(f"\n[verify] mismatch detail:")
        for idx, label, a32, a16, l32, l16 in mismatches[:5]:
            print(f"  idx={idx} true={label} fp32={a32} fp16={a16}")
            print(f"    fp32 logits = {[f'{x:.4f}' for x in l32]}")
            print(f"    fp16 logits = {[f'{x:.4f}' for x in l16]}")
        if len(mismatches) > 5:
            print(f"  ... and {len(mismatches) - 5} more")

    if agreement_pct < args.threshold:
        print(f"\n[verify] FAIL: agreement {agreement_pct:.2f}% < "
              f"threshold {args.threshold}%")
        return 1
    print(f"\n[verify] PASS: agreement {agreement_pct:.2f}% >= "
          f"threshold {args.threshold}%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
