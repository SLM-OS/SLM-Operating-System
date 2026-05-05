//! Integration tests: build a synthetic GGUF in memory with the
//! writer, parse it back, and assert every field round-trips.
//!
//! These tests exercise both the writer and the parser — they would
//! fail if either side drifted from the on-disk encoding the GGUF v3
//! spec defines.

use gguf_inspect::{
    read_vocab_blob, write_vocab_blob, BlobError, GgmlType, Gguf, GgufBuilder, GgufError,
    MetaArray, MetaType, MetaValue, SpecialTokenIds, TensorInfo, HEADER_SIZE, VOCB_MAGIC,
    VOCB_VERSION,
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

// ---------------------------------------------------------------------
// M0.4 vocab-blob tests. Build a synthetic Qwen-like GGUF in memory,
// extract vocab/merges/specials by walking the parsed metadata the same
// way `dump-vocab` does, write a blob, parse it back, and assert exact
// round-trip including the -1 sentinels for missing special-token IDs.
// ---------------------------------------------------------------------

/// Mirrors `extract_byte_array` in main.rs — we duplicate the few lines
/// here to keep `main.rs` private to the binary. If this drifts, the
/// round-trip test will fail.
fn extract_byte_array(g: &Gguf, key: &str) -> Option<Vec<Vec<u8>>> {
    match g.metadata().get(key)? {
        MetaValue::Array(a) if a.elem_type == MetaType::String => {
            let mut out = Vec::with_capacity(a.values.len());
            for v in &a.values {
                if let MetaValue::String(s) = v {
                    out.push(s.as_bytes().to_vec());
                } else {
                    return None;
                }
            }
            Some(out)
        }
        _ => None,
    }
}

fn extract_token_id(g: &Gguf, key: &str) -> i32 {
    match g.metadata().get(key) {
        Some(MetaValue::Uint32(v)) => i32::try_from(*v).unwrap_or(-1),
        Some(MetaValue::Uint64(v)) => i32::try_from(*v).unwrap_or(-1),
        Some(MetaValue::Int32(v)) => *v,
        Some(MetaValue::Int64(v)) => i32::try_from(*v).unwrap_or(-1),
        _ => -1,
    }
}

#[test]
fn dump_vocab_round_trips_synthetic_qwen_tokenizer() {
    // Synthetic Qwen2-like tokenizer metadata. The byte sequence "Ġcat"
    // here is the literal UTF-8 of the BBPE space-marker glyph followed
    // by ASCII; real Qwen vocabs include byte-fallback tokens that
    // aren't valid UTF-8 on their own, but the GGUF layer always
    // wraps tokens in UTF-8 strings and the runtime does the byte
    // unmangling — so a UTF-8 string suffices here.
    let bytes = GgufBuilder::new()
        .add_string("general.architecture", "qwen2")
        .add_string_array(
            "tokenizer.ggml.tokens",
            vec![
                "<|endoftext|>".into(),
                "Hello".into(),
                "World".into(),
                " ".into(),
                "Ġcat".into(),
            ],
        )
        .add_string_array("tokenizer.ggml.merges", vec!["Ġ a".into(), "h e".into()])
        .add_u32("tokenizer.ggml.bos_token_id", 0)
        .add_u32("tokenizer.ggml.eos_token_id", 0)
        .build();

    let g = Gguf::parse(&bytes).expect("parse");

    let vocab = extract_byte_array(&g, "tokenizer.ggml.tokens").expect("tokens");
    let merges = extract_byte_array(&g, "tokenizer.ggml.merges").expect("merges");
    let specials = SpecialTokenIds {
        bos: extract_token_id(&g, "tokenizer.ggml.bos_token_id"),
        eos: extract_token_id(&g, "tokenizer.ggml.eos_token_id"),
        pad: extract_token_id(&g, "tokenizer.ggml.padding_token_id"),
        unk: extract_token_id(&g, "tokenizer.ggml.unknown_token_id"),
        sep: extract_token_id(&g, "tokenizer.ggml.separator_token_id"),
    };

    // Sanity-check what we extracted before serialising.
    assert_eq!(vocab.len(), 5);
    assert_eq!(vocab[0], b"<|endoftext|>");
    assert_eq!(vocab[4], "Ġcat".as_bytes());
    assert_eq!(merges.len(), 2);
    assert_eq!(merges[0], "Ġ a".as_bytes());
    assert_eq!(specials.bos, 0);
    assert_eq!(specials.eos, 0);
    assert_eq!(specials.pad, -1);
    assert_eq!(specials.unk, -1);
    assert_eq!(specials.sep, -1);

    let mut buf = Vec::new();
    write_vocab_blob(&mut buf, &vocab, &merges, specials).expect("write");

    // Header sanity.
    assert_eq!(&buf[0..4], &VOCB_MAGIC);
    assert_eq!(
        u32::from_le_bytes(buf[4..8].try_into().unwrap()),
        VOCB_VERSION
    );
    assert_eq!(u32::from_le_bytes(buf[8..12].try_into().unwrap()), 5);
    assert_eq!(u32::from_le_bytes(buf[12..16].try_into().unwrap()), 2);

    let parsed = read_vocab_blob(&buf).expect("read");
    assert_eq!(parsed.version, VOCB_VERSION);
    assert_eq!(parsed.specials, specials);
    assert_eq!(parsed.vocab, vocab);
    assert_eq!(parsed.merges, merges);
}

#[test]
fn dump_vocab_handles_missing_specials() {
    // GGUF with vocab + merges but no special-token IDs at all.
    let bytes = GgufBuilder::new()
        .add_string("general.architecture", "qwen2")
        .add_string_array(
            "tokenizer.ggml.tokens",
            vec!["a".into(), "b".into(), "c".into()],
        )
        .add_string_array("tokenizer.ggml.merges", vec!["a b".into()])
        .build();

    let g = Gguf::parse(&bytes).expect("parse");
    let vocab = extract_byte_array(&g, "tokenizer.ggml.tokens").expect("tokens");
    let merges = extract_byte_array(&g, "tokenizer.ggml.merges").expect("merges");
    let specials = SpecialTokenIds {
        bos: extract_token_id(&g, "tokenizer.ggml.bos_token_id"),
        eos: extract_token_id(&g, "tokenizer.ggml.eos_token_id"),
        pad: extract_token_id(&g, "tokenizer.ggml.padding_token_id"),
        unk: extract_token_id(&g, "tokenizer.ggml.unknown_token_id"),
        sep: extract_token_id(&g, "tokenizer.ggml.separator_token_id"),
    };
    // All five fields should be the -1 sentinel.
    assert_eq!(specials, SpecialTokenIds::unset());

    let mut buf = Vec::new();
    write_vocab_blob(&mut buf, &vocab, &merges, specials).expect("write");

    // Verify each i32 special slot in the header is exactly -1.
    for off in [16, 20, 24, 28, 32] {
        let raw = i32::from_le_bytes(buf[off..off + 4].try_into().unwrap());
        assert_eq!(raw, -1, "special at offset {off} should be -1");
    }

    let parsed = read_vocab_blob(&buf).expect("read");
    assert_eq!(parsed.specials, SpecialTokenIds::unset());
    assert_eq!(parsed.vocab, vocab);
    assert_eq!(parsed.merges, merges);
}

#[test]
fn vocab_blob_rejects_truncated_input() {
    // Build a real blob, then chop off half of it. Parsing should
    // surface BlobError::Truncated rather than panicking.
    // Use enough vocab/merge entries that buf.len() / 2 lands well
    // past the 40-byte header but inside the body region.
    let vocab: Vec<Vec<u8>> = (0..16).map(|i| vec![b'a' + (i as u8); 8]).collect();
    let merges: Vec<Vec<u8>> = (0..8).map(|i| vec![b'm' + (i as u8); 4]).collect();
    let mut buf = Vec::new();
    write_vocab_blob(&mut buf, &vocab, &merges, SpecialTokenIds::unset()).expect("write");
    let chop_at = HEADER_SIZE + (buf.len() - HEADER_SIZE) / 2;
    assert!(
        chop_at > HEADER_SIZE && chop_at < buf.len(),
        "test fixture must have body to chop"
    );
    let truncated = &buf[..chop_at];
    match read_vocab_blob(truncated) {
        Err(BlobError::Truncated { .. }) => {}
        other => panic!("expected Truncated, got {other:?}"),
    }
}

#[test]
fn vocab_blob_rejects_oversized_vocab_count() {
    // Hand-craft a header that claims 10M vocab entries followed by
    // only ~1 KB of body. The parser's pre-allocate gate (count >
    // remaining / MIN_ENTRY_SIZE = 5) should reject this without
    // entering the read loop.
    let mut buf = Vec::with_capacity(HEADER_SIZE + 1024);
    buf.extend_from_slice(&VOCB_MAGIC);
    buf.extend_from_slice(&VOCB_VERSION.to_le_bytes());
    let claimed_vocab: u32 = 10_000_000;
    buf.extend_from_slice(&claimed_vocab.to_le_bytes());
    buf.extend_from_slice(&0u32.to_le_bytes()); // merges_count
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // bos
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // eos
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // pad
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // unk
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // sep
    buf.extend_from_slice(&0u32.to_le_bytes()); // reserved
    assert_eq!(buf.len(), HEADER_SIZE);
    // Append exactly 1 KB of body — far less than 10M * 5 bytes.
    buf.extend(std::iter::repeat_n(0u8, 1024));

    match read_vocab_blob(&buf) {
        Err(BlobError::OversizedCount { count }) => assert_eq!(count, claimed_vocab),
        other => panic!("expected OversizedCount, got {other:?}"),
    }
}

#[test]
fn vocab_blob_rejects_oversized_entry_len() {
    // Hand-craft a single-vocab-entry blob whose entry-length prefix
    // claims more than MAX_PLAUSIBLE_ENTRY_LEN (1 << 16). The
    // per-entry gate at vocab_blob::read_entry must reject this even
    // though the count gate above passes (one entry is plausible).
    let mut buf = Vec::with_capacity(HEADER_SIZE + 4 + 8);
    buf.extend_from_slice(&VOCB_MAGIC);
    buf.extend_from_slice(&VOCB_VERSION.to_le_bytes());
    buf.extend_from_slice(&1u32.to_le_bytes()); // vocab_count = 1
    buf.extend_from_slice(&0u32.to_le_bytes()); // merges_count = 0
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // bos
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // eos
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // pad
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // unk
    buf.extend_from_slice(&(-1i32).to_le_bytes()); // sep
    buf.extend_from_slice(&0u32.to_le_bytes()); // reserved
    assert_eq!(buf.len(), HEADER_SIZE);
    // Entry length-prefix claims 128 KiB (above the 64 KiB cap).
    let claimed_len: u32 = (1u32 << 17) | 0xCAFE;
    buf.extend_from_slice(&claimed_len.to_le_bytes());
    // A handful of trailing bytes — fewer than claimed_len, but the
    // gate fires on the prefix before any body read.
    buf.extend(std::iter::repeat_n(0u8, 8));

    match read_vocab_blob(&buf) {
        Err(BlobError::OversizedToken { len }) => assert_eq!(len, claimed_len),
        other => panic!("expected OversizedToken, got {other:?}"),
    }
}
