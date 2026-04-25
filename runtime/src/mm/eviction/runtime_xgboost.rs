//! Runtime-loaded XGBoost payload support for eviction.
//!
//! The outer eviction blob header/versioning lives in `blob.rs`.
//! This module defines a compact tree-ensemble payload format that can
//! be staged and activated from the generic blob store.

use alloc::vec;
use alloc::vec::Vec;

use super::policy::BlockFeatures;

pub const PAYLOAD_MAGIC: [u8; 4] = *b"XGB1";
pub const PAYLOAD_VERSION_V1: u16 = 1;
pub const PAYLOAD_HEADER_LEN: usize = 16;
pub const NODE_LEN_V1: usize = 16;
pub const PAYLOAD_LEN_V1: usize = PAYLOAD_HEADER_LEN + 2 + 3 * NODE_LEN_V1;

#[derive(Clone)]
pub struct RuntimeXGBoostModel {
    roots: Vec<u16>,
    nodes: Vec<Node>,
}

#[derive(Clone, Copy)]
struct Node {
    feature_idx: u16,
    flags: u16,
    left_idx: u16,
    right_idx: u16,
    threshold: f32,
    value: f32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RuntimeXGBoostError {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    NonZeroReserved,
    BadLength,
    EmptyModel,
    RootOutOfRange,
    ChildOutOfRange,
    InvalidFeatureIndex,
    InvalidThreshold,
    InvalidLeafValue,
}

const FLAG_LEAF: u16 = 1;
const FEATURE_COUNT: usize = 27;

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

fn read_f32_le(bytes: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([
        bytes[off],
        bytes[off + 1],
        bytes[off + 2],
        bytes[off + 3],
    ])
}

pub fn parse_payload(bytes: &[u8]) -> Result<RuntimeXGBoostModel, RuntimeXGBoostError> {
    if bytes.len() < PAYLOAD_HEADER_LEN {
        return Err(RuntimeXGBoostError::TooShort);
    }
    if bytes[0..4] != PAYLOAD_MAGIC {
        return Err(RuntimeXGBoostError::BadMagic);
    }
    let version = read_u16_le(bytes, 4);
    if version != PAYLOAD_VERSION_V1 {
        return Err(RuntimeXGBoostError::UnsupportedVersion);
    }
    if read_u16_le(bytes, 6) != 0 || read_u32_le(bytes, 12) != 0 {
        return Err(RuntimeXGBoostError::NonZeroReserved);
    }

    let tree_count = read_u16_le(bytes, 8) as usize;
    let node_count = read_u16_le(bytes, 10) as usize;
    if tree_count == 0 || node_count == 0 {
        return Err(RuntimeXGBoostError::EmptyModel);
    }

    let roots_len = tree_count * 2;
    let nodes_len = node_count * NODE_LEN_V1;
    let expected_len = PAYLOAD_HEADER_LEN + roots_len + nodes_len;
    if bytes.len() != expected_len {
        return Err(RuntimeXGBoostError::BadLength);
    }

    let mut roots = vec![0u16; tree_count];
    let mut cursor = PAYLOAD_HEADER_LEN;
    for root in roots.iter_mut() {
        *root = read_u16_le(bytes, cursor);
        cursor += 2;
        if *root as usize >= node_count {
            return Err(RuntimeXGBoostError::RootOutOfRange);
        }
    }

    let mut nodes = Vec::with_capacity(node_count);
    for _ in 0..node_count {
        let node = Node {
            feature_idx: read_u16_le(bytes, cursor),
            flags: read_u16_le(bytes, cursor + 2),
            left_idx: read_u16_le(bytes, cursor + 4),
            right_idx: read_u16_le(bytes, cursor + 6),
            threshold: read_f32_le(bytes, cursor + 8),
            value: read_f32_le(bytes, cursor + 12),
        };
        cursor += NODE_LEN_V1;

        if (node.flags & FLAG_LEAF) != 0 {
            if !node.value.is_finite() {
                return Err(RuntimeXGBoostError::InvalidLeafValue);
            }
        } else {
            if node.feature_idx as usize >= FEATURE_COUNT {
                return Err(RuntimeXGBoostError::InvalidFeatureIndex);
            }
            if node.left_idx as usize >= node_count || node.right_idx as usize >= node_count {
                return Err(RuntimeXGBoostError::ChildOutOfRange);
            }
            if !node.threshold.is_finite() {
                return Err(RuntimeXGBoostError::InvalidThreshold);
            }
        }

        nodes.push(node);
    }

    Ok(RuntimeXGBoostModel { roots, nodes })
}

impl RuntimeXGBoostModel {
    pub fn predict(&self, features: &BlockFeatures) -> f32 {
        let mut sum = 0.0_f32;
        for &root in &self.roots {
            sum += self.eval_tree(root as usize, features);
        }
        1.0 / (1.0 + libm::expf(-sum))
    }

    fn eval_tree(&self, mut idx: usize, features: &BlockFeatures) -> f32 {
        loop {
            let node = self.nodes[idx];
            if (node.flags & FLAG_LEAF) != 0 {
                return node.value;
            }
            idx = if features[node.feature_idx as usize] < node.threshold {
                node.left_idx as usize
            } else {
                node.right_idx as usize
            };
        }
    }
}

#[cfg(feature = "ai_eviction")]
pub fn build_test_payload_first_feature_split(threshold: f32, left: f32, right: f32) -> Vec<u8> {
    let mut out = Vec::with_capacity(PAYLOAD_LEN_V1);
    out.extend_from_slice(&PAYLOAD_MAGIC);
    out.extend_from_slice(&PAYLOAD_VERSION_V1.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&3u16.to_le_bytes());
    out.extend_from_slice(&0u32.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());

    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&2u16.to_le_bytes());
    out.extend_from_slice(&threshold.to_le_bytes());
    out.extend_from_slice(&0.0_f32.to_le_bytes());

    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&FLAG_LEAF.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&0.0_f32.to_le_bytes());
    out.extend_from_slice(&left.to_le_bytes());

    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&FLAG_LEAF.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out.extend_from_slice(&0.0_f32.to_le_bytes());
    out.extend_from_slice(&right.to_le_bytes());

    out
}
