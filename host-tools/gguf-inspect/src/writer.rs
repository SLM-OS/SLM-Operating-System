//! Programmatic GGUF v3 writer.
//!
//! This is intentionally minimal — just enough to round-trip the
//! parser tests and stand up synthetic fixtures resembling Qwen2 /
//! Llama. It is **not** a general-purpose GGUF authoring tool; in
//! particular, no validation is performed on key-name uniqueness or
//! tensor-name uniqueness, and no quantization is performed (callers
//! supply raw bytes for tensor payloads).

use crate::gguf::{
    GgmlType, MetaArray, MetaType, MetaValue, DEFAULT_ALIGNMENT, GGUF_MAGIC, GGUF_VERSION,
};

/// Builder that accumulates KV metadata and tensor descriptors and
/// then emits a valid GGUF byte stream.
#[derive(Default)]
pub struct GgufBuilder {
    kvs: Vec<(String, MetaValue)>,
    tensors: Vec<TensorEntry>,
    alignment: Option<u64>,
}

struct TensorEntry {
    name: String,
    dims: Vec<u64>,
    ggml_type: GgmlType,
    data: Vec<u8>,
}

impl GgufBuilder {
    pub fn new() -> Self {
        Self::default()
    }

    /// Override the tensor-data alignment. The value is also written
    /// out as the `general.alignment` u32 KV (matching what `llama.cpp`
    /// does when alignment differs from the default).
    pub fn alignment(mut self, alignment: u64) -> Self {
        self.alignment = Some(alignment);
        self
    }

    /// Add a metadata KV pair. Order is preserved on the wire.
    pub fn add_kv(mut self, key: impl Into<String>, value: MetaValue) -> Self {
        self.kvs.push((key.into(), value));
        self
    }

    /// Convenience wrappers for the common scalar types.
    pub fn add_string(self, key: impl Into<String>, value: impl Into<String>) -> Self {
        self.add_kv(key, MetaValue::String(value.into()))
    }
    pub fn add_u32(self, key: impl Into<String>, value: u32) -> Self {
        self.add_kv(key, MetaValue::Uint32(value))
    }
    pub fn add_u64(self, key: impl Into<String>, value: u64) -> Self {
        self.add_kv(key, MetaValue::Uint64(value))
    }
    pub fn add_f32(self, key: impl Into<String>, value: f32) -> Self {
        self.add_kv(key, MetaValue::Float32(value))
    }
    pub fn add_bool(self, key: impl Into<String>, value: bool) -> Self {
        self.add_kv(key, MetaValue::Bool(value))
    }

    /// Add a homogeneous array of strings.
    pub fn add_string_array(self, key: impl Into<String>, values: Vec<String>) -> Self {
        let arr = MetaArray {
            elem_type: MetaType::String,
            values: values.into_iter().map(MetaValue::String).collect(),
        };
        self.add_kv(key, MetaValue::Array(arr))
    }

    /// Add a tensor. `data` is the raw bytes that will land in the
    /// tensor-data section; this writer does no quantization or
    /// validation that `data.len()` matches `dims × element_size`.
    pub fn add_tensor(
        mut self,
        name: impl Into<String>,
        dims: Vec<u64>,
        ggml_type: GgmlType,
        data: Vec<u8>,
    ) -> Self {
        self.tensors.push(TensorEntry {
            name: name.into(),
            dims,
            ggml_type,
            data,
        });
        self
    }

    /// Serialize to a complete GGUF byte stream.
    pub fn build(mut self) -> Vec<u8> {
        let alignment = self.alignment.unwrap_or(DEFAULT_ALIGNMENT);

        // If the caller overrode alignment, surface it in the metadata
        // so the parser can recover the same value. We push it last so
        // the caller's own KVs come first in the output (more
        // human-readable), but before tensor info.
        if self.alignment.is_some() {
            // Avoid duplicating if caller already added it.
            if !self.kvs.iter().any(|(k, _)| k == "general.alignment") {
                // The GGUF spec stores `general.alignment` as a u32 KV
                // (and llama.cpp emits it that way). A caller asking
                // for an alignment that doesn't fit is almost certainly
                // a test bug — panic loudly rather than silently
                // truncate.
                assert!(
                    alignment <= u32::MAX as u64,
                    "alignment {alignment} must fit in u32",
                );
                self.kvs.push((
                    "general.alignment".to_string(),
                    MetaValue::Uint32(alignment as u32),
                ));
            }
        }

        let mut out =
            Vec::with_capacity(64 + self.tensors.iter().map(|t| t.data.len()).sum::<usize>());

        // Header.
        out.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        out.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        out.extend_from_slice(&(self.tensors.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.kvs.len() as u64).to_le_bytes());

        // KV metadata.
        for (key, value) in &self.kvs {
            write_string(&mut out, key);
            write_meta_value(&mut out, value);
        }

        // Compute each tensor's offset relative to the (yet-unknown)
        // tensor-data section start, packing them back-to-back with
        // no per-tensor padding (matches `llama.cpp` behaviour for
        // unaligned types and is what our parser expects).
        let mut offset = 0u64;
        let offsets: Vec<u64> = self
            .tensors
            .iter()
            .map(|t| {
                let here = offset;
                offset += t.data.len() as u64;
                here
            })
            .collect();

        // Tensor info.
        for (t, off) in self.tensors.iter().zip(offsets.iter()) {
            write_string(&mut out, &t.name);
            out.extend_from_slice(&(t.dims.len() as u32).to_le_bytes());
            for d in &t.dims {
                out.extend_from_slice(&d.to_le_bytes());
            }
            out.extend_from_slice(&t.ggml_type.0.to_le_bytes());
            out.extend_from_slice(&off.to_le_bytes());
        }

        // Pad to alignment.
        let pad = (alignment - (out.len() as u64 % alignment)) % alignment;
        out.resize(out.len() + pad as usize, 0u8);

        // Tensor data.
        for t in &self.tensors {
            out.extend_from_slice(&t.data);
        }

        out
    }
}

fn write_string(out: &mut Vec<u8>, s: &str) {
    out.extend_from_slice(&(s.len() as u64).to_le_bytes());
    out.extend_from_slice(s.as_bytes());
}

fn write_meta_value(out: &mut Vec<u8>, v: &MetaValue) {
    out.extend_from_slice(&v.meta_type().as_u32().to_le_bytes());
    write_meta_payload(out, v);
}

fn write_meta_payload(out: &mut Vec<u8>, v: &MetaValue) {
    match v {
        MetaValue::Uint8(x) => out.push(*x),
        MetaValue::Int8(x) => out.push(*x as u8),
        MetaValue::Uint16(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Int16(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Uint32(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Int32(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Uint64(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Int64(x) => out.extend_from_slice(&x.to_le_bytes()),
        MetaValue::Float32(x) => out.extend_from_slice(&x.to_bits().to_le_bytes()),
        MetaValue::Float64(x) => out.extend_from_slice(&x.to_bits().to_le_bytes()),
        MetaValue::Bool(x) => out.push(if *x { 1 } else { 0 }),
        MetaValue::String(s) => write_string(out, s),
        MetaValue::Array(arr) => {
            out.extend_from_slice(&arr.elem_type.as_u32().to_le_bytes());
            out.extend_from_slice(&(arr.values.len() as u64).to_le_bytes());
            for elem in &arr.values {
                // Elements use the same encoding as scalar values
                // *minus* the per-element type tag — but for arrays
                // of arrays the inner array still carries its own
                // element-type tag (handled by `write_meta_payload`'s
                // Array arm above).
                write_meta_payload(out, elem);
            }
        }
    }
}
