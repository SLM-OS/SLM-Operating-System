#!/usr/bin/env python3
"""
generate-mock-camera-frame.py — Build a 1640x1232 RAW10 RGGB Bayer
frame from a 28x28 fp32 MNIST digit. Used by the QEMU-side mock
camera component so the camera + preprocess + inference Lua path
has CI coverage without IMX219 hardware.

Flow:

  - Read a 28x28 fp32 digit (3136 bytes, values in [0, 1]).
  - Upscale 44x to 1232x1232 with nearest-neighbour (no smoothing).
  - Pad to 1640x1232 with black on left/right so the centred 1232x1232
    crop done by the C-side preprocess function recovers the original
    pixels exactly (modulo 10-bit + 8-bit quantization).
  - Tile the grayscale into RGGB Bayer (R=G=B=v gives the same value
    at every pixel).
  - Quantize to 10-bit and pack as RAW10 (4 pixels per 5 bytes).

Invocation:

    python3 scripts/generate-mock-camera-frame.py \\
        --digit scripts/fixtures/mock_camera_digit.bin \\
        --output build/mock_camera_frame.bin

Output is 2,525,600 bytes (1640 * 1232 * 10 / 8).
"""

import argparse
import struct
import sys
from pathlib import Path


FRAME_W = 1640
FRAME_H = 1232
DIGIT_PX = 28
SCALE = 44                        # 28 * 44 == 1232
CROP_W = DIGIT_PX * SCALE         # 1232
CROP_X_OFFSET = (FRAME_W - CROP_W) // 2  # 204; even, preserves RGGB alignment
RAW10_BYTES = FRAME_W * FRAME_H * 10 // 8


def read_digit_fp32(path: Path) -> list[float]:
    raw = path.read_bytes()
    if len(raw) != DIGIT_PX * DIGIT_PX * 4:
        raise SystemExit(
            f"digit file {path} is {len(raw)} bytes; "
            f"expected {DIGIT_PX * DIGIT_PX * 4}"
        )
    return list(struct.unpack(f"<{DIGIT_PX * DIGIT_PX}f", raw))


def quantize10(v: float) -> int:
    if v < 0.0:
        v = 0.0
    elif v > 1.0:
        v = 1.0
    q = int(round(v * 1023.0))
    if q < 0:
        q = 0
    elif q > 1023:
        q = 1023
    return q


def build_pixel_row(digit: list[float], y: int) -> list[int]:
    """Return 1640 quantised 10-bit pixels for output row y."""
    row = [0] * FRAME_W
    if 0 <= y < CROP_W:
        src_y = y // SCALE
        for sx in range(DIGIT_PX):
            v = digit[src_y * DIGIT_PX + sx]
            q = quantize10(v)
            x_start = CROP_X_OFFSET + sx * SCALE
            for k in range(SCALE):
                row[x_start + k] = q
    return row


def pack_raw10_row(pixels: list[int]) -> bytes:
    """Pack a row of width pixels (multiple of 4) as RAW10."""
    if len(pixels) % 4 != 0:
        raise SystemExit("row width must be a multiple of 4 for RAW10 packing")
    out = bytearray(len(pixels) * 10 // 8)
    out_i = 0
    for i in range(0, len(pixels), 4):
        p0, p1, p2, p3 = pixels[i], pixels[i + 1], pixels[i + 2], pixels[i + 3]
        out[out_i + 0] = (p0 >> 2) & 0xFF
        out[out_i + 1] = (p1 >> 2) & 0xFF
        out[out_i + 2] = (p2 >> 2) & 0xFF
        out[out_i + 3] = (p3 >> 2) & 0xFF
        out[out_i + 4] = (
            (p0 & 0x3)
            | ((p1 & 0x3) << 2)
            | ((p2 & 0x3) << 4)
            | ((p3 & 0x3) << 6)
        )
        out_i += 5
    return bytes(out)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--digit", type=Path, required=True,
                   help="28x28 fp32 input digit (3136 bytes)")
    p.add_argument("--output", type=Path, required=True,
                   help="Output RAW10 frame path")
    args = p.parse_args()

    digit = read_digit_fp32(args.digit)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as f:
        for y in range(FRAME_H):
            row = build_pixel_row(digit, y)
            f.write(pack_raw10_row(row))

    written = args.output.stat().st_size
    if written != RAW10_BYTES:
        raise SystemExit(
            f"wrote {written} bytes; expected {RAW10_BYTES} "
            f"({FRAME_W}x{FRAME_H} RAW10)"
        )
    print(f"[wrote] {args.output}  {written} bytes  "
          f"({FRAME_W}x{FRAME_H} RAW10 RGGB)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
