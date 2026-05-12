"""Re-validation rollback (corpus-format spec §Re-validation rollback).

Single permitted destructive operation on a corpus. Steps:

1. Find the last validated seq `K` whose `validated_at_commit` is reachable
   from current `main`.
2. Preserve the poisoned corpus by renaming with a `.poisoned` suffix.
3. Write a fresh corpus file (incremented date suffix) seeded with the header
   and seq <= K entries, plus a trailer recording the rollback.

The driver script is the only writer. No other tool may invoke this.
"""

from __future__ import annotations

import datetime as _dt
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

from . import corpus as corpus_mod
from .corpus import Corpus, CorpusError, Trailer


SHA_REACHABLE = Callable[[str], bool]


def git_reachable_from_main(repo_root: Optional[Path] = None,
                            base_ref: str = "main") -> SHA_REACHABLE:
    """Return a predicate: True if SHA is reachable from <base_ref>."""
    cwd = str(repo_root) if repo_root else None

    def predicate(sha: str) -> bool:
        if not sha:
            return False
        proc = subprocess.run(
            ["git", "merge-base", "--is-ancestor", sha, base_ref],
            cwd=cwd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return proc.returncode == 0

    return predicate


@dataclass(frozen=True)
class RollbackResult:
    rollback_to_seq: int
    rollback_from_seq: int
    poisoned_path: Path
    new_corpus_path: Path


def find_rollback_target(corpus: Corpus, reachable: SHA_REACHABLE) -> int:
    """Find the last seq with validated_at_commit reachable from main."""
    target = 0
    for op in corpus.ops:
        sha = op.validated_at_commit
        if sha is None:
            continue
        if reachable(sha):
            target = op.seq
    return target


_DATE_SUFFIX_RE = re.compile(r"-(\d{4}-\d{2}-\d{2})(?=\.[^.]+$)")


def _bump_corpus_filename(path: Path, today: Optional[_dt.date] = None) -> Path:
    """Return a new path with today's date suffix; if the original already
    has today's date, append a numeric disambiguator."""
    today = today or _dt.date.today()
    today_str = today.isoformat()
    name = path.name
    if _DATE_SUFFIX_RE.search(name):
        new_name = _DATE_SUFFIX_RE.sub(f"-{today_str}", name, count=1)
    else:
        stem, dot, ext = name.rpartition(".")
        if not dot:
            new_name = f"{name}-{today_str}"
        else:
            new_name = f"{stem}-{today_str}.{ext}"
    candidate = path.with_name(new_name)
    if candidate.resolve() == path.resolve() or candidate.exists():
        # Disambiguate by adding a numeric suffix.
        i = 2
        while True:
            stem, dot, ext = candidate.name.rpartition(".")
            disambiguated = candidate.with_name(
                f"{stem}.{i}.{ext}" if dot else f"{candidate.name}.{i}"
            )
            if not disambiguated.exists() and disambiguated.resolve() != path.resolve():
                return disambiguated
            i += 1
    return candidate


def handle(
    corpus: Corpus,
    *,
    divergent_seq: int,
    reachable: SHA_REACHABLE,
    reason: str,
    now: Optional[_dt.datetime] = None,
) -> RollbackResult:
    """Execute the rollback procedure. Returns paths of (poisoned, new) files.

    The in-memory `corpus` object is NOT mutated — the caller is expected to
    `corpus.load()` the new path afterwards.
    """
    k = find_rollback_target(corpus, reachable)
    if k == 0:
        raise CorpusError(
            "rollback aborted: no validated entry reachable from main — "
            "the corpus has no usable validated frontier. Operator must "
            "manually start a fresh corpus."
        )
    if k >= divergent_seq:
        raise CorpusError(
            f"rollback target {k} >= divergent seq {divergent_seq}; "
            "nothing to roll back"
        )
    now = now or _dt.datetime.now(tz=_dt.timezone.utc)
    ts = now.strftime("%Y-%m-%dT%H:%M:%SZ")

    poisoned_path = corpus.path.with_name(corpus.path.name + ".poisoned")
    if poisoned_path.exists():
        # Disambiguate forensic artifacts so we never silently overwrite one.
        i = 2
        while True:
            cand = corpus.path.with_name(f"{corpus.path.name}.poisoned.{i}")
            if not cand.exists():
                poisoned_path = cand
                break
            i += 1

    new_path = _bump_corpus_filename(corpus.path, today=now.date())

    # Step 1: write fresh corpus (header + ops up to K + trailer).
    trailer = Trailer(
        ended_at=ts,
        reason=f"divergence_at_{divergent_seq}",
        extras={
            "rollback_from_seq": divergent_seq,
            "rollback_to_seq": k,
            "rollback_cause": reason,
        },
    )
    truncated_ops = [op for op in corpus.ops if op.seq <= k]
    corpus_mod.write_full(new_path, corpus.header, truncated_ops, trailer)

    # Step 2: rename the poisoned file (only after the new one is on disk).
    corpus.path.rename(poisoned_path)

    return RollbackResult(
        rollback_to_seq=k,
        rollback_from_seq=divergent_seq,
        poisoned_path=poisoned_path,
        new_corpus_path=new_path,
    )
