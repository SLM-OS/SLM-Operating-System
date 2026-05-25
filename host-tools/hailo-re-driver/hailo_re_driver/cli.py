"""Command-line entry points for the driver.

Four commands:

- `hailo-re-bootstrap`   — run the Phase 1/2 single-step loop.
- `hailo-re-validate`    — Phase 3 re-validation; stamp validated_at_commit.
- `hailo-re-diff`        — standalone corpus diff tool.
- `hailo-re-native-diff` — diff a SLM-OS native-flow boundary trace
                            against a corpus, locating the first hard
                            divergence (most relevant to the #682
                            reopen criteria).
"""

from __future__ import annotations

import argparse
import datetime as _dt
import logging
import sys
from pathlib import Path
from typing import Optional

from . import corpus as corpus_mod
from . import diff as diff_mod
from . import loop as loop_mod
from . import rollback as rollback_mod
from . import slmos_runner
from .corpus import OpEntry
from .qemu_runner import QemuRunner, default_command_factory, mock_runner_from_lines
from .slmos_runner import (
    MockSlmosRunner,
    ReplayDivergence,
    ReplayError,
    ReplayResponse,
    SlmosRunner,
    dry_run_transport,
)


# --------------------------------------------------------------------------- #
# Shared helpers
# --------------------------------------------------------------------------- #


def _now_iso() -> str:
    return _dt.datetime.now(tz=_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _setup_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        format="%(levelname)s %(name)s: %(message)s",
    )


def _build_qemu_runner(args: argparse.Namespace) -> QemuRunner:
    if args.mock_qemu_lines:
        with open(args.mock_qemu_lines, "r", encoding="utf-8") as f:
            lines = [ln.rstrip("\n") for ln in f if ln.strip()]
        return mock_runner_from_lines(lines, returncode=args.mock_qemu_rc)
    return QemuRunner(command_factory=default_command_factory)


def _build_slmos_runner(args: argparse.Namespace):
    if args.mock_slmos_responses:
        return MockSlmosRunner.from_file(args.mock_slmos_responses)
    runner = slmos_runner.from_env()
    if args.dry_run:
        return SlmosRunner(
            sbc=runner.sbc,
            kernel_path=runner.kernel_path,
            verify_sd_before_flash=False,
            transport=dry_run_transport,
        )
    return runner


# --------------------------------------------------------------------------- #
# hailo-re-bootstrap
# --------------------------------------------------------------------------- #


def bootstrap_main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="hailo-re-bootstrap",
        description=(
            "Run the Hailo RE Phase 1/2 capture-replay loop until the QEMU "
            "stub completes a HailoRT Configure() + Activate() with no "
            "unknown reads, or a divergence triggers rollback."
        ),
    )
    p.add_argument("corpus", type=Path,
                   help="path to the corpus JSONL file")
    p.add_argument("--max-iterations", type=int, default=10_000,
                   help="hard ceiling on iterations (default: 10000)")
    p.add_argument("--dry-run", action="store_true",
                   help="never invoke labctl; SLM-OS replay returns dry-run "
                        "success without effect")
    p.add_argument("--mock-qemu-lines", type=Path, default=None,
                   help="(testing) read QEMU stdout from this file instead of "
                        "spawning real QEMU")
    p.add_argument("--mock-qemu-rc", type=int, default=1,
                   help="(testing) exit code the mock QEMU should return")
    p.add_argument("--mock-slmos-responses", type=Path, default=None,
                   help="(testing) file of HAILO_RE_CORPUS_RESPONSE lines to "
                        "serve in order instead of calling labctl")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)
    _setup_logging(args.verbose)

    qemu = _build_qemu_runner(args)
    slmos = _build_slmos_runner(args)
    outcome = loop_mod.run_loop(
        args.corpus, qemu, slmos,
        loop_mod.LoopConfig(max_iterations=args.max_iterations),
    )
    print(f"status={outcome.status} iterations={outcome.iterations} "
          f"last_seq_appended={outcome.last_seq_appended}")
    if outcome.detail:
        print(outcome.detail)
    if outcome.rollback:
        rb = outcome.rollback
        print(f"rolled back from seq={rb.rollback_from_seq} to "
              f"seq={rb.rollback_to_seq}")
        print(f"poisoned corpus: {rb.poisoned_path}")
        print(f"new corpus:      {rb.new_corpus_path}")
    return 0 if outcome.status == "complete" else 1


