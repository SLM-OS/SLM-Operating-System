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
    ReplayDivergence,
    ReplayError,
    ReplayRefused,
    ReplayResponse,
    SlmosRunner,
    parse_replay_output,
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
        # Leading CR flushes any stray byte in the shell input buffer onto
        # its own "Unknown command:" line so it can't concatenate with our
        # cmd name. See slmos_runner.send_replay_command rationale.
        self.assertEqual(
            argv[cmd_index],
            "\rhailo replay-step 0:/corpus.jsonl 42",
        )

    def test_replay_command_passes_interbyte_delay(self) -> None:
        """labctl serial send now accepts --interbyte-delay-ms to pace the
        TX one byte at a time so the receiving UART's RX FIFO can't
        overrun. The driver must thread this through whenever
        `serial_interbyte_delay_ms` is set, with the configured value."""
        rec = _RecordingTransport()
        runner = SlmosRunner(
            sbc="pi-5-1",
            transport=rec,
            serial_interbyte_delay_ms=2.0,
        )
        runner.send_replay_command(Path("/dev/null"), 1)
        argv, _ = rec.calls[0]
        self.assertIn("--interbyte-delay-ms", argv)
        flag_idx = argv.index("--interbyte-delay-ms")
        self.assertEqual(argv[flag_idx + 1], "2.0")

    def test_replay_command_omits_interbyte_delay_when_zero(self) -> None:
        """A zero / None pacing config must NOT add the flag, so the
        per-byte pacing penalty isn't paid when not asked for."""
        rec = _RecordingTransport()
        runner = SlmosRunner(
            sbc="pi-5-1",
            transport=rec,
            serial_interbyte_delay_ms=0,
        )
        runner.send_replay_command(Path("/dev/null"), 1)
        argv, _ = rec.calls[0]
        self.assertNotIn("--interbyte-delay-ms", argv)

    def test_replay_command_prepends_cr_flush(self) -> None:
        """Regression for `Unknown command: qhailo`/`ehailo`/`Ghailo` class
        transients. A leading CR forces any stray byte left in the shell's
        input buffer to execute as its own (failed) command BEFORE our
        `hailo replay-step` lands on a fresh prompt."""
        rec = _RecordingTransport()
        runner = SlmosRunner(sbc="pi-5-1", transport=rec)
        runner.send_replay_command(Path("/dev/null"), 7)
        argv, _ = rec.calls[0]
        cmd_index = argv.index("send") + 2
        self.assertTrue(
            argv[cmd_index].startswith("\r"),
            f"cmd must start with CR to flush stray bytes: {argv[cmd_index]!r}",
        )

    def test_replay_command_logs_literal_bytes(self) -> None:
        """The host writes the literal bytes handed to labctl into a log
        file so dropped-char transients (e.g. "0:/corpus.json708" with
        'l ' missing) can be triaged by diffing host-side sent bytes
        against what the SLM-OS shell echoed back. The log is best-effort
        but must contain the seq and the hex of the exact cmd string."""
        import os
        import tempfile

        from hailo_re_driver import slmos_runner as sr_mod

        with tempfile.TemporaryDirectory() as td:
            log_path = os.path.join(td, "sent.log")
            original = sr_mod._SENT_CMDS_LOG
            sr_mod._SENT_CMDS_LOG = log_path
            try:
                rec = _RecordingTransport()
                runner = SlmosRunner(
                    sbc="pi-5-1",
                    corpus_on_card_path="0:/corpus.jsonl",
                    transport=rec,
                )
                runner.send_replay_command(Path("/dev/null"), 4242)
                with open(log_path) as f:
                    line = f.read()
            finally:
                sr_mod._SENT_CMDS_LOG = original

        self.assertIn("seq=4242", line)
        expected_cmd = "\rhailo replay-step 0:/corpus.jsonl 4242"
        self.assertIn(f"hex={expected_cmd.encode().hex()}", line)

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


