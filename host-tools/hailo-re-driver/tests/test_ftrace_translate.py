"""Regression coverage for the Linux ftrace → corpus-op translator.

The translator's contract:

  1. Parse a `# BAR<N>: phys=0x... size=0x...` header block produced by
     `scripts/capture-hailort-ftrace.sh` into `BarMapping` records.
  2. For each ftrace kprobe line, if `addr=` falls inside any BAR
     range, emit a `BoundaryTraceOp` with (bar, offset, dir, size,
     value) matching the corpus's wire encoding.
  3. Skip lines that don't match (comments, function-tracer lines
     without args, MMIO outside the Hailo BARs).

These tests pin each behaviour. The bigger question — "does this work
on a real ftrace capture from pi-5-1 in Pi OS?" — needs hardware
access and is out of unit-test scope.
"""

from __future__ import annotations

import unittest

import conftest  # noqa: F401

from hailo_re_driver.ftrace_translate import (
    BarMapping,
    parse_bar_header,
    parse_ftrace_capture,
    parse_ftrace_line,
)


# Realistic-looking capture-file headers from the script. Phys-base
# addresses are made up but match the shape Pi 5's BAR allocator
# produces (PCIe ECAM around 0xfc000000, with each BAR a power-of-two
# aligned chunk).
_SAMPLE_HEADER = [
    "# hailort ftrace capture",
    "# date: 2026-05-25T18:00:00-07:00",
    "# host: pi5",
    "# pci_device: 0000:01:00.0",
    "# bar_resources:",
    "#   BAR0: phys=0x00000000fc010000 size=0x10000 flags=0x40200",
    "#   BAR2: phys=0x00000000fc020000 size=0x40000 flags=0x40200",
    "#   BAR4: phys=0x00000000fc100000 size=0x100000 flags=0x40200",
    "# workload: hailortcli run mnist.hef",
    "# workload_rc: 0",
    "# --- ftrace data follows ---",
]


class ParseBarHeaderTests(unittest.TestCase):
    def test_parses_three_bars_in_order(self) -> None:
        bars = parse_bar_header(_SAMPLE_HEADER)
        self.assertEqual([b.bar for b in bars], [0, 2, 4])
        self.assertEqual(bars[0].phys_base, 0xFC010000)
        self.assertEqual(bars[0].size, 0x10000)
        self.assertEqual(bars[1].phys_base, 0xFC020000)
        self.assertEqual(bars[1].size, 0x40000)
        self.assertEqual(bars[2].phys_base, 0xFC100000)
        self.assertEqual(bars[2].size, 0x100000)

    def test_skips_size_zero_bars(self) -> None:
        """A device that doesn't implement (say) BAR1 lands in
        /sys/.../resource as size=0. Those rows are noise — skip them
        rather than store empty mappings that the address-→-BAR
        lookup would never match anyway."""
        lines = [
            "#   BAR0: phys=0x00000000fc010000 size=0x10000 flags=0x40200",
            "#   BAR1: phys=0x0000000000000000 size=0x0 flags=0x0",
            "#   BAR2: phys=0x00000000fc020000 size=0x40000 flags=0x40200",
        ]
        bars = parse_bar_header(lines)
        self.assertEqual([b.bar for b in bars], [0, 2])

    def test_ignores_non_bar_header_lines(self) -> None:
        """Comment lines that aren't BAR records (date, hostname,
        workload echo) must not raise and must not produce mappings —
        otherwise the parser would error on every capture."""
        bars = parse_bar_header([
            "# date: 2026-05-25",
            "# host: pi5",
            "# workload: hailortcli ...",
        ])
        self.assertEqual(bars, [])


class BarMappingTests(unittest.TestCase):
    def test_contains_and_offset(self) -> None:
        bar = BarMapping(bar=2, phys_base=0xFC020000, size=0x40000)
        self.assertTrue(bar.contains(0xFC020000))
        self.assertTrue(bar.contains(0xFC020004))
        self.assertTrue(bar.contains(0xFC05FFFF))      # last byte
        self.assertFalse(bar.contains(0xFC060000))     # one past end
        self.assertFalse(bar.contains(0xFC01FFFF))     # one before start
        self.assertEqual(bar.offset(0xFC020004), 0x04)
        self.assertEqual(bar.offset(0xFC020700), 0x700)


