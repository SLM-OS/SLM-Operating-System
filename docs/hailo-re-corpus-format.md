# Hailo RE Corpus Format

**Status:** Specification, anchored to issue [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795) Phase 0 Task 0.1. Defines the shared on-disk schema used by all five Phase 0 components: QEMU stub device (Task 0.2), HailoRT-in-VM driver (0.3), SLM-OS `hailo replay-step` shell command (0.4), and the diff + driver script (0.5).

This document is the interface contract between those components. Changes here ripple to every consumer — bump the corpus header `format_version` field on any non-additive change.

## Purpose

The corpus is an append-only ledger that records, in strict sequence order, every PCIe MMIO operation HailoRT performs against the Hailo NPU during a captured session. Two consumers:

- **QEMU stub device** (0.2) uses the corpus to answer reads HailoRT issues inside the VM. For each read at sequence `N` against offset `X`, the stub looks up `(seq=N, dir=read, offset=X)`; if found, returns the recorded value; if not, freezes the VM and emits a corpus-extension request.
- **SLM-OS `hailo replay-step <N>`** (0.4) uses the corpus to reproduce HailoRT's exact write sequence against real hardware. It issues every write entry with `seq < N` in order, then performs the read at `seq = N` and logs the observed response. The response is then appended to the corpus (with `source = "slmos_observed"`) and the QEMU run is restarted with the extended corpus.

The same corpus thus encodes both "what HailoRT did" and "what real firmware replied", in the same sequence. The `validated_at_commit` field on each entry is the load-bearing audit field — it records the git SHA at which the entry was confirmed correct on real hardware. Entries without that field are tentative and MUST NOT be relied on by SLM-OS code outside the RE driver loop.

## File layout

One corpus per capture session. Filename convention:

```
~/slmos-ref/derivatives/hailo-re-corpora/<hailort-version>-<fw-version>-<capture-host>-<YYYY-MM-DD>.jsonl
```

JSONL (JSON Lines) — one JSON object per line, UTF-8, LF line endings. Append-only; never rewrite earlier lines except via the rollback procedure in §Re-validation rollback.

### Line 1: header

```json
{"type":"header","format_version":1,"hailort_version":"4.23.0","fw_version":"4.23.0","capture_host":"qemu-x86_64-ubuntu24.04","slmos_base_sha":"<full sha>","capture_started_at":"2026-05-12T18:30:00Z","notes":"<free-form>"}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `type` | string | yes | Always `"header"` for line 1 |
| `format_version` | integer | yes | Bump on any non-additive change to this spec |
| `hailort_version` | string | yes | Version of libhailort under capture |
| `fw_version` | string | yes | Hailo-8 firmware version on the target |
| `capture_host` | string | yes | QEMU machine identifier — `qemu-x86_64-...`, `qemu-aarch64-...` |
| `slmos_base_sha` | string | yes | SLM-OS commit the SLM-OS replay-step ran against |
| `capture_started_at` | string | yes | ISO 8601 UTC timestamp |
| `notes` | string | no | Free-form human annotation |

### Lines 2..N: operation entries

Two record types. Both share the common envelope:

```json
{"type":"op","seq":<int>,"bar":<int>,"offset":<int>,"size":<int>,"dir":"<read|write>","value":"<hex>","source":"<string>","validated_at_commit":"<sha or null>","validated_at":"<iso or null>","note":"<optional>"}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `type` | string | yes | Always `"op"` for operation lines |
| `seq` | integer | yes | Strictly monotonic from 1. Increments on every R or W. See §Sequence semantics |
| `bar` | integer | yes | Which BAR — 0, 2, or 4. Hailo-8 exposes BAR0 (NNC config), BAR2 (mailboxes), BAR4 (configuration window) |
| `offset` | integer | yes | Byte offset within the BAR. JSON has no native hex — store as decimal; tools render as `0x%x` |
| `size` | integer | yes | Access width in bytes. Hailo's existing SLM-OS code only emits 32-bit (size=4); reserve 1/2/8 for future widths |
| `dir` | string | yes | `"read"` or `"write"` |
| `value` | string | yes | Hex-encoded value, no `0x` prefix, lowercase, exactly `size*2` characters. Little-endian byte order for multi-byte values (matches Hailo wire format). Example: a 32-bit read of `0x40130016` is `"16001340"` |
| `source` | string | yes | `"qemu_capture"` for writes/operations observed in the QEMU stub; `"slmos_observed"` for read responses captured on real hardware. See §Provenance |
| `validated_at_commit` | string\|null | yes | Git SHA where this entry was confirmed correct on real hardware (full SHA, not abbreviated). `null` until validated. See §Validation watermark |
| `validated_at` | string\|null | yes | ISO 8601 UTC timestamp paired with `validated_at_commit`. `null` if not validated |
| `note` | string | no | Free-form annotation. Use for "this is a polling-loop status read", "this is the IDENTIFY response", etc. — anything that helps the next reader |

