#!/usr/bin/env python3
"""
build-operator-library.py — Pack a directory of compiled SASS blobs
into the operator_library.bin format consumed by SLM-OS (#663).

Reads a JSON manifest declaring (op_kind, tier, dtype, sass_path)
tuples and emits the packed binary. The wire format is documented
in kernel/include/operator_library.h — this script is the canonical
producer.

Run from the repo root:

    python3 scripts/build-operator-library.py \\
        scripts/cuda/operator_library/MANIFEST.json \\
        build/operator_library.bin

Manifest format (JSON):

    {
      "version": 1,
      "entries": [
        {
          "op_kind":  "GEMM_GENERIC",
          "tier":     "SIMT",
          "dtype":    "FP32",
          "sass":     "gemm_fp32_shader.sass",
          "comment":  "optional"
        },
        ...
      ]
    }

The script resolves `sass` paths relative to the manifest file's
directory. Missing SASS files are an error (catches "manifest
declares an HMMA kernel but the build hasn't produced its .sass
yet" before the library ships).

The op_kind / tier / dtype names map directly to the C-side
enums (see _OP_KINDS, _TIERS, _DTYPES below). When extending the
operator library with new ops, update both this script's tables
and kernel/include/gpu_handoff.h's enums in lockstep.
"""

import argparse
import json
import os
import struct
import sys
from pathlib import Path
from typing import Tuple

# Mirror of enum slm_gpu_op_kind in kernel/include/gpu_handoff.h.
_OP_KINDS = {
    "RMSNORM":      0,
    "ROPE":         1,
    "EMBEDDING":    2,
    "Q4K_DOT":      3,
    "Q4K_GEMM":     4,
    "GQA_ATTN":     5,
    "SWIGLU":       6,
    "LM_HEAD":      7,
    "GEMM_GENERIC": 8,
    "CONV2D":       9,
    "ADD_BIAS":    10,
    "MAXPOOL":     11,
}

# Mirror of SLM_GPU_TIER_* in kernel/include/gpu_handoff.h.
_TIERS = {
    "AUTO":  0,
    "HMMA":  1,
    "SIMT":  2,
    "CPU":   3,
}

# Mirror of enum slm_gpu_dtype in kernel/include/gpu_handoff.h.
_DTYPES = {
    "FP32":  1,
    "FP16":  2,
    "BF16":  3,
    "TF32":  4,
    "INT8":  5,
    "Q4K":   6,
}

# Constants from operator_library.h.
_MAGIC                = 0x424C504F   # "OPLB" little-endian
_VERSION_V1           = 1
_SCHEMA_V1            = 1
_OUTER_HEADER_LEN     = 24
_INNER_HEADER_LEN     = 8
_ENTRY_LEN            = 32

# FNV-1a constants — must match operator_library.c::checksum32.
_FNV_OFFSET_BASIS = 0x811C9DC5
_FNV_PRIME        = 0x01000193


def fnv1a_32(data: bytes) -> int:
    h = _FNV_OFFSET_BASIS
    for b in data:
        h ^= b
        h = (h * _FNV_PRIME) & 0xFFFFFFFF
    return h


def lookup(table: dict, key: str, kind: str) -> int:
    if key not in table:
        raise SystemExit(
            f"unknown {kind} '{key}' (recognised: {sorted(table.keys())})"
        )
    return table[key]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("manifest", type=Path,
                    help="JSON manifest path")
    ap.add_argument("out", type=Path,
                    help="Output operator_library.bin path")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    with args.manifest.open("r") as f:
        manifest = json.load(f)

    if manifest.get("version") != 1:
        raise SystemExit(
            f"manifest version mismatch: got {manifest.get('version')}, "
            f"expected 1"
        )

    entries = manifest.get("entries", [])
    if not entries:
        raise SystemExit("manifest has no entries — empty library would "
                         "still be valid, but you almost certainly meant "
                         "to declare some")

    manifest_dir = args.manifest.parent.resolve()

    # Resolve every entry: parse fields, load the SASS payload bytes.
    resolved: list[Tuple[int, int, int, int, bytes, str]] = []
    for i, entry in enumerate(entries):
        op_kind = lookup(_OP_KINDS, entry["op_kind"], "op_kind")
        tier    = lookup(_TIERS,    entry["tier"],    "tier")
        dtype   = lookup(_DTYPES,   entry["dtype"],   "dtype")
        flags   = entry.get("flags", 0)
        sass_path = manifest_dir / entry["sass"]
        if not sass_path.exists():
            raise SystemExit(
                f"manifest entry {i} references missing SASS file: "
                f"{sass_path}\n"
                f"  Build the .cu sources on Jetson first (see header "
                f"comment in scripts/cuda/{entry['sass'][:-5]}.cu for "
                f"the nvcc + cuobjdump + dd recipe), or remove the "
                f"entry from the manifest if the kernel isn't ready yet."
            )
        sass_bytes = sass_path.read_bytes()
        resolved.append((op_kind, tier, dtype, flags, sass_bytes,
                         str(sass_path)))

    # Layout: outer header || inner header || entries || SASS region.
    op_count = len(resolved)
    entries_bytes = op_count * _ENTRY_LEN
    sass_region = b""
    sass_offsets: list[int] = []
    for _, _, _, _, sass_bytes, _ in resolved:
        sass_offsets.append(len(sass_region))
        sass_region += sass_bytes

    inner = struct.pack("<II", op_count, 0)
    entries_buf = b""
    for (op_kind, tier, dtype, flags, sass_bytes, _), offset in zip(
            resolved, sass_offsets):
        entries_buf += struct.pack(
            "<IIIIQQ",
            op_kind, tier, dtype, flags,
            offset, len(sass_bytes),
        )
    assert len(entries_buf) == entries_bytes

    payload = inner + entries_buf + sass_region
    payload_len = len(payload)
    checksum = fnv1a_32(payload)

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

    blob = outer + payload
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)

    print(f"[oplb] wrote {args.out}")
    print(f"  op_count    : {op_count}")
    print(f"  payload_len : {payload_len} B "
          f"({_INNER_HEADER_LEN} inner + {entries_bytes} entries + "
          f"{len(sass_region)} sass)")
    print(f"  total       : {len(blob)} B (24 outer + {payload_len} payload)")
    print(f"  checksum    : 0x{checksum:08x}")
    if args.verbose:
        for (op_kind, tier, dtype, flags, sass_bytes, path), offset in zip(
                resolved, sass_offsets):
            kn = next(k for k, v in _OP_KINDS.items() if v == op_kind)
            tn = next(k for k, v in _TIERS.items()    if v == tier)
            dn = next(k for k, v in _DTYPES.items()   if v == dtype)
            print(f"    [{kn:14s} {tn:4s} {dn:4s}] "
                  f"offset=0x{offset:08x} size={len(sass_bytes)} "
                  f"({path})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