class ParseFtraceLineTests(unittest.TestCase):
    """Pin the kprobe line format against the shape
    `set_kprobe_events` produces on a Pi 5 / 6.6-class kernel. The
    bash capture script controls the exact format; if it changes the
    args, this regex needs to follow."""

    BARS = [
        BarMapping(bar=0, phys_base=0xFC010000, size=0x10000),
        BarMapping(bar=2, phys_base=0xFC020000, size=0x40000),
        BarMapping(bar=4, phys_base=0xFC100000, size=0x100000),
    ]

    def test_parses_iowrite32_into_bar2_op(self) -> None:
        """A kprobe-format `iowrite32_entry` with v=0x... addr=0x...
        inside BAR2's range produces a BoundaryTraceOp with the
        correct (bar, offset, dir, size, value), value byte-swapped
        to corpus wire-LE order."""
        line = (
            "hailortcli-12345 [002] d... 7892.123456: iowrite32_entry: "
            "(iowrite32+0x0/0x40) v=0x12345678 addr=0xfc020004"
        )
        op = parse_ftrace_line(line, self.BARS)
        self.assertIsNotNone(op)
        assert op is not None
        self.assertEqual(op.bar, 2)
        self.assertEqual(op.offset, 0x04)
        self.assertEqual(op.dir, "write")
        self.assertEqual(op.size, 4)
        # 0x12345678 → wire LE bytes 78 56 34 12 → "78563412"
        # (same byte-swap convention as boundary_trace.py).
        self.assertEqual(op.value, "78563412")
        self.assertEqual(op.phase, "linux")

    def test_parses_ioread32_into_bar0_op(self) -> None:
        line = (
            "hailortcli-12345 [002] d... 7892.987654: ioread32_entry: "
            "(ioread32+0x0/0x40) v=0x00000001 addr=0xfc010098"
        )
        op = parse_ftrace_line(line, self.BARS)
        assert op is not None
        self.assertEqual(op.bar, 0)
        self.assertEqual(op.offset, 0x98)
        self.assertEqual(op.dir, "read")
        self.assertEqual(op.value, "01000000")

    def test_addr_outside_any_bar_returns_none(self) -> None:
        """A different driver's MMIO (e.g. SD card controller) will
        also appear in the trace — the global iowrite/ioread filter
        catches them too. They must be silently dropped."""
        line = (
            "kworker-123 [001] d... 7892.111111: iowrite32_entry: "
            "(iowrite32+0x0/0x40) v=0xdeadbeef addr=0xfd000000"
        )
        self.assertIsNone(parse_ftrace_line(line, self.BARS))

    def test_function_tracer_line_without_args_returns_none(self) -> None:
        """The default function tracer emits `iowrite32 <-caller`
        without arguments. Useful as a sanity check that the symbol
        is being hit, but unusable for the diff — drop silently."""
        line = "hailortcli-12345 [002] d... 7892.123456: iowrite32 <-hailo_pcie_write_atr"
        self.assertIsNone(parse_ftrace_line(line, self.BARS))

    def test_comment_lines_return_none(self) -> None:
        for comment in [
            "# hailort ftrace capture",
            "# --- ftrace data follows ---",
            "",
        ]:
            with self.subTest(line=comment):
                self.assertIsNone(parse_ftrace_line(comment, self.BARS))

    def test_rejects_value_wider_than_op_size(self) -> None:
        """Parity with `boundary_trace.test_rejects_value_wider_than_op_size`.
        If the kprobe prints more hex digits than the op width can
        hold (e.g. a misconfigured `%lx` formatter on a 32-bit op),
        fail loudly — otherwise the diff would compare a too-wide
        trace value against the corpus's narrower stored value and
        mysteriously mismatch."""
        for line in [
            # 9 hex chars for iowrite32 (max 8):
            "hailortcli-1 [000] d... 1.0: iowrite32_entry: "
            "(iowrite32+0x0/0x40) v=0x123456789 addr=0xfc010098",
            # 17 chars for ioread32:
            "hailortcli-1 [000] d... 1.0: ioread32_entry: "
            "(ioread32+0x0/0x40) v=0x12345678abcdef012 addr=0xfc010098",
        ]:
            with self.subTest(line=line):
                self.assertIsNone(parse_ftrace_line(line, self.BARS))

    def test_byte_swap_at_smaller_widths(self) -> None:
        """Same width-coverage check the boundary_trace tests pin —
        ioread16 and iowrite8 must both byte-swap into wire order.
        iowrite8 is a no-op (single byte), iowrite16 swaps the two."""
        line16 = (
            "hailortcli-1 [000] d... 1.0: iowrite16_entry: "
            "(iowrite16+0x0/0x40) v=0x1234 addr=0xfc020010"
        )
        op16 = parse_ftrace_line(line16, self.BARS)
        assert op16 is not None
        self.assertEqual(op16.size, 2)
        self.assertEqual(op16.value, "3412")
        line8 = (
            "hailortcli-1 [000] d... 1.0: iowrite8_entry: "
            "(iowrite8+0x0/0x40) v=0xab addr=0xfc020012"
        )
        op8 = parse_ftrace_line(line8, self.BARS)
        assert op8 is not None
        self.assertEqual(op8.size, 1)
        self.assertEqual(op8.value, "ab")


