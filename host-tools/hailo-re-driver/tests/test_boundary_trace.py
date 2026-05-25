"""Regression coverage for the kernel boundary-trace line parser.

The kernel's `hailo trace` framework emits one column-padded line per
instrumented op (`kernel/ai_accel/hailo/hailo_trace.c`). The parser
must accept the live format unchanged and reject everything that
isn't an MMIO op so a divergence diff can run against the corpus
without false matches from PCI cfg / RPC framing / IRQ delivery lines.
"""

from __future__ import annotations

import unittest

import conftest  # noqa: F401

from hailo_re_driver.boundary_trace import (
    BoundaryTraceOp,
    parse_trace_line,
    parse_trace_stream,
)


class ParseTraceLineTests(unittest.TestCase):
    """One assertion per behaviour — these are the live-format contract
    the kernel emit helpers in hailo_trace.c enforce."""

    def test_parses_mmio_write_32(self) -> None:
        """The kernel emits the uint32 as integer-as-hex
        (`%08x` → "12345678" for 0x12345678). The corpus stores wire-
        byte order (little-endian: that same 0x12345678 hits the bus
        as the 4-byte sequence 78 56 34 12, recorded as "78563412").
        Parser byte-swaps so trace ops compare directly against the
        corpus encoding."""
        line = "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x0098 val=0x12345678"
        op = parse_trace_line(line)
        self.assertIsNotNone(op)
        assert op is not None  # for mypy
        self.assertEqual(op.bar, 0)
        self.assertEqual(op.offset, 0x98)
        self.assertEqual(op.dir, "write")
        self.assertEqual(op.size, 4)
        self.assertEqual(op.value, "78563412")  # LE byte order
        self.assertEqual(op.phase, "link")

    def test_parses_mmio_read_32(self) -> None:
        line = "[trc] phase=fw_boot    mech=MMIO RD32 bar=4 off=0x0640 val=0x00000000"
        op = parse_trace_line(line)
        self.assertIsNotNone(op)
        assert op is not None
        self.assertEqual(op.dir, "read")
        self.assertEqual(op.bar, 4)
        self.assertEqual(op.offset, 0x640)
        # All zeros is a palindrome under byte-swap; still asserts the
        # field is present + canonical.
        self.assertEqual(op.value, "00000000")
        self.assertEqual(op.phase, "fw_boot")

    def test_value_byte_swapped_for_le_wire_order(self) -> None:
        """The fix found on the first end-to-end native-flow capture:
        every BAR0 ATR-programming write (`val=0x00000017`, ...) was
        silently value-mismatching against the corpus's wire-order
        encoding (`17000000`, ...) until the parser byte-swapped. Pin
        the conversion direction so a refactor can't reintroduce it."""
        line = "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x0700 val=0x00000017"
        op = parse_trace_line(line)
        assert op is not None
        self.assertEqual(op.value, "17000000")

    def test_value_zero_padded_then_swapped(self) -> None:
        """A short hex (`val=0x1` for a 32-bit op) must canonicalise to
        '00000001' as integer-as-hex first, THEN byte-swap to
        '01000000' to match the corpus's wire-order encoding for an
        integer-1 store."""
        line = "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x0098 val=0x1"
        op = parse_trace_line(line)
        assert op is not None
        self.assertEqual(op.value, "01000000")

    def test_value_lowercased(self) -> None:
        """The corpus uses lowercase hex; the trace might emit either
        case depending on uart_printf's `%x` form. Parser canonicalises
        so the diff need not branch."""
        line = "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x00AB val=0xCAFEBABE"
        op = parse_trace_line(line)
        assert op is not None
        self.assertEqual(op.offset, 0xAB)
        # Lowercased AND byte-swapped: 0xCAFEBABE → "cafebabe" →
        # bytes "be ba fe ca" → "bebafeca".
        self.assertEqual(op.value, "bebafeca")

    def test_phase_preserved_for_diagnostics(self) -> None:
        line = "[trc] phase=inference  mech=MMIO RD32 bar=2 off=0x0050 val=0x00000003"
        op = parse_trace_line(line)
        assert op is not None
        self.assertEqual(op.phase, "inference")

    def test_raw_line_preserved_without_trailing_newline(self) -> None:
        line = "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x0098 val=0x12345678\r\n"
        op = parse_trace_line(line)
        assert op is not None
        # Trailing CR/LF must NOT leak into the captured raw_line so
        # error messages quoting it stay one-liner clean.
        self.assertFalse(op.raw_line.endswith("\n"))
        self.assertFalse(op.raw_line.endswith("\r"))

    def test_skips_non_mmio_mech(self) -> None:
        """`mech=` values other than MMIO (cfg, rx, irq, dma, udelay)
        must return None — the corpus only contains BAR MMIO ops."""
        for line in [
            "[trc] phase=link       mech=cfg  CFG_WR bdf=0001 off=0x0004 width=4 val=0x12345678",
            "[trc] phase=postboot   mech=rpc  tx op=4 len=8 cpu=APP",
            "[trc] phase=postboot   mech=irq  spi=42 istatus=0x1 imask=0xff",
            "[trc] phase=inference  mech=dma  arm ch=2 base=0x100",
            "[trc] phase=fw_boot    mech=BUSY udelay us=500 tag=fw-magic",
        ]:
            with self.subTest(line=line):
                self.assertIsNone(parse_trace_line(line))

    def test_skips_completely_unrelated_serial_output(self) -> None:
        """The capture file mixes trace lines with shell prompts, boot
        banners, and hailo replay-step diagnostics — all must skip
        without raising. Streaming-parse depends on this being silent."""
        for line in [
            "",
            "slmos>",
            "hailo: probe: link up at gen2 x1",
            "INFO hailo_re_driver.loop: iter=1 corpus=... last_validated_seq=2",
            "HAILO_RE_CORPUS_RESPONSE seq=128 bar=4 offset=3204 size=4 value=42000000 slmos_sha=abc",
            # close-to-format-but-wrong lines that must still skip:
            "[trc] phase=link mech=MMIO WR32",                # truncated
            "[trc] phase=link mech=MMIO bar=0 off=0x98",      # missing op
            "[xyz] phase=link mech=MMIO WR32 bar=0 off=0x98 val=0x1",  # wrong prefix
        ]:
            with self.subTest(line=line):
                self.assertIsNone(parse_trace_line(line))

    def test_rejects_value_wider_than_op_size(self) -> None:
        """Over-wide hex (e.g. 9 chars for a 4-byte op) must NOT be
        silently kept — the corpus stores 8 chars and a wider trace
        value would mysteriously mismatch. Fail parsing loudly so the
        operator sees the format-divergence, not a confusing value
        miss in the diff report."""
        for line in [
            # 9 hex chars for WR32 (max 8):
            "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x0098 val=0x123456789",
            # 17 chars for RD32:
            "[trc] phase=link       mech=MMIO RD32 bar=0 off=0x0098 val=0x12345678abcdef012",
        ]:
            with self.subTest(line=line):
                self.assertIsNone(parse_trace_line(line))

    def test_rejects_zero_or_unaligned_width(self) -> None:
        """`WR0` / `RD7` aren't valid widths. The kernel doesn't emit
        these, but reject them rather than coerce to a nonsense byte
        count — corrupted serial output is more likely than such a
        kernel-side bug, and corruption shouldn't silently land in a
        diff result."""
        for line in [
            "[trc] phase=link       mech=MMIO WR0  bar=0 off=0x0098 val=0x12345678",
            "[trc] phase=link       mech=MMIO RD7  bar=0 off=0x0098 val=0x12345678",
        ]:
            with self.subTest(line=line):
                self.assertIsNone(parse_trace_line(line))


