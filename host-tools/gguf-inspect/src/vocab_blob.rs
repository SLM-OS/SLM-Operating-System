//! Self-contained binary blob format for the BBPE tokenizer vocab +
//! merges extracted from a GGUF.
//!
//! The blob exists so that the M2 BBPE tokenizer's golden-vector tests
//! (`runtime/src/slm/tokenizer.rs`) can stage a real Qwen vocab without
//! depending on the host-only GGUF parser. Producing it here once and
//! checking the bytes (or generating them on demand from the fetched
//! GGUF) keeps the runtime test data simple: a single `*.vocb` file
//! parsed by ~50 lines of `&[u8]` arithmetic.
//!
//! # Layout
//!
//! Little-endian, no padding between fields.
//!
//! ```text
//!  offset  size      field
//!  0       4         magic        "VOCB"
//!  4       4         version      u32 LE = 1
//!  8       4         vocab_count  u32 LE
//!  12      4         merges_count u32 LE
//!  16      4         bos_id       i32 LE (-1 if absent)
//!  20      4         eos_id       i32 LE
//!  24      4         pad_id       i32 LE
//!  28      4         unk_id       i32 LE
//!  32      4         sep_id       i32 LE
//!  36      4         _reserved    u32 LE = 0
//!  40      ...       vocab        repeated: u32 LE byte_len, then byte_len bytes
//!  ...     ...       merges       repeated: u32 LE byte_len, then byte_len bytes
//! ```
//!
//! Tokens and merges are stored as raw `Vec<u8>` (not `String`) so the
//! blob can carry the BBPE byte-fallback ranges that aren't valid UTF-8
//! on their own. The `i32` special-token ID encoding mirrors HuggingFace
//! convention: `-1` = "unset / not configured".

use std::fmt;
use std::io;

/// Magic bytes at the start of a vocab blob: ASCII `"VOCB"`.
pub const VOCB_MAGIC: [u8; 4] = *b"VOCB";

/// Format version this module reads/writes.
pub const VOCB_VERSION: u32 = 1;

/// Fixed-size header (offset 0 .. 40).
pub const HEADER_SIZE: usize = 40;

/// Per-token / per-merge length-prefix (`u32` byte length).
const LEN_PREFIX_SIZE: usize = 4;

/// Worst-case wire size of a single token or merge entry: 4-byte length
/// prefix + at least 1 byte of body. Used to bound declared counts
/// against the bytes remaining in the blob, mirroring the same DoS-gate
/// posture as the GGUF parser.
const MIN_ENTRY_SIZE: usize = LEN_PREFIX_SIZE + 1;

/// Sanity ceiling on `vocab_count` / `merges_count`. Real BBPE
/// tokenizers ship well under this; capping the *initial* `Vec`
/// allocation at this value keeps a malicious header (e.g. `u32::MAX`)
/// from aborting the process via OOM.
pub const MAX_PLAUSIBLE_VOCAB: u32 = 1 << 20;

/// Reject any single token / merge body longer than this. The longest
/// real BBPE token is the multi-byte UTF-8 form of a CJK ideograph plus
/// the `Ġ` space marker — a few dozen bytes at most. 64 KiB is
/// generous.
const MAX_PLAUSIBLE_ENTRY_LEN: u32 = 1 << 16;

/// Special-token IDs extracted from the GGUF metadata. Each field is
/// `-1` when the corresponding key is absent (HuggingFace convention).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpecialTokenIds {
    pub bos: i32,
    pub eos: i32,
    pub pad: i32,
    pub unk: i32,
    pub sep: i32,
}

impl SpecialTokenIds {
    /// Construct an `all -1` sentinel (every field "absent").
    pub fn unset() -> Self {
        Self {
            bos: -1,
            eos: -1,
            pad: -1,
            unk: -1,
            sep: -1,
        }
    }
}

impl Default for SpecialTokenIds {
    fn default() -> Self {
        Self::unset()
    }
}

/// A parsed vocab blob.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VocabBlob {
    pub version: u32,
    pub specials: SpecialTokenIds,
    pub vocab: Vec<Vec<u8>>,
    pub merges: Vec<Vec<u8>>,
}

/// Errors produced while reading a vocab blob.
#[derive(Debug)]
pub enum BlobError {
    /// Magic at byte 0 didn't match `"VOCB"`.
    BadMagic([u8; 4]),
    /// Format version is not [`VOCB_VERSION`].
    BadVersion(u32),
    /// Blob ended before we could read the requested bytes.
    Truncated { need: usize },
    /// Header `vocab_count` or `merges_count` exceeds the bytes
    /// remaining in the blob (file is corrupt or hostile).
    OversizedCount { count: u32 },
    /// A single token / merge body exceeds [`MAX_PLAUSIBLE_ENTRY_LEN`].
    OversizedToken { len: u32 },
    /// The reserved field at offset 36 was non-zero. Reserved bits must
    /// be zero in v1; if a future version uses them, the version field
    /// will increment first.
    ReservedNonZero(u32),
}

