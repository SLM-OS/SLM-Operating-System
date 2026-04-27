//! ChatML chat-template renderer for Qwen2 (and compatible Llama
//! conversions). M2.1 of the SLM integration plan.
//!
//! ChatML wraps a sequence of `system / user / assistant` turns with
//! `<|im_start|>{role}\n{content}<|im_end|>\n` blocks and primes the
//! model's reply by appending `<|im_start|>assistant\n` after the
//! final user turn:
//!
//! ```text
//! <|im_start|>system
//! You are a helpful assistant.<|im_end|>
//! <|im_start|>user
//! Hi!<|im_end|>
//! <|im_start|>assistant
//! ```
//!
//! The renderer is purely string-formatting; tokenization happens via
//! the [`Bbpe`] tokenizer in [`crate::slm::tokenizer`], which has
//! direct lookup for the ChatML special-token names.

use alloc::string::String;
use alloc::vec::Vec;

use crate::slm::tokenizer::Bbpe;

/// One conversation turn in a ChatML prompt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Role {
    System,
    User,
    Assistant,
}

impl Role {
    fn as_str(&self) -> &'static str {
        match self {
            Role::System => "system",
            Role::User => "user",
            Role::Assistant => "assistant",
        }
    }
}

/// One turn in a ChatML conversation. `content` is borrowed; the
/// renderer copies it into the output buffer.
#[derive(Debug, Clone, Copy)]
pub struct ChatTurn<'a> {
    pub role: Role,
    pub content: &'a str,
}

