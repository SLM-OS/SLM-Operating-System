"""Spawn QEMU + Task 0.2 stub and consume its stdout for HAILO_RE_CORPUS_* lines.

The runner is a thin wrapper around `subprocess.Popen` plus the
`line_protocols` parsers. The launcher itself is parameterised so tests can
pass a callable that emits a deterministic stdout stream without touching real
QEMU. Real callers pass a `command` list that invokes Task 0.3's launch script.

Event taxonomy (see `loop.py` for the dispatch):

- `extend(...)` — stub hit an unknown read; emit EXTEND line, then exit non-zero
- `divergence(...)` — stub detected a write_value or shape mismatch; halts
- `configure_complete()` — process exited 0 without ever emitting EXTEND or
  DIVERGENCE. The Phase 1 exit gate from KICKOFF maps to this.
- `error(...)` — non-zero exit without an EXTEND/DIVERGENCE line on stdout
"""

from __future__ import annotations

import os
import shlex
import subprocess
import threading
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, IO, Iterable, Optional, Union

from .line_protocols import (
    DivergenceReport,
    ExtendRequest,
    LineProtocolError,
    parse_any,
)


# --------------------------------------------------------------------------- #
# Event types
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class ExtendEvent:
    kind: str  # "extend"
    request: ExtendRequest
    raw_line: str


@dataclass(frozen=True)
class DivergenceEvent:
    kind: str  # "divergence"
    report: DivergenceReport
    raw_line: str


@dataclass(frozen=True)
class ConfigureCompleteEvent:
    kind: str = "configure_complete"


@dataclass(frozen=True)
class ErrorEvent:
    kind: str  # "error"
    returncode: int
    stderr_tail: str
    last_stdout_line: str


QemuEvent = Union[ExtendEvent, DivergenceEvent, ConfigureCompleteEvent, ErrorEvent]


# --------------------------------------------------------------------------- #
# Public API
# --------------------------------------------------------------------------- #


@dataclass
class QemuRunner:
    """Run a QEMU+stub process against a corpus and return the first event.

    `command_factory` takes the corpus path and returns the argv list for
    `subprocess.Popen`. The launcher itself is the Task 0.3 deliverable named
    in the `HAILO_RE_QEMU_LAUNCHER` env var; `default_command_factory` raises
    if it isn't set.

    stderr is drained on a background thread to prevent pipe-buffer deadlock
    when QEMU emits more than ~64 KB of stderr while we're still parsing
    stdout.
    """

    command_factory: Callable[[Path], list[str]]
    cwd: Optional[Path] = None
    extra_env: Optional[dict[str, str]] = None
    capture_stderr_tail: int = 64  # lines

    def run(self, corpus_path: Path) -> QemuEvent:
        argv = self.command_factory(corpus_path)
        env = os.environ.copy()
        if self.extra_env:
            env.update(self.extra_env)
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(self.cwd) if self.cwd else None,
            env=env,
            text=True,
        )
        stderr_buf: deque[str] = deque(maxlen=self.capture_stderr_tail)
        stderr_thread = threading.Thread(
            target=_tail_into,
            args=(proc.stderr, stderr_buf),
            daemon=True,
        )
        stderr_thread.start()
        try:
            event = _consume_stream(proc.stdout)  # type: ignore[arg-type]
            proc.wait()
            rc = proc.returncode
        finally:
            for pipe in (proc.stdout, proc.stderr):
                try:
                    if pipe is not None:
                        pipe.close()
                except Exception:
                    pass
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            stderr_thread.join(timeout=1.0)
        stderr_tail = "".join(stderr_buf)
        if event is not None:
            return event
        if rc == 0:
            return ConfigureCompleteEvent()
        return ErrorEvent(
            kind="error",
            returncode=rc,
            stderr_tail=stderr_tail,
            last_stdout_line="",
        )


def _consume_stream(stream: IO[str]) -> Optional[QemuEvent]:
    """Read lines until a HAILO_RE_CORPUS_* line is seen, then drain the rest.

    Returns the first ExtendEvent or DivergenceEvent, or None if no protocol
    line appeared (caller decides whether that's success or error from the
    exit code).
    """
    found: Optional[QemuEvent] = None
    for raw in stream:
        line = raw.rstrip("\n")
        if not line.startswith("HAILO_RE_CORPUS_"):
            continue
        if found is not None:
            # Drain remaining stdout so the process can exit cleanly, but keep
            # only the first event — the protocol guarantees one per run.
            continue
        try:
            parsed = parse_any(line)
        except LineProtocolError:
            continue
        if isinstance(parsed, ExtendRequest):
            found = ExtendEvent(kind="extend", request=parsed, raw_line=line)
        elif isinstance(parsed, DivergenceReport):
            found = DivergenceEvent(
                kind="divergence", report=parsed, raw_line=line
            )
    return found


def _tail_into(stream: Optional[IO[str]], buf: "deque[str]") -> None:
    """Drain a stream into a bounded deque on a background thread."""
    if stream is None:
        return
    try:
        for line in stream:
            buf.append(line)
    except Exception:
        return


# --------------------------------------------------------------------------- #
# Convenience: default command factory
# --------------------------------------------------------------------------- #


def default_command_factory(corpus_path: Path) -> list[str]:
    launcher = os.environ.get("HAILO_RE_QEMU_LAUNCHER")
    if not launcher:
        raise RuntimeError(
            "HAILO_RE_QEMU_LAUNCHER not set and no default launcher wired. "
            "Set it to the Task 0.3 launch script path, e.g. "
            "scripts/run-qemu-stub.sh"
        )
    return [*shlex.split(launcher), "--corpus", str(corpus_path)]


# --------------------------------------------------------------------------- #
# Mock helper for tests
# --------------------------------------------------------------------------- #


def mock_runner_from_lines(
    lines: Iterable[str], returncode: int = 1
) -> "QemuRunner":
    """Build a QemuRunner whose subprocess simply prints the given lines.

    Used by `tests/test_loop_mocked.py` to drive the loop without QEMU.
    """
    script = "; ".join(
        f"printf %s\\\\n {shlex.quote(line)}" for line in lines
    )
    cmd = ["sh", "-c", f"{script}; exit {returncode}"]
    return QemuRunner(command_factory=lambda _path: cmd)
