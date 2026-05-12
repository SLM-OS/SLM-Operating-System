import unittest

import conftest  # noqa: F401 — path injection

from hailo_re_driver.line_protocols import (
    DivergenceReport,
    ExtendRequest,
    ExtendResponse,
    LineProtocolError,
    parse_any,
)


SHA = "ad007df819581b493bcb1fae00f131fef176713b"


class ExtendRequestTests(unittest.TestCase):
    def test_parse_canonical_line(self) -> None:
        line = ("HAILO_RE_CORPUS_EXTEND seq=128 bar=4 offset=3204 "
                "size=4 reason=unknown_read")
        req = ExtendRequest.parse(line)
        self.assertEqual(req.seq, 128)
        self.assertEqual(req.bar, 4)
        self.assertEqual(req.offset, 3204)
        self.assertEqual(req.size, 4)
        self.assertEqual(req.reason, "unknown_read")

    def test_round_trip(self) -> None:
        req = ExtendRequest(seq=1, bar=4, offset=0, size=4,
                            reason="unknown_read")
        self.assertEqual(ExtendRequest.parse(req.emit()), req)

    def test_unknown_reason_tag_is_accepted(self) -> None:
        # Spec: tools must accept future reason tags.
        line = ("HAILO_RE_CORPUS_EXTEND seq=1 bar=4 offset=0 size=4 "
                "reason=novel_tag")
        self.assertEqual(ExtendRequest.parse(line).reason, "novel_tag")

    def test_wrong_prefix_rejected(self) -> None:
        with self.assertRaises(LineProtocolError):
            ExtendRequest.parse("HAILO_RE_CORPUS_RESPONSE seq=1 bar=4 ...")

    def test_missing_field_rejected(self) -> None:
        with self.assertRaises(LineProtocolError):
            ExtendRequest.parse(
                "HAILO_RE_CORPUS_EXTEND seq=1 bar=4 size=4 reason=x"
            )

    def test_non_integer_seq_rejected(self) -> None:
        with self.assertRaises(LineProtocolError):
            ExtendRequest.parse(
                "HAILO_RE_CORPUS_EXTEND seq=abc bar=4 offset=0 size=4 "
                "reason=unknown_read"
            )


class ExtendResponseTests(unittest.TestCase):
    def test_parse_canonical_line(self) -> None:
        line = (
            f"HAILO_RE_CORPUS_RESPONSE seq=128 bar=4 offset=3204 size=4 "
            f"value=42000000 slmos_sha={SHA}"
        )
        resp = ExtendResponse.parse(line)
        self.assertEqual(resp.seq, 128)
        self.assertEqual(resp.bar, 4)
        self.assertEqual(resp.offset, 3204)
        self.assertEqual(resp.size, 4)
        self.assertEqual(resp.value, "42000000")
        self.assertEqual(resp.slmos_sha, SHA)

    def test_round_trip(self) -> None:
        resp = ExtendResponse(seq=1, bar=4, offset=0, size=4,
                              value="deadbeef", slmos_sha=SHA)
        self.assertEqual(ExtendResponse.parse(resp.emit()), resp)

    def test_abbreviated_sha_rejected(self) -> None:
        line = (
            "HAILO_RE_CORPUS_RESPONSE seq=1 bar=4 offset=0 size=4 "
            "value=00000000 slmos_sha=ad007df8"
        )
        with self.assertRaises(LineProtocolError):
            ExtendResponse.parse(line)

    def test_uppercase_hex_value_rejected(self) -> None:
        line = (
            f"HAILO_RE_CORPUS_RESPONSE seq=1 bar=4 offset=0 size=4 "
            f"value=DEADBEEF slmos_sha={SHA}"
        )
        with self.assertRaises(LineProtocolError):
            ExtendResponse.parse(line)

    def test_value_length_must_match_size(self) -> None:
        # size=4 => value must be 8 hex chars
        line = (
            f"HAILO_RE_CORPUS_RESPONSE seq=1 bar=4 offset=0 size=4 "
            f"value=00 slmos_sha={SHA}"
        )
        with self.assertRaises(LineProtocolError):
            ExtendResponse.parse(line)


class DivergenceReportTests(unittest.TestCase):
    def test_parse_canonical_line(self) -> None:
        line = (
            "HAILO_RE_CORPUS_DIVERGENCE seq=87 bar=4 offset=3072 size=4 "
            "dir=write expected=01000000 observed=03000000 source=qemu "
            "reason=write_value_mismatch"
        )
        rep = DivergenceReport.parse(line)
        self.assertEqual(rep.seq, 87)
        self.assertEqual(rep.dir, "write")
        self.assertEqual(rep.expected, "01000000")
        self.assertEqual(rep.observed, "03000000")
        self.assertEqual(rep.source, "qemu")
        self.assertEqual(rep.reason, "write_value_mismatch")

    def test_round_trip(self) -> None:
        rep = DivergenceReport(
            seq=1, bar=4, offset=0, size=4, dir="read",
            expected="00000000", observed="01000000",
            source="slmos", reason="inline_read_mismatch",
        )
        self.assertEqual(DivergenceReport.parse(rep.emit()), rep)

    def test_unknown_reason_accepted(self) -> None:
        line = (
            "HAILO_RE_CORPUS_DIVERGENCE seq=1 bar=4 offset=0 size=4 "
            "dir=read expected=00000000 observed=01000000 "
            "source=qemu reason=future_tag"
        )
        rep = DivergenceReport.parse(line)
        self.assertEqual(rep.reason, "future_tag")

    def test_bad_direction_rejected(self) -> None:
        line = (
            "HAILO_RE_CORPUS_DIVERGENCE seq=1 bar=4 offset=0 size=4 "
            "dir=sideways expected=00000000 observed=01000000 "
            "source=qemu reason=x"
        )
        with self.assertRaises(LineProtocolError):
            DivergenceReport.parse(line)


class ParseAnyTests(unittest.TestCase):
    def test_dispatch(self) -> None:
        ext = parse_any(
            "HAILO_RE_CORPUS_EXTEND seq=1 bar=4 offset=0 size=4 "
            "reason=unknown_read"
        )
        self.assertIsInstance(ext, ExtendRequest)
        resp = parse_any(
            f"HAILO_RE_CORPUS_RESPONSE seq=1 bar=4 offset=0 size=4 "
            f"value=00000000 slmos_sha={SHA}"
        )
        self.assertIsInstance(resp, ExtendResponse)
        div = parse_any(
            "HAILO_RE_CORPUS_DIVERGENCE seq=1 bar=4 offset=0 size=4 "
            "dir=read expected=00000000 observed=01000000 source=qemu "
            "reason=x"
        )
        self.assertIsInstance(div, DivergenceReport)

    def test_unknown_returns_none(self) -> None:
        self.assertIsNone(parse_any("kernel: hello world"))
        self.assertIsNone(parse_any(""))


if __name__ == "__main__":
    unittest.main()