# --------------------------------------------------------------------------- #
# hailo-re-validate
# --------------------------------------------------------------------------- #


def validate_main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="hailo-re-validate",
        description=(
            "Phase 3 re-validation: ask SLM-OS to replay the corpus from "
            "seq=1, capture every observed read, diff against the corpus, "
            "stamp validated_at_commit up to divergence-or-EOF. On divergence, "
            "trigger rollback."
        ),
    )
    p.add_argument("corpus", type=Path)
    p.add_argument("--observed-trace", type=Path, required=True,
                   help="JSONL file SLM-OS wrote when replaying the corpus "
                        "from seq=1 (one op line per real-hardware op)")
    p.add_argument("--slmos-sha", type=str, required=True,
                   help="full 40-char SHA the SLM-OS build under test was "
                        "compiled from — stamped into validated_at_commit")
    p.add_argument("--dry-run", action="store_true",
                   help="report what would be stamped without writing")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)
    _setup_logging(args.verbose)

    if len(args.slmos_sha) != 40:
        print(f"--slmos-sha must be a full 40-char SHA, got {args.slmos_sha!r}",
              file=sys.stderr)
        return 2

    # Load corpus + observed-trace exactly once.
    corpus = corpus_mod.load(args.corpus)
    observed_ops = list(diff_mod.load_observed_ops(args.observed_trace))

    # Detect duplicate seqs in the trace — these point at an SLM-OS bug, not
    # a corpus integrity issue, but stamping past one would be wrong.
    seen: set[int] = set()
    duplicates: set[int] = set()
    for op in observed_ops:
        if op.seq in seen:
            duplicates.add(op.seq)
        seen.add(op.seq)
    if duplicates:
        print(
            f"refusing to validate: observed-trace contains duplicate seq "
            f"values {sorted(duplicates)[:5]}",
            file=sys.stderr,
        )
        return 2

    # Require the observed-trace to cover every seq in the corpus before
    # advancing the validation watermark. Otherwise a partial replay could
    # stamp entries SLM-OS never actually verified.
    corpus_seqs = {op.seq for op in corpus.ops}
    missing = sorted(corpus_seqs - seen)
    if missing:
        print(
            f"refusing to validate: observed-trace covers "
            f"{len(seen & corpus_seqs)}/{len(corpus_seqs)} corpus "
            f"entries. Missing seq examples: {missing[:5]}"
            f"{'...' if len(missing) > 5 else ''}",
            file=sys.stderr,
        )
        return 2

    report = diff_mod.diff_ops_against_path(
        corpus.ops, observed_ops,
        left_path=args.corpus, right_path=args.observed_trace,
    )
    print(diff_mod.format_report(report))

    if report.diverged:
        if args.dry_run:
            print("(dry-run) would trigger rollback")
            return 1
        reachable = rollback_mod.git_reachable_from_main()
        rb = rollback_mod.handle(
            corpus,
            divergent_seq=report.first_divergent_seq,
            reachable=reachable,
            reason="phase3_revalidation",
        )
        print(f"rolled back from seq={rb.rollback_from_seq} to "
              f"seq={rb.rollback_to_seq}")
        print(f"poisoned corpus: {rb.poisoned_path}")
        print(f"new corpus:      {rb.new_corpus_path}")
        return 1

    if args.dry_run:
        print("(dry-run) would stamp validated_at_commit on all entries")
        return 0

    _stamp_validated(args.corpus, args.slmos_sha, _now_iso())
    print(f"stamped validated_at_commit={args.slmos_sha} on every op entry")
    return 0


