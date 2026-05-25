"""Parse Hailo boundary-trace lines into corpus-comparable BAR ops.

The kernel emits one line per instrumented operation when the
boundary-trace toolkit is armed (`hailo trace phase=... mech=mmio` +
PR #780 framework). Format from `kernel/ai_accel/hailo/hailo_trace.c`:

    [trc] phase=link       mech=MMIO WR32 bar=0 off=0x0098 val=0x12345678
    [trc] phase=link       mech=MMIO RD32 bar=4 off=0x0640 val=0x00000000

(`phase=%-10s mech=%-4s` — column-padded; widths vary as the framework
adds phases.) This module extracts the MMIO subset of those lines —
the operations that match the corpus's op-shape `(bar, offset, dir,
size, value)` — and discards everything else (PCI cfg, RPC framing,
IRQ deliveries, DMA events, busy-wait markers). Only 32-bit MMIO is
emitted by the trace today; the parser tolerates an integer suffix in
`WR<N>` / `RD<N>` so 16/8-bit widths land cleanly if the kernel grows
them later.

The corpus uses `dir="read"|"write"` and `size` in bytes; this parser
converts WR/RD + suffix accordingly.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, Iterator, Optional


# Anchors on `mech=MMIO` so we positively skip cfg/rx/dma/irq/udelay
# lines without matching their op-name tokens. Phase column is captured
# for diagnostic reporting (knowing the divergence happened in
# `phase=inference` vs `phase=fw_boot` is the load-bearing context).
_MMIO_RE = re.compile(
    r"^\[trc\]\s+"
    r"phase=(?P<phase>\S+)\s+"
    r"mech=MMIO\s+"
    r"(?P<op>WR|RD)(?P<width>\d+)\s+"
    r"bar=(?P<bar>\d+)\s+"
    r"off=0x(?P<off>[0-9a-fA-F]+)\s+"
    r"val=0x(?P<val>[0-9a-fA-F]+)"
    r"\s*$"
)


@dataclass(frozen=True)
class BoundaryTraceOp:
    """One MMIO operation captured by the kernel's boundary trace.

    Field shapes mirror the corpus op-entry contract (see
    docs/hailo-re-corpus-format.md §Lines 2..N) so a BoundaryTraceOp
    can be lined up against a corpus op by `(bar, offset, dir, size,
    value)`. `phase` is the kernel-side phase mask at emit time and is
    metadata only — corpus entries don't carry it.

    `value` is the little-endian hex string the kernel emitted, with
    no `0x` prefix and zero-padded to `size * 2` chars. Matches the
    corpus's `value` field exactly so equality comparisons are
    string-trivial.
    """

    bar: int
    offset: int
    dir: str       # "read" or "write"
    size: int      # bytes
    value: str     # zero-padded LE-hex, no 0x
    phase: str     # informational; e.g. "fw_boot", "model_load"
    raw_line: str  # original line for error reporting


def parse_trace_line(line: str) -> Optional[BoundaryTraceOp]:
    """Parse one boundary-trace line. Returns None for non-MMIO lines.

    Non-MMIO trace lines (PCI cfg, RPC, IRQ, DMA, busy-wait) and
    completely unrelated serial output (boot banner, shell prompts,
    hailo replay-step diagnostics) all return None — they're not
    errors, they're just not the corpus's op-shape.
    """
    m = _MMIO_RE.match(line.rstrip("\r\n"))
    if m is None:
        return None
    width_bits = int(m.group("width"))
    if width_bits % 8 != 0 or width_bits == 0:
        # Kernel today emits 32 only; reject obvious garbage rather
        # than coerce a nonsense byte count.
        return None
    size = width_bits // 8
    raw_val = m.group("val")
    # Pad to canonical LE-hex width so a malformed-but-otherwise-valid
    # line ("val=0x1" when size=4) still compares correctly against
    # the corpus's zero-padded form ("00000001").
    value = raw_val.zfill(size * 2).lower()
    return BoundaryTraceOp(
        bar=int(m.group("bar")),
        offset=int(m.group("off"), 16),
        dir="write" if m.group("op") == "WR" else "read",
        size=size,
        value=value,
        phase=m.group("phase"),
        raw_line=line.rstrip("\r\n"),
    )


def parse_trace_stream(lines: Iterable[str]) -> Iterator[BoundaryTraceOp]:
    """Yield BoundaryTraceOp for every parseable MMIO line, skipping
    everything else. The caller is free to wrap with `list()` if a
    one-shot in-memory parse is preferred over streaming."""
    for line in lines:
        op = parse_trace_line(line)
        if op is not None:
            yield op
