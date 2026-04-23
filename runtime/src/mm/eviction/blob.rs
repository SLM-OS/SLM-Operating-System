//! Versioned runtime blob format for eviction-model payloads.
//!
//! This is the first step toward dynamic eviction-model loading:
//! define a stable binary header, validate it strictly, and return a
//! typed view over the payload bytes. Activation/staging comes later.

use alloc::vec::Vec;

pub const BLOB_MAGIC: [u8; 4] = *b"SEMB";
pub const BLOB_VERSION_V1: u16 = 1;
pub const HEADER_LEN: usize = 24;
pub const EVICTION_FEATURE_SCHEMA_V1: u16 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u16)]
pub enum BlobKind {
    XGBoost = 1,
    Mlp = 2,
    CacheusConfig = 3,
}

impl BlobKind {
    fn from_u16(v: u16) -> Option<Self> {
        match v {
            1 => Some(Self::XGBoost),
            2 => Some(Self::Mlp),
            3 => Some(Self::CacheusConfig),
            _ => None,
        }
    }

    pub fn as_str(&self) -> &'static str {
        match self {
            Self::XGBoost => "xgboost",
            Self::Mlp => "mlp",
            Self::CacheusConfig => "cacheus_config",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlobHeader {
    pub version: u16,
    pub kind: BlobKind,
    pub feature_schema_version: u16,
    pub payload_len: u32,
    pub checksum: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ParsedBlob {
    pub header: BlobHeader,
    pub payload: Vec<u8>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlobError {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    UnknownKind,
    UnsupportedFeatureSchema,
    LengthMismatch,
    ChecksumMismatch,
}

fn read_u16_le(bytes: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([bytes[off], bytes[off + 1]])
}

fn read_u32_le(bytes: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([
        bytes[off],
        bytes[off + 1],
        bytes[off + 2],
        bytes[off + 3],
    ])
}

/// FNV-1a 32-bit checksum.
///
/// Chosen because it is tiny, deterministic, and good enough for
/// accidental corruption detection in this first loader stage.
pub fn checksum32(bytes: &[u8]) -> u32 {
    let mut hash: u32 = 0x811C9DC5;
    for b in bytes {
        hash ^= *b as u32;
        hash = hash.wrapping_mul(0x01000193);
    }
    hash
}

pub fn parse_blob(bytes: &[u8]) -> Result<ParsedBlob, BlobError> {
    if bytes.len() < HEADER_LEN {
        return Err(BlobError::TooShort);
    }
    if bytes[0..4] != BLOB_MAGIC {
        return Err(BlobError::BadMagic);
    }

    let version = read_u16_le(bytes, 4);
    if version != BLOB_VERSION_V1 {
        return Err(BlobError::UnsupportedVersion);
    }

    let kind = BlobKind::from_u16(read_u16_le(bytes, 6))
        .ok_or(BlobError::UnknownKind)?;

    let feature_schema_version = read_u16_le(bytes, 8);
    if feature_schema_version != EVICTION_FEATURE_SCHEMA_V1 {
        return Err(BlobError::UnsupportedFeatureSchema);
    }

    let payload_len = read_u32_le(bytes, 12);
    let checksum = read_u32_le(bytes, 16);
    let total_len = HEADER_LEN
        .checked_add(payload_len as usize)
        .ok_or(BlobError::LengthMismatch)?;

    if bytes.len() != total_len {
        return Err(BlobError::LengthMismatch);
    }

    let payload = &bytes[HEADER_LEN..];
    if checksum32(payload) != checksum {
        return Err(BlobError::ChecksumMismatch);
    }

    Ok(ParsedBlob {
        header: BlobHeader {
            version,
            kind,
            feature_schema_version,
            payload_len,
            checksum,
        },
        payload: payload.to_vec(),
    })
}

#[cfg(feature = "ai_eviction")]
pub fn build_test_blob(kind: BlobKind, payload: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(HEADER_LEN + payload.len());
    out.extend_from_slice(&BLOB_MAGIC);
    out.extend_from_slice(&BLOB_VERSION_V1.to_le_bytes());
    out.extend_from_slice(&(kind as u16).to_le_bytes());
    out.extend_from_slice(&EVICTION_FEATURE_SCHEMA_V1.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes()); // reserved
    out.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    out.extend_from_slice(&checksum32(payload).to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes()); // reserved
    out.extend_from_slice(payload);
    out
}
