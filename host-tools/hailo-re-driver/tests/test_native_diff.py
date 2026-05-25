"""Regression coverage for native-flow vs corpus diff alignment.

The diff has to tolerate three real-world skews between SLM-OS's
native Hailo driver and the HailoRT-derived corpus:

  1. Extra ops in trace (SLM-OS does cache maintenance / sanity reads
     HailoRT doesn't).
  2. Missing ops in trace (HailoRT does init steps SLM-OS skips —
     this is what we WANT to surface for #682 reopen).
  3. Same wire location, different value (status registers; some
     reads of e.g. interrupt-counters legitimately differ between
     captures).

These tests pin the alignment behaviour for each case. The bigger
question — "does this find the actual #682 ch=2 divergence in a real
trace" — needs a hardware capture and is out of unit-test scope.
"""

from __future__ import annotations

import unittest
from typing import List

import conftest  # noqa: F401

from hailo_re_driver.boundary_trace import BoundaryTraceOp
from hailo_re_driver.corpus import OpEntry
from hailo_re_driver.native_diff import (
    NativeDiffReport,
    diff_native_against_corpus,
    format_report,
)


def _c(seq: int, bar: int, off: int, dir: str, val: str,
       size: int = 4) -> OpEntry:
    """Compact corpus-entry builder for tests."""
    return OpEntry(
        seq=seq, bar=bar, offset=off, size=size, dir=dir, value=val,
        source="qemu_capture" if dir == "write" else "slmos_observed",
        validated_at_commit=None, validated_at=None,
    )


def _t(bar: int, off: int, dir: str, val: str,
       size: int = 4, phase: str = "fw_boot") -> BoundaryTraceOp:
    """Compact trace-op builder for tests."""
    return BoundaryTraceOp(
        bar=bar, offset=off, dir=dir, size=size, value=val,
        phase=phase, raw_line="<test>",
    )


class CleanMatchTests(unittest.TestCase):
    def test_identical_sequences_report_no_divergence(self) -> None:
        corpus = [_c(1, 0, 0x98, "write", "00000001"),
                  _c(2, 0, 0x98, "read",  "00000001"),
                  _c(3, 4, 0x640, "read", "00000000")]
        trace = [_t(0, 0x98, "write", "00000001"),
                 _t(0, 0x98, "read",  "00000001"),
                 _t(4, 0x640, "read", "00000000")]
        report = diff_native_against_corpus(corpus, trace)
        self.assertFalse(report.diverged)
        self.assertEqual(report.matched_op_count, 3)
        self.assertEqual(report.value_mismatches, [])
        self.assertEqual(report.asymmetric_skips, [])

    def test_empty_corpus_and_empty_trace(self) -> None:
        report = diff_native_against_corpus([], [])
        self.assertFalse(report.diverged)
        self.assertEqual(report.matched_op_count, 0)


class HardDivergenceTests(unittest.TestCase):
    """A position with no resync within the look-ahead window is the
    actionable finding — it's where SLM-OS native takes a different
    branch from HailoRT and we can't pretend they're on the same path."""

    def test_first_op_mismatch_with_no_resync_diverges(self) -> None:
        corpus = [_c(1, 0, 0x98, "write", "00000001"),
                  _c(2, 0, 0x9c, "write", "00000002"),
                  _c(3, 0, 0xa0, "write", "00000003")]
        # Trace touches completely unrelated addresses → no shape match
        # found within lookahead → hard divergence at idx 0.
        trace = [_t(2, 0x40, "write", "ffffffff"),
                 _t(2, 0x44, "write", "ffffffff"),
                 _t(2, 0x48, "write", "ffffffff")]
        report = diff_native_against_corpus(corpus, trace, lookahead=4)
        self.assertTrue(report.diverged)
        self.assertEqual(report.first_divergent_corpus_idx, 0)
        self.assertEqual(report.first_divergent_trace_idx, 0)
        assert report.first_divergent_corpus_op is not None
        self.assertEqual(report.first_divergent_corpus_op.offset, 0x98)

    def test_divergence_after_clean_prefix_reports_correct_index(self) -> None:
        """If the first N ops match, the reported divergence index must
        point at the FIRST hard mismatch — not the last clean match."""
        common = [_c(i + 1, 0, 0x100 + i * 4, "write", f"{i:08x}")
                  for i in range(5)]
        # Trace replicates the first 5, then takes a different path.
        trace = [_t(0, 0x100 + i * 4, "write", f"{i:08x}") for i in range(5)]
        trace += [_t(2, 0x40, "write", "ffffffff")]  # divergence
        corpus = common + [_c(6, 0, 0x114, "write", "00000005"),
                           _c(7, 0, 0x118, "write", "00000006")]
        report = diff_native_against_corpus(corpus, trace, lookahead=2)
        self.assertTrue(report.diverged)
        self.assertEqual(report.first_divergent_corpus_idx, 5)
        self.assertEqual(report.first_divergent_trace_idx, 5)
        self.assertEqual(report.matched_op_count, 5)


