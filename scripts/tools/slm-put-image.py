#!/usr/bin/env python3
"""
slm-put-image.py — Convert an image to MNIST-shaped fp32 and push to SLM-OS.

The SLM-OS inference engine expects raw little-endian fp32 buffers
matching the model's input shape; for MNIST that is 1×1×28×28 = 784
floats = 3,136 bytes, normalized to [0, 1]. The kernel has no PNG/JPEG
decoder (no libc, no zlib), so any image format conversion has to
happen on the host before the framed `xput` transfer.

This wrapper:
  1. Opens any format Pillow understands (PNG, JPEG, BMP, ...)
  2. Converts to 8-bit grayscale ('L' mode)
  3. Resizes to 28×28 if needed (Lanczos filter)
  4. Optionally inverts intensity — MNIST trains on white-on-black, so
     a typical photo of a black digit on white paper needs --invert
  5. Normalizes to fp32 in [0, 1]
  6. Writes a 3,136-byte temp file
  7. Hands off to slm-put.py for the actual transfer

The default target shape (28×28×1, fp32, [0, 1]) matches MNIST. For
other models a future spec will widen the shape options.

Example:
    python3 scripts/tools/slm-put-image.py 192.168.4.5 \\
        my_digit.png /mnt/files/digit.bin --invert

After upload, on the device:
    lua
    > idx = slm.model_load_mnist()
    > _, pred = slm.model_infer_file(idx, "/mnt/files/digit.bin")
    > print(pred)
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("error: Pillow is required. Install with: pip install Pillow",
          file=sys.stderr)
    sys.exit(2)


MNIST_SIDE = 28
MNIST_BYTES = MNIST_SIDE * MNIST_SIDE * 4  # 3136 — fp32 little-endian


def decode_to_fp32(local_path: Path,
                   side: int,
                   invert: bool,
                   resize_filter: int) -> bytes:
    """Open the image and produce side*side fp32 bytes in [0, 1]."""
    with Image.open(local_path) as img:
        img = img.convert("L")
        if img.size != (side, side):
            img = img.resize((side, side), resize_filter)
        # Pillow uses row-major (top-left origin) which matches MNIST's
        # raw IDX layout — no transpose needed.
        raw = img.tobytes()  # uint8, row-major, length = side*side

    if len(raw) != side * side:
        raise RuntimeError(
            f"unexpected raw byte count: got {len(raw)}, want {side * side}")

    if invert:
        raw = bytes(255 - b for b in raw)

    # Pack as little-endian fp32 in [0, 1].
    floats = [b / 255.0 for b in raw]
    return struct.pack(f"<{len(floats)}f", *floats)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Convert an image to MNIST fp32 and push to SLM-OS",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    p.add_argument("target",
                   help="Target hostname/IP for telnet, or SBC name with --labctl")
    p.add_argument("local_path", type=Path,
                   help="Local image file (PNG, JPEG, BMP, ...)")
    p.add_argument("remote_path",
                   help="Destination path on the SLM-OS VFS")
    p.add_argument("--invert", action="store_true",
                   help="Invert intensity (use for black-on-white inputs; "
                        "MNIST trains on white-on-black)")
    p.add_argument("--side", type=int, default=MNIST_SIDE,
                   help=f"Square image side length (default: {MNIST_SIDE})")
    p.add_argument("--resize-filter",
                   choices=("lanczos", "bilinear", "bicubic", "nearest"),
                   default="lanczos",
                   help="Pillow resize filter (default: lanczos)")
    p.add_argument("--keep-tmp", action="store_true",
                   help="Keep the intermediate fp32 .bin (printed to stderr)")
    p.add_argument("--dry-run", action="store_true",
                   help="Decode + write fp32 only; skip the slm-put step")
    # Pass-through flags forwarded to slm-put.py verbatim
    p.add_argument("--port", type=int, default=2323,
                   help="Telnet port (default: 2323)")
    p.add_argument("--labctl", action="store_true",
                   help="Resolve target via labctl info")
    p.add_argument("--timeout", type=float, default=10.0,
                   help="slm-put per-prompt timeout in seconds (default: 10.0)")
    p.add_argument("--debug", action="store_true",
                   help="Forward --debug to slm-put.py")
    return p.parse_args()


def main() -> int:
    args = parse_args()

    if not args.local_path.is_file():
        print(f"error: not a file: {args.local_path}", file=sys.stderr)
        return 1

    # Pillow 10 moved the resize-filter constants under
    # `Image.Resampling.*`; the bare `Image.LANCZOS` aliases still
    # work in 10.x but are deprecation-warned and slated for removal.
    # `getattr` falls back to the module itself on older Pillow so
    # `RESAMPLING.LANCZOS` resolves to `Image.LANCZOS` there too.
    RESAMPLING = getattr(Image, "Resampling", Image)
    filter_map = {
        "lanczos":  RESAMPLING.LANCZOS,
        "bilinear": RESAMPLING.BILINEAR,
        "bicubic":  RESAMPLING.BICUBIC,
        "nearest":  RESAMPLING.NEAREST,
    }
    fp32_bytes = decode_to_fp32(args.local_path,
                                args.side,
                                args.invert,
                                filter_map[args.resize_filter])

    expected = args.side * args.side * 4
    if len(fp32_bytes) != expected:
        print(f"error: produced {len(fp32_bytes)} bytes, expected {expected}",
              file=sys.stderr)
        return 1

    print(f"decoded {args.local_path.name} -> "
          f"{args.side}x{args.side} grayscale fp32 ({len(fp32_bytes)} B"
          f"{', inverted' if args.invert else ''})",
          file=sys.stderr)

    # Stage to a temp file. Use a deterministic suffix so a `--keep-tmp`
    # path is human-readable.
    tmp_dir = Path(tempfile.gettempdir())
    stem = args.local_path.stem
    tmp_path = tmp_dir / f"slmput-image-{os.getpid()}-{stem}.bin"
    tmp_path.write_bytes(fp32_bytes)

    if args.keep_tmp or args.dry_run:
        print(f"fp32 staged at: {tmp_path}", file=sys.stderr)

    if args.dry_run:
        return 0

    # Compose with slm-put.py rather than re-implementing the framed
    # xput protocol. slm-put.py lives next to this file.
    here = Path(__file__).resolve().parent
    slm_put = here / "slm-put.py"
    if not slm_put.is_file():
        print(f"error: cannot find slm-put.py at {slm_put}", file=sys.stderr)
        return 1

    cmd = [sys.executable, str(slm_put)]
    if args.labctl:
        cmd.append("--labctl")
    cmd += ["--port", str(args.port),
            "--timeout", str(args.timeout)]
    if args.debug:
        cmd.append("--debug")
    cmd += [args.target, str(tmp_path), args.remote_path]

    try:
        rc = subprocess.run(cmd).returncode
    finally:
        if not args.keep_tmp:
            try:
                tmp_path.unlink()
            except OSError:
                pass

    return rc


if __name__ == "__main__":
    sys.exit(main())
