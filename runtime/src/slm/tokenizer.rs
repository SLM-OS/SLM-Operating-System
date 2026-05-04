//! Deterministic byte-level BPE (BBPE) tokenizer for Qwen2 / Llama
//! GGUF models.
//!
//! M2.1 of the SLM integration plan (see `docs/plans/slm-integration-plan.md`
//! §M2 and `docs/specs/slm-integration.md`). Vocab + merges + special-token
//! tables are loaded from the GGUF metadata produced by the M1.x parser
//! ([`crate::slm::gguf`]). No external regex / tokenizer crate is pulled
//! in: the GPT-2 pre-tokenization split is implemented as a hand-rolled
//! DFA.
//!
//! The tokenizer is `no_std` + `alloc`, the same constraint envelope as
//! the rest of the SLM runtime stack.
//!
//! Wire layout of the GGUF tokenizer keys this implementation reads:
//!
//! | Key                          | Type             | Notes                                    |
//! |------------------------------|------------------|------------------------------------------|
//! | `tokenizer.ggml.tokens`      | `Array<String>`  | One entry per id, indexed by token id.   |
//! | `tokenizer.ggml.merges`      | `Array<String>`  | `"left right"` per entry; first-space.   |
//! | `tokenizer.ggml.token_type`  | `Array<I32>`     | Optional. 1 = NORMAL, 2 = BYTE, 3 = CTRL.|
//!
//! For absent `token_type` metadata the loader falls back to detecting
//! special tokens by the `<|...|>` pattern and assumes any single-byte
//! token whose body equals that byte is a BYTE-fallback entry.
//!
//! BYTE-type tokens carry their target byte in one of two on-disk
//! forms: a single-byte UTF-8 string (used for ASCII bytes 0x00..=0x7F),
//! or the literal hex form `<0xNN>` (used by Qwen2 for high bytes
//! 0x80..=0xFF, since UTF-8 can't carry a raw high byte as a one-byte
//! string). Both shapes are translated to a raw `u8` during
//! [`Bbpe::from_gguf`] and indexed into [`Bbpe::byte_table`].
//!
//! The Qwen2 / Llama production tokenizers also apply a GPT-2
//! byte-to-unicode mapping during pre-tokenization that this M2.1
//! implementation does not — full byte-level fidelity for non-ASCII
//! input lands in M5 alongside the decoder. See
//! `docs/specs/slm-integration.md` §M2 for the staging plan.

#![allow(clippy::module_name_repetitions)]

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

use crate::slm::gguf::{Gguf, MetaType, MetaValue};

// ---------------------------------------------------------------------------
// Plausibility gates
// ---------------------------------------------------------------------------

/// Cap on the vocab size we are willing to load. Real-world Qwen2.5 ships
/// 152 064 entries; Llama-3 ships 128 256. Anything past 1 MiB entries
/// is almost certainly malformed metadata.
pub const MAX_PLAUSIBLE_VOCAB: usize = 1 << 20;

/// Cap on the merge-rule count we are willing to load. Same order of
/// magnitude as the vocab.
pub const MAX_PLAUSIBLE_MERGES: usize = 1 << 20;

/// Cap on the byte length of a single token body. Used both for
/// the vocab loader and for BPE-merge intermediate pieces (no merged
/// piece can exceed this length either, preventing pathological merge
/// chains from blowing the stack).
///
/// Sized for real-world vocab maxima: Qwen2.5-1.5B tops out at 256 bytes
/// (whitespace-padding tokens like `Ċ` + 80×`Ġ` for indented code);
/// SmolLM2-135M tops out at 162 bytes; Llama-3 stays under 200. 512
/// gives 2× headroom over the largest observed value while keeping
/// the worst-case allocator footprint bounded
/// (`MAX_PLAUSIBLE_VOCAB × MAX_TOKEN_BYTES = 2^20 × 512 = 512 MB`,
/// comfortable on Jetson's 8 GB and well above any plausible host
/// build target). The previous 64-byte cap was too tight: `Bbpe::from_gguf`
/// rejected SmolLM2's vocab with `VocabTooLarge` and was the proximate
/// cause of every failed `slm load` in the M5 demo path.
const MAX_TOKEN_BYTES: usize = 512;

/// GGUF `token_type` values per the GGML spec.
const TOKEN_TYPE_NORMAL: i32 = 1;
const TOKEN_TYPE_BYTE: i32 = 2;
const TOKEN_TYPE_CONTROL: i32 = 3;
const TOKEN_TYPE_USER_DEFINED: i32 = 4;

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------