class SoftRefuseClassificationTests(unittest.TestCase):
    """The kernel's `hailo replay-step` emits informational refuse lines
    (kernel/ai_accel/hailo/hailo_shell.c) that contain neither
    `slmos_sha=` nor `reason=`. Before this regression net those caused
    `labctl serial send --until` to burn the full 30 s replay timeout
    and the wrapper logged "no RESPONSE or DIVERGENCE line" — i.e. a
    silent classification bug that ate hours of overnight grind. Each
    test below pins one refuse class to ReplayRefused with the right
    reason tag."""

    def test_parse_classifies_parse_failed_refuse(self) -> None:
        text = (
            "hailo replay-step 0:/corpus.jsonl 100\n"
            "hailo: replay-step: corpus parse failed (rc=-3, ops_parsed=2056)\n"
            "slmos>\n"
        )
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayRefused)
        assert isinstance(result, ReplayRefused)
        self.assertEqual(result.reason, "parse_failed")
        self.assertIn("rc=-3", result.raw_line)

    def test_parse_classifies_no_entry_refuse(self) -> None:
        text = "hailo: replay-step: no entry at seq=99999\nslmos>\n"
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayRefused)
        assert isinstance(result, ReplayRefused)
        self.assertEqual(result.reason, "no_entry")

    def test_parse_classifies_is_write_refuse(self) -> None:
        text = (
            "hailo: replay-step: seq=52288 is a write — refusing "
            "(seq=N must be a read with validated_at_commit=null)\n"
            "slmos>\n"
        )
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayRefused)
        assert isinstance(result, ReplayRefused)
        self.assertEqual(result.reason, "is_write")
        self.assertIn("seq=52288", result.raw_line)

    def test_parse_classifies_already_validated_refuse(self) -> None:
        text = (
            "hailo: replay-step: seq=51511 is already "
            "validated — refusing (corpus is inconsistent)\n"
            "slmos>\n"
        )
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayRefused)
        assert isinstance(result, ReplayRefused)
        self.assertEqual(result.reason, "already_validated")

    def test_parse_classifies_wrong_size_refuse(self) -> None:
        text = (
            "hailo: replay-step: seq=100 has size=8; only size=4 is "
            "supported by the platform shim\n"
            "slmos>\n"
        )
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayRefused)
        assert isinstance(result, ReplayRefused)
        self.assertEqual(result.reason, "wrong_size")

    def test_parse_prefers_response_over_refuse(self) -> None:
        """If both a RESPONSE line and a refuse marker appear (impossible
        in practice but cheap to pin), the protocol line wins so a
        success isn't mis-classified as a refuse."""
        text = (
            "hailo: replay-step: corpus parse failed (rc=-1, ops_parsed=0)\n"
            "HAILO_RE_CORPUS_RESPONSE seq=1 bar=4 offset=0 size=4 "
            "value=12345678 slmos_sha=" + "ab" * 20 + "\n"
        )
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayResponse)

    def test_parse_unmatched_output_stays_replay_error(self) -> None:
        """Output that's neither a protocol line nor a known refuse
        falls through to ReplayError — so genuine "Pi 5 hung" cases are
        still distinguishable from refuses."""
        text = "some unrelated boot noise\nslmos>\n"
        result = parse_replay_output(text)
        self.assertIsInstance(result, ReplayError)


class SoftRefuseUntilRegexTests(unittest.TestCase):
    """The `--until` regex passed to `labctl serial send` must fire
    immediately on a soft-refuse line so the wrapper doesn't burn
    replay_timeout_s (30 s) per refuse iteration. Pinned per-class so a
    future kernel message tweak that breaks the regex fails fast."""

    def _get_until_re(self) -> "re.Pattern[str]":
        import re as re_mod

        rec = _RecordingTransport()
        runner = SlmosRunner(sbc="pi-5-1", transport=rec)
        runner.send_replay_command(Path("/dev/null"), 1)
        argv = rec.calls[0][0]
        until_idx = argv.index("--until")
        return re_mod.compile(argv[until_idx + 1])

    def test_until_fires_on_parse_failed(self) -> None:
        rx = self._get_until_re()
        line = "hailo: replay-step: corpus parse failed (rc=-3, ops_parsed=2056)"
        self.assertIsNotNone(rx.search(line))

    def test_until_fires_on_no_entry(self) -> None:
        rx = self._get_until_re()
        line = "hailo: replay-step: no entry at seq=99999"
        self.assertIsNotNone(rx.search(line))

    def test_until_fires_on_is_write(self) -> None:
        rx = self._get_until_re()
        line = "hailo: replay-step: seq=52288 is a write — refusing (...)"
        self.assertIsNotNone(rx.search(line))

    def test_until_fires_on_already_validated(self) -> None:
        rx = self._get_until_re()
        line = "hailo: replay-step: seq=51511 is already validated — refusing"
        self.assertIsNotNone(rx.search(line))

    def test_until_fires_on_wrong_size(self) -> None:
        rx = self._get_until_re()
        line = "hailo: replay-step: seq=100 has size=8; only size=4 supported"
        self.assertIsNotNone(rx.search(line))

    def test_until_does_not_fire_on_in_progress_diag(self) -> None:
        """The success-path diagnostic line `hailo: replay-step: corpus
        ops=N target seq=M ...` must NOT match the until regex — if it
        did, labctl would return before the actual RESPONSE line
        landed."""
        rx = self._get_until_re()
        line = (
            "hailo: replay-step: corpus ops=2057 target seq=51511 "
            "bar=4 offset=0x0"
        )
        self.assertIsNone(rx.search(line))


if __name__ == "__main__":
    unittest.main()
