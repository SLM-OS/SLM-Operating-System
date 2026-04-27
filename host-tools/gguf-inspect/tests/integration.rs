//! Integration tests: build a synthetic GGUF in memory with the
//! writer, parse it back, and assert every field round-trips.
//!
//! These tests exercise both the writer and the parser — they would
//! fail if either side drifted from the on-disk encoding the GGUF v3
//! spec defines.

use gguf_inspect::{
    GgmlType, Gguf, GgufBuilder, GgufError, MetaArray, MetaType, MetaValue, TensorInfo,
};

/// Helper: assert a tensor descriptor matches the expected fields.
fn assert_tensor(t: &TensorInfo, name: &str, dims: &[u64], ggml_type: u32) {
    assert_eq!(t.name, name, "tensor name mismatch");
    assert_eq!(t.dims, dims, "tensor {name} dims mismatch");
    assert_eq!(
        t.ggml_type,
        GgmlType(ggml_type),
        "tensor {name} ggml_type mismatch"
    );
}

#[test]
fn writes_then_parses_minimal_qwen2() {
    // Synthetic Qwen2.5-1.5B-Instruct-shaped metadata. Field set
    // mirrors what the real Qwen2 GGUF carries; values match the
    // public Qwen/Qwen2.5-1.5B-Instruct config.
    let bytes = GgufBuilder::new()
        .add_string("general.architecture", "qwen2")
        .add_string("general.name", "Qwen2.5-1.5B-Instruct")
        .add_string("general.quantization_version", "2")
        .add_u32("qwen2.block_count", 28)
        .add_u32("qwen2.embedding_length", 1536)
        .add_u32("qwen2.attention.head_count", 12)
        .add_u32("qwen2.attention.head_count_kv", 2)
        .add_u32("qwen2.feed_forward_length", 8960)
        .add_u32("qwen2.context_length", 32768)
        .add_f32("qwen2.rope.freq_base", 1_000_000.0)
        .add_kv(
            "qwen2.attention.layer_norm_rms_epsilon",
            MetaValue::Float32(1.0e-6),
        )
        .add_string_array(
            "tokenizer.ggml.tokens",
            vec![
                "<|endoftext|>".into(),
                "<|im_start|>".into(),
                "<|im_end|>".into(),
            ],
        )
        .add_bool("tokenizer.ggml.add_bos_token", false)
        // A compact integer array — exercises arrays of scalars.
        .add_kv(
            "tokenizer.ggml.token_type",
            MetaValue::Array(MetaArray {
                elem_type: MetaType::Int32,
                values: vec![
                    MetaValue::Int32(1),
                    MetaValue::Int32(1),
                    MetaValue::Int32(1),
                ],
            }),
        )
        // A handful of fake tensors of varied ggml types. Sizes are
        // small but plausible (we don't quantize, just feed raw bytes).
        .add_tensor(
            "token_embd.weight",
            vec![1536, 152064],
            GgmlType::Q4_K,
            vec![0u8; 64],
        )
        .add_tensor(
            "output.weight",
            vec![1536, 152064],
            GgmlType::Q4_K,
            vec![0u8; 64],
        )
        .add_tensor(
            "output_norm.weight",
            vec![1536],
            GgmlType::F32,
            vec![0u8; 16],
        )
        .add_tensor(
            "blk.0.attn_q.weight",
            vec![1536, 1536],
            GgmlType::F16,
            vec![0u8; 32],
        )
        .add_tensor(
            "blk.0.attn_k.weight",
            vec![1536, 256],
            GgmlType::Q8_K,
            vec![0u8; 32],
        )
        .add_tensor(
            "blk.0.ffn_down.weight",
            vec![8960, 1536],
            GgmlType::Q4_K,
            vec![0u8; 64],
        )
        .build();

    let g = Gguf::parse(&bytes).expect("parse");

    // Header.
    assert_eq!(g.version, 3);
    assert_eq!(g.architecture(), Some("qwen2"));
    assert_eq!(g.tensors().len(), 6);
    assert_eq!(g.metadata().len(), 14);
    assert_eq!(g.alignment, gguf_inspect::DEFAULT_ALIGNMENT);

    // Scalars.
    assert_eq!(
        g.metadata().get("general.name"),
        Some(&MetaValue::String("Qwen2.5-1.5B-Instruct".into()))
    );
    assert_eq!(
        g.metadata().get("qwen2.block_count"),
        Some(&MetaValue::Uint32(28))
    );
    assert_eq!(
        g.metadata().get("qwen2.embedding_length"),
        Some(&MetaValue::Uint32(1536))
    );
    assert_eq!(
        g.metadata().get("qwen2.attention.head_count"),
        Some(&MetaValue::Uint32(12))
    );
    assert_eq!(
        g.metadata().get("qwen2.attention.head_count_kv"),
        Some(&MetaValue::Uint32(2))
    );
    assert_eq!(
        g.metadata().get("qwen2.feed_forward_length"),
        Some(&MetaValue::Uint32(8960))
    );
    assert_eq!(
        g.metadata().get("qwen2.context_length"),
        Some(&MetaValue::Uint32(32768))
    );
    assert_eq!(
        g.metadata().get("qwen2.rope.freq_base"),
        Some(&MetaValue::Float32(1_000_000.0))
    );
    assert_eq!(
        g.metadata().get("tokenizer.ggml.add_bos_token"),
        Some(&MetaValue::Bool(false))
    );

    // String array.
    match g.metadata().get("tokenizer.ggml.tokens") {
        Some(MetaValue::Array(a)) => {
            assert_eq!(a.elem_type, MetaType::String);
            assert_eq!(a.values.len(), 3);
            assert_eq!(a.values[0], MetaValue::String("<|endoftext|>".into()));
            assert_eq!(a.values[1], MetaValue::String("<|im_start|>".into()));
            assert_eq!(a.values[2], MetaValue::String("<|im_end|>".into()));
        }
        other => panic!("tokens array missing or wrong shape: {other:?}"),
    }

    // Integer array.
    match g.metadata().get("tokenizer.ggml.token_type") {
        Some(MetaValue::Array(a)) => {
            assert_eq!(a.elem_type, MetaType::Int32);
            assert_eq!(
                a.values,
                vec![
                    MetaValue::Int32(1),
                    MetaValue::Int32(1),
                    MetaValue::Int32(1),
                ]
            );
        }
        other => panic!("token_type array missing or wrong shape: {other:?}"),
    }

    // Tensors. Offsets are computed by the writer back-to-back from
    // 0; verify that and that descriptors round-trip exactly.
    let ts = g.tensors();
    assert_tensor(&ts[0], "token_embd.weight", &[1536, 152064], 12);
    assert_eq!(ts[0].offset, 0);
    assert_tensor(&ts[1], "output.weight", &[1536, 152064], 12);
    assert_eq!(ts[1].offset, 64);
    assert_tensor(&ts[2], "output_norm.weight", &[1536], 0);
    assert_eq!(ts[2].offset, 128);
    assert_tensor(&ts[3], "blk.0.attn_q.weight", &[1536, 1536], 1);
    assert_eq!(ts[3].offset, 144);
    assert_tensor(&ts[4], "blk.0.attn_k.weight", &[1536, 256], 15);
    assert_eq!(ts[4].offset, 176);
    assert_tensor(&ts[5], "blk.0.ffn_down.weight", &[8960, 1536], 12);
    assert_eq!(ts[5].offset, 208);

    // Tensor data start should be aligned to the default (32 B).
    assert_eq!(g.tensor_data_start % gguf_inspect::DEFAULT_ALIGNMENT, 0);
}