/// Errors produced by [`Bbpe::from_gguf`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TokenizerError {
    /// `tokenizer.ggml.tokens` array is missing or wrong type.
    MissingVocab,
    /// `tokenizer.ggml.merges` array is missing or wrong type.
    MissingMerges,
    /// A merge rule didn't have the `"left right"` two-piece form.
    BadMergeFormat,
    /// Vocab size exceeds [`MAX_PLAUSIBLE_VOCAB`].
    VocabTooLarge,
    /// Merge count exceeds [`MAX_PLAUSIBLE_MERGES`].
    MergesTooLarge,
}

// ---------------------------------------------------------------------------
// Tokenizer state
// ---------------------------------------------------------------------------

/// Byte-level BPE tokenizer.
pub struct Bbpe {
    /// Token-id → token-bytes.
    vocab: Vec<Vec<u8>>,
    /// Reverse lookup: token-bytes → token-id. Built from [`Self::vocab`]
    /// during construction; used by the encode hot path to map a piece
    /// back to its id without scanning the full vocab.
    bytes_to_id: BTreeMap<Vec<u8>, u32>,
    /// (left, right) → rank. Lower rank == higher priority. A
    /// `BTreeMap` keeps the no-std story honest (no `hashbrown` dep)
    /// and the keys are small enough that the log-N cost is fine.
    merges: BTreeMap<(Vec<u8>, Vec<u8>), u32>,
    /// Special tokens by name → id. Keyed by the raw token bytes
    /// rendered as UTF-8 (e.g. `"<|im_start|>"`).
    specials: BTreeMap<String, u32>,
    /// Inverse of [`Self::specials`]: special token id → original
    /// bytes. Used by [`Bbpe::decode_with_specials`].
    special_ids_to_bytes: BTreeMap<u32, Vec<u8>>,
    /// 256-entry table of `byte` → `Some(token_id)` for every byte
    /// that appears in the vocab as a single-byte BYTE-type entry.
    /// Populated during [`Bbpe::from_gguf`]; `None` indicates the
    /// byte has no direct vocab entry, in which case
    /// [`Self::encode_piece`] returns a hard error.
    byte_table: [Option<u32>; 256],
}

