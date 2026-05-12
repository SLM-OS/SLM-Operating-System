import json
import tempfile
import unittest
from pathlib import Path

import conftest  # noqa: F401

from hailo_re_driver import corpus as corpus_mod
from hailo_re_driver.corpus import CorpusError, Header, OpEntry, Trailer


SHA_A = "ad007df819581b493bcb1fae00f131fef176713b"
SHA_B = "4b571f29bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"


def make_header(notes: str = "") -> Header:
    return Header(
        format_version=1,
        hailort_version="4.23.0",
        fw_version="4.23.0",
        capture_host="qemu-x86_64-ubuntu24.04",
        slmos_base_sha=SHA_A,
        capture_started_at="2026-05-12T18:30:00Z",
        notes=notes,
    )


def make_write(seq: int, value: str = "01000000") -> OpEntry:
    return OpEntry(
        seq=seq, bar=4, offset=2304, size=4, dir="write",
        value=value, source="qemu_capture",
        validated_at_commit=None, validated_at=None,
    )


def make_read(seq: int, value: str, validated: bool = False) -> OpEntry:
    return OpEntry(
        seq=seq, bar=4, offset=2308, size=4, dir="read",
        value=value, source="slmos_observed",
        validated_at_commit=SHA_A if validated else None,
        validated_at="2026-05-12T19:00:00Z" if validated else None,
    )


class CorpusRoundTripTests(unittest.TestCase):
    def test_init_then_append_then_load(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            corpus_mod.append_op(corpus, make_write(1))
            corpus_mod.append_op(corpus, make_read(2, "00000000"))
            corpus_mod.append_op(corpus, make_read(3, "01000000", validated=True))

            reloaded = corpus_mod.load(p)
            self.assertEqual(reloaded.header, corpus.header)
            self.assertEqual(len(reloaded.ops), 3)
            self.assertEqual(reloaded.next_seq, 4)
            self.assertEqual(reloaded.last_validated_seq, 0)
            # Validation watermark only advances when EVERY prior entry is
            # validated; entry 1 has no validation stamp, so frontier stays 0.

    def test_last_validated_seq_advances_when_contiguous(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            # All three entries validated -> frontier = 3
            corpus_mod.append_op(corpus, OpEntry(
                seq=1, bar=4, offset=0, size=4, dir="write",
                value="00000000", source="qemu_capture",
                validated_at_commit=SHA_A,
                validated_at="2026-05-12T18:30:00Z",
            ))
            corpus_mod.append_op(corpus, make_read(2, "00000000", validated=True))
            corpus_mod.append_op(corpus, make_read(3, "01000000", validated=True))
            self.assertEqual(corpus.last_validated_seq, 3)


class CorpusValidationTests(unittest.TestCase):
    def test_seq_must_start_at_one(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            with self.assertRaises(CorpusError):
                corpus_mod.append_op(corpus, make_write(2))

    def test_seq_must_be_monotonic(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            corpus_mod.append_op(corpus, make_write(1))
            with self.assertRaises(CorpusError):
                corpus_mod.append_op(corpus, make_write(3))  # skipped 2
            with self.assertRaises(CorpusError):
                corpus_mod.append_op(corpus, make_write(1))  # duplicate

    def test_value_length_must_match_size(self) -> None:
        bad = OpEntry(
            seq=1, bar=4, offset=0, size=4, dir="write",
            value="01", source="qemu_capture",
            validated_at_commit=None, validated_at=None,
        )
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            with self.assertRaises(CorpusError):
                corpus_mod.append_op(corpus, bad)

    def test_validation_pair_consistency(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            bad = OpEntry(
                seq=1, bar=4, offset=0, size=4, dir="write",
                value="00000000", source="qemu_capture",
                validated_at_commit=SHA_A, validated_at=None,
            )
            with self.assertRaises(CorpusError):
                corpus_mod.append_op(corpus, bad)

    def test_load_rejects_seq_gap(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            with p.open("w") as f:
                f.write(json.dumps(make_header().to_json_obj()) + "\n")
                f.write(json.dumps(make_write(1).to_json_obj()) + "\n")
                # skip seq=2 — should be rejected on load
                f.write(json.dumps(make_write(3).to_json_obj()) + "\n")
            with self.assertRaises(CorpusError):
                corpus_mod.load(p)

    def test_load_requires_supported_format_version(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            header = make_header().to_json_obj()
            header["format_version"] = 99
            with p.open("w") as f:
                f.write(json.dumps(header) + "\n")
            with self.assertRaises(CorpusError):
                corpus_mod.load(p)

    def test_cannot_append_after_trailer(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus = corpus_mod.init(p, make_header())
            corpus_mod.append_op(corpus, make_write(1))
            corpus_mod.append_trailer(
                corpus, Trailer(ended_at="2026-05-12T20:00:00Z",
                                last_seq=1, reason="test"),
            )
            with self.assertRaises(CorpusError):
                corpus_mod.append_op(corpus, make_read(2, "00000000"))

    def test_init_refuses_overwrite(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            corpus_mod.init(p, make_header())
            with self.assertRaises(CorpusError):
                corpus_mod.init(p, make_header())


class WriteFullTests(unittest.TestCase):
    def test_round_trip_with_trailer(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "corpus.jsonl"
            ops = [make_write(1), make_read(2, "00000000", validated=True)]
            trailer = Trailer(
                ended_at="2026-05-12T20:00:00Z",
                last_seq=2,
                reason="hailort_configure_complete",
            )
            corpus_mod.write_full(p, make_header(), ops, trailer)
            reloaded = corpus_mod.load(p)
            self.assertEqual(len(reloaded.ops), 2)
            self.assertIsNotNone(reloaded.trailer)
            self.assertEqual(reloaded.trailer.last_seq, 2)


if __name__ == "__main__":
    unittest.main()