#[test]
fn writes_then_parses_minimal_llama() {
    // Synthetic Llama-3.2-1B-shaped metadata.
    let bytes = GgufBuilder::new()
        .add_string("general.architecture", "llama")
        .add_string("general.name", "Llama-3.2-1B-Instruct")
        .add_u32("llama.block_count", 16)
        .add_u32("llama.embedding_length", 2048)
        .add_u32("llama.attention.head_count", 32)
        .add_u32("llama.attention.head_count_kv", 8)
        .add_u32("llama.feed_forward_length", 8192)
        .add_u32("llama.context_length", 131072)
        .add_f32("llama.rope.freq_base", 500_000.0)
        .add_kv(
            "llama.attention.layer_norm_rms_epsilon",
            MetaValue::Float32(1.0e-5),
        )
        .add_u64("llama.rope.dimension_count", 64)
        .add_string_array(
            "tokenizer.ggml.tokens",
            vec!["<|begin_of_text|>".into(), "<|end_of_text|>".into()],
        )
        .add_tensor(
            "token_embd.weight",
            vec![2048, 128256],
            GgmlType::Q4_K,
            vec![0u8; 64],
        )
        .add_tensor(
            "output_norm.weight",
            vec![2048],
            GgmlType::F32,
            vec![0u8; 16],
        )
        .add_tensor(
            "blk.0.attn_norm.weight",
            vec![2048],
            GgmlType::F32,
            vec![0u8; 16],
        )
        .add_tensor(
            "blk.0.attn_q.weight",
            vec![2048, 2048],
            GgmlType::Q4_K,
            vec![0u8; 48],
        )
        .add_tensor(
            "blk.0.ffn_gate.weight",
            vec![2048, 8192],
            GgmlType::Q8_K,
            vec![0u8; 48],
        )
        .build();

    let g = Gguf::parse(&bytes).expect("parse");

    // Header.
    assert_eq!(g.version, 3);
    assert_eq!(g.architecture(), Some("llama"));
    assert_eq!(g.tensors().len(), 5);
    assert_eq!(g.metadata().len(), 12);

    // Scalars.
    assert_eq!(
        g.metadata().get("llama.block_count"),
        Some(&MetaValue::Uint32(16))
    );
    assert_eq!(
        g.metadata().get("llama.embedding_length"),
        Some(&MetaValue::Uint32(2048))
    );
    assert_eq!(
        g.metadata().get("llama.attention.head_count"),
        Some(&MetaValue::Uint32(32))
    );
    assert_eq!(
        g.metadata().get("llama.attention.head_count_kv"),
        Some(&MetaValue::Uint32(8))
    );
    assert_eq!(
        g.metadata().get("llama.feed_forward_length"),
        Some(&MetaValue::Uint32(8192))
    );
    assert_eq!(
        g.metadata().get("llama.context_length"),
        Some(&MetaValue::Uint32(131072))
    );
    assert_eq!(
        g.metadata().get("llama.rope.freq_base"),
        Some(&MetaValue::Float32(500_000.0))
    );
    assert_eq!(
        g.metadata().get("llama.rope.dimension_count"),
        Some(&MetaValue::Uint64(64))
    );

    // Tensors.
    let ts = g.tensors();
    assert_tensor(&ts[0], "token_embd.weight", &[2048, 128256], 12);
    assert_eq!(ts[0].offset, 0);
    assert_tensor(&ts[1], "output_norm.weight", &[2048], 0);
    assert_eq!(ts[1].offset, 64);
    assert_tensor(&ts[2], "blk.0.attn_norm.weight", &[2048], 0);
    assert_eq!(ts[2].offset, 80);
    assert_tensor(&ts[3], "blk.0.attn_q.weight", &[2048, 2048], 12);
    assert_eq!(ts[3].offset, 96);
    assert_tensor(&ts[4], "blk.0.ffn_gate.weight", &[2048, 8192], 15);
    assert_eq!(ts[4].offset, 144);

    // Tensor data start should be aligned.
    assert_eq!(g.tensor_data_start % gguf_inspect::DEFAULT_ALIGNMENT, 0);
}