class ParseFtraceCaptureTests(unittest.TestCase):
    def test_end_to_end_capture_with_mixed_lines(self) -> None:
        """A realistic mini-capture: header + a few Hailo-BAR MMIO ops
        + a couple of other-driver MMIO ops + comment/junk lines. The
        translator must return the correct BAR table and only the
        Hailo ops."""
        lines = _SAMPLE_HEADER + [
            "hailortcli-1 [000] d... 1.000: iowrite32_entry: "
            "(iowrite32+0x0/0x40) v=0x00000001 addr=0xfc010098",
            "kworker-99 [001] d... 1.001: iowrite32_entry: "
            "(iowrite32+0x0/0x40) v=0xdeadbeef addr=0xfd000000",
            "hailortcli-1 [000] d... 1.002: ioread32_entry: "
            "(ioread32+0x0/0x40) v=0x006e3801 addr=0xfc020000",
            "hailortcli-1 [000] d... 1.003: iowrite32_entry: "
            "(iowrite32+0x0/0x40) v=0x06000000 addr=0xfc100640",
            # function-tracer line without args — sanity hit, dropped
            "hailortcli-1 [000] d... 1.004: iowrite32 <-hailo_pcie_write_atr",
        ]
        bars, ops = parse_ftrace_capture(lines)
        self.assertEqual([b.bar for b in bars], [0, 2, 4])
        ops_list = list(ops)
        self.assertEqual(len(ops_list), 3)  # 4 MMIO lines, 1 was other-driver
        self.assertEqual([(op.bar, op.offset, op.dir) for op in ops_list], [
            (0, 0x98, "write"),
            (2, 0x00, "read"),
            (4, 0x640, "write"),
        ])

    def test_empty_capture_yields_empty(self) -> None:
        bars, ops = parse_ftrace_capture([])
        self.assertEqual(bars, [])
        self.assertEqual(list(ops), [])

    def test_capture_with_only_header_yields_no_ops(self) -> None:
        """Operator ran the capture but the workload exited before any
        MMIO happened, or all MMIO was filtered out. The translator
        must return the BAR table but no ops — not raise."""
        bars, ops = parse_ftrace_capture(_SAMPLE_HEADER)
        self.assertEqual(len(bars), 3)
        self.assertEqual(list(ops), [])


if __name__ == "__main__":
    unittest.main()
