import tempfile
import unittest
from pathlib import Path

import conftest  # noqa: F401

from hailo_re_driver import corpus as corpus_mod
from hailo_re_driver import diff as diff_mod
from hailo_re_driver import rollback as rollback_mod
from hailo_re_driver.corpus import Header, OpEntry


SHA_GOOD = "ad007df819581b493bcb1fae00f131fef176713b"
SHA_BAD = "ffffffffffffffffffffffffffffffffffffffff"


def header() -> Header:
    return Header(
        format_version=1,
        hailort_version="4.23.0",
        fw_version="4.23.0",
        capture_host="qemu-x86_64",
        slmos_base_sha=SHA_GOOD,
        capture_started_at="2026-05-12T18:30:00Z",
    )


def validated_read(seq: int, value: str, sha: str = SHA_GOOD) -> OpEntry:
    return OpEntry(
        seq=seq, bar=4, offset=2308, size=4, dir="read",
        value=value, source="slmos_observed",
        validated_at_commit=sha,
        validated_at="2026-05-12T19:00:00Z",
    )


def tentative_read(seq: int, value: str) -> OpEntry:
    return OpEntry(
        seq=seq, bar=4, offset=2308, size=4, dir="read",
        value=value, source="slmos_observed",
        validated_at_commit=None, validated_at=None,
    )


class RollbackTests(unittest.TestCase):
    def test_find_rollback_target_picks_last_reachable(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c-2026-05-12.jsonl"
            corpus = corpus_mod.init(p, header())
            corpus_mod.append_op(corpus, validated_read(1, "00000000"))
            corpus_mod.append_op(corpus, validated_read(2, "00000000"))
            corpus_mod.append_op(corpus, validated_read(3, "01000000",
                                                       sha=SHA_BAD))
            corpus_mod.append_op(corpus, tentative_read(4, "02000000"))

            target = rollback_mod.find_rollback_target(
                corpus, reachable=lambda sha: sha == SHA_GOOD
            )
            self.assertEqual(target, 2)

    def test_handle_writes_new_file_and_renames_poisoned(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c-2026-05-12.jsonl"
            corpus = corpus_mod.init(p, header())
            corpus_mod.append_op(corpus, validated_read(1, "00000000"))
            corpus_mod.append_op(corpus, validated_read(2, "00000000"))
            corpus_mod.append_op(corpus, tentative_read(3, "deadbeef"))

            result = rollback_mod.handle(
                corpus,
                divergent_seq=3,
                reachable=lambda sha: sha == SHA_GOOD,
                reason="test_divergence",
            )
            self.assertEqual(result.rollback_to_seq, 2)
            self.assertTrue(result.poisoned_path.exists())
            self.assertTrue(result.new_corpus_path.exists())
            self.assertFalse(p.exists())

            fresh = corpus_mod.load(result.new_corpus_path)
            self.assertEqual(len(fresh.ops), 2)
            self.assertEqual(fresh.ops[-1].seq, 2)
            self.assertIsNotNone(fresh.trailer)
            self.assertEqual(fresh.trailer.extras["rollback_from_seq"], 3)
            self.assertEqual(fresh.trailer.extras["rollback_to_seq"], 2)

    def test_handle_refuses_when_no_validated_frontier(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "c-2026-05-12.jsonl"
            corpus = corpus_mod.init(p, header())
            corpus_mod.append_op(corpus, tentative_read(1, "00000000"))
            with self.assertRaises(Exception):
                rollback_mod.handle(
                    corpus,
                    divergent_seq=1,
                    reachable=lambda sha: True,
                    reason="x",
                )


class DiffTests(unittest.TestCase):
    def test_identical_corpora(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            left = Path(d) / "left.jsonl"
            right = Path(d) / "right.jsonl"
            for path in (left, right):
                c = corpus_mod.init(path, header())
                corpus_mod.append_op(c, validated_read(1, "00000000"))
                corpus_mod.append_op(c, validated_read(2, "01000000"))
            report = diff_mod.diff_corpora(left, right)
            self.assertFalse(report.diverged)

    def test_value_mismatch_reports_first_divergent_seq(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            left = Path(d) / "left.jsonl"
            right = Path(d) / "right.jsonl"
            l = corpus_mod.init(left, header())
            r = corpus_mod.init(right, header())
            corpus_mod.append_op(l, validated_read(1, "00000000"))
            corpus_mod.append_op(r, validated_read(1, "00000000"))
            corpus_mod.append_op(l, validated_read(2, "01000000"))
            corpus_mod.append_op(r, validated_read(2, "02000000"))  # diff
            report = diff_mod.diff_corpora(left, right)
            self.assertEqual(report.first_divergent_seq, 2)


if __name__ == "__main__":
    unittest.main()
