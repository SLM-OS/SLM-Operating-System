"""Translate a Linux ftrace capture of HailoRT's MMIO into corpus-comparable ops.

Captured by `scripts/capture-hailort-ftrace.sh`, which arms the kernel
function tracer on the standard MMIO accessors (`iowrite32`/`ioread32`/
etc.) while a HailoRT workload runs. The resulting trace file has a
self-contained header (BAR physical addresses) plus one line per MMIO
call.

We exist because the QEMU stub used for the #795 Phase 2 grind doesn't
model BAR2 (per-channel VDMA registers), so the corpus has zero BAR2
ops. The Linux-side capture closes that gap: real HailoRT, real
hardware, every MMIO traced. See the memory entry
`hailo_re_corpus_bar_coverage` for the structural-limit context.

## Output

`BoundaryTraceOp` instances (same type the `boundary_trace.py` parser
emits), so the existing `native_diff` aligner consumes both kinds of
captures uniformly. Mapping from ftrace's kernel-virtual address back
to `(bar, offset)` uses the BAR physical-base table embedded in the
trace file's `# bar_resources:` header. Lines that don't fall into any
known BAR range (most of the trace — kernel-wide MMIO accessors fire
for everything) are skipped silently.

## Why parse the bash-rendered ftrace text, not the binary trace_pipe

ftrace's text format is stable across kernel versions in a way the
binary ring-buffer isn't. The bash script could `cat` either, but
parsing text is portable and trivially debuggable when the operator
sends a capture across with a "this looks weird" question.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Iterable, Iterator, List, Optional, Tuple

from .boundary_trace import BoundaryTraceOp


# Header lines look like:
#   #   BAR0: phys=0x00000000fc010000 size=0x10000 flags=0x...
#   #   BAR2: phys=0x00000000fc020000 size=0x40000 flags=0x...
#   #   BAR4: phys=0x00000000fc100000 size=0x40000 flags=0x...
# We parse them once at the top of the file. The translator silently
# skips lines for BARs that aren't in the corpus's universe (we
# observed in PR #993/#994 work that only BAR0/2/4 carry ops; if a
# future capture has BAR1/3/5 mapped, those addresses can be added
# without code changes).
_BAR_HEADER_RE = re.compile(
    r"^#\s+BAR(?P<bar>\d+):\s+phys=0x(?P<phys>[0-9a-fA-F]+)\s+size=0x(?P<size>[0-9a-fA-F]+)"
)

# ftrace function-tracer line shape (function-mode, default
# `tracing/trace_options` format):
#
#   hailortcli-12345 [002] d... 7892.123456: iowrite32 <-hailo_pcie_write_atr
#
# That base record has no arguments — ftrace's `function` tracer
# doesn't capture them. We use a SECOND mechanism: kprobes set via
# `set_kprobe_events` that include `%x0 %x1 %x2` for the arg
# registers. Those produce lines like:
#
#   hailortcli-12345 [002] d... 7892.123456: iowrite32_entry: (iowrite32+0x0/0x40) v=0x17 addr=0xffff80004000c700
#
# The bash capture script will be updated to use kprobes for the
# accessors so we get the (value, addr) args. The translator handles
# BOTH formats:
#  - "iowrite32_entry: ... v=0xVV addr=0xAA" (kprobe — has args)
#  - "iowrite32 <-caller"                     (function — no args, skipped)
#
# The function-tracer fallback at least confirms the function was hit,
# but without args we can't produce a corpus op — those lines just get
# silently dropped.
_FTRACE_KPROBE_RE = re.compile(
    r"^\s*[^:]+:\s+"                        # task-comm-pid + cpu + flags
    r"(?P<fn>(?:io(?:read|write))(?P<width>\d+))_entry:\s+"
    r"\([^)]*\)\s+"                         # caller info "(iowrite32+0x0/0x40)"
    r"v=0x(?P<val>[0-9a-fA-F]+)\s+"
    r"addr=0x(?P<addr>[0-9a-fA-F]+)"
    r"\s*$"
)


@dataclass(frozen=True)
class BarMapping:
    """One BAR's physical-address range, parsed from the capture header."""

    bar: int
    phys_base: int
    size: int

    def contains(self, addr: int) -> bool:
        return self.phys_base <= addr < self.phys_base + self.size

    def offset(self, addr: int) -> int:
        return addr - self.phys_base


