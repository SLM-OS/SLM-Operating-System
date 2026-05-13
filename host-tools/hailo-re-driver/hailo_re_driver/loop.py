"""The Phase 1/2 bootstrap loop.

Pseudocode from KICKOFF.md (with rollback hooked up):

    load corpus
    while True:
        event = qemu_runner.run(corpus_path)
        if event.kind == 'configure_complete':
            return  # Phase 1 / Phase 2 done for this iteration
        elif event.kind == 'extend':
            response = slmos_runner.replay_step(event.seq, corpus_path)
            corpus.append(response_to_entry(response))
        elif event.kind == 'divergence':
            rollback.handle(corpus, event)
            return
"""

from __future__ import annotations

import datetime as _dt
import json
import logging
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional

from . import corpus as corpus_mod
from . import rollback as rollback_mod
from .corpus import Corpus, OpEntry
from .line_protocols import ExtendRequest, ExtendResponse, DivergenceReport
from .qemu_runner import (
    ConfigureCompleteEvent,
    DivergenceEvent,
    ErrorEvent,
    ExtendEvent,
    QemuRunner,
)
from .slmos_runner import (
    ReplayDivergence,
    ReplayError,
    ReplayResponse,
    SlmosRunner,
)


log = logging.getLogger("hailo_re_driver.loop")


# --------------------------------------------------------------------------- #
# Loop outcome
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class LoopOutcome:
    status: str  # "complete" | "diverged" | "error" | "max-iterations"
    iterations: int
    last_seq_appended: Optional[int]
    detail: str = ""
    rollback: Optional[rollback_mod.RollbackResult] = None


@dataclass
class LoopConfig:
    max_iterations: int = 10_000
    iso_now: Callable[[], str] = field(
        default=lambda: _dt.datetime.now(
            tz=_dt.timezone.utc
        ).strftime("%Y-%m-%dT%H:%M:%SZ")
    )
    reachable: rollback_mod.SHA_REACHABLE = field(
        default_factory=rollback_mod.git_reachable_from_main
    )


def response_to_entry(
    response: ExtendResponse, *, source: str = "slmos_observed",
    iso_now: Callable[[], str] = lambda: _dt.datetime.now(
        tz=_dt.timezone.utc
    ).strftime("%Y-%m-%dT%H:%M:%SZ"),
) -> OpEntry:
    return OpEntry(
        seq=response.seq,
        bar=response.bar,
        offset=response.offset,
        size=response.size,
        dir="read",
        value=response.value,
        source=source,
        validated_at_commit=response.slmos_sha,
        validated_at=iso_now(),
    )


def run_loop(
    corpus_path: Path,
    qemu: QemuRunner,
    slmos: SlmosRunner,
    config: Optional[LoopConfig] = None,
) -> LoopOutcome:
    """Run the bootstrap loop until exit gate or hard stop."""
    cfg = config or LoopConfig()
    last_seq: Optional[int] = None

    for iteration in range(1, cfg.max_iterations + 1):
        corpus = corpus_mod.load(corpus_path)
        log.info(
            "iter=%d corpus=%s last_validated_seq=%d next_seq=%d",
            iteration, corpus_path, corpus.last_validated_seq, corpus.next_seq,
        )
        event = qemu.run(corpus_path)
        # The Task 0.2 stub appends unknown writes to the corpus file in
        # place during its run (per hailo8.c::hailo8_bar_write). Reload so
        # our in-memory view picks up those appended writes before deciding
        # what to do with the event.
        corpus = corpus_mod.load(corpus_path)

        if isinstance(event, ConfigureCompleteEvent):
            return LoopOutcome(
                status="complete",
                iterations=iteration,
                last_seq_appended=last_seq,
                detail="QEMU stub exited 0 with no unknown reads",
            )

        if isinstance(event, ErrorEvent):
            return LoopOutcome(
                status="error",
                iterations=iteration,
                last_seq_appended=last_seq,
                detail=(
                    f"QEMU exited rc={event.returncode} without protocol line. "
                    f"stderr tail:\n{event.stderr_tail}"
                ),
            )

        if isinstance(event, DivergenceEvent):
            outcome = _handle_divergence(
                corpus, event.report, cfg, source="qemu"
            )
            return outcome._replace_iterations(iteration, last_seq)

        if isinstance(event, ExtendEvent):
            req = event.request
            result = _extend_corpus(corpus, req, slmos, cfg)
            if result.status != "complete":
                return result._replace_iterations(iteration, last_seq)
            last_seq = req.seq
            continue

        return LoopOutcome(
            status="error",
            iterations=iteration,
            last_seq_appended=last_seq,
            detail=f"unknown event type from qemu_runner: {event!r}",
        )

    return LoopOutcome(
        status="max-iterations",
        iterations=cfg.max_iterations,
        last_seq_appended=last_seq,
        detail=f"hit max_iterations={cfg.max_iterations} without completion",
    )


# --------------------------------------------------------------------------- #
# Step handlers
# --------------------------------------------------------------------------- #


