//! Integration tests: build a synthetic GGUF in memory with the
//! writer, parse it back, and assert every field round-trips.
//!
//! These tests exercise both the writer and the parser — they would
//! fail if either side drifted from the on-disk encoding the GGUF v3
//! spec defines.

use gguf_inspect::{GgmlType, Gguf, GgufBuilder, MetaArray, MetaType, MetaValue, TensorInfo};

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
            GgmlType(12),
            vec![0u8; 64],
        )
        .add_tensor(
            "output.weight",
            vec![1536, 152064],
            GgmlType(12),
            vec![0u8; 64],
        )
        .add_tensor("output_norm.weight", vec![1536], GgmlType(0), vec![0u8; 16])
        .add_tensor(
            "blk.0.attn_q.weight",
            vec![1536, 1536],
            GgmlType(1), // f16
            vec![0u8; 32],
        )
        .add_tensor(
            "blk.0.attn_k.weight",
            vec![1536, 256],
            GgmlType(15), // q8_K
            vec![0u8; 32],
        )
        .add_tensor(
            "blk.0.ffn_down.weight",
            vec![8960, 1536],
            GgmlType(12), // q4_K
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
            GgmlType(12),
            vec![0u8; 64],
        )
        .add_tensor("output_norm.weight", vec![2048], GgmlType(0), vec![0u8; 16])
        .add_tensor(
            "blk.0.attn_norm.weight",
            vec![2048],
            GgmlType(0), // f32
            vec![0u8; 16],
        )
        .add_tensor(
            "blk.0.attn_q.weight",
            vec![2048, 2048],
            GgmlType(12), // q4_K
            vec![0u8; 48],
        )
        .add_tensor(
            "blk.0.ffn_gate.weight",
            vec![2048, 8192],
            GgmlType(15), // q8_K
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