class ParseTraceStreamTests(unittest.TestCase):
    """The streaming helper must skip non-MMIO lines silently — typical
    capture files have ~10:1 noise:signal so a stream that raised on
    every cfg/rpc line would be unusable."""

    def test_filters_mmio_lines_from_mixed_stream(self) -> None:
        lines = [
            "boot banner",
            "[trc] phase=link       mech=MMIO WR32 bar=0 off=0x0098 val=0x00000001",
            "[trc] phase=link       mech=cfg  CFG_RD bdf=0001 off=0x0004 width=4 val=0x12",
            "[trc] phase=link       mech=MMIO RD32 bar=0 off=0x0098 val=0x00000001",
            "slmos>",
            "[trc] phase=fw_boot    mech=MMIO WR32 bar=4 off=0x1018 val=0xdeadbeef",
        ]
        ops = list(parse_trace_stream(lines))
        self.assertEqual(len(ops), 3)
        self.assertEqual([op.dir for op in ops], ["write", "read", "write"])
        self.assertEqual([op.offset for op in ops], [0x98, 0x98, 0x1018])
        # 0xdeadbeef → byte-swapped to LE wire order:
        # bytes ef be ad de → "efbeadde".
        self.assertEqual(ops[2].value, "efbeadde")
        self.assertEqual(ops[2].phase, "fw_boot")

    def test_empty_input_yields_nothing(self) -> None:
        self.assertEqual(list(parse_trace_stream([])), [])

    def test_no_mmio_lines_yields_nothing(self) -> None:
        lines = [
            "slmos>",
            "[trc] phase=link       mech=cfg  CFG_RD bdf=0001 off=0x0004 width=4 val=0x12",
            "hailo: probe: link up",
        ]
        self.assertEqual(list(parse_trace_stream(lines)), [])


if __name__ == "__main__":
    unittest.main()
