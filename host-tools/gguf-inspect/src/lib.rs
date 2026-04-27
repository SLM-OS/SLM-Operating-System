//! `gguf-inspect` — host-side library for parsing and constructing
//! GGUF v3 files.
//!
//! This crate is intentionally **host-only** (uses `std`) and has zero
//! third-party dependencies. The runtime-side parser used by SLM-OS
//! itself lives separately in `runtime/src/slm/gguf.rs` (see M1 of the
//! SLM integration plan); this host tool exists to inspect and
//! validate GGUF files during development without touching the
//! runtime build.
//!
//! The library is re-exported from `lib.rs` so future subcommands
//! (M0.4 `dump-vocab`, etc.) can build on it without churning the
//! binary's `main.rs`.

pub mod gguf;
pub mod vocab_blob;
pub mod writer;

pub use gguf::{
    GgmlType, Gguf, GgufError, MetaArray, MetaType, MetaValue, TensorInfo, DEFAULT_ALIGNMENT,
    GGUF_MAGIC, GGUF_VERSION,
};
pub use vocab_blob::{
    read_vocab_blob, write_vocab_blob, BlobError, SpecialTokenIds, VocabBlob, HEADER_SIZE,
    MAX_PLAUSIBLE_VOCAB, VOCB_MAGIC, VOCB_VERSION,
};
pub use writer::GgufBuilder;
