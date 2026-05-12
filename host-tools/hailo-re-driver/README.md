# hailo-re-driver

Driver script for the Hailo BAR4 reverse-engineering capture-and-replay loop.

This is the Phase 0 Task 0.5 deliverable for issue [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795) — the conductor that stitches together:

- the QEMU stub device (Task 0.2),
- the HailoRT-in-VM environment (Task 0.3),
- the SLM-OS `hailo replay-step` shell command (Task 0.4),

into one self-paced bootstrap loop. See `docs/hailo-re-corpus-format.md` for the on-disk corpus contract this tool reads and writes.

## Install

```bash
cd host-tools/hailo-re-driver
pip install -e .            # exposes hailo-re-{bootstrap,validate,diff} on PATH
# or run directly from a checkout without installing:
./bin/hailo-re-bootstrap --help
```

Python 3.10+. Standard library only; pytest is optional (`pip install -e .[dev]`). Tests use `unittest.TestCase` and run under both `pytest tests` and `python -m unittest discover -s tests`.

## Commands

### `hailo-re-bootstrap` — Phase 1/2 single-step loop

Runs QEMU + Task 0.2's stub against the corpus, captures the `HAILO_RE_CORPUS_EXTEND` line on an unknown read, invokes `hailo replay-step` against `pi-5-1` via labctl, parses the `HAILO_RE_CORPUS_RESPONSE` line, appends the new entry, and loops until QEMU completes HailoRT `Configure() + Activate()` with no unknown reads. On `HAILO_RE_CORPUS_DIVERGENCE` it triggers §Re-validation rollback and exits.

```bash
hailo-re-bootstrap path/to/corpus.jsonl
hailo-re-bootstrap --max-iterations 50 path/to/corpus.jsonl
```

QEMU is launched via the script named in `$HAILO_RE_QEMU_LAUNCHER` (the Task 0.3 deliverable). SLM-OS uses the SBC named in `$HAILO_RE_SBC` (default: `pi-5-1`) and the kernel at `$HAILO_RE_KERNEL` (default: `build/kernel/slmos.bin`).

### `hailo-re-validate` — Phase 3 re-validation

Takes an SLM-OS observed-trace JSONL (one op line per real-hardware op) and diffs it against the corpus. On clean match, stamps `validated_at_commit` and `validated_at` onto every op entry. On divergence, triggers rollback to the last validated frontier reachable from `main`.

```bash
hailo-re-validate --observed-trace observed.jsonl \
                  --slmos-sha <full-40-char-sha> \
                  path/to/corpus.jsonl
```

### `hailo-re-diff` — standalone corpus diff

Compares two corpora, or a corpus against an SLM-OS observed-trace. Reports the first divergent seq. Exit code 0 on clean match, 1 on divergence.

```bash
hailo-re-diff left.jsonl right.jsonl
hailo-re-diff --observed corpus.jsonl observed.jsonl
hailo-re-diff --show-matches a.jsonl b.jsonl    # include matching rows
```

## Corpus directory layout

Per the spec, capture sessions live at:

```
~/slmos-ref/derivatives/hailo-re-corpora/
    <hailort-version>-<fw-version>-<capture-host>-<YYYY-MM-DD>.jsonl
    <…same filename>.poisoned          # forensic artifact after a rollback
```

The directory is local-only; do not check corpora into the public repo. The reference cache at `~/slmos-ref/` is described in `CLAUDE.md` and the `reference_cache_location` memory.

## labctl prerequisites

This driver is the only component that talks to lab hardware — and it does so exclusively through `labctl`. Verify before running on real hardware:

```bash
labctl claim pi-5-1                       # exclusive access
labctl sdwire info pi-5-1                 # confirm SLMOS-only single-partition card is installed
labctl power status pi-5-1                # confirm power control is reachable
```

The driver invokes:

- `labctl sdwire info <sbc>` — pre-flash identity check (CLAUDE.md: "verify SD card identity before flashing").
- `labctl sdwire update <sbc> -p 1 -c <kernel>:kernel_2712.img --reboot` — flash + power-cycle.
- `labctl serial capture <sbc> --until '<shell-prompt>'` — wait for SLM-OS shell.
- `labctl serial send <sbc> 'hailo replay-step <corpus> <N>' --capture …` — issue replay command and capture the `HAILO_RE_CORPUS_RESPONSE` line.

No code path in this package opens `/dev/sd*`, `mount`s, or talks to `ser2net` directly. Per CLAUDE.md, that is non-negotiable — workarounds break device sharing with other lab projects.

## Dry-run and mock modes

For CI and local development without hardware:

| Flag | Behaviour |
|---|---|
| `--dry-run` | Replaces the labctl transport with a logger; commands are echoed to stderr and return success. Doesn't synthesise SLM-OS responses — used for surface-checking the CLI plumbing. |
| `--mock-qemu-lines FILE` | Skip real QEMU; read its stdout from FILE (one line per protocol message). `--mock-qemu-rc N` controls the simulated exit code. |
| `--mock-slmos-responses FILE` | Skip real SLM-OS; read canned `HAILO_RE_CORPUS_RESPONSE` lines from FILE, served in order. |

For an end-to-end loop simulation with no hardware, pass both `--mock-qemu-lines` and `--mock-slmos-responses`:

```bash
# Synthesise one EXTEND -> RESPONSE cycle entirely on the dev machine:
printf 'HAILO_RE_CORPUS_EXTEND seq=2 bar=4 offset=2308 size=4 reason=unknown_read\n' > /tmp/qemu.txt
printf 'HAILO_RE_CORPUS_RESPONSE seq=2 bar=4 offset=2308 size=4 value=00000000 slmos_sha=%s\n' \
    "$(git rev-parse HEAD)" > /tmp/slmos.txt
hailo-re-bootstrap \
    --mock-qemu-lines /tmp/qemu.txt --mock-qemu-rc 1 \
    --mock-slmos-responses /tmp/slmos.txt \
    --max-iterations 1 \
    ./corpus.jsonl
```

The unit test `tests/test_loop_mocked.py` exercises this path under both subprocess and pure-Python mocks.

## Tests

```bash
python -m unittest discover -s tests       # stdlib runner
pytest tests                                # if installed
```

36 tests cover line protocols, corpus IO, rollback, diff, and the mocked loop. Coverage is concentrated on `line_protocols.py` and `corpus.py` — the surfaces the spec is most strict about.

## Workflow notes

- Branch: `hailo-re/0.5-driver-script`. PR target: `main`.
- The driver script is the ONLY writer to a corpus file. `hailo replay-step` (Task 0.4) emits protocol lines only — it never touches the JSONL.
- The loop exits on the Phase 1 gate: a QEMU run completes with rc=0 and no protocol line on stdout. That's `ConfigureCompleteEvent` internally and `status=complete` externally.
- On divergence, the loop calls `rollback.handle` which renames the poisoned corpus to `*.poisoned` and writes a fresh one seeded with the validated frontier. The operator decides whether to resume; this driver does not auto-restart.

## References

- Issue [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795) — full RE plan.
- `docs/hailo-re-corpus-format.md` — corpus schema.
- `docs/hailo-protocol-architecture.md` — protocol context.
- `CLAUDE.md` — labctl-is-only-hardware-interface, verify-card-before-flash, no-emoji.