// ---------------------------------------------------------------------
// Negative-path tests. These exercise the parser's bounds checks
// against deliberately-malformed inputs — they're the regression
// safety net for the DoS-via-malformed-GGUF fixes (PR #487 review).
// ---------------------------------------------------------------------

/// Convenience: build a minimal but valid GGUF byte stream (header
/// only, zero tensors, zero KVs) so each negative test can splice in
/// just the malformation it cares about.
fn minimal_header_bytes(tensor_count: u64, kv_count: u64) -> Vec<u8> {
    let mut out = Vec::with_capacity(24);
    out.extend_from_slice(&gguf_inspect::GGUF_MAGIC.to_le_bytes());
    out.extend_from_slice(&gguf_inspect::GGUF_VERSION.to_le_bytes());
    out.extend_from_slice(&tensor_count.to_le_bytes());
    out.extend_from_slice(&kv_count.to_le_bytes());
    out
}

#[test]
fn rejects_bad_magic() {
    let mut bytes = minimal_header_bytes(0, 0);
    bytes[0..4].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
    match Gguf::parse(&bytes) {
        Err(GgufError::BadMagic(0xDEAD_BEEF)) => {}
        other => panic!("expected BadMagic, got {other:?}"),
    }
}

#[test]
fn rejects_bad_version() {
    let mut bytes = minimal_header_bytes(0, 0);
    bytes[4..8].copy_from_slice(&99u32.to_le_bytes());
    match Gguf::parse(&bytes) {
        Err(GgufError::UnsupportedVersion(99)) => {}
        other => panic!("expected UnsupportedVersion(99), got {other:?}"),
    }
}

