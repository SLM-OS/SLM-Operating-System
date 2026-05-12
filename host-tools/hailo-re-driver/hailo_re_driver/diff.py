"""Corpus diff tool — compare two corpora, or a corpus vs an observed trace.

Reports the first divergent seq and a one-line summary per entry up to that
point. Used as the Phase 3 oracle and for human inspection of poisoned vs
fresh corpora.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

from . import corpus as corpus_mod
from .corpus import OpEntry


@dataclass(frozen=True)
class DiffRow:
    seq: int
    status: str   # "match" | "shape_mismatch" | "value_mismatch" | "left_only" | "right_only"
    left: Optional[OpEntry]
    right: Optional[OpEntry]
    detail: str = ""


@dataclass(frozen=True)
class DiffReport:
    left_path: Path
    right_path: Path
    rows: list[DiffRow]
    first_divergent_seq: Optional[int]

    @property
    def diverged(self) -> bool:
        return self.first_divergent_seq is not None


def diff_corpora(left_path: Path, right_path: Path) -> DiffReport:
    left = corpus_mod.load(left_path)
    right = corpus_mod.load(right_path)
    rows = _diff_op_streams(left.ops, right.ops)
    first = next((r.seq for r in rows if r.status != "match"), None)
    return DiffReport(left_path=left_path, right_path=right_path,
                      rows=rows, first_divergent_seq=first)


def diff_against_observed(
    corpus_path: Path, observed_path: Path
) -> DiffReport:
    """Diff a corpus against an `observed-trace` JSONL with bare op lines.

    Observed trace lines: same op schema as the corpus, but no header.
    Produced by SLM-OS Phase 3 replay (one line per real-hardware op).
    """
    left = corpus_mod.load(corpus_path)
    right_ops = list(load_observed_ops(observed_path))
    return diff_ops_against_path(
        left.ops, right_ops,
        left_path=corpus_path, right_path=observed_path,
    )


def diff_ops_against_path(
    left_ops: list[OpEntry], right_ops: list[OpEntry],
    *, left_path: Path, right_path: Path,
) -> DiffReport:
    """Diff already-loaded op lists. Used when the caller already has both
    sides in memory and wants to avoid re-reading files."""
    rows = _diff_op_streams(left_ops, right_ops)
    first = next((r.seq for r in rows if r.status != "match"), None)
    return DiffReport(left_path=left_path, right_path=right_path,
                      rows=rows, first_divergent_seq=first)


def load_observed_ops(path: Path) -> Iterable[OpEntry]:
    """Stream-parse an observed-trace JSONL (no header; op lines only)."""
    with path.open("r", encoding="utf-8") as f:
        for i, raw in enumerate(f, start=1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError as e:
                raise ValueError(
                    f"{path}:{i}: observed-trace line is not valid JSON: {e}"
                ) from e
            if obj.get("type") != "op":
                raise ValueError(
                    f"{path}:{i}: observed-trace lines must all be op entries "
                    f"(got type={obj.get('type')!r})"
                )
            yield corpus_mod._validate_op(obj)  # type: ignore[attr-defined]


def observed_seqs(path: Path) -> list[int]:
    """Return the seq values present in an observed-trace JSONL.

    Prefer `load_observed_ops` directly when the caller also needs the
    parsed op entries themselves.
    """
    return [op.seq for op in load_observed_ops(path)]


def _diff_op_streams(
    left: list[OpEntry], right: list[OpEntry]
) -> list[DiffRow]:
    by_seq_left = {op.seq: op for op in left}
    by_seq_right = {op.seq: op for op in right}
    seqs = sorted(set(by_seq_left) | set(by_seq_right))
    rows: list[DiffRow] = []
    for seq in seqs:
        l = by_seq_left.get(seq)
        r = by_seq_right.get(seq)
        if l is None:
            rows.append(DiffRow(seq=seq, status="right_only",
                                left=None, right=r,
                                detail="present in right only"))
            continue
        if r is None:
            rows.append(DiffRow(seq=seq, status="left_only",
                                left=l, right=None,
                                detail="present in left only"))
            continue
        if (l.dir, l.bar, l.offset, l.size) != (r.dir, r.bar, r.offset, r.size):
            rows.append(DiffRow(
                seq=seq, status="shape_mismatch", left=l, right=r,
                detail=(
                    f"shape differs: left=({l.dir},bar{l.bar},off{l.offset},"
                    f"sz{l.size}) right=({r.dir},bar{r.bar},off{r.offset},"
                    f"sz{r.size})"
                ),
            ))
            continue
        if l.value != r.value:
            rows.append(DiffRow(
                seq=seq, status="value_mismatch", left=l, right=r,
                detail=f"value: left={l.value} right={r.value}",
            ))
            continue
        rows.append(DiffRow(seq=seq, status="match", left=l, right=r))
    return rows


def format_report(report: DiffReport, *, show_matches: bool = False) -> str:
    lines = [
        f"left:  {report.left_path}",
        f"right: {report.right_path}",
    ]
    if report.diverged:
        lines.append(
            f"first divergence at seq={report.first_divergent_seq}"
        )
    else:
        lines.append("no divergence")
    for row in report.rows:
        if row.status == "match" and not show_matches:
            continue
        lines.append(f"  seq={row.seq:>6}  {row.status:<16}  {row.detail}")
    return "\n".join(lines)