impl fmt::Display for BlobError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BlobError::BadMagic(m) => {
                write!(
                    f,
                    "bad vocab-blob magic: {:?} (expected {:?})",
                    m, &VOCB_MAGIC
                )
            }
            BlobError::BadVersion(v) => {
                write!(
                    f,
                    "unsupported vocab-blob version {v} (expected {VOCB_VERSION})"
                )
            }
            BlobError::Truncated { need } => {
                write!(f, "truncated vocab blob: needed {need} more bytes")
            }
            BlobError::OversizedCount { count } => {
                write!(f, "vocab/merges count {count} exceeds remaining blob size")
            }
            BlobError::OversizedToken { len } => {
                write!(
                    f,
                    "token/merge body length {len} exceeds plausibility cap ({})",
                    MAX_PLAUSIBLE_ENTRY_LEN
                )
            }
            BlobError::ReservedNonZero(v) => {
                write!(f, "reserved field is 0x{v:08x} (expected 0)")
            }
        }
    }
}

impl std::error::Error for BlobError {}

/// Write a vocab blob to `w`.
///
/// # Panics
///
/// Panics if `vocab.len()` or `merges.len()` exceeds `u32::MAX`, or if
/// any individual token/merge body exceeds `u32::MAX` bytes. Real
/// vocabs are well under either limit; tripping this is almost
/// certainly a caller bug.
pub fn write_vocab_blob<W: io::Write>(
    w: &mut W,
    vocab: &[Vec<u8>],
    merges: &[Vec<u8>],
    specials: SpecialTokenIds,
) -> io::Result<()> {
    let vocab_count: u32 = vocab
        .len()
        .try_into()
        .expect("vocab.len() fits in u32 (real vocabs are ~150k)");
    let merges_count: u32 = merges.len().try_into().expect("merges.len() fits in u32");

    // Header.
    w.write_all(&VOCB_MAGIC)?;
    w.write_all(&VOCB_VERSION.to_le_bytes())?;
    w.write_all(&vocab_count.to_le_bytes())?;
    w.write_all(&merges_count.to_le_bytes())?;
    w.write_all(&specials.bos.to_le_bytes())?;
    w.write_all(&specials.eos.to_le_bytes())?;
    w.write_all(&specials.pad.to_le_bytes())?;
    w.write_all(&specials.unk.to_le_bytes())?;
    w.write_all(&specials.sep.to_le_bytes())?;
    // Reserved.
    w.write_all(&0u32.to_le_bytes())?;

    // Bodies.
    for entry in vocab.iter().chain(merges.iter()) {
        let len: u32 = entry
            .len()
            .try_into()
            .expect("token/merge length fits in u32");
        w.write_all(&len.to_le_bytes())?;
        w.write_all(entry)?;
    }

    Ok(())
}

/// Parse a vocab blob from a byte slice. Returns a typed [`BlobError`]
/// on any malformation; never panics on hostile input.
pub fn read_vocab_blob(data: &[u8]) -> Result<VocabBlob, BlobError> {
    if data.len() < HEADER_SIZE {
        return Err(BlobError::Truncated {
            need: HEADER_SIZE - data.len(),
        });
    }

    let mut magic = [0u8; 4];
    magic.copy_from_slice(&data[0..4]);
    if magic != VOCB_MAGIC {
        return Err(BlobError::BadMagic(magic));
    }

    let version = u32::from_le_bytes(data[4..8].try_into().unwrap());
    if version != VOCB_VERSION {
        return Err(BlobError::BadVersion(version));
    }

    let vocab_count = u32::from_le_bytes(data[8..12].try_into().unwrap());
    let merges_count = u32::from_le_bytes(data[12..16].try_into().unwrap());
    let bos = i32::from_le_bytes(data[16..20].try_into().unwrap());
    let eos = i32::from_le_bytes(data[20..24].try_into().unwrap());
    let pad = i32::from_le_bytes(data[24..28].try_into().unwrap());
    let unk = i32::from_le_bytes(data[28..32].try_into().unwrap());
    let sep = i32::from_le_bytes(data[32..36].try_into().unwrap());
    let reserved = u32::from_le_bytes(data[36..40].try_into().unwrap());
    if reserved != 0 {
        return Err(BlobError::ReservedNonZero(reserved));
    }

    // Cap declared counts against the bytes remaining *before*
    // allocating. Each entry needs at least 5 bytes on the wire
    // (`MIN_ENTRY_SIZE`), so `count > remaining / 5` is impossible.
    let remaining = data.len() - HEADER_SIZE;
    let max_count = (remaining / MIN_ENTRY_SIZE) as u64;
    if vocab_count as u64 > max_count {
        return Err(BlobError::OversizedCount { count: vocab_count });
    }
    // After the vocab body, what's left has to fit `merges_count`
    // entries — but we don't know the exact split until we walk the
    // vocab. Use the same upper bound here; the per-entry walk below
    // will EOF cleanly if the actual layout disagrees.
    if merges_count as u64 > max_count {
        return Err(BlobError::OversizedCount {
            count: merges_count,
        });
    }

    let specials = SpecialTokenIds {
        bos,
        eos,
        pad,
        unk,
        sep,
    };

    let mut cursor = HEADER_SIZE;

    // Cap the *initial* Vec allocation at MAX_PLAUSIBLE_VOCAB. The loop
    // will still push `vocab_count` entries; the `Vec` will grow if the
    // caller genuinely supplied more than 1M tokens.
    let init_cap = (vocab_count as usize).min(MAX_PLAUSIBLE_VOCAB as usize);
    let mut vocab = Vec::with_capacity(init_cap);
    for _ in 0..vocab_count {
        vocab.push(read_entry(data, &mut cursor)?);
    }

    let init_cap = (merges_count as usize).min(MAX_PLAUSIBLE_VOCAB as usize);
    let mut merges = Vec::with_capacity(init_cap);
    for _ in 0..merges_count {
        merges.push(read_entry(data, &mut cursor)?);
    }

    Ok(VocabBlob {
        version,
        specials,
        vocab,
        merges,
    })
}