#[test]
fn rejects_truncated_string() {
    // Header claims 1 KV pair; the KV body declares a huge key length
    // (1 MB) but only 3 bytes of body follow. The per-KV minimum-size
    // gate requires ≥ 13 bytes remaining for `kv_count = 1`; the body
    // here is 11 bytes (8 length + 3 partial body), so we have to pad
    // a bit to clear the gate. Once past it, reading the key string
    // EOFs cleanly because 1 MB of body isn't there.
    let mut bytes = minimal_header_bytes(0, 1);
    // key length = 1 MB
    let key_len: u64 = 1 << 20;
    bytes.extend_from_slice(&key_len.to_le_bytes());
    // body: only 3 bytes of "key" + 2 bytes of padding so total
    // remaining-after-header is 13 bytes (clears MIN_KV_RECORD_SIZE).
    bytes.extend_from_slice(b"key");
    bytes.extend_from_slice(&[0u8; 2]);
    match Gguf::parse(&bytes) {
        Err(GgufError::UnexpectedEof { .. }) => {}
        other => panic!("expected UnexpectedEof, got {other:?}"),
    }
}

#[test]
fn rejects_oversized_tensor_count() {
    // u64::MAX tensors in a 24-byte file. Pre-fix this aborts the
    // process via Vec::with_capacity. Post-fix it's a typed error.
    let bytes = minimal_header_bytes(u64::MAX, 0);
    match Gguf::parse(&bytes) {
        Err(GgufError::TooManyTensors(n)) if n == u64::MAX => {}
        other => panic!("expected TooManyTensors(u64::MAX), got {other:?}"),
    }
}

#[test]
fn rejects_oversized_kv_count() {
    let bytes = minimal_header_bytes(0, u64::MAX);
    match Gguf::parse(&bytes) {
        Err(GgufError::TooManyKvs(n)) if n == u64::MAX => {}
        other => panic!("expected TooManyKvs(u64::MAX), got {other:?}"),
    }
}

#[test]
fn rejects_oversized_array_length() {
    // 1 KV pair: key="a", value=Array<String> with claimed length
    // u64::MAX. Each string element needs ≥ 8 bytes, so the bound
    // check rejects this without ever entering the loop.
    let mut bytes = minimal_header_bytes(0, 1);
    // key
    bytes.extend_from_slice(&1u64.to_le_bytes());
    bytes.extend_from_slice(b"a");
    // value type tag = Array (9)
    bytes.extend_from_slice(&(MetaType::Array.as_u32()).to_le_bytes());
    // elem type = String (8)
    bytes.extend_from_slice(&(MetaType::String.as_u32()).to_le_bytes());
    // length = u64::MAX
    bytes.extend_from_slice(&u64::MAX.to_le_bytes());
    match Gguf::parse(&bytes) {
        Err(GgufError::ArrayTooLong(n)) if n == u64::MAX => {}
        other => panic!("expected ArrayTooLong, got {other:?}"),
    }
}

#[test]
fn rejects_oversized_nested_array_length() {
    // Same idea but the oversize is in a *nested* array, exercising
    // the Critical-fix-#3 path that previously had no length check.
    let mut bytes = minimal_header_bytes(0, 1);
    bytes.extend_from_slice(&1u64.to_le_bytes());
    bytes.extend_from_slice(b"a");
    // value type tag = Array
    bytes.extend_from_slice(&(MetaType::Array.as_u32()).to_le_bytes());
    // outer elem type = Array
    bytes.extend_from_slice(&(MetaType::Array.as_u32()).to_le_bytes());
    // outer length = 1
    bytes.extend_from_slice(&1u64.to_le_bytes());
    // inner elem type = Uint64
    bytes.extend_from_slice(&(MetaType::Uint64.as_u32()).to_le_bytes());
    // inner length = u64::MAX
    bytes.extend_from_slice(&u64::MAX.to_le_bytes());
    match Gguf::parse(&bytes) {
        Err(GgufError::ArrayTooLong(n)) if n == u64::MAX => {}
        other => panic!("expected ArrayTooLong (nested), got {other:?}"),
    }
}

#[test]
fn rejects_non_power_of_two_alignment() {
    // Build a real file with general.alignment = 24 (not a power of
    // two) — parser should reject before computing tensor_data_start.
    let bytes = GgufBuilder::new()
        .add_string("general.architecture", "test")
        .add_u32("general.alignment", 24)
        .build();
    match Gguf::parse(&bytes) {
        Err(GgufError::BadAlignment(24)) => {}
        other => panic!("expected BadAlignment(24), got {other:?}"),
    }
}

#[test]
fn rejects_unknown_meta_type_tag() {
    // 1 KV pair, key="a", value with type tag 0xFFFF (not a real type).
    let mut bytes = minimal_header_bytes(0, 1);
    bytes.extend_from_slice(&1u64.to_le_bytes());
    bytes.extend_from_slice(b"a");
    bytes.extend_from_slice(&0xFFFFu32.to_le_bytes());
    match Gguf::parse(&bytes) {
        Err(GgufError::UnknownMetaType(0xFFFF)) => {}
        other => panic!("expected UnknownMetaType(0xFFFF), got {other:?}"),
    }
}
