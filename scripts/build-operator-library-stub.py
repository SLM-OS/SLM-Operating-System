#!/usr/bin/env python3
"""
build-operator-library-stub.py — Emit a minimal empty operator-library
blob for kernel builds where no real SASS files are available
(e.g. QEMU dev builds, CI without Jetson access).

The stub has the same wire format as `build-operator-library.py`'s
output but declares `op_count=0` and an empty SASS region. The
parser in `kernel/gpu/operator_library.c` accepts it; every
`operator_library_lookup` then returns ERR_NOT_FOUND so callers
gracefully fall through to "no GPU kernel for this op".

Usage:
    python3 scripts/build-operator-library-stub.py <output_path>
"""

import argparse
import struct
import sys
from pathlib import Path


_MAGIC                = 0x424C504F   # "OPLB" little-endian
_VERSION_V1           = 1
_SCHEMA_V1            = 1
_OUTER_HEADER_LEN     = 24
_INNER_HEADER_LEN     = 8

_FNV_OFFSET_BASIS = 0x811C9DC5
_FNV_PRIME        = 0x01000193


def fnv1a_32(data: bytes) -> int:
    h = _FNV_OFFSET_BASIS
    for b in data:
        h ^= b
        h = (h * _FNV_PRIME) & 0xFFFFFFFF
    return h


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out", type=Path, help="Output stub blob path")
    args = ap.parse_args()

    # Inner header: op_count = 0, reserved = 0. No entries, no SASS region.
    inner = struct.pack("<II", 0, 0)
    payload_len = len(inner)
    checksum = fnv1a_32(inner)

    outer = struct.pack(
        "<4sHHHHIII",
        b"OPLB",
        _VERSION_V1,
        0,                     # kind_id (reserved)
        _SCHEMA_V1,
        0,                     # reserved
        payload_len,
        checksum,
        0,                     # reserved
    )
    assert len(outer) == _OUTER_HEADER_LEN

    blob = outer + inner
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print(f"[oplib-stub] wrote {args.out} ({len(blob)} B, op_count=0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