def _extend_corpus(
    corpus: Corpus,
    request: ExtendRequest,
    slmos: SlmosRunner,
    cfg: LoopConfig,
) -> "_StepOutcome":
    if request.seq != corpus.next_seq:
        return _StepOutcome(
            status="error",
            detail=(
                f"EXTEND seq={request.seq} but corpus next_seq="
                f"{corpus.next_seq} — corpus and stub disagree"
            ),
        )

    # SLM-OS `hailo replay-step` (Task 0.4) requires the seq=N entry to pre-
    # exist in the corpus so it knows what to read. Build a per-iteration
    # pending corpus = the persistent corpus + a placeholder read entry at
    # the EXTEND seq. SLM-OS reads this pending corpus from the SD card;
    # only the real RESPONSE value is appended to the persistent corpus.
    pending_path = _write_pending_corpus(corpus, request)
    try:
        result = slmos.replay_step(pending_path, request.seq)
    finally:
        try:
            pending_path.unlink()
        except FileNotFoundError:
            pass

    if isinstance(result, ReplayError):
        return _StepOutcome(
            status="error",
            detail=f"replay-step failed: {result.message}\nstderr: {result.stderr}",
        )

    if isinstance(result, ReplayDivergence):
        return _handle_divergence(
            corpus, result.report, cfg, source="slmos"
        )

    assert isinstance(result, ReplayResponse)
    resp = result.response
    if (resp.seq, resp.bar, resp.offset, resp.size) != (
        request.seq, request.bar, request.offset, request.size
    ):
        return _StepOutcome(
            status="error",
            detail=(
                "RESPONSE shape disagrees with EXTEND request: "
                f"req=({request.seq},{request.bar},{request.offset},"
                f"{request.size}) resp=({resp.seq},{resp.bar},"
                f"{resp.offset},{resp.size})"
            ),
        )

    entry = response_to_entry(resp, iso_now=cfg.iso_now)
    corpus_mod.append_op(corpus, entry)
    return _StepOutcome(status="complete", detail="")


def _handle_divergence(
    corpus: Corpus,
    report: DivergenceReport,
    cfg: LoopConfig,
    *,
    source: str,
) -> "_StepOutcome":
    log.warning(
        "divergence reported by %s at seq=%d: expected=%s observed=%s reason=%s",
        source, report.seq, report.expected, report.observed, report.reason,
    )
    try:
        rb = rollback_mod.handle(
            corpus,
            divergent_seq=report.seq,
            reachable=cfg.reachable,
            reason=f"{source}:{report.reason}",
        )
    except Exception as e:  # noqa: BLE001 — rollback failures must abort loop
        return _StepOutcome(
            status="error",
            detail=f"rollback failed: {e}",
        )
    return _StepOutcome(
        status="diverged",
        detail=(
            f"divergence at seq={report.seq} ({source}); rolled back to "
            f"seq={rb.rollback_to_seq}; new corpus at {rb.new_corpus_path}"
        ),
        rollback=rb,
    )


def _write_pending_corpus(corpus: Corpus, request: ExtendRequest) -> Path:
    """Materialize a temp corpus = persistent corpus + a placeholder seq=N read.

    The placeholder's value is a zero-filled hex string of the correct width;
    SLM-OS replay-step ignores the corpus's recorded value at seq=N (it issues
    a real BAR read) and only consumes (bar, offset, size), so the value is a
    don't-care. The pending file lives next to the persistent corpus and is
    deleted by the caller once replay_step returns.
    """
    placeholder = OpEntry(
        seq=request.seq,
        bar=request.bar,
        offset=request.offset,
        size=request.size,
        dir="read",
        value="0" * (request.size * 2),
        source="slmos_observed",
        validated_at_commit=None,
        validated_at=None,
        note="pending: placeholder for replay-step",
    )
    fd, tmp_name = tempfile.mkstemp(
        prefix=corpus.path.stem + "-pending-",
        suffix=corpus.path.suffix,
        dir=corpus.path.parent,
    )
    pending_path = Path(tmp_name)
    with corpus.path.open("r", encoding="utf-8") as src, \
            open(fd, "w", encoding="utf-8") as dst:
        dst.write(src.read())
        if not dst.tell() or not _ends_with_newline(corpus.path):
            dst.write("\n")
        dst.write(json.dumps(placeholder.to_json_obj(), separators=(",", ":")))
        dst.write("\n")
    return pending_path


def _ends_with_newline(path: Path) -> bool:
    with path.open("rb") as f:
        try:
            f.seek(-1, 2)  # SEEK_END
        except OSError:
            return True  # empty file — nothing to terminate
        return f.read(1) == b"\n"


# Internal step outcome that carries forward into the top-level LoopOutcome.
@dataclass(frozen=True)
class _StepOutcome:
    status: str  # "complete" | "diverged" | "error"
    detail: str
    rollback: Optional[rollback_mod.RollbackResult] = None

    def _replace_iterations(
        self, iteration: int, last_seq: Optional[int]
    ) -> LoopOutcome:
        return LoopOutcome(
            status=self.status,
            iterations=iteration,
            last_seq_appended=last_seq,
            detail=self.detail,
            rollback=self.rollback,
        )