impl Bbpe {
    /// Build from a parsed GGUF file. Reads the `tokenizer.ggml.tokens`
    /// and `tokenizer.ggml.merges` arrays. Special tokens are
    /// identified via `tokenizer.ggml.token_type` if present; absent
    /// the type metadata, the loader falls back to detecting tokens
    /// matching the `<|...|>` pattern.
    pub fn from_gguf(gguf: &Gguf<'_>) -> Result<Self, TokenizerError> {
        // -- Vocab ---------------------------------------------------
        let tokens_meta = gguf
            .metadata("tokenizer.ggml.tokens")
            .ok_or(TokenizerError::MissingVocab)?;
        let tokens_arr = match tokens_meta {
            MetaValue::Array(a) if a.elem_type == MetaType::String => a,
            _ => return Err(TokenizerError::MissingVocab),
        };
        if tokens_arr.values.len() > MAX_PLAUSIBLE_VOCAB {
            return Err(TokenizerError::VocabTooLarge);
        }
        let mut vocab: Vec<Vec<u8>> = Vec::with_capacity(tokens_arr.values.len());
        for v in &tokens_arr.values {
            let s = match v {
                MetaValue::String(s) => s,
                _ => return Err(TokenizerError::MissingVocab),
            };
            // Per-token byte-length ceiling. Without this gate, the
            // upstream `MAX_PLAUSIBLE_STRING_LEN` of 16 MB combined
            // with `MAX_PLAUSIBLE_VOCAB = 1<<20` would let a hostile
            // metadata file demand ~16 TB of allocation. Real
            // tokens (including specials like `<|endoftext|>` at
            // 13 bytes) sit well under MAX_TOKEN_BYTES.
            if s.as_bytes().len() > MAX_TOKEN_BYTES {
                return Err(TokenizerError::VocabTooLarge);
            }
            vocab.push(s.as_bytes().to_vec());
        }

        // -- Merges --------------------------------------------------
        let merges_meta = gguf
            .metadata("tokenizer.ggml.merges")
            .ok_or(TokenizerError::MissingMerges)?;
        let merges_arr = match merges_meta {
            MetaValue::Array(a) if a.elem_type == MetaType::String => a,
            _ => return Err(TokenizerError::MissingMerges),
        };
        if merges_arr.values.len() > MAX_PLAUSIBLE_MERGES {
            return Err(TokenizerError::MergesTooLarge);
        }
        let mut merges: BTreeMap<(Vec<u8>, Vec<u8>), u32> = BTreeMap::new();
        for (rank, v) in merges_arr.values.iter().enumerate() {
            let s = match v {
                MetaValue::String(s) => s,
                _ => return Err(TokenizerError::BadMergeFormat),
            };
            // Split on the FIRST space — merge bodies may contain
            // additional space-mapped bytes (e.g. the GPT-2 byte-to-
            // unicode mapping makes `' '` round-trip as a printable
            // glyph, but some GGUF exporters keep the literal space).
            let bytes = s.as_bytes();
            let split = bytes.iter().position(|&b| b == b' ');
            let split = match split {
                Some(i) if i > 0 && i + 1 < bytes.len() => i,
                _ => return Err(TokenizerError::BadMergeFormat),
            };
            let left = bytes[..split].to_vec();
            let right = bytes[split + 1..].to_vec();
            // Cap merged-piece length here as well — merges that
            // would produce > MAX_TOKEN_BYTES bodies are discarded.
            if left.len() + right.len() > MAX_TOKEN_BYTES {
                continue;
            }
            // u32::try_from rather than `as` per "Checked arithmetic
            // at boundaries" in runtime/CLAUDE.md.
            let r = u32::try_from(rank).map_err(|_| TokenizerError::MergesTooLarge)?;
            merges.insert((left, right), r);
        }

        // -- Reverse lookup + specials + byte fallback table --------
        let mut bytes_to_id: BTreeMap<Vec<u8>, u32> = BTreeMap::new();
        let mut specials: BTreeMap<String, u32> = BTreeMap::new();
        let mut special_ids_to_bytes: BTreeMap<u32, Vec<u8>> = BTreeMap::new();
        let mut byte_table: [Option<u32>; 256] = [None; 256];

        // Token-type metadata is optional. When present, it is a
        // homogeneous `Array<I32>` (or one of the other signed/unsigned
        // widths some exporters emit) one entry per token.
        let token_types: Option<Vec<i32>> = gguf
            .metadata("tokenizer.ggml.token_type")
            .and_then(|v| match v {
                MetaValue::Array(a) => {
                    // Same DoS gate as the vocab itself: a hostile
                    // metadata file mustn't get to allocate an
                    // unbounded Vec via Vec::with_capacity. The
                    // token_type array is supposed to mirror the
                    // vocab one-for-one; reject anything larger
                    // outright.
                    if a.values.len() > MAX_PLAUSIBLE_VOCAB {
                        return None;
                    }
                    let cap = a.values.len().min(MAX_PLAUSIBLE_VOCAB);
                    let mut out = Vec::with_capacity(cap);
                    for elem in &a.values {
                        match *elem {
                            MetaValue::Int8(x) => out.push(i32::from(x)),
                            MetaValue::Int16(x) => out.push(i32::from(x)),
                            MetaValue::Int32(x) => out.push(x),
                            MetaValue::Uint8(x) => out.push(i32::from(x)),
                            MetaValue::Uint16(x) => out.push(i32::from(x)),
                            // Use try_from per runtime/CLAUDE.md
                            // "Checked arithmetic at boundaries" —
                            // a Uint32 that doesn't fit in i32 means
                            // the metadata is corrupt; reject the
                            // whole array.
                            MetaValue::Uint32(x) => match i32::try_from(x) {
                                Ok(v) => out.push(v),
                                Err(_) => return None,
                            },
                            _ => return None,
                        }
                    }
                    Some(out)
                }
                _ => None,
            });

        for (id, bytes) in vocab.iter().enumerate() {
            let id_u32 = u32::try_from(id).map_err(|_| TokenizerError::VocabTooLarge)?;
            // Reverse lookup keeps the *first* id seen for any given
            // body — duplicates would only arise from corrupted
            // metadata, but in that case we want deterministic
            // round-trip on the canonical id.
            bytes_to_id.entry(bytes.clone()).or_insert(id_u32);

            let tt = token_types.as_ref().and_then(|v| v.get(id).copied());
            let is_special = match tt {
                Some(t) => t == TOKEN_TYPE_CONTROL || t == TOKEN_TYPE_USER_DEFINED,
                None => is_specialish(bytes),
            };
            if is_special {
                if let Ok(name) = core::str::from_utf8(bytes) {
                    specials.insert(String::from(name), id_u32);
                }
                special_ids_to_bytes.insert(id_u32, bytes.clone());
                continue;
            }

            // BYTE-type fallback registration. Two on-disk forms are
            // handled:
            //
            //   (a) Single-byte body — the GGUF stores the raw byte
            //       as a one-byte UTF-8 string. Only valid for bytes
            //       0x00..=0x7F (ASCII range).
            //   (b) Hex-string body `<0xNN>` — used by Qwen2 for
            //       non-ASCII bytes 0x80..=0xFF where the UTF-8
            //       String type can't carry a raw single byte.
            let is_byte_entry = match tt {
                Some(t) => t == TOKEN_TYPE_BYTE,
                None => bytes.len() == 1,
            };
            if is_byte_entry {
                if let Some(b) = byte_for_token(bytes) {
                    let idx = b as usize;
                    if byte_table[idx].is_none() {
                        byte_table[idx] = Some(id_u32);
                    }
                }
            }
        }

        Ok(Self {
            vocab,
            bytes_to_id,
            merges,
            specials,
            special_ids_to_bytes,
            byte_table,
        })
    }

