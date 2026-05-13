"""Regression coverage for the labctl-shaped behaviour of SlmosRunner.

The host-side driver never owns hardware in tests — the `transport` field
is a callable returning `CmdResult`, so we drop in a recorder that
captures argv + timeout and never spawns a process.

These tests pin the wire-level contract with labctl: the argv shape of
each pipeline step, the regex used for `--until`, and the corpus-flash
multi-`-c` invocation. They are the regression net the slmos-review
called out as missing.
"""

from __future__ import annotations

import unittest
from pathlib import Path
from typing import Optional

import conftest  # noqa: F401

from hailo_re_driver.slmos_runner import (
    CmdResult,
    SlmosRunner,
)


class _RecordingTransport:
    """Drop-in for SlmosRunner.transport that records every invocation."""

    def __init__(self, returncodes: Optional[list[int]] = None,
                 stdouts: Optional[list[str]] = None) -> None:
        self.calls: list[tuple[list[str], Optional[float]]] = []
        self._returncodes = list(returncodes or [])
        self._stdouts = list(stdouts or [])

    def __call__(self, argv: list[str],
                 timeout: Optional[float]) -> CmdResult:
        self.calls.append((argv, timeout))
        rc = self._returncodes.pop(0) if self._returncodes else 0
        out = self._stdouts.pop(0) if self._stdouts else ""
        return CmdResult(returncode=rc, stdout=out, stderr="")


class FlashArgvTests(unittest.TestCase):
    """The flash step must copy BOTH the kernel and the corpus onto the
    SD card in a single `labctl sdwire update` invocation, using two
    `-c <host>:<dest>` pairs."""

    def test_flash_argv_contains_kernel_and_corpus(self) -> None:
        rec = _RecordingTransport()
        runner = SlmosRunner(
            sbc="pi-5-1",
            kernel_path=Path("build/kernel/slmos.bin"),
            kernel_dst="kernel_2712.img",
            corpus_dst="corpus.jsonl",
            transport=rec,
        )
        runner.flash_and_reboot(Path("/tmp/c.jsonl"))

        self.assertEqual(len(rec.calls), 1)
        argv, _ = rec.calls[0]
        self.assertEqual(
            argv[:5],
            ["labctl", "sdwire", "update", "pi-5-1", "-p"],
        )
        # Two -c pairs must be present in order: kernel, then corpus.
        c_indices = [i for i, a in enumerate(argv) if a == "-c"]
        self.assertEqual(
            len(c_indices), 2,
            f"expected two -c flags, got argv={argv!r}",
        )
        self.assertEqual(
            argv[c_indices[0] + 1],
            "build/kernel/slmos.bin:kernel_2712.img",
        )
        self.assertEqual(
            argv[c_indices[1] + 1],
            "/tmp/c.jsonl:corpus.jsonl",
        )
        self.assertIn("--reboot", argv)


class ReplayCommandArgvTests(unittest.TestCase):
    """The replay-step shell command must include the on-card corpus
    path (FatFs `0:/corpus.jsonl`), and its `--until` regex must anchor
    on tokens that only land at end-of-line."""

    def test_replay_command_uses_on_card_path(self) -> None:
        rec = _RecordingTransport()
        runner = SlmosRunner(
            sbc="pi-5-1",
            corpus_on_card_path="0:/corpus.jsonl",
            transport=rec,
        )
        runner.send_replay_command(Path("/dev/null"), 42)
        self.assertEqual(len(rec.calls), 1)
        argv, _ = rec.calls[0]
        # 4th positional arg is the command string.
        cmd_index = argv.index("send") + 2
        self.assertEqual(
            argv[cmd_index],
            "hailo replay-step 0:/corpus.jsonl 42",
        )

    def test_replay_command_until_regex_matches_full_response_only(self) -> None:
        """`--until` must NOT match a truncated RESPONSE line that
        lacks slmos_sha. Otherwise labctl returns before the line is
        fully received and the parser fails with 'missing slmos_sha'.
        """
        import re

        rec = _RecordingTransport()
        runner = SlmosRunner(sbc="pi-5-1", transport=rec)
        runner.send_replay_command(Path("/dev/null"), 1)
        argv, _ = rec.calls[0]
        # Pull the regex out of argv: `--until` is followed by the value.
        until_idx = argv.index("--until")
        until_re = argv[until_idx + 1]
        rx = re.compile(until_re)

        truncated = (
            "HAILO_RE_CORPUS_RESPONSE seq=1 bar=0 offset=152 size=4 value=f"
        )
        self.assertIsNone(
            rx.search(truncated),
            f"truncated line must NOT match until regex {until_re!r}",
        )

        full = (
            "HAILO_RE_CORPUS_RESPONSE seq=1 bar=0 offset=152 size=4 "
            "value=601e6428 slmos_sha=" + "ab" * 20
        )
        self.assertIsNotNone(
            rx.search(full),
            f"full RESPONSE must match until regex {until_re!r}",
        )

    def test_replay_command_until_accepts_arbitrary_divergence_reason(
        self,
    ) -> None:
        """Per docs/hailo-re-corpus-format.md §Divergence report:
        'Tools must accept any string' for the reason tag. A narrower
        pattern like `reason=[a-z_]+` would wait the full timeout on a
        future tag like `dma-violation` or `mismatch_v2`.
        """
        import re

        rec = _RecordingTransport()
        runner = SlmosRunner(sbc="pi-5-1", transport=rec)
        runner.send_replay_command(Path("/dev/null"), 1)
        until_idx = rec.calls[0][0].index("--until")
        until_re = rec.calls[0][0][until_idx + 1]
        rx = re.compile(until_re)

        for tag in (
            "write_value_mismatch",        # spec-listed
            "op_shape_mismatch",           # spec-listed
            "inline_read_mismatch",        # spec-listed
            "dma-violation",               # hypothetical future tag w/ dash
            "mismatch_v2",                 # hypothetical future tag w/ digit
            "ECC_FATAL",                   # hypothetical uppercase tag
        ):
            line = (
                "HAILO_RE_CORPUS_DIVERGENCE seq=5 bar=4 offset=0 size=4 "
                "dir=write expected=00000000 observed=01000000 "
                f"source=qemu reason={tag}"
            )
            self.assertIsNotNone(
                rx.search(line),
                f"until regex must accept reason={tag!r}: pattern={until_re!r}",
            )


class VerifyCardSideEffectTests(unittest.TestCase):
    """`verify_card` is documented to power off the board before running
    sdwire info. The argv sequence must therefore be `power off` first,
    then `sdwire info` — and `sdwire info` must not run if power-off
    failed."""

    def test_verify_card_powers_off_first(self) -> None:
        rec = _RecordingTransport()
        runner = SlmosRunner(sbc="pi-5-1", transport=rec)
        runner.verify_card()
        self.assertEqual(len(rec.calls), 2)
        self.assertEqual(
            rec.calls[0][0],
            ["labctl", "power", "off", "pi-5-1"],
        )
        self.assertEqual(
            rec.calls[1][0],
            ["labctl", "sdwire", "info", "pi-5-1"],
        )

    def test_verify_card_skips_info_on_power_off_failure(self) -> None:
        rec = _RecordingTransport(returncodes=[1])
        runner = SlmosRunner(sbc="pi-5-1", transport=rec)
        result = runner.verify_card()
        self.assertEqual(result.returncode, 1)
        # Only the power-off call should have happened.
        self.assertEqual(len(rec.calls), 1)
        self.assertEqual(
            rec.calls[0][0],
            ["labctl", "power", "off", "pi-5-1"],
        )


if __name__ == "__main__":
    unittest.main()
