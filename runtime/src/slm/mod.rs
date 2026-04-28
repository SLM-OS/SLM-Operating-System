//! Small Language Model runtime support.
//!
//! Includes the GGUF v3 parser, BBPE tokenizer, INT4 dequantization
//! kernels, transformer ops, KV cache, and decoder. See
//! `docs/specs/slm-integration.md` for the full spec.

#![cfg(feature = "slm")]

pub mod chat_template;
pub mod gguf;
pub mod kv_cache;
pub mod registry;
pub mod sampler;
pub mod tokenizer;
