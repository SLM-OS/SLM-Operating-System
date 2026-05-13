"""End-to-end mock-driven loop test.

Drives the Phase 1/2 bootstrap loop with:
  - a fake QEMU subprocess (sh -c "printf ... ; exit ...") that prints a
    HAILO_RE_CORPUS_EXTEND line on first run and exits clean on subsequent
    runs;
  - a fake SLM-OS runner whose `replay_step` returns a canned ExtendResponse.

The corpus is a real file on disk, exercising corpus.append_op + load.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import conftest  # noqa: F401

from hailo_re_driver import corpus as corpus_mod
from hailo_re_driver import loop as loop_mod
from hailo_re_driver.corpus import Header, OpEntry
from hailo_re_driver.line_protocols import ExtendResponse
from hailo_re_driver.qemu_runner import mock_runner_from_lines, QemuRunner, ConfigureCompleteEvent
from hailo_re_driver.slmos_runner import (
    ReplayResponse,
    SlmosRunner,
)


SHA = "ad007df819581b493bcb1fae00f131fef176713b"


def header() -> Header:
    return Header(
        format_version=1,
        hailort_version="4.23.0",
        fw_version="4.23.0",
        capture_host="qemu-x86_64",
        slmos_base_sha=SHA,
        capture_started_at="2026-05-12T18:30:00Z",
    )


class _ScriptedQemuRunner(QemuRunner):
    """Fake runner: returns canned events in sequence."""

    def __init__(self, events: list):
        super().__init__(command_factory=lambda _p: ["true"])
        self._events = list(events)

    def run(self, corpus_path):  # type: ignore[override]
        return self._events.pop(0)


class _ScriptedSlmosRunner:
    """Fake SLM-OS runner: returns canned responses in seq order."""

    def __init__(self, responses: list[ExtendResponse]):
        self._by_seq = {r.seq: r for r in responses}
        self.calls: list[tuple[Path, int]] = []

    def replay_step(self, corpus_path: Path, seq: int, *,
                    fresh_boot: bool = True):
        self.calls.append((corpus_path, seq))
        resp = self._by_seq.get(seq)
        assert resp is not None, f"no canned response for seq={seq}"
        return ReplayResponse(kind="response", response=resp, raw_line=resp.emit())


class LoopE2ETests(unittest.TestCase):
    def _make_corpus(self, d: Path) -> Path:
        p = d / "c-2026-05-12.jsonl"
        c = corpus_mod.init(p, header())
        # One pre-existing write entry — the stub will have replayed this
        # silently before asking for seq=2.
        corpus_mod.append_op(c, OpEntry(
            seq=1, bar=4, offset=2304, size=4, dir="write",
            value="01000000", source="qemu_capture",
            validated_at_commit=None, validated_at=None,
        ))
        return p

    def test_one_extend_then_complete(self) -> None:
        from hailo_re_driver.line_protocols import ExtendRequest
        from hailo_re_driver.qemu_runner import ExtendEvent

        with tempfile.TemporaryDirectory() as d:
            corpus_path = self._make_corpus(Path(d))
            extend_event = ExtendEvent(
                kind="extend",
                request=ExtendRequest(
                    seq=2, bar=4, offset=2308, size=4,
                    reason="unknown_read",
                ),
                raw_line=("HAILO_RE_CORPUS_EXTEND seq=2 bar=4 offset=2308 "
                          "size=4 reason=unknown_read"),
            )
            qemu = _ScriptedQemuRunner([
                extend_event,
                ConfigureCompleteEvent(),
            ])
            slmos = _ScriptedSlmosRunner([
                ExtendResponse(seq=2, bar=4, offset=2308, size=4,
                               value="00000000", slmos_sha=SHA),
            ])

            outcome = loop_mod.run_loop(
                corpus_path, qemu, slmos,  # type: ignore[arg-type]
                loop_mod.LoopConfig(
                    max_iterations=10,
                    reachable=lambda _sha: True,
                ),
            )

            self.assertEqual(outcome.status, "complete")
            self.assertEqual(outcome.iterations, 2)
            self.assertEqual(outcome.last_seq_appended, 2)
            # Corpus must now contain the new read entry stamped with SHA.
            reloaded = corpus_mod.load(corpus_path)
            self.assertEqual(len(reloaded.ops), 2)
            self.assertEqual(reloaded.ops[1].value, "00000000")
            self.assertEqual(reloaded.ops[1].validated_at_commit, SHA)

    def test_real_subprocess_mock_qemu_prints_extend_line(self) -> None:
        """Drive the loop through actual subprocess.Popen via mock_runner_from_lines."""
        from hailo_re_driver.qemu_runner import ExtendEvent

        with tempfile.TemporaryDirectory() as d:
            corpus_path = self._make_corpus(Path(d))
            line = ("HAILO_RE_CORPUS_EXTEND seq=2 bar=4 offset=2308 "
                    "size=4 reason=unknown_read")
            qemu_real = mock_runner_from_lines([line], returncode=1)
            event = qemu_real.run(corpus_path)
            self.assertIsInstance(event, ExtendEvent)
            self.assertEqual(event.request.seq, 2)

    def test_divergence_triggers_rollback_and_stops(self) -> None:
        from hailo_re_driver.line_protocols import DivergenceReport
        from hailo_re_driver.qemu_runner import DivergenceEvent

        with tempfile.TemporaryDirectory() as d:
            corpus_path = self._make_corpus(Path(d))
            # Add a validated entry so rollback has a frontier.
            c = corpus_mod.load(corpus_path)
            corpus_mod.append_op(c, OpEntry(
                seq=2, bar=4, offset=2308, size=4, dir="read",
                value="00000000", source="slmos_observed",
                validated_at_commit=SHA,
                validated_at="2026-05-12T19:00:00Z",
            ))

            div = DivergenceReport(
                seq=3, bar=4, offset=2308, size=4, dir="read",
                expected="01000000", observed="02000000",
                source="qemu", reason="inline_read_mismatch",
            )
            qemu = _ScriptedQemuRunner([
                DivergenceEvent(kind="divergence", report=div,
                                raw_line=div.emit()),
            ])
            slmos = _ScriptedSlmosRunner([])
            outcome = loop_mod.run_loop(
                corpus_path, qemu, slmos,  # type: ignore[arg-type]
                loop_mod.LoopConfig(
                    max_iterations=5,
                    reachable=lambda sha: sha == SHA,
                ),
            )
            self.assertEqual(outcome.status, "diverged")
            self.assertIsNotNone(outcome.rollback)
            self.assertEqual(outcome.rollback.rollback_to_seq, 2)
            self.assertFalse(corpus_path.exists(),
                             "original corpus must be renamed to .poisoned")


class StubAppendDuringRunTests(unittest.TestCase):
    """Regression coverage for the "QEMU stub appends writes to the corpus
    file in place during its run, so the driver's in-memory view is stale
    when it returns" gap. The fix is the `corpus_mod.load` call placed
    immediately after `qemu.run` in `run_loop`; this test would catch a
    future refactor that drops it.
    """

    SHA = "cd" * 20

    def _header(self) -> Header:
        return Header(
            format_version=1, hailort_version="4.23.0",
            fw_version="4.23.0",
            capture_host="qemu-x86_64",
            slmos_base_sha=self.SHA,
            capture_started_at="2026-05-12T18:30:00Z",
        )

    def test_extend_seq_matches_post_run_next_seq(self) -> None:
        """A QEMU run that appends writes to the corpus file before
        emitting EXTEND must not be flagged as
        'EXTEND seq=N but corpus next_seq=M — corpus and stub disagree'.
        """
        from hailo_re_driver.line_protocols import ExtendRequest
        from hailo_re_driver.qemu_runner import ExtendEvent

        with tempfile.TemporaryDirectory() as d:
            corpus_path = Path(d) / "c.jsonl"
            corpus_mod.init(corpus_path, self._header())
            c = corpus_mod.load(corpus_path)
            # Pre-existing seq=1..3 to put the in-memory snapshot at
            # next_seq=4.
            for s in (1, 2, 3):
                corpus_mod.append_op(c, OpEntry(
                    seq=s, bar=4, offset=0x100 + s, size=4, dir="read",
                    value="00000000", source="slmos_observed",
                    validated_at_commit=self.SHA,
                    validated_at="2026-05-12T19:00:00Z",
                ))

            # Build a runner that mutates the corpus file as a side effect
            # before returning its EXTEND (simulating Task 0.2's stub
            # appending captured writes during its run). The driver must
            # reload the corpus after the run; if it doesn't, request.seq
            # (8) and the stale corpus.next_seq (4) disagree and the loop
            # bails with status=error.
            class _AppendingRunner:
                def __init__(self, corpus_path: Path):
                    self.calls = 0
                    self.corpus_path = corpus_path

                def run(self, path: Path):
                    self.calls += 1
                    if self.calls == 1:
                        # Simulate four writes the stub recorded during
                        # the run, immediately before halting on the
                        # unknown read at seq=8.
                        fresh = corpus_mod.load(path)
                        for s in range(4, 8):
                            corpus_mod.append_op(fresh, OpEntry(
                                seq=s, bar=4, offset=0x200 + s, size=4,
                                dir="write", value="ffffffff",
                                source="qemu_capture",
                                validated_at_commit=None,
                                validated_at=None,
                            ))
                        return ExtendEvent(
                            kind="extend",
                            request=ExtendRequest(
                                seq=8, bar=4, offset=0x800, size=4,
                                reason="unknown_read",
                            ),
                            raw_line=(
                                "HAILO_RE_CORPUS_EXTEND seq=8 bar=4 "
                                "offset=2048 size=4 reason=unknown_read"
                            ),
                        )
                    return ConfigureCompleteEvent()

            slmos = _ScriptedSlmosRunner([
                ExtendResponse(seq=8, bar=4, offset=0x800, size=4,
                               value="42424242", slmos_sha=self.SHA),
            ])

            outcome = loop_mod.run_loop(
                corpus_path,
                _AppendingRunner(corpus_path),  # type: ignore[arg-type]
                slmos,  # type: ignore[arg-type]
                loop_mod.LoopConfig(
                    max_iterations=5,
                    reachable=lambda _sha: True,
                ),
            )
            self.assertEqual(
                outcome.status, "complete",
                f"loop must reload corpus after qemu.run; "
                f"got status={outcome.status} detail={outcome.detail}",
            )
            reloaded = corpus_mod.load(corpus_path)
            self.assertEqual(reloaded.next_seq, 9)
            self.assertEqual(reloaded.ops[-1].seq, 8)
            self.assertEqual(reloaded.ops[-1].value, "42424242")


if __name__ == "__main__":
    unittest.main()
