# `hailo replay-step` — Phase 0 Task 0.4 reference

**Status:** Shipped 2026-05-12 (#795 Phase 0). Pinned by `docs/hailo-re-corpus-format.md` §"Lookup rules / SLM-OS"; that spec is the contract this command implements.

The command replays a prefix of an RE corpus against real Hailo NPU hardware and observes a single read, emitting one machine-parseable response line per invocation. It is the SLM-OS leg of the record-and-replay loop SLM-OS runs to reverse-engineer the Hailo BAR4 protocol.

## Usage

```text
hailo replay-step <corpus-path> <seq-N>
```

| Argument | Shape | Notes |
|---|---|---|
| `<corpus-path>` | `0:/<file>` (FatFs) or `/mnt/files/<file>` (VFS) | The driver script in Task 0.5 deposits corpora on the boot SD card via `labctl sdwire_update`; the `0:/...` path reads them directly from FAT without going through LittleFS. The VFS variant works for corpora that have been pushed via the normal `/mnt/files` route. |
| `<seq-N>` | Decimal `uint32`, ≥1 | The sequence number to read at. Must resolve to an entry with `dir="read"` and `validated_at_commit=null`. |

The command returns 0 in every case the shell can recover from (parse failure, unknown seq, etc. all return 0 and print a diagnostic); the wire-protocol indicator is the presence of the response line.

## Procedure

1. Read the corpus from `<corpus-path>` and parse the JSONL into an in-memory op array. Reject any line that breaks strict-monotonic seq ordering.
2. Find the entry at `seq=N`. Refuse if the entry is missing, is a write, has `size != 4`, or already has a non-null `validated_at_commit`.
3. Walk every entry with `seq < N` in seq order:
   - For `dir="write"`: issue `hailo_platform->write32(bar, offset, value)`.
   - For `dir="read"`: issue `hailo_platform->read32(bar, offset)`, discard the return. Reads are replayed for their device-side side effects (W1C status latches, FIFO pops, IRQ acks). If the observed value differs from the corpus's recorded value, emit one `HAILO_RE_CORPUS_DIVERGENCE` line and continue.
4. Issue the read at `seq=N`, capture the response.
5. Print one `HAILO_RE_CORPUS_RESPONSE` line in the spec format.

## Output line format

```
HAILO_RE_CORPUS_RESPONSE seq=<N> bar=<B> offset=<X> size=<S> value=<hex> slmos_sha=<full-40-char-sha>
```

The `value` field is the little-endian byte stream rendered as `2*size` lowercase hex characters, matching the encoding `value` uses in op entries. The `slmos_sha` is the full 40-character git SHA the SLM-OS build was compiled from (`SLMOS_GIT_SHA` build define plumbed through CMake). The driver script copies the SHA verbatim into the corpus entry's `validated_at_commit` field — abbreviating it here would force a `git rev-parse` round-trip the driver should not need.

### Divergence line (optional, fires per mismatched seq<N read)

```
HAILO_RE_CORPUS_DIVERGENCE seq=<N> bar=<B> offset=<X> size=<S> dir=read \
    expected=<hex> observed=<hex> source=slmos reason=inline_read_mismatch
```

## Example — hardware smoke test (pi-5-1, 2026-05-12)

`build/tiny-corpus.jsonl`:

```json
{"type":"header","format_version":1,"hailort_version":"hand-crafted",...}
{"type":"op","seq":1,"bar":0,"offset":392,"size":4,"dir":"write","value":"00000000",
 "validated_at_commit":null,"note":"BAR0+0x188 IMASK_HOST <- 0"}
{"type":"op","seq":2,"bar":0,"offset":396,"size":4,"dir":"read","value":"00000000",
 "validated_at_commit":null,"note":"BAR0+0x18C ISTATUS_HOST"}
```

```text
slmos> hailo probe
hailo: probe OK, vendor=0x1e60 device=0x2864, state=probed

slmos> hailo replay-step 0:/tiny.jsonl 2
hailo: replay-step: corpus ops=2 target seq=2 bar=0 offset=0x18c
HAILO_RE_CORPUS_RESPONSE seq=2 bar=0 offset=396 size=4 value=00008000 \
    slmos_sha=4b571f2941b3b116d28cfa2af1059a161da33d88
```

The `value=00008000` decodes (LE) to `0x00008000` — VDMA channel 7 destination IRQ bit set in `BCS_ISTATUS_HOST`, plausible for a Hailo-8 sitting idle on the bus.

## What this command does NOT do

- **Modify the corpus.** The Task 0.5 driver script is the single corpus writer.
- **Replay an entire corpus end-to-end.** That is the Phase 3 re-validation pass (`hailo replay-validate <corpus>`, separate command, separate work).
- **Handle widths other than 32-bit.** The corpus spec reserves `size ∈ {1, 2, 4, 8}` for future widths; only `size=4` is plumbed through the platform shim today. A non-4 entry at any seq up to and including N aborts with a clear error.

## Cross-references

- Issue [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795) — full RE plan.
- `docs/hailo-re-corpus-format.md` — the wire contract this command implements.
- `kernel/ai_accel/hailo/hailo_re_corpus.{h,c}` — corpus reader.
- `kernel/ai_accel/hailo/hailo_shell.c` — `cmd_hailo_replay_step` dispatch.
- `kernel/tests/test_hailo_replay.c` — parser + replay-loop coverage.