class ValueMismatchTests(unittest.TestCase):
    """Same wire-location, different value — soft divergence. Recorded
    + walk continues, because the most common cause is a status
    register that flipped between captures, not a protocol bug."""

    def test_value_mismatch_recorded_and_walk_continues(self) -> None:
        corpus = [_c(1, 0, 0x98, "write", "00000001"),
                  _c(2, 0, 0x9c, "read", "00000002"),  # value differs
                  _c(3, 0, 0xa0, "write", "00000003")]
        trace = [_t(0, 0x98, "write", "00000001"),
                 _t(0, 0x9c, "read", "deadbeef"),  # native saw different
                 _t(0, 0xa0, "write", "00000003")]
        report = diff_native_against_corpus(corpus, trace)
        self.assertFalse(report.diverged)
        self.assertEqual(report.matched_op_count, 2)  # the 2 that matched fully
        self.assertEqual(len(report.value_mismatches), 1)
        vm = report.value_mismatches[0]
        self.assertEqual(vm.corpus_idx, 1)
        self.assertEqual(vm.trace_idx, 1)
        self.assertEqual(vm.corpus_op.value, "00000002")
        self.assertEqual(vm.trace_op.value, "deadbeef")


class AsymmetricSkipTests(unittest.TestCase):
    """Bounded look-ahead lets us resync when one side has a small
    burst of extra ops. Look-ahead can't be too wide or it papers
    over real divergences as "skips" — keep the default narrow."""

    def test_extra_ops_in_trace_are_skipped_and_recorded(self) -> None:
        """SLM-OS native often emits cache-maintenance or sanity-read
        ops HailoRT didn't. The resync should look ahead in TRACE,
        skip the extras, and continue matching."""
        corpus = [_c(1, 0, 0x98, "write", "00000001"),
                  _c(2, 0, 0x9c, "write", "00000002"),
                  _c(3, 0, 0xa0, "write", "00000003")]
        trace = [_t(0, 0x98, "write", "00000001"),
                 _t(0, 0x500, "read", "0badbabe"),  # extra noise op
                 _t(0, 0x504, "read", "deadcafe"),  # extra noise op
                 _t(0, 0x9c, "write", "00000002"),
                 _t(0, 0xa0, "write", "00000003")]
        report = diff_native_against_corpus(corpus, trace, lookahead=4)
        self.assertFalse(report.diverged)
        self.assertEqual(report.matched_op_count, 3)
        self.assertEqual(len(report.asymmetric_skips), 1)
        skip = report.asymmetric_skips[0]
        self.assertEqual(skip.side, "trace")
        self.assertEqual(skip.skipped_ops_count, 2)
        self.assertEqual(skip.corpus_idx, 1)  # corpus expected 0x9c next

    def test_extra_ops_in_corpus_are_skipped_and_recorded(self) -> None:
        """HailoRT does init steps SLM-OS skips. Look-ahead in CORPUS
        finds the resync point. Recording this as a `corpus`-side skip
        is the actionable signal — these are ops we KNOW HailoRT did
        that our native driver isn't doing, which for the #682 reopen
        is exactly what we want to surface."""
        corpus = [_c(1, 0, 0x98, "write", "00000001"),
                  _c(2, 0, 0x100, "write", "babeface"),  # skipped by native
                  _c(3, 0, 0x104, "write", "12345678"),  # skipped by native
                  _c(4, 0, 0x9c, "write", "00000002"),
                  _c(5, 0, 0xa0, "write", "00000003")]
        trace = [_t(0, 0x98, "write", "00000001"),
                 _t(0, 0x9c, "write", "00000002"),
                 _t(0, 0xa0, "write", "00000003")]
        report = diff_native_against_corpus(corpus, trace, lookahead=4)
        self.assertFalse(report.diverged)
        self.assertEqual(report.matched_op_count, 3)
        self.assertEqual(len(report.asymmetric_skips), 1)
        skip = report.asymmetric_skips[0]
        self.assertEqual(skip.side, "corpus")
        self.assertEqual(skip.skipped_ops_count, 2)
        self.assertEqual(skip.trace_idx, 1)  # trace already at 0x9c

    def test_lookahead_bound_is_respected(self) -> None:
        """A gap wider than `lookahead` must produce a HARD divergence,
        not silently skip past it. Otherwise the diff would paper over
        the very thing it exists to find."""
        corpus = [_c(1, 0, 0x98, "write", "00000001")]
        # Insert (lookahead+1) noise ops in the trace before the
        # corpus op shows up → no resync within window.
        trace = ([_t(0, 0x500 + i * 4, "read", "00000000")
                  for i in range(5)] +
                 [_t(0, 0x98, "write", "00000001")])
        report = diff_native_against_corpus(corpus, trace, lookahead=4)
        self.assertTrue(report.diverged)
        self.assertEqual(report.first_divergent_corpus_idx, 0)

    def test_tail_imbalance_recorded_not_divergence(self) -> None:
        """If we walk off one end with a clean tail on the other, that's
        a soft skip on the longer side, not a hard divergence — the
        run may just have stopped early."""
        corpus = [_c(1, 0, 0x98, "write", "00000001"),
                  _c(2, 0, 0x9c, "write", "00000002"),
                  _c(3, 0, 0xa0, "write", "00000003")]
        trace = [_t(0, 0x98, "write", "00000001")]  # trace stops short
        report = diff_native_against_corpus(corpus, trace)
        self.assertFalse(report.diverged)
        self.assertEqual(report.matched_op_count, 1)
        self.assertEqual(len(report.asymmetric_skips), 1)
        skip = report.asymmetric_skips[0]
        self.assertEqual(skip.side, "corpus")
        self.assertEqual(skip.skipped_ops_count, 2)


class FormatReportTests(unittest.TestCase):
    """The formatted report is meant to be paste-into-ticket
    actionable. Pin the structure so a future refactor doesn't break
    the consumers it was designed for."""

    def test_clean_match_renders_summary(self) -> None:
        corpus = [_c(1, 0, 0x98, "write", "00000001")]
        trace = [_t(0, 0x98, "write", "00000001")]
        report = diff_native_against_corpus(corpus, trace)
        text = format_report(report)
        self.assertIn("matched ops:", text)
        self.assertIn("clean match", text)
        self.assertNotIn("DIVERGED", text)

    def test_divergence_renders_both_sides_with_addresses(self) -> None:
        corpus = [_c(7, 0, 0x98, "write", "00000001")]
        trace = [_t(2, 0x40, "read", "ffffffff")]
        report = diff_native_against_corpus(corpus, trace)
        text = format_report(report)
        self.assertIn("DIVERGED", text)
        self.assertIn("seq=7", text)        # corpus seq for cross-ref
        self.assertIn("bar=0", text)
        self.assertIn("bar=2", text)
        self.assertIn("0x0098", text)
        self.assertIn("0x0040", text)


if __name__ == "__main__":
    unittest.main()
