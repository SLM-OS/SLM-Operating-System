"""Drive SLM-OS on the target SBC via labctl to perform `hailo replay-step <N>`.

labctl is the ONLY hardware interface (per CLAUDE.md). This module never
touches `/dev/sd*`, never calls `mount`, never opens the serial line itself.
Everything goes through `labctl ...` invocations, and the transport is
parameterised so tests pass a fake.

Steps for one replay:

1. (Optional) `labctl sdwire info <sbc>` — verify card identity before flash.
2. `labctl sdwire update <sbc> -p 1 -c <kernel>:kernel_2712.img
                                       -c <corpus>:corpus.jsonl --reboot`
3. Poll `labctl serial capture <sbc> --until '<shell-prompt>'` for the prompt.
4. `labctl serial send <sbc> 'hailo replay-step 0:/corpus.jsonl <N>'
       --capture ... --until 'HAILO_RE_CORPUS_(RESPONSE|DIVERGENCE)'`.
5. Parse the captured line.

The corpus is flashed onto the boot partition alongside the kernel, so the
on-card path that SLM-OS sees is FatFs `0:/corpus.jsonl` per the Task 0.4
`hailo replay-step` reference (docs/hailo-re-replay-step.md §Usage).
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional, Union

from .line_protocols import (
    DivergenceReport,
    ExtendResponse,
    LineProtocolError,
    parse_any,
)


# --------------------------------------------------------------------------- #
# Result types
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class ReplayResponse:
    kind: str  # "response"
    response: ExtendResponse
    raw_line: str


@dataclass(frozen=True)
class ReplayDivergence:
    kind: str  # "divergence"
    report: DivergenceReport
    raw_line: str


@dataclass(frozen=True)
class ReplayError:
    kind: str  # "error"
    message: str
    stdout: str
    stderr: str


@dataclass(frozen=True)
class ReplayRefused:
    """SLM-OS understood the command but declined to execute it.

    Refuses are emitted by `hailo replay-step` for state conditions that
    won't resolve on retry — the corpus has a duplicate seq, the target
    seq is already validated, the target is a write, etc. Distinct from
    ReplayError (which means the request never reached the kernel or the
    output was unreadable) so callers can stop counting refuses as
    "transient" and surface them as actionable feedback to the operator.

    `reason` is one of: "parse_failed", "no_entry", "is_write",
    "already_validated", "wrong_size". `raw_line` is the kernel's
    diagnostic text verbatim for the log.
    """

    kind: str  # "refused"
    reason: str
    raw_line: str


ReplayResult = Union[ReplayResponse, ReplayDivergence, ReplayError, ReplayRefused]


# --------------------------------------------------------------------------- #
# Transport
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class CmdResult:
    returncode: int
    stdout: str
    stderr: str


Transport = Callable[[list[str], Optional[float]], CmdResult]
"""Run an argv list with optional timeout, return CmdResult."""


def subprocess_transport(argv: list[str], timeout: Optional[float]) -> CmdResult:
    proc = subprocess.run(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        text=True,
        check=False,
    )
    return CmdResult(
        returncode=proc.returncode, stdout=proc.stdout, stderr=proc.stderr
    )


def dry_run_transport(argv: list[str], _timeout: Optional[float]) -> CmdResult:
    """No-op transport: log the command, return success with empty stdout.

    Used by the CLI's `--dry-run` to surface-check labctl invocations without
    touching hardware. For end-to-end loop simulation (where the SLM-OS side
    must actually return synthesised RESPONSE lines), use
    `MockSlmosRunner` with `--mock-slmos-responses`.
    """
    return CmdResult(
        returncode=0,
        stdout="",
        stderr=f"[dry-run] {' '.join(shlex.quote(a) for a in argv)}\n",
    )


class MockSlmosRunner:
    """Drop-in SlmosRunner replacement that returns canned RESPONSE lines.

    Used for end-to-end loop simulation in CI and local dev. The canned lines
    are stored in a plain-text file (one HAILO_RE_CORPUS_RESPONSE per line)
    and consumed in order.
    """

    def __init__(self, response_lines: list[str]):
        self._lines = list(response_lines)
        self._idx = 0

    def replay_step(self, corpus_path, seq, *, fresh_boot: bool = True):
        if self._idx >= len(self._lines):
            return ReplayError(
                kind="error",
                message=f"MockSlmosRunner exhausted (asked for seq={seq})",
                stdout="", stderr="",
            )
        line = self._lines[self._idx]
        self._idx += 1
        return parse_replay_output(line + "\n")

    @classmethod
    def from_file(cls, path) -> "MockSlmosRunner":
        with open(path, "r", encoding="utf-8") as f:
            lines = [ln.rstrip("\n") for ln in f if ln.strip()
                     and ln.startswith("HAILO_RE_CORPUS_")]
        return cls(lines)


# --------------------------------------------------------------------------- #
# SLM-OS runner
# --------------------------------------------------------------------------- #


_PROTOCOL_RE = re.compile(
    r"^HAILO_RE_CORPUS_(RESPONSE|DIVERGENCE) .*$", re.MULTILINE
)

# Kernel soft-refuse messages emitted by `hailo replay-step` in
# kernel/ai_accel/hailo/hailo_shell.c. These are persistent state
# conditions (corpus inconsistency, target seq is a write, etc.) — not
# transients. The host classifies them as ReplayRefused so the wrapper
# can surface the cause instead of burning the full replay timeout
# waiting for a RESPONSE that will never come.
_REFUSE_PATTERNS: tuple[tuple[str, "re.Pattern[str]"], ...] = (
    ("parse_failed", re.compile(
        r"^hailo: replay-step: corpus parse failed \(rc=-?\d+.*$",
        re.MULTILINE,
    )),
    ("no_entry", re.compile(
        r"^hailo: replay-step: no entry at seq=\d+.*$", re.MULTILINE,
    )),
    ("is_write", re.compile(
        r"^hailo: replay-step: seq=\d+ is a write — refusing.*$",
        re.MULTILINE,
    )),
    ("already_validated", re.compile(
        r"^hailo: replay-step: seq=\d+ is already validated.*$",
        re.MULTILINE,
    )),
    ("wrong_size", re.compile(
        r"^hailo: replay-step: seq=\d+ has size=\d+.*$", re.MULTILINE,
    )),
)


@dataclass
class SlmosRunner:
    """End-to-end driver for one `hailo replay-step <N>` cycle on a real SBC.

    `shell_prompt` is a regex compiled once at construction. Mutating it
    after the fact (e.g. `runner.shell_prompt = "new>"`) does NOT refresh
    the cached pattern — build a new SlmosRunner instead.
    """

    sbc: str = "pi-5-1"
    kernel_path: Path = Path("build/kernel/slmos.bin")
    kernel_dst: str = "kernel_2712.img"
    corpus_dst: str = "corpus.jsonl"
    corpus_on_card_path: str = "0:/corpus.jsonl"
    boot_partition: int = 1
    shell_prompt: str = r"slmos>"
    boot_timeout_s: float = 45.0
    replay_timeout_s: float = 30.0
    cmd_timeout_s: float = 120.0
    verify_sd_before_flash: bool = True
    transport: Transport = field(default=subprocess_transport)

    def __post_init__(self) -> None:
        # Validate + compile the shell-prompt regex once at construction
        # time. A bad regex would otherwise surface on every replay_step.
        self._shell_prompt_re = re.compile(self.shell_prompt)

    # ----- pipeline steps --------------------------------------------------

    def power_off(self) -> CmdResult:
        return self.transport(
            ["labctl", "power", "off", self.sbc], self.cmd_timeout_s
        )

    def verify_card(self) -> CmdResult:
        """Power off the board, then query `labctl sdwire info`.

        WARNING: this method has a side effect — it powers off the SBC
        before running sdwire info. The power-off is required because
        sdwire info needs the SD card switched to the host bus, which
        labctl refuses to do while the board is powered on. Each replay
        iteration leaves the board ON (the previous `sdwire update
        --reboot` finishes with power ON), so the power-off cycle must
        precede info on every iteration.

        Returns the labctl power-off result (with a non-zero returncode)
        if that step fails; otherwise returns the sdwire info result.
        """
        off = self.transport(
            ["labctl", "power", "off", self.sbc], self.cmd_timeout_s
        )
        if off.returncode != 0:
            return off
        return self.transport(
            ["labctl", "sdwire", "info", self.sbc], self.cmd_timeout_s
        )

    def flash_and_reboot(
        self,
        corpus_path: Path,
        kernel_path: Optional[Path] = None,
    ) -> CmdResult:
        kp = kernel_path or self.kernel_path
        return self.transport(
            [
                "labctl", "sdwire", "update", self.sbc,
                "-p", str(self.boot_partition),
                "-c", f"{kp}:{self.kernel_dst}",
                "-c", f"{corpus_path}:{self.corpus_dst}",
                "--reboot",
            ],
            self.cmd_timeout_s,
        )

    def wait_for_shell(self) -> CmdResult:
        return self.transport(
            [
                "labctl", "serial", "capture", self.sbc,
                "--until", self.shell_prompt,
                "--timeout", str(self.boot_timeout_s),
            ],
            self.boot_timeout_s + 5.0,
        )

    def send_replay_command(
        self, corpus_path: Path, seq: int
    ) -> CmdResult:
        # The dev-side corpus_path is unused here — the SD-card flash step
        # has already copied the corpus to `corpus_on_card_path` on the FAT
        # boot partition, which is the path SLM-OS reads from. Task 0.4's
        # `hailo replay-step` command (docs/hailo-re-replay-step.md §Usage)
        # takes the on-card path as its first argument.
        del corpus_path
        cmd = f"hailo replay-step {self.corpus_on_card_path} {seq}"
        # labctl --until evaluates the regex against the receive buffer as
        # bytes arrive, so a prefix-only anchor like
        # `^HAILO_RE_CORPUS_RESPONSE` cuts the line off mid-value. Anchor on
        # tokens that only land near end-of-line: the full slmos_sha for the
        # success path, the reason= tag for the divergence path.
        #
        # The reason= alternative uses `\S+` rather than `[a-z_]+` because
        # the corpus spec (docs/hailo-re-corpus-format.md §Divergence
        # report) explicitly says tools must accept any string for the
        # reason tag — additive tags can be added without bumping the
        # corpus format_version, so the matcher must not narrow that.
        # The labctl --until regex must fire on success AND on any
        # kernel soft-refuse so the wrapper doesn't burn replay_timeout_s
        # (30 s) waiting for a RESPONSE/DIVERGENCE that the kernel
        # explicitly declined to produce. Each refuse phrase appears
        # before its trailing newline, so anchoring on the tail token
        # ("refusing", "(rc=", "supported") keeps the line intact for
        # the downstream parser.
        until_re = (
            r"(?:slmos_sha=[0-9a-f]{40}|"
            r"HAILO_RE_CORPUS_DIVERGENCE.*reason=\S+|"
            r"hailo: replay-step: corpus parse failed \(rc=|"
            r"hailo: replay-step: no entry at seq=\d+|"
            r"hailo: replay-step: seq=\d+ (?:is a write|is already validated|has size=\d+))"
        )
        return self.transport(
            [
                "labctl", "serial", "send", self.sbc, cmd,
                "--capture", str(self.replay_timeout_s),
                "--until", until_re,
            ],
            self.replay_timeout_s + 5.0,
        )

    # ----- end-to-end ------------------------------------------------------

    def replay_step(
        self, corpus_path: Path, seq: int, *, fresh_boot: bool = True
    ) -> ReplayResult:
        """Flash, boot, run `hailo replay-step <N>`, parse the response line."""
        if self.verify_sd_before_flash and fresh_boot:
            info = self.verify_card()
            if info.returncode != 0:
                return ReplayError(
                    kind="error",
                    message=(
                        f"sdwire_info failed (rc={info.returncode}) — refusing "
                        "to flash without card identity confirmation"
                    ),
                    stdout=info.stdout,
                    stderr=info.stderr,
                )
        if fresh_boot:
            flash = self.flash_and_reboot(corpus_path)
            if flash.returncode != 0:
                return ReplayError(
                    kind="error",
                    message=f"sdwire_update failed (rc={flash.returncode})",
                    stdout=flash.stdout,
                    stderr=flash.stderr,
                )
            shell = self.wait_for_shell()
            if shell.returncode != 0 or not self._shell_prompt_re.search(shell.stdout):
                return ReplayError(
                    kind="error",
                    message=(
                        "did not see shell prompt after flash+reboot "
                        f"(rc={shell.returncode})"
                    ),
                    stdout=shell.stdout,
                    stderr=shell.stderr,
                )
        send = self.send_replay_command(corpus_path, seq)
        if send.returncode != 0:
            return ReplayError(
                kind="error",
                message=f"serial_send failed (rc={send.returncode})",
                stdout=send.stdout,
                stderr=send.stderr,
            )
        return parse_replay_output(send.stdout)


def parse_replay_output(text: str) -> ReplayResult:
    """Classify `hailo replay-step` output. Prefers protocol lines
    (RESPONSE/DIVERGENCE); falls back to soft-refuse detection so
    state-condition messages from the kernel are surfaced as
    ReplayRefused rather than the generic "no RESPONSE" timeout."""
    match = _PROTOCOL_RE.search(text)
    if not match:
        for reason, pattern in _REFUSE_PATTERNS:
            rm = pattern.search(text)
            if rm:
                return ReplayRefused(
                    kind="refused",
                    reason=reason,
                    raw_line=rm.group(0),
                )
        return ReplayError(
            kind="error",
            message="no HAILO_RE_CORPUS_RESPONSE or _DIVERGENCE line in output",
            stdout=text,
            stderr="",
        )
    line = match.group(0)
    try:
        parsed = parse_any(line)
    except LineProtocolError as e:
        return ReplayError(
            kind="error",
            message=f"could not parse protocol line: {e}",
            stdout=text,
            stderr="",
        )
    if isinstance(parsed, ExtendResponse):
        return ReplayResponse(kind="response", response=parsed, raw_line=line)
    if isinstance(parsed, DivergenceReport):
        return ReplayDivergence(kind="divergence", report=parsed, raw_line=line)
    return ReplayError(
        kind="error",
        message=f"unexpected protocol message: {type(parsed).__name__}",
        stdout=text,
        stderr="",
    )


# --------------------------------------------------------------------------- #
# Convenience builders
# --------------------------------------------------------------------------- #


def from_env() -> SlmosRunner:
    """Build a runner from environment variables (for the CLI)."""
    return SlmosRunner(
        sbc=os.environ.get("HAILO_RE_SBC", "pi-5-1"),
        kernel_path=Path(
            os.environ.get("HAILO_RE_KERNEL", "build/kernel/slmos.bin")
        ),
    )
