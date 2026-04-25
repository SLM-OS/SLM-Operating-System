//! Runtime-loaded MLP payload support for eviction.
//!
//! The outer eviction blob header/versioning lives in `blob.rs`.
//! This module defines the MLP-specific payload format and evaluator.

use alloc::vec;
use alloc::vec::Vec;

use super::policy::BlockFeatures;

pub const PAYLOAD_MAGIC: [u8; 4] = *b"MLP1";
pub const PAYLOAD_VERSION_V1: u16 = 1;
pub const PAYLOAD_HEADER_LEN: usize = 8;

pub const L1_IN: usize = 27;
pub const L1_OUT: usize = 64;
pub const L2_OUT: usize = 32;
pub const L3_OUT: usize = 16;
pub const OUT: usize = 1;

const W_L1_LEN: usize = L1_OUT * L1_IN;
const B_L1_LEN: usize = L1_OUT;
const W_L2_LEN: usize = L2_OUT * L1_OUT;
const B_L2_LEN: usize = L2_OUT;
const W_L3_LEN: usize = L3_OUT * L2_OUT;
const B_L3_LEN: usize = L3_OUT;
const W_OUT_LEN: usize = OUT * L3_OUT;
const B_OUT_LEN: usize = OUT;
const FLOAT_COUNT: usize =
    W_L1_LEN + B_L1_LEN + W_L2_LEN + B_L2_LEN + W_L3_LEN + B_L3_LEN + W_OUT_LEN + B_OUT_LEN;
pub const PAYLOAD_LEN_V1: usize = PAYLOAD_HEADER_LEN + FLOAT_COUNT * 4;

#[derive(Clone)]
pub struct RuntimeMlpModel {
    w_l1: Vec<f32>,
    b_l1: Vec<f32>,
    w_l2: Vec<f32>,
    b_l2: Vec<f32>,
    w_l3: Vec<f32>,
    b_l3: Vec<f32>,
    w_out: Vec<f32>,
    b_out: Vec<f32>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimeMlpError {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    BadLength,
}

fn read_u16_le(bytes: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([bytes[off], bytes[off + 1]])
}

fn read_f32_le(bytes: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]])
}

fn parse_vec(bytes: &[u8], cursor: &mut usize, count: usize) -> Vec<f32> {
    let mut out = vec![0.0_f32; count];
    for slot in out.iter_mut() {
        *slot = read_f32_le(bytes, *cursor);
        *cursor += 4;
    }
    out
}

pub fn parse_payload(bytes: &[u8]) -> Result<RuntimeMlpModel, RuntimeMlpError> {
    if bytes.len() < PAYLOAD_HEADER_LEN {
        return Err(RuntimeMlpError::TooShort);
    }
    if bytes[0..4] != PAYLOAD_MAGIC {
        return Err(RuntimeMlpError::BadMagic);
    }
    let version = read_u16_le(bytes, 4);
    if version != PAYLOAD_VERSION_V1 {
        return Err(RuntimeMlpError::UnsupportedVersion);
    }
    if bytes.len() != PAYLOAD_LEN_V1 {
        return Err(RuntimeMlpError::BadLength);
    }

    let mut cursor = PAYLOAD_HEADER_LEN;
    let model = RuntimeMlpModel {
        w_l1: parse_vec(bytes, &mut cursor, W_L1_LEN),
        b_l1: parse_vec(bytes, &mut cursor, B_L1_LEN),
        w_l2: parse_vec(bytes, &mut cursor, W_L2_LEN),
        b_l2: parse_vec(bytes, &mut cursor, B_L2_LEN),
        w_l3: parse_vec(bytes, &mut cursor, W_L3_LEN),
        b_l3: parse_vec(bytes, &mut cursor, B_L3_LEN),
        w_out: parse_vec(bytes, &mut cursor, W_OUT_LEN),
        b_out: parse_vec(bytes, &mut cursor, B_OUT_LEN),
    };
    debug_assert_eq!(cursor, bytes.len());
    Ok(model)
}

impl RuntimeMlpModel {
    pub fn predict(&self, features: &BlockFeatures) -> f32 {
        let mut h1 = [0.0_f32; L1_OUT];
        for (i, out) in h1.iter_mut().enumerate() {
            let row = &self.w_l1[i * L1_IN..(i + 1) * L1_IN];
            let mut acc = 0.0_f32;
            for j in 0..L1_IN {
                acc += features[j] * row[j];
            }
            *out = (acc + self.b_l1[i]).max(0.0);
        }

        let mut h2 = [0.0_f32; L2_OUT];
        for (i, out) in h2.iter_mut().enumerate() {
            let row = &self.w_l2[i * L1_OUT..(i + 1) * L1_OUT];
            let mut acc = 0.0_f32;
            for j in 0..L1_OUT {
                acc += h1[j] * row[j];
            }
            *out = (acc + self.b_l2[i]).max(0.0);
        }

        let mut h3 = [0.0_f32; L3_OUT];
        for (i, out) in h3.iter_mut().enumerate() {
            let row = &self.w_l3[i * L2_OUT..(i + 1) * L2_OUT];
            let mut acc = 0.0_f32;
            for j in 0..L2_OUT {
                acc += h2[j] * row[j];
            }
            *out = (acc + self.b_l3[i]).max(0.0);
        }

        let mut out = self.b_out[0];
        let row = &self.w_out[0..L3_OUT];
        for j in 0..L3_OUT {
            out += h3[j] * row[j];
        }
        1.0 / (1.0 + libm::expf(-out))
    }
}

#[cfg(feature = "ai_eviction")]
pub fn build_test_payload_first_feature_model(out_weight: f32) -> Vec<u8> {
    let mut floats = vec![0.0_f32; FLOAT_COUNT];
    let mut idx = 0usize;
    floats[idx] = 1.0; // W_L1[0][0]
    idx += W_L1_LEN;
    idx += B_L1_LEN;
    floats[idx] = 1.0; // W_L2[0][0]
    idx += W_L2_LEN;
    idx += B_L2_LEN;
    floats[idx] = 1.0; // W_L3[0][0]
    idx += W_L3_LEN;
    idx += B_L3_LEN;
    floats[idx] = out_weight; // W_OUT[0][0]

    let mut out = Vec::with_capacity(PAYLOAD_LEN_V1);
    out.extend_from_slice(&PAYLOAD_MAGIC);
    out.extend_from_slice(&PAYLOAD_VERSION_V1.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    for f in floats {
        out.extend_from_slice(&f.to_le_bytes());
    }
    out
}