### Trailer (optional)

A capture session may end with a trailer line for bookkeeping. Tools must tolerate its absence.

```json
{"type":"trailer","ended_at":"2026-05-12T19:45:00Z","last_seq":1247,"reason":"hailort_configure_complete"}
```

### Region rule (Phase 4 compression)

A region rule short-circuits per-`seq` capture for a contiguous BAR range whose bytes come from a known external artifact (typically a firmware blob shipped with HailoRT). Region rules let the corpus represent ~160 KB of firmware upload as a single ~200-byte JSONL line instead of ~40 000 per-seq op entries.

```json
{"type":"region","bar":4,"start":0,"end":164536,
 "source_kind":"file","source_path":"/lib/firmware/hailo/hailo8_fw.bin","source_offset":24,
 "validated_at_commit":"<sha>","validated_at":"<iso>","note":"..."}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `type` | string | yes | Always `"region"` |
| `bar` | integer | yes | Which BAR — 0, 2, or 4 |
| `start` | integer | yes | First covered BAR offset, inclusive (decimal) |
| `end` | integer | yes | Last covered BAR offset + 1, **exclusive** (decimal) — half-open `[start, end)` |
| `source_kind` | string | yes | Currently `"file"`. Reserve other values (`"inline"`, `"const"`) for future additive extensions |
| `source_path` | string | yes (for `source_kind="file"`) | Absolute path to the artifact file. Resolved at QEMU stub startup |
| `source_offset` | integer | yes (for `source_kind="file"`) | Byte offset within the file corresponding to BAR `start` |
| `validated_at_commit` | string\|null | yes | Audit field, same semantics as op entries — full 40-char SHA when validated, `null` until a verification pass confirms HailoRT writes match the region |
| `validated_at` | string\|null | yes | ISO 8601 timestamp, same semantics |
| `applies_from_seq` | integer | no | Inclusive lower seq bound — region rule fires only for accesses with `seq >= applies_from_seq`. Defaults to `1`. Must be `>= 1`. See "Seq-bounded regions" below |
| `applies_to_seq` | integer | no | Inclusive upper seq bound — region rule fires only for accesses with `seq <= applies_to_seq`. **Omit the field to mean "unbounded"** — the loader's hand-rolled JSON parser stores integers as `int64_t`, so explicitly encoding `UINT64_MAX` (`18446744073709551615`) overflows to a negative value and is rejected. Defaults to unbounded. Must be `>= applies_from_seq` |
| `note` | string | no | Free-form annotation (e.g. "Hailo-8 firmware code section, identified via signature match on first 32 bytes") |

(Fields use flat names — `source_kind` etc. — rather than a nested `source` object so the existing hand-rolled JSON parser in the QEMU stub doesn't need to grow nested-object support. Functionally equivalent to a nested representation.)

**Precedence:** A region rule covering a `(bar, offset, size)` access **takes precedence over any per-`seq` op entry** at that location. The QEMU stub consults regions FIRST; only if no region covers the access does it fall back to the per-seq lookup. This matters because the legacy per-seq entries captured during single-step grinding for the same range are equivalent (they were derived from the same artifact) — the region rule is the authoritative summary.

**Read semantics under a region rule:** the stub serves the read from `source_path` at byte `source_offset + (access_offset - start)`. The corresponding seq counter still increments — region serving is transparent to seq sequencing.

**Write semantics under a region rule:** the stub computes the expected bytes from the file and compares against the incoming write data. On match: silent success, no corpus append. On mismatch: emit `HAILO_RE_CORPUS_DIVERGENCE` with `reason=region_write_mismatch`, halt. This catches the case where HailoRT writes something not present in the artifact, indicating either an incorrect region rule or HailoRT applying a runtime transformation before upload.

**Multiple regions:** A corpus may contain multiple region rules for different BAR ranges. Overlapping regions within the same BAR are a configuration error and tools SHOULD reject the corpus on load — UNLESS their seq windows are disjoint (see below).

### Seq-bounded regions

`applies_from_seq` and `applies_to_seq` narrow a region rule to a specific seq window. The intended use case is **multi-pass firmware uploads**, where HailoRT writes one firmware section into a BAR window, then later writes a *different* section into the *same* BAR window. A single region rule can only describe one of the two passes; seq-bounded regions let both coexist.

Two regions on the same BAR with overlapping offset ranges are legitimate IF their seq windows are disjoint (`A.applies_to_seq < B.applies_from_seq` OR vice versa). The loader rejects regions that overlap in BOTH offset and seq.

Lookup: for each access, the stub finds the unique region whose `(bar, offset+size)` covers the access AND whose `[applies_from_seq, applies_to_seq]` includes the access's `seq`. If no such region exists, the stub falls through to per-seq lookup as usual.

Example: a corpus with two passes over BAR4 [0, 0x272B8):

```json
{"type":"region","bar":4,"start":72,"end":164536,"source_kind":"file","source_path":"/lib/firmware/hailo/hailo8_fw.4.23.0.bin","source_offset":96,"applies_to_seq":2107,"validated_at_commit":null,"validated_at":null,"note":"pass 1 — section A"}
{"type":"region","bar":4,"start":0,"end":160440,"source_kind":"file","source_path":"/lib/firmware/hailo/hailo8_fw.4.23.0.bin","source_offset":4120,"applies_from_seq":2118,"validated_at_commit":null,"validated_at":null,"note":"pass 2 — section B"}
```

**Backward compatibility:** Region entries are an additive extension. Tools that don't recognize `type="region"` MUST silently skip those lines. `format_version` remains at `1`. Tools that DO consume region rules MUST also implement the precedence rule above. Tools that consume region rules but predate `applies_from_seq` / `applies_to_seq` see them as unknown keys (tolerated) and treat the region as universally applicable — which is wrong for a corpus that depends on the seq filter to avoid overlap. Producers of seq-bounded regions therefore should not co-publish such corpora to consumers known to be on the older schema; in practice this isn't a concern since the only consumer is the in-tree QEMU stub built from the same revision.

## Sequence semantics

`seq` is strictly monotonic across the entire session, starting at 1. Every BAR R or W from HailoRT increments it by 1. This is what defeats the cross-step contamination problem identified in #795: the same offset read at two different points in the session can return two different values (typical for polling loops, status bits, completion flags), and keying by `(seq, offset)` makes each occurrence independent.

**Worked example — polling loop.** HailoRT writes a control opcode, then polls a status register until a bit goes high:

```
{"type":"op","seq":42,"bar":4,"offset":2304,"size":4,"dir":"write","value":"01000000",...}  # kick off
{"type":"op","seq":43,"bar":4,"offset":2308,"size":4,"dir":"read","value":"00000000",...}   # poll: not ready
{"type":"op","seq":44,"bar":4,"offset":2308,"size":4,"dir":"read","value":"00000000",...}   # poll: not ready
{"type":"op","seq":45,"bar":4,"offset":2308,"size":4,"dir":"read","value":"01000000",...}   # poll: ready
```

The QEMU stub returns `00000000` for the reads at `seq=43,44` and `01000000` at `seq=45` even though all three read the same offset. Without per-seq keying the stub would return one constant and HailoRT would either spin forever or proceed too early.

## Lookup rules

### QEMU stub — read fulfillment

On a BAR read at the stub's current sequence counter `N` against offset `X`:

1. If a corpus entry exists at `seq=N` whose `dir`, `bar`, `offset`, or `size` doesn't match the current operation, emit a §Divergence report with `reason=op_shape_mismatch` and halt. (The unknown-read path below only fires when there is no entry at `seq=N` at all.)
2. Search the corpus for an entry where `type="op"`, `seq=N`, `bar=<the bar>`, `offset=X`, `dir="read"`, `size=<the access width>`.
3. If found: return `value` to HailoRT, increment the stub's seq counter.
4. If not found: freeze the VM, emit a corpus-extension request to stdout in the format defined by §Corpus-extension request, and exit non-zero so the driver script knows to advance the loop.

The stub MUST NOT silently fall back to returning 0 for unknown reads. Doing so re-introduces the contamination failure mode this design exists to prevent.

### QEMU stub — write logging

On a BAR write at the stub's current sequence counter `N` to offset `X` with value `V`:

1. If a corpus entry exists at `(seq=N, bar, offset=X, dir="write")`, assert `value == V`. Mismatch means HailoRT's write order is non-deterministic between runs — this is a fatal capture-invariant violation; emit a §Divergence report and halt.
2. If no entry exists, append a fresh entry with `source="qemu_capture"`, `validated_at_commit=null`, `validated_at=null`. Writes are recorded but not yet validated (their validation happens implicitly when SLM-OS successfully replays them).
3. Increment the stub's seq counter.

### SLM-OS `hailo replay-step <N>` — operation replay + read capture

1. Read the corpus and find the entry at `seq=N` — must be `dir="read"` and `validated_at_commit=null` (otherwise there is nothing to do, the entry is already validated).
2. For each entry with `seq < N`, in seq order:
   - If `dir="write"`, issue the BAR write against real hardware with the recorded value.
   - If `dir="read"`, issue the BAR read against real hardware and discard the returned value. Reads are replayed for their device-side side effects (W1C status registers, FIFO pops, IRQ acknowledgment) so the device state machine matches the state HailoRT-in-QEMU drove the corpus from. Optionally, the replay-step command may compare the observed value against the corpus's recorded value and report a divergence per §Divergence report — this turns every replay-step into a free inline mini-validation.
   Do not skip; do not reorder.
3. Issue the read at `seq=N` against the offset specified in the corpus entry. Capture the response value `V`.
4. Print a single line in the **Corpus-extension response** format (§Corpus-extension response) so the driver script can append it.

The replay-step command MUST NOT mutate the corpus directly. The driver script is the single writer.

## Provenance

The `source` field distinguishes how an entry got into the corpus. Two valid values for now:

- `"qemu_capture"` — captured from HailoRT's writes to the QEMU stub. Always used for write entries. Never used for read entries (those come from real hardware).
- `"slmos_observed"` — read response captured by SLM-OS replay-step against real hardware. Used for every read entry.

Reserve other values for future sources (e.g., `"manual_annotation"` for hand-derived entries during pattern compression in Phase 4). Tools must tolerate unknown values gracefully.

## Validation watermark

The corpus has a known-good frontier at any point in time: the highest `seq` value for which every entry up to and including that seq has a non-null `validated_at_commit`. The driver script (Task 0.5) maintains this frontier as `last_validated_seq` in memory; the periodic re-validation pass (Phase 3) is what advances it.

Single-step Phase 2 captures all run *ahead* of the validated frontier — entries are appended with `validated_at_commit=null`. The Phase 3 re-validation pass replays the entire operation trace (every write and every read, in seq order) against real hardware, confirms each observed read value matches the corpus, and stamps `validated_at_commit` on every entry up to the divergence point or end-of-trace. After a successful re-validation pass at seq `K`, `last_validated_seq` advances to `K`.

## Corpus-extension request (QEMU stub → driver script)

When QEMU hits an unknown read, it emits exactly one line to stdout in this format before exiting non-zero:

```
HAILO_RE_CORPUS_EXTEND seq=<N> bar=<B> offset=<X> size=<S> reason=unknown_read
```

Example:

```
HAILO_RE_CORPUS_EXTEND seq=128 bar=4 offset=3204 size=4 reason=unknown_read
```

The driver script parses this line, invokes `hailo replay-step <N>` against real hardware, parses the response, and appends the resulting entry.

## Corpus-extension response (SLM-OS replay-step → driver script)

`hailo replay-step <N>` prints a single line in this format to the serial console once it has issued the writes and the read:

```
HAILO_RE_CORPUS_RESPONSE seq=<N> bar=<B> offset=<X> size=<S> value=<hex> slmos_sha=<full-sha>
```

`slmos_sha` MUST be the full 40-character SHA the SLM-OS build was compiled from — not abbreviated. The driver script copies this value verbatim into the corpus entry's `validated_at_commit` field, so abbreviating it here forces a `git rev-parse` round-trip the driver shouldn't need.

Example:

```
HAILO_RE_CORPUS_RESPONSE seq=128 bar=4 offset=3204 size=4 value=42000000 slmos_sha=ad007df819581b493bcb1fae00f131fef176713b
```

The driver script captures the line via `serial_capture`, validates the `slmos_sha` matches the build under test, and appends a new corpus entry with `source="slmos_observed"`, `validated_at_commit="<full-sha>"`, `validated_at="<now>"`.

## Divergence report (QEMU stub or SLM-OS replay-step → driver script)

Emitted on any of three conditions:

- QEMU sees HailoRT issue a write whose `value` differs from the corpus's recorded write at the same `(seq, bar, offset)` — write-order non-determinism (see §Lookup rules / QEMU stub — write logging).
- QEMU sees HailoRT issue an operation whose `dir` / `bar` / `offset` / `size` doesn't match the corpus entry at that seq — capture invariant violation.
- SLM-OS replay-step issues a `seq < N` read for side-effect replay and the observed value differs from the corpus's recorded value, AND the implementation has the optional inline-validation enabled (see §Lookup rules / SLM-OS step 2).

Format:

```
HAILO_RE_CORPUS_DIVERGENCE seq=<N> bar=<B> offset=<X> size=<S> dir=<read|write> expected=<hex> observed=<hex> source=<qemu|slmos> reason=<short-tag>
```

`reason` tags: `write_value_mismatch`, `op_shape_mismatch`, `inline_read_mismatch`. Tools must accept any string; new tags can be added without bumping `format_version`.

Example:

```
HAILO_RE_CORPUS_DIVERGENCE seq=87 bar=4 offset=3072 size=4 dir=write expected=01000000 observed=03000000 source=qemu reason=write_value_mismatch
```

The driver script treats a divergence as a hard stop on Phase 2 single-stepping. It triggers the §Re-validation rollback procedure starting from the divergence point.

## Re-validation rollback

When the Phase 3 re-validation pass detects a divergence at seq `J` (SLM-OS observes a different read value than the corpus records), the corpus has been poisoned starting at or before `J`. Rollback procedure:

1. Identify the last seq with `validated_at_commit != null` AND `validated_at_commit` matches a SHA in the SLM-OS git history reachable from `main`. Call this `K`.
2. Truncate the corpus file to retain only the header and entries with `seq <= K`. Append a comment-style trailer recording the rollback:
   ```json
   {"type":"trailer","ended_at":"<now>","rollback_from_seq":<J>,"rollback_to_seq":<K>,"reason":"divergence_at_J"}
   ```
3. Open a fresh corpus file (incremented date suffix) seeded with the truncated content. Continue capture from `K+1`.

Rollback is the ONE permitted destructive operation. It happens via the driver script; no other tool may rewrite corpus contents. The truncated file is preserved (renamed with a `.poisoned` suffix) for forensic review.

## What this format does NOT cover

- **DMA buffer contents.** HailoRT uploads firmware patches, network-group parameters, and descriptor lists via DMA — those bytes are not visible in BAR R/W traffic and not stored here. Capture them separately as opaque blobs alongside the corpus, keyed by the seq of the descriptor-program write that hands the DMA address to fw. Out of scope for Phase 0; revisit in Phase 4 if DMA blobs turn out to be the bottleneck.
- **IRQ/MSI events.** When the QEMU stub fakes an MSI, log the event in a side channel — IRQ timing is a separate causal track from MMIO ops and conflating them in one seq stream creates ordering ambiguity. Out of scope for v1 of this format.
- **Cross-session correlation.** Each capture session is self-contained. Pattern-recognition tools (Phase 4) that span multiple sessions are responsible for their own indexing.

## References

- Issue [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795) — full RE plan, phase breakdown, parallelization layout.
- `docs/hailo-protocol-architecture.md` — protocol architecture, reopen criteria, empirical baseline for the Linux MNIST run (84,003 inferences, all configuration via mmap'd BAR4 writes).
- `kernel/ai_accel/hailo/hailo_trace.h` — existing trace toolkit; `hailo replay-step` will hook into the same plumbing.