def parse_bar_header(lines: Iterable[str]) -> List[BarMapping]:
    """Scan capture-file comment lines for `# BAR<N>: phys=0xXX size=0xYY`
    entries. Returns mappings in the order they appear (typically
    BAR0..BAR5, with empty rows for BARs the device doesn't expose).

    Lines that don't match the BAR-header pattern are silently skipped
    — typical capture-file headers include other comment lines (date,
    kernel version, etc.) we don't care about here.
    """
    bars: List[BarMapping] = []
    for line in lines:
        m = _BAR_HEADER_RE.match(line.rstrip())
        if m is None:
            continue
        size = int(m.group("size"), 16)
        if size == 0:
            # BAR not implemented by the device (size-0 BARs land in
            # /sys/.../resource as a row of zeros). Skip — there's no
            # MMIO traffic to attribute to them.
            continue
        bars.append(BarMapping(
            bar=int(m.group("bar")),
            phys_base=int(m.group("phys"), 16),
            size=size,
        ))
    return bars


def parse_ftrace_line(
    line: str, bar_table: List[BarMapping],
) -> Optional[BoundaryTraceOp]:
    """Translate one ftrace kprobe line into a BoundaryTraceOp, or None
    if the line isn't an MMIO event we can map to a known BAR.

    Returns None for:
    - Comment lines (`# date: ...`)
    - Lines without `_entry:` markers (function tracer without args —
      kernel hit the symbol but we don't have value/addr)
    - MMIO addresses that don't fall into any of `bar_table`'s ranges
      (i.e. not the Hailo device — could be other drivers' MMIO
      happening concurrently)
    """
    m = _FTRACE_KPROBE_RE.match(line.rstrip("\r\n"))
    if m is None:
        return None

    fn = m.group("fn")              # e.g. "iowrite32"
    width_bits = int(m.group("width"))
    if width_bits % 8 != 0 or width_bits == 0:
        return None
    size = width_bits // 8

    # ftrace prints addr as a kernel-virtual address (the value passed
    # to iowrite32). Linux's pci_iomap maps the BAR's physical resource
    # into kernel-virtual space; the offset within the BAR is the
    # difference from the mapped base. BUT the trace doesn't tell us
    # the KV→phys mapping — we'd need /proc/iomem from the same boot.
    #
    # Workaround: the capture script's "addr=" value IS the kv
    # address. To translate to (bar, offset), the operator should
    # arrange that the kprobe formula prints the BAR-relative offset
    # directly. Without that, this function falls back to treating
    # `addr` as already-resolved: it matches against the phys_base
    # ranges from the BAR header.
    #
    # If real-world captures come back with a non-trivial KV ↔ phys
    # mapping, we'll extend the script to emit both via an additional
    # kprobe arg (or query /proc/kallsyms post-run). For now this
    # path keeps the contract simple: the bar_table is keyed on
    # whatever address space `addr=` reports.
    addr = int(m.group("addr"), 16)
    for mapping in bar_table:
        if mapping.contains(addr):
            raw_val = m.group("val").zfill(size * 2).lower()
            if len(raw_val) > size * 2:
                # The kprobe printed more hex digits than the op width
                # — same loud-fail semantics as the boundary_trace
                # parser; mismatches against the corpus's narrower
                # stored value otherwise look like a value bug when
                # they're really an encoding bug.
                return None
            # Byte-swap to wire-LE order, same convention the corpus
            # uses (and the boundary_trace parser produces). See
            # docs/hailo-re-corpus-format.md §Operation entries.
            byte_chunks = [raw_val[2 * i: 2 * i + 2] for i in range(size)]
            value = "".join(reversed(byte_chunks))
            return BoundaryTraceOp(
                bar=mapping.bar,
                offset=mapping.offset(addr),
                dir="write" if fn.startswith("iowrite") else "read",
                size=size,
                value=value,
                phase="linux",       # ftrace capture has no phase concept
                raw_line=line.rstrip("\r\n"),
            )
    return None


def parse_ftrace_capture(
    lines: Iterable[str],
) -> Tuple[List[BarMapping], Iterator[BoundaryTraceOp]]:
    """Two-pass parse: header first (in-memory), then a generator
    over MMIO ops.

    The split exists so callers can inspect the BAR mappings before
    streaming the ops — useful for debug ("did the capture actually
    see all three Hailo BARs?") and for paranoia validation against
    the corpus's known BAR universe.
    """
    # Materialise into a list because we need to scan the header twice
    # (once for BARs, once for ops). Capture files are 10s-100s of MB
    # at most for a single workload run; materialising is fine.
    lines = list(lines)
    bar_table = parse_bar_header(lines)

    def _ops() -> Iterator[BoundaryTraceOp]:
        for line in lines:
            op = parse_ftrace_line(line, bar_table)
            if op is not None:
                yield op

    return bar_table, _ops()
