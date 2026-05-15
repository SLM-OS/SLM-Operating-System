"""Append-only JSONL corpus IO + schema validation.

Contract: `docs/hailo-re-corpus-format.md`. One header line, then op lines, then
an optional trailer. The driver script (this package) is the single writer.
"""

from __future__ import annotations

import bisect
import contextlib
import json
import os
import re
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import IO, Iterable, Iterator, Optional

try:
    import fcntl  # POSIX advisory locks
    _HAVE_FLOCK = True
except ImportError:  # pragma: no cover — Windows fallback
    _HAVE_FLOCK = False


@contextlib.contextmanager
def _exclusive(fh: IO[str]):
    """Non-blocking advisory exclusive lock on the open file handle.

    The spec mandates a single writer per corpus. If a second process is
    already writing the same corpus, fail loud (CorpusError) rather than
    silently wait — silent serialization hides operator mistakes that the
    single-writer invariant exists to catch. On platforms without fcntl
    (Windows) the lock is a no-op.
    """
    if _HAVE_FLOCK:
        try:
            fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as e:
            raise CorpusError(
                f"{getattr(fh, 'name', '?')}: another writer holds the "
                "corpus lock — the single-writer invariant has been "
                "violated (is a second hailo-re-bootstrap running?)"
            ) from e
        try:
            yield
        finally:
            fcntl.flock(fh.fileno(), fcntl.LOCK_UN)
    else:
        yield


SUPPORTED_FORMAT_VERSION = 1
_HEX_RE = re.compile(r"^[0-9a-f]*$")
_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
_ISO_RE = re.compile(
    r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z$"
)
_KNOWN_DIRS = {"read", "write"}
_KNOWN_SIZES = {1, 2, 4, 8}


class CorpusError(ValueError):
    """Raised on any corpus schema or invariant violation."""


@dataclass(frozen=True)
class Header:
    format_version: int
    hailort_version: str
    fw_version: str
    capture_host: str
    slmos_base_sha: str
    capture_started_at: str
    notes: str = ""

    def to_json_obj(self) -> dict:
        obj = {"type": "header", **asdict(self)}
        if not self.notes:
            obj.pop("notes")
        return obj


@dataclass(frozen=True)
class OpEntry:
    seq: int
    bar: int
    offset: int
    size: int
    dir: str
    value: str
    source: str
    validated_at_commit: Optional[str]
    validated_at: Optional[str]
    note: str = ""

    def to_json_obj(self) -> dict:
        obj = {
            "type": "op",
            "seq": self.seq,
            "bar": self.bar,
            "offset": self.offset,
            "size": self.size,
            "dir": self.dir,
            "value": self.value,
            "source": self.source,
            "validated_at_commit": self.validated_at_commit,
            "validated_at": self.validated_at,
        }
        if self.note:
            obj["note"] = self.note
        return obj


@dataclass(frozen=True)
class Trailer:
    ended_at: str
    last_seq: Optional[int] = None
    reason: str = ""
    extras: dict = field(default_factory=dict)  # rollback_from_seq, etc.

    def to_json_obj(self) -> dict:
        reserved = {"type", "ended_at", "last_seq", "reason"}
        clashing = sorted(k for k in self.extras if k in reserved)
        if clashing:
            raise CorpusError(
                f"trailer.extras must not contain reserved keys {clashing!r}"
            )
        obj: dict = {"type": "trailer", "ended_at": self.ended_at}
        if self.last_seq is not None:
            obj["last_seq"] = self.last_seq
        if self.reason:
            obj["reason"] = self.reason
        obj.update(self.extras)
        return obj


# --------------------------------------------------------------------------- #
# Validation primitives
# --------------------------------------------------------------------------- #


def _validate_header(obj: dict) -> Header:
    if obj.get("type") != "header":
        raise CorpusError(f"expected header, got type={obj.get('type')!r}")
    for k in (
        "format_version",
        "hailort_version",
        "fw_version",
        "capture_host",
        "slmos_base_sha",
        "capture_started_at",
    ):
        if k not in obj:
            raise CorpusError(f"header missing required field {k!r}")
    fv = obj["format_version"]
    if not isinstance(fv, int) or fv != SUPPORTED_FORMAT_VERSION:
        raise CorpusError(
            f"unsupported format_version={fv!r} (driver supports "
            f"{SUPPORTED_FORMAT_VERSION})"
        )
    if not _ISO_RE.match(obj["capture_started_at"]):
        raise CorpusError(
            f"capture_started_at={obj['capture_started_at']!r} "
            "is not ISO 8601 UTC (YYYY-MM-DDTHH:MM:SS[.fff]Z)"
        )
    return Header(
        format_version=fv,
        hailort_version=str(obj["hailort_version"]),
        fw_version=str(obj["fw_version"]),
        capture_host=str(obj["capture_host"]),
        slmos_base_sha=str(obj["slmos_base_sha"]),
        capture_started_at=str(obj["capture_started_at"]),
        notes=str(obj.get("notes", "")),
    )