/// Read one length-prefixed entry from `data` at `*cursor`, advancing
/// the cursor past the body.
fn read_entry(data: &[u8], cursor: &mut usize) -> Result<Vec<u8>, BlobError> {
    let len_end = cursor
        .checked_add(LEN_PREFIX_SIZE)
        .ok_or(BlobError::Truncated {
            need: LEN_PREFIX_SIZE,
        })?;
    if len_end > data.len() {
        return Err(BlobError::Truncated {
            need: LEN_PREFIX_SIZE,
        });
    }
    let len = u32::from_le_bytes(data[*cursor..len_end].try_into().unwrap());
    if len > MAX_PLAUSIBLE_ENTRY_LEN {
        return Err(BlobError::OversizedToken { len });
    }
    *cursor = len_end;

    let body_end = cursor
        .checked_add(len as usize)
        .ok_or(BlobError::Truncated { need: len as usize })?;
    if body_end > data.len() {
        return Err(BlobError::Truncated {
            need: body_end - data.len(),
        });
    }
    let body = data[*cursor..body_end].to_vec();
    *cursor = body_end;
    Ok(body)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_empty_blob() {
        let mut buf = Vec::new();
        write_vocab_blob(&mut buf, &[], &[], SpecialTokenIds::unset()).unwrap();
        assert_eq!(buf.len(), HEADER_SIZE);
        let parsed = read_vocab_blob(&buf).unwrap();
        assert_eq!(parsed.vocab.len(), 0);
        assert_eq!(parsed.merges.len(), 0);
        assert_eq!(parsed.specials, SpecialTokenIds::unset());
    }

    #[test]
    fn round_trips_with_specials() {
        let vocab: Vec<Vec<u8>> = vec![b"<|endoftext|>".to_vec(), b"hi".to_vec()];
        let merges: Vec<Vec<u8>> = vec![b"a b".to_vec()];
        let specials = SpecialTokenIds {
            bos: 0,
            eos: 0,
            pad: -1,
            unk: 1,
            sep: -1,
        };
        let mut buf = Vec::new();
        write_vocab_blob(&mut buf, &vocab, &merges, specials).unwrap();
        let parsed = read_vocab_blob(&buf).unwrap();
        assert_eq!(parsed.vocab, vocab);
        assert_eq!(parsed.merges, merges);
        assert_eq!(parsed.specials, specials);
    }

    #[test]
    fn rejects_bad_magic() {
        let mut buf = vec![0u8; HEADER_SIZE];
        buf[0..4].copy_from_slice(b"NOPE");
        match read_vocab_blob(&buf) {
            Err(BlobError::BadMagic(m)) => assert_eq!(&m, b"NOPE"),
            other => panic!("expected BadMagic, got {other:?}"),
        }
    }

    #[test]
    fn rejects_bad_version() {
        let mut buf = vec![0u8; HEADER_SIZE];
        buf[0..4].copy_from_slice(&VOCB_MAGIC);
        buf[4..8].copy_from_slice(&99u32.to_le_bytes());
        match read_vocab_blob(&buf) {
            Err(BlobError::BadVersion(99)) => {}
            other => panic!("expected BadVersion(99), got {other:?}"),
        }
    }

    #[test]
    fn rejects_short_header() {
        let buf = vec![0u8; HEADER_SIZE - 1];
        match read_vocab_blob(&buf) {
            Err(BlobError::Truncated { .. }) => {}
            other => panic!("expected Truncated, got {other:?}"),
        }
    }

    #[test]
    fn rejects_reserved_nonzero() {
        let mut buf = Vec::new();
        write_vocab_blob(&mut buf, &[], &[], SpecialTokenIds::unset()).unwrap();
        buf[36..40].copy_from_slice(&0xDEADBEEFu32.to_le_bytes());
        match read_vocab_blob(&buf) {
            Err(BlobError::ReservedNonZero(0xDEADBEEF)) => {}
            other => panic!("expected ReservedNonZero, got {other:?}"),
        }
    }
}
