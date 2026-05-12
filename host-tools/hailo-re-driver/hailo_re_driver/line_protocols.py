"""Parse and emit the three HAILO_RE_CORPUS_* line protocols.

Contract: `docs/hailo-re-corpus-format.md` §Corpus-extension request,
§Corpus-extension response, §Divergence report.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Optional


class LineProtocolError(ValueError):
    """Raised when a HAILO_RE_CORPUS_* line is malformed."""


_KV_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=(\S+)")
_HEX_RE = re.compile(r"^[0-9a-f]+$")
_SHA_RE = re.compile(r"^[0-9a-f]{40}$")
_DIRS = {"read", "write"}


def _kvparse(rest: str) -> dict[str, str]:
    pairs = _KV_RE.findall(rest)
    if not pairs:
        raise LineProtocolError(f"no key=value tokens in: {rest!r}")
    out: dict[str, str] = {}
    for k, v in pairs:
        if k in out:
            raise LineProtocolError(f"duplicate key {k!r} in line")
        out[k] = v
    return out


def _require(kv: dict[str, str], keys: tuple[str, ...]) -> None:
    missing = [k for k in keys if k not in kv]
    if missing:
        raise LineProtocolError(f"missing keys {missing!r} (got {sorted(kv)})")


def _as_int(kv: dict[str, str], key: str) -> int:
    raw = kv[key]
    try:
        return int(raw, 0)
    except ValueError as e:
        raise LineProtocolError(f"{key}={raw!r} is not an integer") from e


def _as_hex(kv: dict[str, str], key: str, size_bytes: int) -> str:
    raw = kv[key]
    expected = size_bytes * 2
    if not _HEX_RE.match(raw):
        raise LineProtocolError(
            f"{key}={raw!r} is not lowercase hex (spec: lowercase, no 0x prefix)"
        )
    if len(raw) != expected:
        raise LineProtocolError(
            f"{key}={raw!r} length {len(raw)} != size*2 ({expected})"
        )
    return raw


def _as_dir(kv: dict[str, str], key: str) -> str:
    v = kv[key]
    if v not in _DIRS:
        raise LineProtocolError(f"{key}={v!r} must be one of {sorted(_DIRS)}")
    return v


# --------------------------------------------------------------------------- #
# Corpus-extension request: QEMU stub -> driver
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class ExtendRequest:
    seq: int
    bar: int
    offset: int
    size: int
    reason: str  # always "unknown_read" today, but tolerate new tags

    @classmethod
    def parse(cls, line: str) -> "ExtendRequest":
        prefix, _, rest = line.strip().partition(" ")
        if prefix != "HAILO_RE_CORPUS_EXTEND":
            raise LineProtocolError(f"not an EXTEND line: {prefix!r}")
        kv = _kvparse(rest)
        _require(kv, ("seq", "bar", "offset", "size", "reason"))
        return cls(
            seq=_as_int(kv, "seq"),
            bar=_as_int(kv, "bar"),
            offset=_as_int(kv, "offset"),
            size=_as_int(kv, "size"),
            reason=kv["reason"],
        )

    def emit(self) -> str:
        return (
            f"HAILO_RE_CORPUS_EXTEND seq={self.seq} bar={self.bar} "
            f"offset={self.offset} size={self.size} reason={self.reason}"
        )


# --------------------------------------------------------------------------- #
# Corpus-extension response: SLM-OS replay-step -> driver
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class ExtendResponse:
    seq: int
    bar: int
    offset: int
    size: int
    value: str       # lowercase hex, len = size*2
    slmos_sha: str   # full 40-char SHA

    @classmethod
    def parse(cls, line: str) -> "ExtendResponse":
        prefix, _, rest = line.strip().partition(" ")
        if prefix != "HAILO_RE_CORPUS_RESPONSE":
            raise LineProtocolError(f"not a RESPONSE line: {prefix!r}")
        kv = _kvparse(rest)
        _require(kv, ("seq", "bar", "offset", "size", "value", "slmos_sha"))
        size = _as_int(kv, "size")
        sha = kv["slmos_sha"]
        if not _SHA_RE.match(sha):
            raise LineProtocolError(
                f"slmos_sha={sha!r} is not a full 40-char hex SHA"
            )
        return cls(
            seq=_as_int(kv, "seq"),
            bar=_as_int(kv, "bar"),
            offset=_as_int(kv, "offset"),
            size=size,
            value=_as_hex(kv, "value", size),
            slmos_sha=sha,
        )

    def emit(self) -> str:
        return (
            f"HAILO_RE_CORPUS_RESPONSE seq={self.seq} bar={self.bar} "
            f"offset={self.offset} size={self.size} value={self.value} "
            f"slmos_sha={self.slmos_sha}"
        )


# --------------------------------------------------------------------------- #
# Divergence report: QEMU stub or SLM-OS -> driver
# --------------------------------------------------------------------------- #


@dataclass(frozen=True)
class DivergenceReport:
    seq: int
    bar: int
    offset: int
    size: int
    dir: str         # "read" or "write"
    expected: str    # lowercase hex, len = size*2
    observed: str    # lowercase hex, len = size*2
    source: str      # "qemu" or "slmos" (tools must accept future values)
    reason: str      # short tag, tools must accept future values

    @classmethod
    def parse(cls, line: str) -> "DivergenceReport":
        prefix, _, rest = line.strip().partition(" ")
        if prefix != "HAILO_RE_CORPUS_DIVERGENCE":
            raise LineProtocolError(f"not a DIVERGENCE line: {prefix!r}")
        kv = _kvparse(rest)
        _require(
            kv,
            ("seq", "bar", "offset", "size", "dir",
             "expected", "observed", "source", "reason"),
        )
        size = _as_int(kv, "size")
        return cls(
            seq=_as_int(kv, "seq"),
            bar=_as_int(kv, "bar"),
            offset=_as_int(kv, "offset"),
            size=size,
            dir=_as_dir(kv, "dir"),
            expected=_as_hex(kv, "expected", size),
            observed=_as_hex(kv, "observed", size),
            source=kv["source"],
            reason=kv["reason"],
        )

    def emit(self) -> str:
        return (
            f"HAILO_RE_CORPUS_DIVERGENCE seq={self.seq} bar={self.bar} "
            f"offset={self.offset} size={self.size} dir={self.dir} "
            f"expected={self.expected} observed={self.observed} "
            f"source={self.source} reason={self.reason}"
        )


# --------------------------------------------------------------------------- #
# Detection helper
# --------------------------------------------------------------------------- #


_PREFIXES = {
    "HAILO_RE_CORPUS_EXTEND": ExtendRequest,
    "HAILO_RE_CORPUS_RESPONSE": ExtendResponse,
    "HAILO_RE_CORPUS_DIVERGENCE": DivergenceReport,
}


def parse_any(line: str) -> Optional[object]:
    """Return the parsed event for any HAILO_RE_CORPUS_* line, else None."""
    head = line.strip().split(" ", 1)[0]
    cls = _PREFIXES.get(head)
    return cls.parse(line) if cls else None
