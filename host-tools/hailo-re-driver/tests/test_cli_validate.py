"""Unit tests for `hailo_re_driver.cli.validate_main`.

Exercises the four observable behaviours added in the review polish rounds:
1. duplicate-seq observed-trace -> exit 2
2. partial-coverage observed-trace -> exit 2
3. divergent observed-trace -> rollback triggered, exit 1
4. clean observed-trace -> stamp validated_at_commit on every entry, exit 0
"""

from __future__ import annotations

import json
import tempfile
import unittest
from contextlib import contextmanager
from io import StringIO
from pathlib import Path
from unittest.mock import patch

import conftest  # noqa: F401

from hailo_re_driver import corpus as corpus_mod
from hailo_re_driver.cli import validate_main
from hailo_re_driver.corpus import Header, OpEntry


SHA_GOOD = "ad007df819581b493bcb1fae00f131fef176713b"


def header() -> Header:
    return Header(
        format_version=1,
        hailort_version="4.23.0",
        fw_version="4.23.0",
        capture_host="qemu-x86_64",
        slmos_base_sha=SHA_GOOD,
        capture_started_at="2026-05-12T18:30:00Z",
    )


def write_entry(seq: int, value: str = "01000000") -> OpEntry:
    return OpEntry(
        seq=seq, bar=4, offset=2304, size=4, dir="write",
        value=value, source="qemu_capture",
        validated_at_commit=None, validated_at=None,
    )


def read_entry(seq: int, value: str = "00000000",
               validated: bool = False) -> OpEntry:
    return OpEntry(
        seq=seq, bar=4, offset=2308, size=4, dir="read",
        value=value, source="slmos_observed",
        validated_at_commit=SHA_GOOD if validated else None,
        validated_at="2026-05-12T19:00:00Z" if validated else None,
    )


def make_corpus(path: Path, ops: list[OpEntry]) -> None:
    c = corpus_mod.init(path, header())
    for op in ops:
        corpus_mod.append_op(c, op)


def make_trace(path: Path, ops: list[OpEntry]) -> None:
    """Write an observed-trace JSONL (no header)."""
    with path.open("w", encoding="utf-8") as f:
        for op in ops:
            f.write(json.dumps(op.to_json_obj(), separators=(",", ":")))
            f.write("\n")


@contextmanager
def capture_streams():
    """Capture stdout + stderr during validate_main."""
    out, err = StringIO(), StringIO()
    with patch("sys.stdout", out), patch("sys.stderr", err):
        yield out, err


class ValidateMainTests(unittest.TestCase):
    def test_duplicate_seq_in_trace_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            cp = Path(d) / "c.jsonl"
            tp = Path(d) / "t.jsonl"
            make_corpus(cp, [write_entry(1), read_entry(2)])
            make_trace(tp, [
                write_entry(1), write_entry(1),  # duplicate
                read_entry(2),
            ])
            with capture_streams() as (out, err):
                rc = validate_main([
                    str(cp), "--observed-trace", str(tp),
                    "--slmos-sha", SHA_GOOD,
                ])
            self.assertEqual(rc, 2)
            self.assertIn("duplicate seq", err.getvalue())

    def test_partial_coverage_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            cp = Path(d) / "c.jsonl"
            tp = Path(d) / "t.jsonl"
            make_corpus(cp, [write_entry(1), read_entry(2), write_entry(3)])
            make_trace(tp, [write_entry(1), read_entry(2)])  # missing seq=3
            with capture_streams() as (out, err):
                rc = validate_main([
                    str(cp), "--observed-trace", str(tp),
                    "--slmos-sha", SHA_GOOD,
                ])
            self.assertEqual(rc, 2)
            msg = err.getvalue()
            self.assertIn("covers 2/3", msg)
            self.assertIn("[3]", msg)

    def test_divergent_trace_triggers_rollback(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            cp = Path(d) / "c-2026-05-12.jsonl"
            tp = Path(d) / "t.jsonl"
            # Seq=1 already validated so rollback has a frontier.
            make_corpus(cp, [
                read_entry(1, "11111111", validated=True),
                read_entry(2, "22222222"),
            ])
            # Trace disagrees on seq=2 — divergence.
            make_trace(tp, [
                read_entry(1, "11111111"),
                read_entry(2, "99999999"),  # diverges
            ])
            # Don't actually call git; pretend everything is reachable.
            with patch(
                "hailo_re_driver.rollback.git_reachable_from_main",
                return_value=lambda _sha: True,
            ), capture_streams() as (out, err):
                rc = validate_main([
                    str(cp), "--observed-trace", str(tp),
                    "--slmos-sha", SHA_GOOD,
                ])
            self.assertEqual(rc, 1)
            self.assertIn("rolled back from seq=2 to seq=1", out.getvalue())
            # Original corpus renamed to .poisoned; new file written.
            self.assertFalse(cp.exists())
            self.assertTrue((cp.parent / (cp.name + ".poisoned")).exists())

    def test_clean_trace_stamps_and_returns_zero(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            cp = Path(d) / "c.jsonl"
            tp = Path(d) / "t.jsonl"
            ops = [write_entry(1), read_entry(2)]
            make_corpus(cp, ops)
            make_trace(tp, ops)
            with capture_streams() as (out, err):
                rc = validate_main([
                    str(cp), "--observed-trace", str(tp),
                    "--slmos-sha", SHA_GOOD,
                ])
            self.assertEqual(rc, 0)
            self.assertIn("stamped validated_at_commit", out.getvalue())
            # Reload — every entry now carries the stamp.
            reloaded = corpus_mod.load(cp)
            for op in reloaded.ops:
                self.assertEqual(op.validated_at_commit, SHA_GOOD)
                self.assertIsNotNone(op.validated_at)

    def test_bad_sha_length_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            cp = Path(d) / "c.jsonl"
            tp = Path(d) / "t.jsonl"
            make_corpus(cp, [write_entry(1)])
            make_trace(tp, [write_entry(1)])
            with capture_streams() as (out, err):
                rc = validate_main([
                    str(cp), "--observed-trace", str(tp),
                    "--slmos-sha", "tooshort",
                ])
            self.assertEqual(rc, 2)
            self.assertIn("full 40-char SHA", err.getvalue())


if __name__ == "__main__":
    unittest.main()
