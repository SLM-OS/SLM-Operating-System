//! Small Language Model runtime support.
//!
//! Includes the GGUF v3 parser, BBPE tokenizer, INT4 dequantization
//! kernels, transformer ops, KV cache, and decoder. See
//! `docs/design/slm-integration.md` for the full spec.

#![cfg(feature = "slm")]

pub mod chat_template;
pub mod decoder;
pub mod forward;
pub mod gpu_program;
pub mod gguf;
pub mod golden;
pub mod kv_cache;
#[cfg(test)]
pub mod numerics_probe;
pub mod registry;
pub mod sampler;
#[cfg(test)]
mod sampler_bench;
pub mod session;
pub mod telemetry;
pub mod tokenizer;
mod unicode_tables;