def _validate_op(obj: dict) -> OpEntry:
    if obj.get("type") != "op":
        raise CorpusError(f"expected op, got type={obj.get('type')!r}")
    for k in (
        "seq", "bar", "offset", "size", "dir", "value",
        "source", "validated_at_commit", "validated_at",
    ):
        if k not in obj:
            raise CorpusError(f"op missing required field {k!r}")
    seq = obj["seq"]
    if not isinstance(seq, int) or seq < 1:
        raise CorpusError(f"seq must be int >= 1, got {seq!r}")
    bar = obj["bar"]
    if not isinstance(bar, int) or bar not in (0, 2, 4):
        raise CorpusError(f"bar must be 0, 2, or 4, got {bar!r}")
    offset = obj["offset"]
    if not isinstance(offset, int) or offset < 0:
        raise CorpusError(f"offset must be non-negative int, got {offset!r}")
    size = obj["size"]
    if size not in _KNOWN_SIZES:
        raise CorpusError(f"size must be one of {sorted(_KNOWN_SIZES)}, got {size!r}")
    direction = obj["dir"]
    if direction not in _KNOWN_DIRS:
        raise CorpusError(f"dir must be read|write, got {direction!r}")
    value = obj["value"]
    if not isinstance(value, str) or not _HEX_RE.match(value):
        raise CorpusError(f"value must be lowercase hex string, got {value!r}")
    if len(value) != size * 2:
        raise CorpusError(
            f"value length {len(value)} != size*2 ({size*2}) for value={value!r}"
        )
    source = obj["source"]
    if not isinstance(source, str) or not source:
        raise CorpusError(f"source must be non-empty string, got {source!r}")
    vac = obj["validated_at_commit"]
    if vac is not None:
        if not isinstance(vac, str) or not _SHA_RE.match(vac):
            raise CorpusError(
                f"validated_at_commit must be null or full 40-char SHA, "
                f"got {vac!r}"
            )
    vat = obj["validated_at"]
    if vat is not None:
        if not isinstance(vat, str) or not _ISO_RE.match(vat):
            raise CorpusError(
                f"validated_at must be null or ISO 8601 UTC, got {vat!r}"
            )
    if (vac is None) != (vat is None):
        raise CorpusError(
            "validated_at_commit and validated_at must both be null or both set"
        )
    return OpEntry(
        seq=seq,
        bar=bar,
        offset=offset,
        size=size,
        dir=direction,
        value=value,
        source=source,
        validated_at_commit=vac,
        validated_at=vat,
        note=str(obj.get("note", "")),
    )


def _validate_trailer(obj: dict) -> Trailer:
    if obj.get("type") != "trailer":
        raise CorpusError(f"expected trailer, got type={obj.get('type')!r}")
    if "ended_at" not in obj:
        raise CorpusError("trailer missing required field 'ended_at'")
    if not _ISO_RE.match(obj["ended_at"]):
        raise CorpusError(
            f"trailer.ended_at={obj['ended_at']!r} is not ISO 8601 UTC"
        )
    reserved = {"type", "ended_at", "last_seq", "reason"}
    extras = {k: v for k, v in obj.items() if k not in reserved}
    return Trailer(
        ended_at=str(obj["ended_at"]),
        last_seq=obj.get("last_seq"),
        reason=str(obj.get("reason", "")),
        extras=extras,
    )


# --------------------------------------------------------------------------- #
# Corpus container
# --------------------------------------------------------------------------- #


@dataclass
class Corpus:
    path: Path
    header: Header
    ops: list[OpEntry] = field(default_factory=list)
    trailer: Optional[Trailer] = None
    # Phase 4 region rules — kept as raw dicts; the driver does not author
    # these (the standalone region-authoring tool does), so a structured
    # dataclass would be dead weight here. Stored so callers that DO care
    # (e.g. inspection tools) can introspect without re-parsing the file.
    regions: list[dict] = field(default_factory=list)
    # O(1) seq -> OpEntry index, kept in sync with `ops` by load and
    # append_op. Avoids the O(N) linear scan find_op() used to do, which
    # showed up in profiles once a single append_op started doing a
    # uniqueness check on every captured seq.
    _by_seq: dict[int, OpEntry] = field(default_factory=dict, repr=False)

    @property
    def next_seq(self) -> int:
        return self.ops[-1].seq + 1 if self.ops else 1

    @property
    def last_validated_seq(self) -> int:
        """Highest contiguous seq where every prior entry is validated."""
        last = 0
        for op in self.ops:
            if op.validated_at_commit is None:
                break
            last = op.seq
        return last

    def find_op(self, seq: int) -> Optional[OpEntry]:
        return self._by_seq.get(seq)


