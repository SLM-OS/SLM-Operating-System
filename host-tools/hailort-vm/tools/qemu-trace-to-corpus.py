#!/usr/bin/env python3
# Convert a QEMU --trace memory_region_ops_{read,write} log into the corpus JSONL
# format defined in docs/hailo-re-corpus-format.md.
#
# QEMU 8.x emits memory_region_ops_read/write trace events in this shape:
#
#   memory_region_ops_read cpu 0 mr 0x55a... addr 0x10 value 0x0 size 4 name 'edu-mmio'
#   memory_region_ops_write cpu 0 mr 0x55a... addr 0x14 value 0x42 size 4 name 'edu-mmio'
#
# (The exact bytes-per-line vary by QEMU version. The parser is permissive —
# it pulls addr / value / size / name / direction via regex and ignores the rest.)
#
# This parser is intentionally minimal and scoped to the Task 0.3 placeholder.
# Once Task 0.2 lands a real Hailo stub that emits corpus entries directly,
# this script becomes redundant.

from __future__ import annotations

import argparse
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path


TRACE_LINE = re.compile(
    r"^memory_region_ops_(?P<dir>read|write)\s+"
    r"cpu\s+\d+\s+"
    r"mr\s+0x[0-9a-fA-F]+\s+"
    r"addr\s+0x(?P<addr>[0-9a-fA-F]+)\s+"
    r"value\s+0x(?P<value>[0-9a-fA-F]+)\s+"
    r"size\s+(?P<size>\d+)"
    r"(?:\s+name\s+'(?P<name>[^']*)')?"
)


# MemoryRegion names of interest. The Task 0.3 stub-stub device exposes three
# BARs named hailo-stub-bar0/2/4 (matching the corpus spec). The edu-mmio
# entry is a fallback for environments where the custom QEMU isn't built —
# corpora from edu are not useful protocol artifacts.
BAR_NAME_MAP = {
    "hailo-stub-bar0": 0,
    "hailo-stub-bar2": 2,
    "hailo-stub-bar4": 4,
    "edu-mmio": 0,    # fallback only
}

# Each BAR's size, used to convert the absolute guest-physical addresses in
# QEMU 8.2's memory_region_ops_* trace events back to BAR-relative offsets.
# Match hailo-stub-stub.c's HAILO_BAR*_SIZE constants.
BAR_SIZE_BY_NAME = {
    "hailo-stub-bar0": 4 * 1024,
    "hailo-stub-bar2": 16 * 1024,
    "hailo-stub-bar4": 1 * 1024 * 1024,
    "edu-mmio":        1 * 1024 * 1024,
}


def parse_capture_window(serial_log: Path) -> tuple[int | None, int | None]:
    """
    Return (begin_byte, end_byte) offsets bracketing the HAILORT_CAPTURE_BEGIN /
    HAILORT_CAPTURE_DONE markers in the serial log. Used only for human-friendly
    reporting — the QEMU trace itself has no synchronized timestamps with the
    serial log, so we capture *everything* the trace emitted during the boot
    and rely on the placeholder device having no other MMIO consumers.
    """
    begin, end = None, None
    text = serial_log.read_text(errors="replace")
    if "HAILORT_CAPTURE_BEGIN" in text:
        begin = text.index("HAILORT_CAPTURE_BEGIN")
    if "HAILORT_CAPTURE_DONE" in text:
        end = text.index("HAILORT_CAPTURE_DONE")
    return begin, end


def hex_value_to_le_bytes(value_hex: str, size: int) -> str:
    """
    Corpus spec § operation entries: value is hex-encoded, little-endian, no 0x
    prefix, lowercase, exactly size*2 characters. QEMU prints values as
    big-endian integers (host byte order in hex), so we convert.
    """
    v = int(value_hex, 16)
    return v.to_bytes(size, "little").hex()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--trace", required=True, type=Path,
                    help="QEMU trace log (from -trace ...,file=...)")
    ap.add_argument("--serial", required=True, type=Path,
                    help="Guest serial log (for capture markers)")
    ap.add_argument("--hailort-version", required=True)
    ap.add_argument("--capture-host", required=True)
    ap.add_argument("--slmos-base-sha", required=True)
    ap.add_argument("--fw-version", default="4.23.0")
    ap.add_argument("--output", required=True, type=Path)
    args = ap.parse_args()

    begin, end = parse_capture_window(args.serial)
    if begin is None or end is None:
        print("warn: serial log missing CAPTURE_BEGIN/DONE markers; including all trace lines",
              file=sys.stderr)

    seq = 0
    entries: list[dict] = []
    unknown_names: set[str] = set()
    dropped = 0
    total_trace_lines = 0

    with args.trace.open(errors="replace") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            m = TRACE_LINE.match(line)
            if not m:
                continue
            total_trace_lines += 1
            name = m.group("name") or ""

            if name not in BAR_NAME_MAP:
                if name:
                    unknown_names.add(name)
                # Drop trace lines for memory regions we don't recognize (e.g.
                # serial UART MMIO, PCI config space). The placeholder edu
                # device is what we care about; ignore the rest.
                dropped += 1
                continue

            size = int(m.group("size"))
            value_hex = hex_value_to_le_bytes(m.group("value"), size)
            # QEMU 8.x's memory_region_ops_{read,write} trace emits the
            # *absolute* guest-physical address. BARs are size-aligned, so
            # offset-within-BAR = abs_addr & (bar_size - 1).
            abs_addr = int(m.group("addr"), 16)
            bar_size = BAR_SIZE_BY_NAME.get(name, 1 << 20)
            offset = abs_addr & (bar_size - 1)
            seq += 1
            entries.append({
                "type": "op",
                "seq": seq,
                "bar": BAR_NAME_MAP[name],
                "offset": offset,
                "size": size,
                "dir": m.group("dir"),
                "value": value_hex,
                "source": "qemu_capture",
                "validated_at_commit": None,
                "validated_at": None,
                "note": f"placeholder/{name}",
            })

    header = {
        "type": "header",
        "format_version": 1,
        "hailort_version": args.hailort_version,
        "fw_version": args.fw_version,
        "capture_host": args.capture_host,
        "slmos_base_sha": args.slmos_base_sha,
        "capture_started_at": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "notes": "Task 0.3 placeholder capture against hailo-stub-stub QEMU device. BAR0 reads return 0 except for vendor magic (0x98) and firmware-loaded magic (0x708); all BAR2/BAR4 reads return 0. Captured MMIO is HailoRT's deterministic write sequence under that stub — not a real Hailo protocol artifact, but a faithful capture-invariant proof.",
    }
    trailer = {
        "type": "trailer",
        "ended_at": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "last_seq": seq,
        "reason": "capture_run_complete",
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w") as out:
        out.write(json.dumps(header) + "\n")
        for e in entries:
            out.write(json.dumps(e) + "\n")
        out.write(json.dumps(trailer) + "\n")

    print(f"corpus: {args.output}", file=sys.stderr)
    print(f"  ops: {len(entries)}", file=sys.stderr)
    print(f"  trace lines parsed: {total_trace_lines}", file=sys.stderr)
    print(f"  dropped (unknown MR): {dropped}", file=sys.stderr)
    if unknown_names:
        print(f"  unknown MR names seen: {sorted(unknown_names)}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