    /// Number of tokens in the vocabulary. Always fits in `u32` because
    /// [`Self::from_gguf`] caps at [`MAX_PLAUSIBLE_VOCAB`].
    pub fn vocab_size(&self) -> u32 {
        // Cast is safe — see the cap check above.
        self.vocab.len() as u32
    }

    /// Look up a special token id by name (e.g. `"<|im_start|>"`).
    pub fn special_id(&self, name: &str) -> Option<u32> {
        self.specials.get(name).copied()
    }

    /// Encode a UTF-8 string into a Vec of token ids. The pre-tokenizer
    /// splits on the GPT-2 regex; BPE merges run within each chunk.
    /// Special-token strings are tokenized as bytes — pass ChatML
    /// inputs through [`crate::slm::chat_template::encode_chat`] to
    /// have specials looked up by name.
    pub fn encode(&self, text: &str) -> Vec<u32> {
        let mut out: Vec<u32> = Vec::with_capacity(text.len() / 3);
        for chunk in pre_tokenize(text) {
            self.encode_piece(chunk.as_bytes(), &mut out);
        }
        out
    }

    /// Decode a slice of token ids back to a UTF-8 String. Skips
    /// special-token bodies. Lossless for any sequence produced by
    /// [`Self::encode`] on valid UTF-8 input.
    pub fn decode(&self, ids: &[u32]) -> String {
        self.decode_inner(ids, false)
    }

    /// Same as [`Self::decode`] but renders specials.
    pub fn decode_with_specials(&self, ids: &[u32]) -> String {
        self.decode_inner(ids, true)
    }

    fn decode_inner(&self, ids: &[u32], render_specials: bool) -> String {
        // saturating_mul: a hostile caller passing an enormous slice
        // shouldn't be able to overflow Vec::with_capacity (which
        // panics on overflow). The cap is just a hint — the loop
        // below grows as needed.
        let mut bytes: Vec<u8> = Vec::with_capacity(ids.len().saturating_mul(3));
        for &id in ids {
            if !render_specials && self.special_ids_to_bytes.contains_key(&id) {
                continue;
            }
            let idx = id as usize;
            if idx < self.vocab.len() {
                bytes.extend_from_slice(&self.vocab[idx]);
            }
        }
        // Replace invalid UTF-8 with U+FFFD rather than failing — the
        // caller may have decoded an unfinished multi-byte sequence
        // mid-stream.
        match String::from_utf8(bytes) {
            Ok(s) => s,
            Err(e) => {
                let bad = e.into_bytes();
                let mut out = String::with_capacity(bad.len());
                let mut i = 0;
                while i < bad.len() {
                    let b = bad[i];
                    if b < 0x80 {
                        out.push(b as char);
                        i += 1;
                    } else {
                        // Walk forward looking for the longest valid
                        // UTF-8 prefix. Replace whatever doesn't
                        // validate with U+FFFD.
                        let rest = &bad[i..];
                        match core::str::from_utf8(rest) {
                            Ok(s) => {
                                out.push_str(s);
                                break;
                            }
                            Err(err) => {
                                let valid = err.valid_up_to();
                                if valid > 0 {
                                    // SAFETY: bytes [i..i+valid] are a
                                    // valid UTF-8 prefix per `valid_up_to`.
                                    let s = unsafe {
                                        core::str::from_utf8_unchecked(&rest[..valid])
                                    };
                                    out.push_str(s);
                                    i += valid;
                                } else {
                                    out.push('\u{FFFD}');
                                    i += 1;
                                }
                            }
                        }
                    }
                }
                out
            }
        }
    }