def load(path: os.PathLike | str) -> Corpus:
    p = Path(path)
    with p.open("r", encoding="utf-8") as f:
        lines = [ln for ln in f.read().splitlines() if ln.strip()]
    if not lines:
        raise CorpusError(f"{p}: empty corpus (header required)")
    try:
        header_obj = json.loads(lines[0])
    except json.JSONDecodeError as e:
        raise CorpusError(f"{p}: header is not valid JSON: {e}") from e
    header = _validate_header(header_obj)
    ops: list[OpEntry] = []
    op_line_by_seq: dict[int, int] = {}  # seq -> file line number, for diagnostics
    regions: list[dict] = []
    trailer: Optional[Trailer] = None
    for i, raw in enumerate(lines[1:], start=2):
        try:
            obj = json.loads(raw)
        except json.JSONDecodeError as e:
            raise CorpusError(f"{p}:{i}: not valid JSON: {e}") from e
        kind = obj.get("type")
        if kind == "op":
            op = _validate_op(obj)
            # File order is unconstrained — the C-side QEMU stub appends
            # at EOF in capture time order, which is monotonic within a
            # single run but not necessarily across runs (e.g. when a
            # later run captures a write at a seq the earlier run didn't
            # reach, then a still-later run uncovers a hole at a smaller
            # seq). The invariant we DO enforce: every seq appears at
            # most once. Strict ordering is checked on the sorted view
            # below.
            if op.seq in op_line_by_seq:
                raise CorpusError(
                    f"{p}:{i}: duplicate seq={op.seq} "
                    f"(first seen at line {op_line_by_seq[op.seq]})"
                )
            op_line_by_seq[op.seq] = i
            ops.append(op)
        elif kind == "region":
            # Tolerance per docs/hailo-re-corpus-format.md §"Backward
            # compatibility": tools that don't author regions still load
            # corpora that contain them. Stored as a raw dict for
            # introspection; the QEMU stub is the source of truth for
            # region semantics.
            regions.append(obj)
        elif kind == "trailer":
            if trailer is not None:
                raise CorpusError(f"{p}:{i}: more than one trailer")
            trailer = _validate_trailer(obj)
        else:
            raise CorpusError(f"{p}:{i}: unknown type={kind!r}")
        if trailer is not None and i != len(lines):
            raise CorpusError(
                f"{p}:{i}: trailer must be the final non-empty line"
            )
    # Sort ops by seq so callers iterating Corpus.ops get them in order
    # regardless of file order. Uniqueness was checked above.
    ops.sort(key=lambda o: o.seq)
    by_seq = {op.seq: op for op in ops}
    return Corpus(path=p, header=header, ops=ops, trailer=trailer,
                  regions=regions, _by_seq=by_seq)


def init(path: os.PathLike | str, header: Header) -> Corpus:
    """Create a brand-new corpus file with only the header line."""
    p = Path(path)
    if p.exists():
        raise CorpusError(f"{p}: refusing to overwrite existing corpus")
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8") as f:
        f.write(json.dumps(header.to_json_obj(), separators=(",", ":")))
        f.write("\n")
    return Corpus(path=p, header=header)


def append_op(corpus: Corpus, op: OpEntry) -> None:
    """Append a new op, enforcing seq uniqueness.

    The seq just needs to be unique within the corpus; gaps and
    out-of-file-order appends are both allowed. The file may end up
    with op lines in non-seq order — ``load`` sorts on the way in.
    """
    if corpus.trailer is not None:
        raise CorpusError(
            f"{corpus.path}: cannot append after trailer is written"
        )
    if corpus.find_op(op.seq) is not None:
        raise CorpusError(
            f"{corpus.path}: appended seq={op.seq} already exists in corpus"
        )
    # Validate the entry round-tripped through the same gate `load` uses.
    _validate_op(op.to_json_obj())
    with corpus.path.open("a", encoding="utf-8") as f, _exclusive(f):
        f.write(json.dumps(op.to_json_obj(), separators=(",", ":")))
        f.write("\n")
    # Keep corpus.ops sorted by seq so iteration order matches load order.
    bisect.insort(corpus.ops, op, key=lambda o: o.seq)
    corpus._by_seq[op.seq] = op


def append_trailer(corpus: Corpus, trailer: Trailer) -> None:
    if corpus.trailer is not None:
        raise CorpusError(f"{corpus.path}: trailer already present")
    _validate_trailer(trailer.to_json_obj())
    with corpus.path.open("a", encoding="utf-8") as f, _exclusive(f):
        f.write(json.dumps(trailer.to_json_obj(), separators=(",", ":")))
        f.write("\n")
    corpus.trailer = trailer


def iter_ops(corpus: Corpus) -> Iterator[OpEntry]:
    return iter(corpus.ops)


def write_full(path: os.PathLike | str, header: Header,
               ops: Iterable[OpEntry], trailer: Optional[Trailer] = None) -> None:
    """Write a fresh corpus from scratch (used by rollback)."""
    p = Path(path)
    if p.exists():
        raise CorpusError(f"{p}: refusing to overwrite existing corpus")
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8") as f:
        f.write(json.dumps(header.to_json_obj(), separators=(",", ":")))
        f.write("\n")
        prev = 0
        for op in ops:
            if op.seq != prev + 1:
                raise CorpusError(
                    f"write_full: non-monotonic seq {prev} -> {op.seq}"
                )
            _validate_op(op.to_json_obj())
            f.write(json.dumps(op.to_json_obj(), separators=(",", ":")))
            f.write("\n")
            prev = op.seq
        if trailer is not None:
            _validate_trailer(trailer.to_json_obj())
            f.write(json.dumps(trailer.to_json_obj(), separators=(",", ":")))
            f.write("\n")
