# gguf-inspect — host-side inspector for GGUF v3 model files

A small Rust CLI that reads a [GGUF v3] model file and prints its
header, metadata KV pairs, and tensor descriptors. Intended as a
developer convenience while bringing up SLM-OS's small-language-model
support (M0 of the [SLM integration plan]).

[GGUF v3]: https://github.com/ggerganov/ggml/blob/master/docs/gguf.md
[SLM integration plan]: ../../docs/plans/slm-integration-plan.md

## Why a separate host tool?

The runtime's GGUF parser (`runtime/src/slm/gguf.rs`, M1) targets
`no_std` + zero-copy mmap and runs on the SBC. This host tool is a
plain `std` Rust binary with zero third-party dependencies, optimised
for host-side debugging — building or downloading a GGUF, eyeballing
its metadata, and verifying that tensor offsets land where expected
before flashing the file to a target.

## Building

```bash
cd host-tools/gguf-inspect
cargo build --release
```

`gguf-inspect` is a standalone crate. It does not appear in any
workspace `Cargo.toml` and does not depend on the SLM-OS runtime
crate.

## Usage

```bash
gguf-inspect <path> [--list-tensors] [--list-metadata]
```

Default output prints the header summary, all metadata, and all
tensor descriptors. Pass `--list-tensors` to suppress metadata or
`--list-metadata` to suppress the tensor list.

Example (synthesised fixture):

```text
gguf v3
  arch: qwen2
  tensors: 5
  metadata: 9 entries
  alignment: 32 (tensor data starts at file offset 0x...)

[metadata]
  general.architecture        = "qwen2"
  general.name                = "Qwen2.5-1.5B-Instruct"
  qwen2.attention.head_count  = 12
  qwen2.block_count           = 28
  qwen2.embedding_length      = 1536
  ...

[tensors]
  output.weight       [1536, 152064]   q4_K   offset=0x0
  output_norm.weight  [1536]           f32    offset=0x...
  ...
```

## Tests

```bash
cargo test
```

Two integration tests build synthetic GGUF fixtures via the
`GgufBuilder` helper (in `src/writer.rs`) — one with Qwen2.5-1.5B-
shaped metadata, one with Llama-3.2-1B-shaped metadata — and assert
they round-trip through the parser. No real model files are
committed.

## Scope (M0.1)

Implemented:

- Header parsing (magic, version, counts).
- All GGUF v3 metadata value types, including nested arrays.
- Tensor descriptors (name, dims, ggml type, offset).
- Effective `general.alignment` resolution and tensor-data start.
- Pretty-printing of metadata and tensor list.
- Programmatic writer for tests.

Deliberately out of scope here (handled later in the plan):

- Tokenizer vocab/merges extraction (M0.4 — `dump-vocab` subcommand).
- Quantized block decoding (M1 — runtime parser).
- Memory-mapped zero-copy reads (M1).