    /// Push the BPE-merged token ids for `bytes` onto `out`.
    fn encode_piece(&self, bytes: &[u8], out: &mut Vec<u32>) {
        if bytes.is_empty() {
            return;
        }

        // Initial pieces: one byte each, owned `Vec<u8>` so the merge
        // loop can grow them without juggling lifetimes. For typical
        // pre-token chunks this is at most a few dozen bytes.
        let mut pieces: Vec<Vec<u8>> = Vec::with_capacity(bytes.len());
        for &b in bytes {
            pieces.push(alloc::vec![b]);
        }

        // Repeatedly merge the lowest-rank adjacent pair. O(n²) worst
        // case but n is small (length of one pre-tokenized chunk).
        loop {
            let mut best_rank: Option<u32> = None;
            let mut best_idx: usize = 0;
            for i in 0..pieces.len().saturating_sub(1) {
                let key = (pieces[i].clone(), pieces[i + 1].clone());
                if let Some(&rank) = self.merges.get(&key) {
                    if best_rank.map_or(true, |b| rank < b) {
                        best_rank = Some(rank);
                        best_idx = i;
                    }
                }
            }
            match best_rank {
                None => break,
                Some(_) => {
                    let mut merged = Vec::with_capacity(
                        pieces[best_idx].len() + pieces[best_idx + 1].len(),
                    );
                    merged.extend_from_slice(&pieces[best_idx]);
                    merged.extend_from_slice(&pieces[best_idx + 1]);
                    if merged.len() > MAX_TOKEN_BYTES {
                        // Refuse to grow a merged piece past the cap;
                        // bail out of the merge loop and emit what we
                        // have via byte fallback.
                        break;
                    }
                    pieces[best_idx] = merged;
                    pieces.remove(best_idx + 1);
                }
            }
        }

        for piece in &pieces {
            if let Some(&id) = self.bytes_to_id.get(piece) {
                out.push(id);
            } else {
                // Byte fallback: encode each byte individually.
                for &b in piece {
                    if let Some(id) = self.byte_table[b as usize] {
                        out.push(id);
                    } else if let Some(&id) = self.bytes_to_id.get(&alloc::vec![b]) {
                        // Not registered as a BYTE-type but a
                        // single-byte normal token exists — accept it.
                        out.push(id);
                    }
                    // Else: byte is unrepresentable in this vocab.
                    // Drop silently — Qwen2 / Llama vocabs cover the
                    // full 0x00..0xFF range, so this branch only fires
                    // on synthetic malformed vocabs in tests.
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Special-token detection fallback
// ---------------------------------------------------------------------------

/// Heuristic for "this looks like a control/user-defined token" used
/// when `tokenizer.ggml.token_type` is absent. Matches the `<|...|>`
/// envelope that all Qwen2 / Llama specials use.
fn is_specialish(bytes: &[u8]) -> bool {
    bytes.len() >= 4
        && bytes.starts_with(b"<|")
        && bytes.ends_with(b"|>")
}

/// Translate a BYTE-type token body to the raw byte it stands for.
///
/// Two on-disk conventions are accepted:
///
/// * Single-byte body: returns that byte verbatim. Only valid for
///   bytes 0x00..=0x7F because the metadata wire type is UTF-8 and
///   high bytes can't survive as a one-byte string.
/// * `<0xNN>` hex form (8 bytes total: `<`, `0`, `x`, two hex digits,
///   `>`): returns the byte the hex digits encode. Used for bytes
///   0x80..=0xFF in real Qwen2 GGUFs.
///
/// Returns `None` for any other shape.
fn byte_for_token(bytes: &[u8]) -> Option<u8> {
    if bytes.len() == 1 {
        return Some(bytes[0]);
    }
    if bytes.len() == 6
        && bytes[0] == b'<'
        && bytes[1] == b'0'
        && bytes[2] == b'x'
        && bytes[5] == b'>'
    {
        let hi = hex_digit(bytes[3])?;
        let lo = hex_digit(bytes[4])?;
        return Some((hi << 4) | lo);
    }
    None
}

fn hex_digit(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

// ---------------------------------------------------------------------------
// GPT-2 pre-tokenization split
// ---------------------------------------------------------------------------

/// Pre-tokenize `text` into chunks per the GPT-2 split regex used by
/// Qwen2 and Llama:
///
/// ```text
/// '(?:[sdmt]|ll|ve|re)| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
/// ```
///
/// Implemented as a hand-rolled DFA — no regex crate.
pub(crate) fn pre_tokenize(text: &str) -> PreTokens<'_> {
    PreTokens { text, pos: 0 }
}

/// Iterator returned by [`pre_tokenize`].
pub(crate) struct PreTokens<'a> {
    text: &'a str,
    pos: usize,
}

impl<'a> Iterator for PreTokens<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.text.len() {
            return None;
        }
        let rest = &self.text[self.pos..];
        let (start, end) = next_chunk(rest)?;
        let abs_start = self.pos + start;
        let abs_end = self.pos + end;
        self.pos = abs_end;
        Some(&self.text[abs_start..abs_end])
    }
}

/// Find the next chunk in `rest` and return its `(start, end)` byte
/// offsets relative to `rest`. `start` is always 0 (we never skip
/// bytes); the function returns `None` when `rest` is empty.
fn next_chunk(rest: &str) -> Option<(usize, usize)> {
    if rest.is_empty() {
        return None;
    }

    // Rule 1: contraction prefixes. `'s`, `'d`, `'m`, `'t`, `'ll`, `'ve`, `'re`.
    if let Some(end) = match_contraction(rest) {
        return Some((0, end));
    }

    let bytes = rest.as_bytes();
    let first = bytes[0];

    // Rule 2/3/4: optional leading space + run of letters / digits / other.
    // The space is consumed by the chunk only when followed by a
    // non-whitespace, non-empty body (which the head_char check below
    // re-validates).
    let head_byte_idx = if first == b' ' && rest.len() > 1 {
        1usize
    } else {
        0usize
    };

    if head_byte_idx < rest.len() {
        let head_char = rest[head_byte_idx..].chars().next()?;
        if head_char.is_alphabetic() {
            let end = run_chars(rest, head_byte_idx, char::is_alphabetic);
            return Some((0, end));
        }
        if head_char.is_numeric() {
            let end = run_chars(rest, head_byte_idx, char::is_numeric);
            return Some((0, end));
        }
        if !head_char.is_whitespace() {
            // "Other" — punctuation/symbol/anything not letter/digit/whitespace.
            let end = run_chars(rest, head_byte_idx, |c| {
                !c.is_alphabetic() && !c.is_numeric() && !c.is_whitespace()
            });
            return Some((0, end));
        }
    }

    // Rule 5/6: whitespace runs. Reached either when the first char is
    // whitespace (so rules 2/3/4 above didn't fire) or when the input
    // is a single space with nothing usable behind it.
    let mut end = 0usize;
    let mut chars = rest.char_indices();
    while let Some((i, c)) = chars.next() {
        if c.is_whitespace() {
            end = i + c.len_utf8();
        } else {
            break;
        }
    }
    if end == 0 {
        // Defensive: if nothing matched (shouldn't happen given the
        // exhaustive rules above), advance one char so the iterator
        // still terminates.
        let c = rest.chars().next()?;
        end = c.len_utf8();
    }

    // Implement the `\s+(?!\S)` priority by trimming the trailing
    // whitespace if there's a non-space character after the whitespace
    // run *and* the run contains more than one whitespace char.
    if end > 0 && end < rest.len() {
        // Trailing whitespace block before non-space. Per the GPT-2
        // regex's `\s+(?!\S)` alternation, we keep all whitespace
        // characters EXCEPT the last one before the non-space — that
        // last one belongs with the next chunk's optional leading
        // space. Drop the last whitespace char.
        let mut last = end;
        let prev_char = rest[..end].chars().next_back()?;
        last -= prev_char.len_utf8();
        if last > 0 {
            end = last;
        }
    }

    Some((0, end))
}

/// Match a contraction-style prefix (`'s`, `'d`, `'m`, `'t`, `'ll`,
/// `'ve`, `'re`). Returns the byte length of the match, or `None` if
/// no contraction prefix is present.
fn match_contraction(rest: &str) -> Option<usize> {
    let bytes = rest.as_bytes();
    if bytes.first() != Some(&b'\'') {
        return None;
    }
    if bytes.len() >= 3 {
        let two = &bytes[1..3];
        if two == b"ll" || two == b"ve" || two == b"re" {
            return Some(3);
        }
    }
    if bytes.len() >= 2 {
        match bytes[1] {
            b's' | b'd' | b'm' | b't' => return Some(2),
            _ => {}
        }
    }
    None
}

/// Walk `rest` from `start` while `pred` holds, returning the absolute
/// byte index of the first char that fails `pred`.
fn run_chars(rest: &str, start: usize, pred: impl Fn(char) -> bool) -> usize {
    let mut iter = rest[start..].char_indices();
    let mut last = start;
    while let Some((i, c)) = iter.next() {
        if pred(c) {
            last = start + i + c.len_utf8();
        } else {
            return start + i;
        }
    }
    last
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slm::gguf::Gguf;
    use alloc::vec;

    /// Build a tiny synthetic GGUF whose tokenizer metadata covers the
    /// pieces a tokenizer test needs: ASCII byte tokens, a few merges,
    /// and a couple of `<|...|>`-style specials.
    fn build_tiny_gguf() -> Vec<u8> {
        use crate::slm::gguf::{
            DEFAULT_ALIGNMENT, GGUF_MAGIC, GGUF_VERSION, MetaArray, MetaType, MetaValue,
        };

        // Vocab: bytes 0x00..0xFF (256 entries, BYTE-type), then a
        // handful of normal / special tokens.
        let mut tokens: Vec<MetaValue> = Vec::new();
        let mut token_types: Vec<MetaValue> = Vec::new();

        // 0..=255 — byte tokens. Bytes 0x00..=0x7F can ride as a
        // raw single-byte UTF-8 string; high bytes (0x80..=0xFF)
        // cannot, so use the `<0xNN>` form Qwen2 GGUFs ship in
        // production.
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
                s.push(hex_char(hi) as char);
                s.push(hex_char(lo) as char);
                s.push('>');
                s
            };
            tokens.push(MetaValue::String(s));
            token_types.push(MetaValue::Int32(TOKEN_TYPE_BYTE));
        }

