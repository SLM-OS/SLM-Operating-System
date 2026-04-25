//! Runtime-loaded CACHEUS configuration payload support.

pub const PAYLOAD_MAGIC: [u8; 4] = *b"CCFG";
pub const PAYLOAD_VERSION_V1: u16 = 1;
pub const PAYLOAD_LEN_V1: usize = 24;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ExpertPool {
    MlOnly,
    All5,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RuntimeCacheusConfig {
    pub expert_pool: ExpertPool,
    pub learning_rate: f32,
    pub window_size: usize,
    pub min_weight: f32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimeCacheusError {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    BadLength,
    UnknownExpertPool,
    InvalidWindow,
    InvalidLearningRate,
    InvalidMinWeight,
}

fn read_u16_le(bytes: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([bytes[off], bytes[off + 1]])
}

fn read_u32_le(bytes: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]])
}

fn read_f32_le(bytes: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]])
}

pub fn parse_payload(bytes: &[u8]) -> Result<RuntimeCacheusConfig, RuntimeCacheusError> {
    if bytes.len() < PAYLOAD_LEN_V1 {
        return Err(RuntimeCacheusError::TooShort);
    }
    if bytes[0..4] != PAYLOAD_MAGIC {
        return Err(RuntimeCacheusError::BadMagic);
    }
    let version = read_u16_le(bytes, 4);
    if version != PAYLOAD_VERSION_V1 {
        return Err(RuntimeCacheusError::UnsupportedVersion);
    }
    if bytes.len() != PAYLOAD_LEN_V1 {
        return Err(RuntimeCacheusError::BadLength);
    }

    let expert_pool = match read_u32_le(bytes, 8) {
        0 => ExpertPool::MlOnly,
        1 => ExpertPool::All5,
        _ => return Err(RuntimeCacheusError::UnknownExpertPool),
    };
    let learning_rate = read_f32_le(bytes, 12);
    let window_size = read_u32_le(bytes, 16) as usize;
    let min_weight = read_f32_le(bytes, 20);

    if !learning_rate.is_finite() || learning_rate < 0.0 || learning_rate > 1.0 {
        return Err(RuntimeCacheusError::InvalidLearningRate);
    }
    if window_size == 0 {
        return Err(RuntimeCacheusError::InvalidWindow);
    }
    if !min_weight.is_finite() || min_weight < 0.0 || min_weight > 1.0 {
        return Err(RuntimeCacheusError::InvalidMinWeight);
    }

    Ok(RuntimeCacheusConfig {
        expert_pool,
        learning_rate,
        window_size,
        min_weight,
    })
}

#[cfg(feature = "ai_eviction")]
pub fn build_test_payload(
    expert_pool: ExpertPool,
    learning_rate: f32,
    window_size: u32,
    min_weight: f32,
) -> alloc::vec::Vec<u8> {
    let mut out = alloc::vec::Vec::with_capacity(PAYLOAD_LEN_V1);
    out.extend_from_slice(&PAYLOAD_MAGIC);
    out.extend_from_slice(&PAYLOAD_VERSION_V1.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    let pool_id = match expert_pool {
        ExpertPool::MlOnly => 0u32,
        ExpertPool::All5 => 1u32,
    };
    out.extend_from_slice(&pool_id.to_le_bytes());
    out.extend_from_slice(&learning_rate.to_le_bytes());
    out.extend_from_slice(&window_size.to_le_bytes());
    out.extend_from_slice(&min_weight.to_le_bytes());
    out
}
