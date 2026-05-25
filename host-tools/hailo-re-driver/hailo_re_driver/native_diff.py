"""Diff a SLM-OS native-flow boundary trace against a corpus.

Unlike `hailo-re-validate` (which assumes SLM-OS replays the corpus
op-by-op and emits one observed line per corpus seq), this diff
operates on a free-running native flow trace from the kernel's
boundary-trace toolkit. The native flow does NOT preserve corpus seq
order — SLM-OS's native driver issues ops in its own order, may add
ops HailoRT didn't, and may skip ops HailoRT did. So strict
position-by-position comparison breaks almost immediately.

The strategy here is **ordered lockstep with bounded look-ahead
resync**:

- Walk corpus and trace pointers forward in parallel.
- At each step compare `(bar, offset, dir, size, value)` tuples.
- On match: advance both, continue.
- On mismatch: peek up to `lookahead` ops ahead on each side to see
  if a small skew explains it (SLM-OS added/missed a handful of ops
  but otherwise stays on the captured path). If a resync is found,
  record the skipped ops as `extra_in_trace` or `missing_from_trace`
  and continue from the resync point.
- If no resync is found within the window, the divergence is real —
  record `first_divergent_corpus_op` + `first_divergent_trace_op`
  and stop walking. The caller decides what to do with the report.

`lookahead=8` is the V1 default. Wider is more forgiving (better
for early-init ops where SLM-OS may probe before HailoRT does), but
also more likely to skip past a real divergence by interpreting it
as an asymmetric skip. Capture the cmdline-level argument so a real
investigation can tune it without code changes.

This module is **strictly** position-based. A multi-set / Hungarian-
alignment approach would surface "all the ops one side has and the
other doesn't" without an ordering claim, but answers a different
question — for the #682 reopen criteria we want to know *where in
the dispatch sequence* SLM-OS deviates from HailoRT, which is an
ordered-divergence question.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Sequence

from .boundary_trace import BoundaryTraceOp
from .corpus import OpEntry


def _ops_equal(corpus_op: OpEntry, trace_op: BoundaryTraceOp) -> bool:
    """A corpus op and trace op match iff every wire-observable field
    agrees. `phase` is metadata only (trace-side context), `seq` and
    `source` are corpus-side bookkeeping — neither participates."""
    return (
        corpus_op.bar == trace_op.bar
        and corpus_op.offset == trace_op.offset
        and corpus_op.dir == trace_op.dir
        and corpus_op.size == trace_op.size
        and corpus_op.value.lower() == trace_op.value.lower()
    )


def _shape_match(corpus_op: OpEntry, trace_op: BoundaryTraceOp) -> bool:
    """Shape match ignores value. Used by resync look-ahead so a
    different read-back value (e.g. a status register that flipped
    between captures) doesn't prevent us from rejoining the sequence
    — that's reported as a `value_mismatch` instead of an asymmetric
    skip, which is more actionable for the operator."""
    return (
        corpus_op.bar == trace_op.bar
        and corpus_op.offset == trace_op.offset
        and corpus_op.dir == trace_op.dir
        and corpus_op.size == trace_op.size
    )


@dataclass(frozen=True)
class _ValueMismatch:
    corpus_idx: int
    corpus_op: OpEntry
    trace_idx: int
    trace_op: BoundaryTraceOp


@dataclass(frozen=True)
class _AsymmetricSkip:
    """Records that one side had ops the other side didn't, recovered
    via look-ahead. `side` is "trace" (extra ops in trace not in
    corpus) or "corpus" (missing ops from trace that the corpus has)."""

    side: str           # "trace" or "corpus"
    corpus_idx: int     # corpus index where the skip began
    trace_idx: int      # trace index where the skip began
    skipped_ops_count: int


@dataclass
class NativeDiffReport:
    """Result of a `diff_native_against_corpus` call.

    `diverged` is True iff a position was reached where no look-ahead
    resync could explain the mismatch (i.e. SLM-OS native and the
    corpus genuinely disagree on what should happen next). The
    first_divergent_* fields locate it; `value_mismatches` and
    `asymmetric_skips` collect the soft divergences encountered
    along the way for context.
    """

    diverged: bool
    matched_op_count: int
    corpus_op_count: int
    trace_op_count: int
    first_divergent_corpus_idx: Optional[int] = None
    first_divergent_corpus_op: Optional[OpEntry] = None
    first_divergent_trace_idx: Optional[int] = None
    first_divergent_trace_op: Optional[BoundaryTraceOp] = None
    value_mismatches: List[_ValueMismatch] = field(default_factory=list)
    asymmetric_skips: List[_AsymmetricSkip] = field(default_factory=list)


def diff_native_against_corpus(
    corpus_ops: Sequence[OpEntry],
    trace_ops: Sequence[BoundaryTraceOp],
    *,
    lookahead: int = 8,
) -> NativeDiffReport:
    """Align trace_ops against corpus_ops and report the first hard
    divergence (or None if they line up cleanly).

    `corpus_ops` is assumed to be in seq-monotonic order (the order
    `corpus.load(...).ops` returns). `trace_ops` is in capture order
    from the kernel boundary trace.
    """
    i = 0  # corpus pointer
    j = 0  # trace pointer
    matched = 0
    value_mismatches: List[_ValueMismatch] = []
    asymmetric_skips: List[_AsymmetricSkip] = []

    while i < len(corpus_ops) and j < len(trace_ops):
        c = corpus_ops[i]
        t = trace_ops[j]

        if _ops_equal(c, t):
            matched += 1
            i += 1
            j += 1
            continue

        if _shape_match(c, t):
            # Same wire location, different value. Soft divergence —
            # most often a status-register read that flipped, sometimes
            # a real protocol-level value-mismatch (e.g. SLM-OS wrote
            # the wrong descriptor field). Record + advance both so
            # we can find the next divergence too.
            value_mismatches.append(_ValueMismatch(
                corpus_idx=i, corpus_op=c, trace_idx=j, trace_op=t,
            ))
            i += 1
            j += 1
            continue

        # Hard mismatch in shape. Try resync:
        # 1. Does corpus[i] match trace[j+k] for some k in [1, lookahead]?
        #    → SLM-OS did `k` extra ops; advance trace past them.
        resync_in_trace: Optional[int] = None
        for k in range(1, lookahead + 1):
            if j + k >= len(trace_ops):
                break
            if _shape_match(c, trace_ops[j + k]):
                resync_in_trace = k
                break

        # 2. Does corpus[i+k] match trace[j] for some k in [1, lookahead]?
        #    → SLM-OS missed `k` ops; advance corpus past them.
        resync_in_corpus: Optional[int] = None
        for k in range(1, lookahead + 1):
            if i + k >= len(corpus_ops):
                break
            if _shape_match(corpus_ops[i + k], t):
                resync_in_corpus = k
                break

        # Prefer the closer resync; tie-break toward "trace had extra"
        # because in practice native-flow traces are noisier than the
        # HailoRT-derived corpus (cache maintenance, sanity reads,
        # etc.).
        if resync_in_trace is not None and (
            resync_in_corpus is None or resync_in_trace <= resync_in_corpus
        ):
            asymmetric_skips.append(_AsymmetricSkip(
                side="trace", corpus_idx=i, trace_idx=j,
                skipped_ops_count=resync_in_trace,
            ))
            j += resync_in_trace
            continue
        if resync_in_corpus is not None:
            asymmetric_skips.append(_AsymmetricSkip(
                side="corpus", corpus_idx=i, trace_idx=j,
                skipped_ops_count=resync_in_corpus,
            ))
            i += resync_in_corpus
            continue

        # No resync within the window → hard divergence.
        return NativeDiffReport(
            diverged=True,
            matched_op_count=matched,
            corpus_op_count=len(corpus_ops),
            trace_op_count=len(trace_ops),
            first_divergent_corpus_idx=i,
            first_divergent_corpus_op=c,
            first_divergent_trace_idx=j,
            first_divergent_trace_op=t,
            value_mismatches=value_mismatches,
            asymmetric_skips=asymmetric_skips,
        )

    # Reached the end of one side without a hard divergence. If the
    # tail of either side is non-empty, that's reportable as an
    # asymmetric-skip too — but not a "divergence" in the strict
    # sense.
    if i < len(corpus_ops):
        asymmetric_skips.append(_AsymmetricSkip(
            side="corpus",
            corpus_idx=i,
            trace_idx=j,
            skipped_ops_count=len(corpus_ops) - i,
        ))
    if j < len(trace_ops):
        asymmetric_skips.append(_AsymmetricSkip(
            side="trace",
            corpus_idx=i,
            trace_idx=j,
            skipped_ops_count=len(trace_ops) - j,
        ))

    return NativeDiffReport(
        diverged=False,
        matched_op_count=matched,
        corpus_op_count=len(corpus_ops),
        trace_op_count=len(trace_ops),
        value_mismatches=value_mismatches,
        asymmetric_skips=asymmetric_skips,
    )


def format_report(report: NativeDiffReport) -> str:
    """Render a NativeDiffReport for the operator. Designed to be
    pasted into a Hailo support ticket / GH issue verbatim."""
    lines = [
        "native-flow vs corpus diff",
        "  matched ops:        %d / corpus=%d trace=%d" % (
            report.matched_op_count,
            report.corpus_op_count,
            report.trace_op_count,
        ),
        "  value mismatches:   %d" % len(report.value_mismatches),
        "  asymmetric skips:   %d" % len(report.asymmetric_skips),
        "",
    ]
    if report.diverged:
        c = report.first_divergent_corpus_op
        t = report.first_divergent_trace_op
        assert c is not None and t is not None
        lines.append(
            "DIVERGED at corpus_idx=%d trace_idx=%d:" % (
                report.first_divergent_corpus_idx,
                report.first_divergent_trace_idx,
            )
        )
        lines.append(
            "  corpus expected: bar=%d off=0x%04x size=%d dir=%-5s value=%s seq=%d" % (
                c.bar, c.offset, c.size, c.dir, c.value, c.seq,
            )
        )
        lines.append(
            "  trace observed:  bar=%d off=0x%04x size=%d dir=%-5s value=%s phase=%s" % (
                t.bar, t.offset, t.size, t.dir, t.value, t.phase,
            )
        )
    else:
        lines.append("clean match (no hard divergence)")
    if report.value_mismatches:
        lines.append("")
        lines.append("first 5 value mismatches:")
        for vm in report.value_mismatches[:5]:
            lines.append(
                "  corpus[%d] bar=%d off=0x%04x dir=%s: expected=%s observed=%s (phase=%s)" % (
                    vm.corpus_idx, vm.corpus_op.bar, vm.corpus_op.offset,
                    vm.corpus_op.dir, vm.corpus_op.value,
                    vm.trace_op.value, vm.trace_op.phase,
                )
            )
    return "\n".join(lines)