def _stamp_validated(corpus_path: Path, sha: str, ts: str) -> None:
    """Rewrite the corpus with validated_at_commit/validated_at filled in.

    This is destructive — only the validate path may do it, and only after a
    full clean diff. Preserves any trailer.
    """
    corpus = corpus_mod.load(corpus_path)
    new_ops = [
        OpEntry(
            seq=op.seq, bar=op.bar, offset=op.offset, size=op.size,
            dir=op.dir, value=op.value, source=op.source,
            validated_at_commit=sha,
            validated_at=ts,
            note=op.note,
        )
        for op in corpus.ops
    ]
    tmp = corpus_path.with_suffix(corpus_path.suffix + ".tmp")
    if tmp.exists():
        tmp.unlink()
    corpus_mod.write_full(tmp, corpus.header, new_ops, corpus.trailer)
    tmp.replace(corpus_path)


# --------------------------------------------------------------------------- #
# hailo-re-diff
# --------------------------------------------------------------------------- #


def diff_main(argv: Optional[list[str]] = None) -> int:
    p = argparse.ArgumentParser(
        prog="hailo-re-diff",
        description=(
            "Compare two corpora, or one corpus against an observed-trace "
            "JSONL produced by SLM-OS. Reports the first divergent seq."
        ),
    )
    p.add_argument("left", type=Path, help="left corpus JSONL")
    p.add_argument("right", type=Path,
                   help="right corpus JSONL, or (with --observed) an "
                        "observed-trace JSONL")
    p.add_argument("--observed", action="store_true",
                   help="treat <right> as an SLM-OS observed-trace (no header)")
    p.add_argument("--show-matches", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)
    _setup_logging(args.verbose)

    if args.observed:
        report = diff_mod.diff_against_observed(args.left, args.right)
    else:
        report = diff_mod.diff_corpora(args.left, args.right)
    print(diff_mod.format_report(report, show_matches=args.show_matches))
    return 1 if report.diverged else 0


# --------------------------------------------------------------------------- #
# hailo-re-native-diff
# --------------------------------------------------------------------------- #


def native_diff_main(argv: Optional[list[str]] = None) -> int:
    """Diff a native-flow boundary trace against a corpus.

    Unlike `hailo-re-diff --observed`, this expects a raw capture from
    the kernel boundary-trace toolkit (`hailo trace ...` plus
    `scripts/capture-hailo-trace.sh`), not a pre-formatted observed-
    trace JSONL. The aligner does bounded look-ahead resync, so
    extra/missing ops on either side don't immediately stop the walk.
    """
    from . import native_diff as native_diff_mod
    from .boundary_trace import parse_trace_stream

    p = argparse.ArgumentParser(
        prog="hailo-re-native-diff",
        description=(
            "Diff a SLM-OS native-flow boundary-trace capture against a "
            "corpus. Locates the first position where SLM-OS native and "
            "HailoRT-via-corpus take divergent paths — the actionable "
            "finding for the #682 reopen criteria."
        ),
    )
    p.add_argument("corpus", type=Path,
                   help="corpus JSONL (header + ops)")
    p.add_argument("trace", type=Path,
                   help="capture-hailo-trace.sh output (mixed serial log; "
                        "non-MMIO lines are silently skipped)")
    p.add_argument("--lookahead", type=int, default=8,
                   help="resync look-ahead window; wider papers over more "
                        "skew, narrower surfaces more divergences (default: 8)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args(argv)
    _setup_logging(args.verbose)

    # A non-positive look-ahead would make every shape mismatch an
    # immediate hard divergence (the inner range(1, lookahead+1) is
    # empty). Probably not what the operator meant — fail fast with a
    # specific message instead of producing a confusing report.
    if args.lookahead <= 0:
        print(
            f"--lookahead must be > 0, got {args.lookahead}",
            file=sys.stderr,
        )
        return 2

    corpus = corpus_mod.load(args.corpus)
    with args.trace.open() as f:
        trace_ops = list(parse_trace_stream(f))
    report = native_diff_mod.diff_native_against_corpus(
        corpus.ops, trace_ops, lookahead=args.lookahead,
    )
    print(native_diff_mod.format_report(report))
    return 1 if report.diverged else 0