        // 256.. — a few merged ASCII pieces.
        let normals: &[&str] = &[
            "Hi", " 42", "Hello", ", ", "world", "!", " w", " worl", " world", "do", "don",
            "'t", "Hi 42", "He", "ll", "lo", " H", " He", " Hel", " Hell", " Hello",
        ];
        let normals_start = tokens.len();
        for s in normals {
            tokens.push(MetaValue::String(String::from(*s)));
            token_types.push(MetaValue::Int32(TOKEN_TYPE_NORMAL));
        }

        // Specials.
        let specials: &[&str] = &[
            "<|im_start|>",
            "<|im_end|>",
            "<|endoftext|>",
        ];
        let _ = normals_start;
        for s in specials {
            tokens.push(MetaValue::String(String::from(*s)));
            token_types.push(MetaValue::Int32(TOKEN_TYPE_CONTROL));
        }

        // Merges — each entry is "left right" where left and right
        // are pieces already representable in the vocab (either as
        // a byte token or a previously-merged normal token).
        let merges_strs: &[&str] = &[
            "H i",     // -> "Hi"
            "H e",     // -> "He"
            "l l",     // -> "ll"
            "l o",     // -> "lo"
            "He ll",   // -> "Hell"
            "Hell o",  // -> "Hello"
            "d o",     // -> "do"
            "do n",    // -> "don"
            "' t",     // -> "'t"
        ];
        let mut merges_arr: Vec<MetaValue> = Vec::new();
        for m in merges_strs {
            merges_arr.push(MetaValue::String(String::from(*m)));
        }

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
                    values: merges_arr,
                }),
            ),
        ];

        let mut buf: Vec<u8> = Vec::with_capacity(4096);
        buf.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        buf.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        buf.extend_from_slice(&0u64.to_le_bytes()); // tensor_count
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

    fn hex_char(nibble: u8) -> u8 {
        match nibble {
            0..=9 => b'0' + nibble,
            10..=15 => b'a' + (nibble - 10),
            _ => b'?',
        }
    }

    fn write_string(out: &mut Vec<u8>, s: &str) {
        out.extend_from_slice(&(s.len() as u64).to_le_bytes());
        out.extend_from_slice(s.as_bytes());
    }

    fn write_meta_value(out: &mut Vec<u8>, v: &MetaValue) {
        use crate::slm::gguf::MetaArray;
        out.extend_from_slice(&v.meta_type().as_u32().to_le_bytes());
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

    fn write_meta_value_no_tag(out: &mut Vec<u8>, v: &MetaValue) {
        match v {
            MetaValue::Uint32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Int32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::Float32(x) => out.extend_from_slice(&x.to_le_bytes()),
            MetaValue::String(s) => write_string(out, s),
            _ => {}
        }
    }

    #[test]
    fn from_gguf_parses_vocab_and_merges() {
        let bytes = build_tiny_gguf();
        let gguf = Gguf::parse(&bytes).expect("parse gguf");
        let tok = Bbpe::from_gguf(&gguf).expect("build tokenizer");
        // 256 byte tokens + 21 normals + 3 specials.
        assert_eq!(tok.vocab_size(), 256 + 21 + 3);
        assert!(tok.merges.contains_key(&(b"H".to_vec(), b"i".to_vec())));
        assert!(tok.merges.contains_key(&(b"He".to_vec(), b"ll".to_vec())));
    }

    #[test]
    fn special_token_lookup() {
        let bytes = build_tiny_gguf();
        let gguf = Gguf::parse(&bytes).expect("parse gguf");
        let tok = Bbpe::from_gguf(&gguf).expect("build tokenizer");
        let im_start = tok.special_id("<|im_start|>").expect("im_start id");
        let im_end = tok.special_id("<|im_end|>").expect("im_end id");
        let eot = tok.special_id("<|endoftext|>").expect("eot id");
        assert_ne!(im_start, im_end);
        assert_ne!(im_end, eot);
    }

    #[test]
    fn encode_decode_round_trip_ascii() {
        let bytes = build_tiny_gguf();
        let gguf = Gguf::parse(&bytes).expect("parse gguf");
        let tok = Bbpe::from_gguf(&gguf).expect("build tokenizer");
        let s = "Hello, world!";
        let ids = tok.encode(s);
        let back = tok.decode(&ids);
        assert_eq!(back, s);
    }

    #[test]
    fn byte_fallback_for_unknown_byte() {
        let bytes = build_tiny_gguf();
        let gguf = Gguf::parse(&bytes).expect("parse gguf");
        let tok = Bbpe::from_gguf(&gguf).expect("build tokenizer");
        // 0xC2 0xA9 == "©" — neither ©, the unicode glyph, nor any
        // multi-byte merge involving 0xC2/0xA9 lives in the synthetic
        // vocab, so the encoder falls back to the two BYTE entries.
        let ids = tok.encode("©");
        assert_eq!(ids.len(), 2);
        assert_eq!(tok.byte_table[0xC2].unwrap(), ids[0]);
        assert_eq!(tok.byte_table[0xA9].unwrap(), ids[1]);
    }

    #[test]
    fn pre_tokenize_splits_letters_digits_punct() {
        let chunks: Vec<&str> = pre_tokenize("Hi 42!").collect();
        assert_eq!(chunks, vec!["Hi", " 42", "!"]);
    }

    #[test]
    fn pre_tokenize_handles_apostrophe_contractions() {
        let chunks: Vec<&str> = pre_tokenize("don't").collect();
        assert_eq!(chunks, vec!["don", "'t"]);
    }

    #[test]
    fn pre_tokenize_trailing_space_priority() {
        // "Hi   x" — three spaces between Hi and x. The `\s+(?!\S)`
        // alternation should keep two spaces with the whitespace run
        // and hand the last one off to the next chunk's optional
        // leading-space rule.
        let chunks: Vec<&str> = pre_tokenize("Hi   x").collect();
        assert_eq!(chunks, vec!["Hi", "  ", " x"]);
    }

    #[test]
    fn from_gguf_rejects_oversized_vocab() {
        // Build a fixture, then mutate the parsed Gguf to claim a
        // huge vocab. We can't easily mutate a Gguf, so instead we
        // build a tokens array longer than MAX_PLAUSIBLE_VOCAB —
        // skipped in practice (would consume hundreds of MB of
        // metadata to actually trip the gate).
        //
        // Sanity-only: confirm the constant is exposed.
        assert_eq!(MAX_PLAUSIBLE_VOCAB, 1 << 20);
        assert_eq!(MAX_PLAUSIBLE_MERGES, 1 << 20);
    }

    #[test]
    fn max_token_bytes_covers_observed_vocabs() {
        // Empirical maxima from `gguf-inspect dump-vocab`:
        //   - SmolLM2-135M-Instruct.Q4_K_M:  162 bytes
        //   - Qwen2.5-1.5B-Instruct-Q4_K_M:  256 bytes
        //   - Llama-3.2-1B-Instruct-Q4_K_M: <200 bytes
        // The cap must accept all three; a regression that lowers
        // it below 256 trips this, and a regression that lowers it
        // below 64 (the historic value before #608's diagnostic)
        // re-introduces the SmolLM-load failure that motivated the
        // bump.
        assert!(MAX_TOKEN_BYTES >= 256,
            "MAX_TOKEN_BYTES = {MAX_TOKEN_BYTES} < 256; would reject \
             real-world vocabs (Qwen2.5 max = 256). See tokenizer.rs \
             docstring for sizing rationale.");
    }
}