/// Render `turns` into a ChatML wire-format string. Each turn becomes
/// `<|im_start|>{role}\n{content}<|im_end|>\n`; the function then
/// appends `<|im_start|>assistant\n` so a subsequent decoder step
/// runs starting from the model's reply.
///
/// Useful for `slm prompt --show-prompt` and tokenizer-bypass tests.
pub fn render_chatml(turns: &[ChatTurn<'_>]) -> String {
    // Pre-size: ~24 bytes of envelope per turn plus content. Use
    // saturating arithmetic so a turn with `content.len()` near
    // usize::MAX doesn't overflow String::with_capacity (which
    // would panic). The capacity is a hint; the String grows as
    // needed.
    let estimate = turns
        .iter()
        .fold(32usize, |acc, t| {
            acc.saturating_add(t.content.len()).saturating_add(24)
        });
    let mut out = String::with_capacity(estimate);
    for turn in turns {
        out.push_str("<|im_start|>");
        out.push_str(turn.role.as_str());
        out.push('\n');
        out.push_str(turn.content);
        out.push_str("<|im_end|>\n");
    }
    out.push_str("<|im_start|>assistant\n");
    out
}

/// Render `turns` and tokenize the result, intermixing direct
/// special-token lookups for `<|im_start|>` / `<|im_end|>` with BPE
/// encoding for the role/content text. Special-token strings are
/// looked up by name through [`Bbpe::special_id`]; if the tokenizer
/// doesn't carry the special, the renderer falls back to byte-level
/// BPE on the literal `<|...|>` form (which produces a longer but
/// still correct token stream).
pub fn encode_chat(tokenizer: &Bbpe, turns: &[ChatTurn<'_>]) -> Vec<u32> {
    let mut ids: Vec<u32> = Vec::with_capacity(turns.len() * 16);

    for turn in turns {
        push_special_or_bytes(tokenizer, &mut ids, "<|im_start|>");
        // Role text (e.g. "system") + newline.
        let mut role_with_nl = String::with_capacity(turn.role.as_str().len() + 1);
        role_with_nl.push_str(turn.role.as_str());
        role_with_nl.push('\n');
        ids.extend(tokenizer.encode(&role_with_nl));
        // Content body.
        ids.extend(tokenizer.encode(turn.content));
        push_special_or_bytes(tokenizer, &mut ids, "<|im_end|>");
        ids.extend(tokenizer.encode("\n"));
    }

    // Assistant opener primes the decoder.
    push_special_or_bytes(tokenizer, &mut ids, "<|im_start|>");
    ids.extend(tokenizer.encode("assistant\n"));

    ids
}

/// Push `name` onto `ids` using the special-token id if the
/// tokenizer carries one; otherwise fall back to byte-level encoding
/// of the literal name. Keeps `encode_chat` working on test
/// fixtures whose synthetic vocab might not register specials.
fn push_special_or_bytes(tokenizer: &Bbpe, ids: &mut Vec<u32>, name: &str) {
    if let Some(id) = tokenizer.special_id(name) {
        ids.push(id);
    } else {
        ids.extend(tokenizer.encode(name));
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::gguf::{Gguf, MetaArray, MetaType, MetaValue, GGUF_MAGIC, GGUF_VERSION};
    use alloc::vec;

    fn write_string(out: &mut Vec<u8>, s: &str) {
        out.extend_from_slice(&(s.len() as u64).to_le_bytes());
        out.extend_from_slice(s.as_bytes());
    }

    fn write_meta_value(out: &mut Vec<u8>, v: &MetaValue) {
        out.extend_from_slice(&v.meta_type().as_u32().to_le_bytes());
        write_meta_value_no_tag(out, v);
    }

    fn write_meta_value_no_tag(out: &mut Vec<u8>, v: &MetaValue) {
        match v {
            MetaValue::Uint32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Int32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Float32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::String(s) => write_string(out, s),
            MetaValue::Array(MetaArray { elem_type, values }) => {
                out.extend_from_slice(&elem_type.as_u32().to_le_bytes());
                out.extend_from_slice(&(values.len() as u64).to_le_bytes());
                for elem in values {
                    write_meta_value_no_tag(out, elem);
                }
            }
            _ => {}
        }
    }

    /// Synthetic GGUF with the bare minimum tokenizer metadata to
    /// support `encode_chat`. Distinct from `tokenizer::tests::build_tiny_gguf`
    /// to keep the test fixtures encapsulated; most tests for the
    /// renderer don't care about merges.
    fn build_chatml_gguf() -> Vec<u8> {
        use crate::slm::gguf::DEFAULT_ALIGNMENT;

        let mut tokens: Vec<MetaValue> = Vec::new();
        let mut token_types: Vec<MetaValue> = Vec::new();

        // Byte tokens 0x00..=0xFF. Same convention as tokenizer.rs's
        // tests: ASCII bytes ride as single-byte UTF-8, high bytes
        // ride as `<0xNN>` hex strings.
        for b in 0u16..=255 {
            let s = if b <= 0x7F {
                let mut s = String::with_capacity(1);
                s.push(b as u8 as char);
                s
            } else {
                let mut s = String::with_capacity(6);
                s.push_str("<0x");
                let hi = (b >> 4) as u8;
                let lo = (b & 0xF) as u8;
                s.push(if hi < 10 { (b'0' + hi) as char } else { (b'a' + hi - 10) as char });
                s.push(if lo < 10 { (b'0' + lo) as char } else { (b'a' + lo - 10) as char });
                s.push('>');
                s
            };
            tokens.push(MetaValue::String(s));
            token_types.push(MetaValue::Int32(2)); // BYTE
        }
        // Specials.
        let specials: &[&str] = &[
            "<|im_start|>",
            "<|im_end|>",
            "<|endoftext|>",
        ];
        for s in specials {
            tokens.push(MetaValue::String(String::from(*s)));
            token_types.push(MetaValue::Int32(3)); // CONTROL
        }

        // Empty merges array — fine for a chat-template test.
        let merges: Vec<MetaValue> = Vec::new();

        let kvs: Vec<(&'static str, MetaValue)> = vec![
            ("general.alignment", MetaValue::Uint32(DEFAULT_ALIGNMENT as u32)),
            ("general.architecture", MetaValue::String(String::from("qwen2"))),
            ("qwen2.block_count", MetaValue::Uint32(1)),
            ("qwen2.embedding_length", MetaValue::Uint32(16)),
            ("qwen2.attention.head_count", MetaValue::Uint32(2)),
            ("qwen2.attention.head_count_kv", MetaValue::Uint32(2)),
            ("qwen2.feed_forward_length", MetaValue::Uint32(32)),
            ("qwen2.context_length", MetaValue::Uint32(64)),
            ("qwen2.rope.freq_base", MetaValue::Float32(10_000.0)),
            (
                "tokenizer.ggml.tokens",
                MetaValue::Array(MetaArray {
                    elem_type: MetaType::String,
                    values: tokens,
                }),
            ),
            (
                "tokenizer.ggml.token_type",
                MetaValue::Array(MetaArray {
                    elem_type: MetaType::Int32,
                    values: token_types,
                }),
            ),
            (
                "tokenizer.ggml.merges",
                MetaValue::Array(MetaArray {
                    elem_type: MetaType::String,
                    values: merges,
                }),
            ),
        ];

        let mut buf: Vec<u8> = Vec::with_capacity(4096);
        buf.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        buf.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        buf.extend_from_slice(&0u64.to_le_bytes());
        buf.extend_from_slice(&(kvs.len() as u64).to_le_bytes());
        for (k, v) in &kvs {
            write_string(&mut buf, k);
            write_meta_value(&mut buf, v);
        }
        let pad = (DEFAULT_ALIGNMENT - (buf.len() as u64 % DEFAULT_ALIGNMENT))
            % DEFAULT_ALIGNMENT;
        buf.extend(core::iter::repeat_n(0u8, pad as usize));
        buf
    }

    #[test]
    fn render_chatml_two_turn() {
        let turns = [
            ChatTurn { role: Role::System, content: "You are a helpful assistant." },
            ChatTurn { role: Role::User, content: "Hi!" },
        ];
        let s = render_chatml(&turns);
        let expected = "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n\
                        <|im_start|>user\nHi!<|im_end|>\n\
                        <|im_start|>assistant\n";
        assert_eq!(s, expected);
    }

    #[test]
    fn encode_chat_appends_assistant_opener() {
        let bytes = build_chatml_gguf();
        let gguf = Gguf::parse(&bytes).expect("parse gguf");
        let tok = Bbpe::from_gguf(&gguf).expect("build tokenizer");
        let im_start = tok.special_id("<|im_start|>").expect("im_start");
        let nl_id = tok.encode("\n");
        assert_eq!(nl_id.len(), 1, "newline encodes to a single byte token");

        let turns = [
            ChatTurn { role: Role::User, content: "Hi" },
        ];
        let ids = encode_chat(&tok, &turns);

        // Final tail must be: <|im_start|> a s s i s t a n t \n
        // i.e. one special id + 9 byte ids for "assistant\n".
        let assistant_bytes = b"assistant\n";
        let tail_len = 1 + assistant_bytes.len();
        assert!(ids.len() >= tail_len);
        let tail = &ids[ids.len() - tail_len..];
        assert_eq!(tail[0], im_start);
        for (i, &b) in assistant_bytes.iter().enumerate() {
            let expected_id = tok
                .special_id(core::str::from_utf8(&[b]).unwrap_or(""))
                .unwrap_or(b as u32);
            // Byte tokens land at id == byte value in the synthetic
            // vocab (built byte-by-byte starting at id 0).
            let _ = expected_id;
            assert_eq!(tail[1 + i], b as u32);
        }
    }
}
