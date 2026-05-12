# qemu-hailo8-stub

QEMU PCIe stub device impersonating a Hailo-8 NPU. Captures every BAR
R/W operation HailoRT performs against the device from inside the VM,
answering reads from a JSONL corpus and halting on unknown reads so a
driver script can extend the corpus from real hardware.

This is Phase 0 Task 0.2 of the Hailo BAR4 protocol reverse-engineering
plan ([issue #795](https://github.com/SLM-OS/SLM-Operating-System/issues/795)).
Corpus format spec: [`docs/hailo-re-corpus-format.md`](../../docs/hailo-re-corpus-format.md).

## Layout

```
host-tools/qemu-hailo8-stub/
├── README.md           — this file
├── install.sh          — symlink sources into a QEMU source tree
├── src/
│   ├── hailo8.c        — QEMU device model (PCI, BAR0/2/4, MSI)
│   ├── hailo8_corpus.c — JSONL corpus loader / lookup / append
│   └── hailo8_corpus.h — public API for the corpus module
├── tests/
│   ├── Makefile        — standalone unit-test build
│   └── test_corpus.c   — unit tests for the corpus module
└── sample/
    └── empty.jsonl     — minimal corpus (header only) for smoke tests
```

The corpus module is QEMU-independent: pure C11 with a POSIX feature
test macro. The unit tests link against it directly so they run without
a QEMU build.

## Build

### 1. Build deps

```sh
sudo apt install libglib2.0-dev libpixman-1-dev libslirp-dev \
                 flex bison meson python3-venv ninja-build
```

### 2. Get a QEMU source tree

Tested against QEMU 8.2.x (matches Ubuntu 24.04 and Pi OS bookworm
packages). Clone shallowly:

```sh
cd ~/projects
git clone --depth 1 --branch stable-8.2 \
    https://gitlab.com/qemu-project/qemu.git
```

### 3. Wire the device in

`install.sh` symlinks the device sources into `hw/misc/` and appends
the Kconfig + meson.build entries the QEMU build system needs. Safe to
re-run after a `git pull` in the QEMU tree.

```sh
./install.sh ~/projects/qemu
```

### 4. Build QEMU

```sh
cd ~/projects/qemu
./configure --target-list=x86_64-softmmu --enable-debug --disable-werror
make -j$(nproc) qemu-system-x86_64
```

The result is `build/qemu-system-x86_64`.

### 5. Run the unit tests

These exercise the corpus parser, the seq-keyed lookup, the write
append, and the hex round-trip. They don't need QEMU built.

```sh
make -C tests test
```

## Launching

```sh
~/projects/qemu/build/qemu-system-x86_64 \
    -nodefaults -display none -no-reboot \
    -accel tcg \
    -machine q35 -m 256 \
    -device hailo8,corpus=/path/to/corpus.jsonl
```

### Properties

| Property      | Type   | Default      | Meaning                                       |
|---------------|--------|--------------|-----------------------------------------------|
| `corpus`      | string | (required)   | Path to the JSONL corpus file                 |
| `bar0_size`   | uint64 | `0x4000`     | BAR0 (NNC config) size in bytes               |
| `bar2_size`   | uint64 | `0x40000`    | BAR2 (vDMA registers) size in bytes           |
| `bar4_size`   | uint64 | `0x1000000`  | BAR4 (fw access window) size in bytes         |

The defaults match the BAR sizes the Linux `hailo_pci` driver maps for
Hailo-8 on the AI HAT+ (BAR4 = 16 MiB from `pcie_common.c`'s
`max_size`).

## Stdout / stderr protocols

The stub emits three kinds of machine-parseable lines that the Task 0.5
driver script consumes:

### `HAILO_RE_CORPUS_EXTEND` (stdout, then exit 1)

Emitted when a HailoRT read hits a `seq` not in the corpus. Per
[`docs/hailo-re-corpus-format.md` §Corpus-extension request][spec]:

```
HAILO_RE_CORPUS_EXTEND seq=<N> bar=<B> offset=<X> size=<S> reason=unknown_read
```

The driver script invokes `hailo replay-step <N>` against real
hardware, parses the resulting `HAILO_RE_CORPUS_RESPONSE`, appends the
read entry to the corpus, and restarts QEMU.

### `HAILO_RE_CORPUS_DIVERGENCE` (stdout, then exit 1)

Emitted in two cases per [§Divergence report][spec]:

| Reason                | Trigger                                                          |
|-----------------------|------------------------------------------------------------------|
| `op_shape_mismatch`   | Corpus has an entry at this `seq` but `dir`/`bar`/`offset`/`size` differs from the current op |
| `write_value_mismatch`| Corpus has a recorded write at this `(seq, bar, offset)` whose `value` differs from what HailoRT wrote |

Base format (always present):

```
HAILO_RE_CORPUS_DIVERGENCE seq=<N> bar=<B> offset=<X> size=<S> dir=<read|write> \
    expected=<hex> observed=<hex> source=qemu reason=<tag>
```

For `reason=write_value_mismatch`, `expected` and `observed` carry the
real LE-encoded hex values of the corpus's recorded write and HailoRT's
attempted write.

For `reason=op_shape_mismatch`, the value fields aren't the divergence
axis — the front-matter `bar=B offset=X size=S dir=W` already shows the
observed op shape, so `expected`/`observed` are zero-filled at the
access width and four extra tokens follow `reason=` carrying the
corpus-side shape:

```
HAILO_RE_CORPUS_DIVERGENCE seq=<N> bar=<B> offset=<X> size=<S> dir=<W> \
    expected=00000000 observed=00000000 source=qemu reason=op_shape_mismatch \
    expected_bar=<E_B> expected_offset=<E_X> expected_size=<E_S> expected_dir=<E_W>
```

The extra tokens are additive and tolerated-but-ignored by readers that
don't understand them, so they don't require a corpus `format_version`
bump.

A divergence is fatal for Phase 2 single-stepping — the driver script
treats it as a poisoned-corpus signal and starts the rollback procedure
per [§Re-validation rollback][spec].

### `HAILO_RE_MSI_FIRE` (stderr, capture continues)

Worktree extension to the corpus protocol. MSI fires are out of scope
for v1 of the corpus format (per [§What this format does NOT cover][spec]),
but the stub still needs a way to deliver IRQs to HailoRT. The
mechanism: corpus read entries may carry an optional `"msi_after": V`
field; immediately after the stub returns the recorded value, it fires
MSI vector `V` and emits:

```
HAILO_RE_MSI_FIRE after_seq=<N> vector=<V>
```

to stderr (not stdout — IRQ events are a separate causal track from
MMIO ops and conflating them in the corpus-extension stream would
create ordering ambiguity).

This field is additive and tolerated-but-ignored by readers that
don't understand it, so it doesn't require bumping the corpus
`format_version`.

[spec]: ../../docs/hailo-re-corpus-format.md

## Write capture

Unknown writes are appended to the corpus file in place by the stub
(one JSONL line per write, `source="qemu_capture"`,
`validated_at_commit=null`). Writes are flushed to disk on every
append, so the corpus is consistent even if QEMU later exits via an
unknown-read halt. A duplicate write at the same `seq` whose value
matches is silently accepted; a value mismatch triggers
`write_value_mismatch` divergence as above.

## Smoke tests

`tests/smoke.py` exercises the Definition-of-Done scenarios against a
built QEMU using the qtest accelerator:

1. **Empty corpus**: first BAR read produces `HAILO_RE_CORPUS_EXTEND`
   and QEMU exits 1.
2. **Hand-crafted lookup**: a corpus with one read entry returns the
   recorded value to a qtest `readl`.
3. **Shape divergence**: a corpus expecting a read at `seq=1` but the
   guest issues a write produces `HAILO_RE_CORPUS_DIVERGENCE` with
   `reason=op_shape_mismatch` plus the `expected_*` extension tokens.
4. **Write divergence**: a corpus with one write entry expecting `V1`
   produces `HAILO_RE_CORPUS_DIVERGENCE` when qtest issues `writel V2`.

Run with:

```sh
QEMU=~/projects/qemu/build/qemu-system-x86_64 python3 tests/smoke.py
```

## Limitations

- DMA emulation is not modeled. HailoRT uploads firmware patches,
  descriptor lists, and network-group parameters via DMA; those bytes
  don't appear in BAR R/W traffic. Capture them separately as opaque
  blobs keyed by the seq of the descriptor-program write that hands
  the DMA address to fw. Out of scope for v1; revisit if Phase 4 finds
  DMA blobs are the bottleneck.
- The stub doesn't model fw timing. Real fw has settle windows (the
  500 ms post-`BOOT_IRQ` one is the known example); the stub answers
  reads instantly. The corpus driver compensates by interposing
  delays on the SLM-OS replay side.
- The stub is not migratable (`vmsd.unmigratable = 1`). The corpus +
  seq counter pin it to its launch.

## References

- Issue [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795) — full RE plan, phase breakdown.
- [`docs/hailo-re-corpus-format.md`](../../docs/hailo-re-corpus-format.md) — corpus format spec (the load-bearing contract).
- [`docs/hailo-protocol-architecture.md`](../../docs/hailo-protocol-architecture.md) — protocol architecture, reopen criteria.
