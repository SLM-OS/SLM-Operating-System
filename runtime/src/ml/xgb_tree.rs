//! Shared XGBoost gradient-boosted decision-tree engine.
//!
//! Two payload shapes share the same in-memory `Node` representation
//! but differ in wire-format width:
//!
//! - **Single classifier (`XGB1`, 16 bytes/node, u16 children):** one
//!   root array + one node array. Used by the eviction `XGBoostPolicy`
//!   (binary classifier; predict returns `sigmoid(sum_of_trees)`).
//!   Sized for ≤ 65 535 nodes, which covers every shipping eviction
//!   model.
//! - **Cascade (`XGBC`, 20 bytes/node, u32 children):** N classifier
//!   sections back-to-back. Used by the AI scheduler cascade policy
//!   (#855 — three classifiers for `core / priority / preempt`). The
//!   widened indices are required: the shipping `core_clf` flattens to
//!   ~450 K nodes — well past the u16 ceiling.
//!
//! All parsers bounds-check children against the per-classifier node
//! count, reject non-finite thresholds/leaf values, and cap tree-walk
//! depth to detect cycles in malformed-but-checksum-valid blobs (PR
//! #465 regression).

use alloc::vec;
use alloc::vec::Vec;

pub const PAYLOAD_MAGIC_SINGLE_V1: [u8; 4] = *b"XGB1";
pub const PAYLOAD_MAGIC_CASCADE_V1: [u8; 4] = *b"XGBC";
pub const PAYLOAD_VERSION_V1: u16 = 1;
pub const SINGLE_HEADER_LEN: usize = 16;
pub const CASCADE_HEADER_LEN: usize = 16;
/// XGBC per-classifier section header. Trees + node count widened to
/// u32 to fit the trained scheduler `core_clf` (~450 K nodes).
pub const CLASSIFIER_HEADER_LEN: usize = 16;
/// XGB1 (single-classifier) on-disk node size — u16 children.
pub const NODE_LEN_V1: usize = 16;
/// XGBC (cascade) on-disk node size — u32 children. Required for
/// classifiers that exceed 65 535 nodes.
pub const CASCADE_NODE_LEN_V1: usize = 20;

pub const FLAG_LEAF: u16 = 1;

// Header field offsets — keep these in sync with the writer in
// `slm-os-scheduler-ai/scripts/export_models.py` (see `_wrap_semb`,
// `_build_xgbc_payload`, `_build_classifier_section`).
//
// XGB1 single-classifier header (16 bytes):
//   [0..4]   magic = "XGB1"
//   [4..6]   version (u16)
//   [6..8]   reserved (u16)
//   [8..10]  tree_count (u16)
//   [10..12] node_count (u16)
//   [12..16] reserved (u32)
const OFF_VERSION: usize = 4;
const OFF_SINGLE_RESERVED_U16: usize = 6;
const OFF_SINGLE_TREE_COUNT: usize = 8;
const OFF_SINGLE_NODE_COUNT: usize = 10;
const OFF_SINGLE_RESERVED_U32: usize = 12;

// XGBC cascade header (16 bytes):
//   [0..4]   magic = "XGBC"
//   [4..6]   version (u16)
//   [6..8]   reserved (u16)
//   [8..10]  n_classifiers (u16)
//   [10..12] reserved (u16)
//   [12..16] reserved (u32)
const OFF_CASCADE_RESERVED_U16_LO: usize = 6;
const OFF_CASCADE_N_CLASSIFIERS: usize = 8;
const OFF_CASCADE_RESERVED_U16_HI: usize = 10;
const OFF_CASCADE_RESERVED_U32: usize = 12;

// XGBC per-classifier section header (16 bytes, relative to section start):
//   [0..4]   tree_count (u32)
//   [4..8]   node_count (u32)
//   [8..10]  n_classes (u16)
//   [10..12] reserved (u16)
//   [12..16] reserved (u32)
const OFF_CLF_TREE_COUNT: usize = 0;
const OFF_CLF_NODE_COUNT: usize = 4;
const OFF_CLF_N_CLASSES: usize = 8;
const OFF_CLF_RESERVED_U16: usize = 10;
const OFF_CLF_RESERVED_U32: usize = 12;

const MAX_TREES_SINGLE: usize = 4096;
/// XGB1 per-classifier node ceiling — bounded by the u16 wire field.
const MAX_NODES_SINGLE: usize = 65_535;
const MAX_TREES_CASCADE: usize = 16_384;
/// XGBC per-classifier node ceiling. 1 M covers the shipping
/// scheduler cascade with headroom; bump if a future training run
/// produces a deeper / wider model.
const MAX_NODES_CASCADE: usize = 2_000_000;
const MAX_CLASSIFIERS: usize = 8;
const MAX_LABEL_CLASSES: usize = 64;

/// Tree-walk safety cap. Realistic XGBoost trees stay below ~32 deep;
/// 256 catches malformed cyclic graphs without burning iterations.
pub const MAX_TREE_DEPTH: usize = 256;

#[derive(Clone, Copy, Debug)]
pub struct Node {
    pub feature_idx: u16,
    pub flags: u16,
    /// Child indices held as u32 in memory regardless of wire width.
    /// XGB1 reads u16 and zero-extends; XGBC reads u32 directly.
    pub left_idx: u32,
    pub right_idx: u32,
    pub threshold: f32,
    pub value: f32,
}

/// Single XGBoost classifier (one or more decision trees). For binary
/// classification `label_classes` is empty and the convention is
/// `sigmoid(sum_of_trees)`. For multiclass classification trees are
/// laid out one-per-class-per-round and `label_classes[i]` gives the
/// raw output label for argmax index `i`.
#[derive(Clone, Debug)]
pub struct XgbModel {
    /// Root index per tree. u32 to share with the cascade format —
    /// single-classifier roots are still bounded at u16 by parser.
    roots: Vec<u32>,
    nodes: Vec<Node>,
    label_classes: Vec<i32>,
}

#[derive(Clone, Debug)]
pub struct XgbCascade {
    pub classifiers: Vec<XgbModel>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum XgbError {
    TooShort,
    BadMagic,
    UnsupportedVersion,
    NonZeroReserved,
    BadLength,
    EmptyModel,
    TooManyTrees,
    TooManyNodes,
    TooManyClassifiers,
    TooManyLabels,
    RootOutOfRange,
    ChildOutOfRange,
    InvalidFeatureIndex,
    InvalidThreshold,
    InvalidLeafValue,
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

fn read_i32_le(bytes: &[u8], off: usize) -> i32 {
    i32::from_le_bytes([
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

/// Parse a single-classifier `XGB1` payload. `max_feature_idx` bounds
/// every interior node's feature index.
pub fn parse_single(bytes: &[u8], max_feature_idx: usize) -> Result<XgbModel, XgbError> {
    if bytes.len() < SINGLE_HEADER_LEN {
        return Err(XgbError::TooShort);
    }
    if bytes[0..4] != PAYLOAD_MAGIC_SINGLE_V1 {
        return Err(XgbError::BadMagic);
    }
    let version = read_u16_le(bytes, OFF_VERSION);
    if version != PAYLOAD_VERSION_V1 {
        return Err(XgbError::UnsupportedVersion);
    }
    if read_u16_le(bytes, OFF_SINGLE_RESERVED_U16) != 0
        || read_u32_le(bytes, OFF_SINGLE_RESERVED_U32) != 0
    {
        return Err(XgbError::NonZeroReserved);
    }

    let tree_count = read_u16_le(bytes, OFF_SINGLE_TREE_COUNT) as usize;
    let node_count = read_u16_le(bytes, OFF_SINGLE_NODE_COUNT) as usize;
    if tree_count == 0 || node_count == 0 {
        return Err(XgbError::EmptyModel);
    }
    if tree_count > MAX_TREES_SINGLE {
        return Err(XgbError::TooManyTrees);
    }
    if node_count > MAX_NODES_SINGLE {
        return Err(XgbError::TooManyNodes);
    }

    // `MAX_TREES_SINGLE * 2` and `MAX_NODES_SINGLE * 16` both fit
    // comfortably in usize on every supported platform, but use
    // `checked_*` for consistency with `parse_classifier_section`
    // and to guard against a future cap bump.
    let roots_len = tree_count
        .checked_mul(2)
        .ok_or(XgbError::BadLength)?;
    let nodes_len = node_count
        .checked_mul(NODE_LEN_V1)
        .ok_or(XgbError::BadLength)?;
    let expected_len = SINGLE_HEADER_LEN
        .checked_add(roots_len)
        .and_then(|n| n.checked_add(nodes_len))
        .ok_or(XgbError::BadLength)?;
    if bytes.len() != expected_len {
        return Err(XgbError::BadLength);
    }

    let roots = parse_roots_u16(bytes, SINGLE_HEADER_LEN, tree_count, node_count)?;
    let nodes_off = SINGLE_HEADER_LEN + roots_len;
    let nodes = parse_nodes_u16(bytes, nodes_off, node_count, max_feature_idx)?;
    Ok(XgbModel {
        roots,
        nodes,
        label_classes: Vec::new(),
    })
}

/// Cascade-payload absolute byte ceiling. Each classifier is
/// independently bounded by `MAX_NODES_CASCADE * 20` ≈ 40 MB; with
/// 8 classifiers the worst case is ~320 MB. Cap the entire blob at
/// 128 MB so a malicious header that declares the maximum N + max
/// trees + max nodes can't cause the parser to chase a payload that
/// would never realistically exist on the file system.
const MAX_CASCADE_PAYLOAD_BYTES: usize = 128 * 1024 * 1024;

/// Parse an `XGBC` cascade payload (N classifiers in one blob).
/// `max_feature_idx_per_classifier` is a slice of length N, supplying
/// the per-classifier feature-count bound. The cascade ordering is
/// preserved; the caller decides how each classifier composes (e.g.
/// the scheduler appends each classifier's prediction to the input
/// vector before invoking the next — see #855).
pub fn parse_cascade(
    bytes: &[u8],
    max_feature_idx_per_classifier: &[usize],
) -> Result<XgbCascade, XgbError> {
    if bytes.len() < CASCADE_HEADER_LEN {
        return Err(XgbError::TooShort);
    }
    if bytes.len() > MAX_CASCADE_PAYLOAD_BYTES {
        return Err(XgbError::BadLength);
    }
    if bytes[0..4] != PAYLOAD_MAGIC_CASCADE_V1 {
        return Err(XgbError::BadMagic);
    }
    let version = read_u16_le(bytes, OFF_VERSION);
    if version != PAYLOAD_VERSION_V1 {
        return Err(XgbError::UnsupportedVersion);
    }
    if read_u16_le(bytes, OFF_CASCADE_RESERVED_U16_LO) != 0
        || read_u16_le(bytes, OFF_CASCADE_RESERVED_U16_HI) != 0
        || read_u32_le(bytes, OFF_CASCADE_RESERVED_U32) != 0
    {
        return Err(XgbError::NonZeroReserved);
    }

    let n = read_u16_le(bytes, OFF_CASCADE_N_CLASSIFIERS) as usize;
    if n == 0 {
        return Err(XgbError::EmptyModel);
    }
    if n > MAX_CLASSIFIERS {
        return Err(XgbError::TooManyClassifiers);
    }
    if n != max_feature_idx_per_classifier.len() {
        return Err(XgbError::BadLength);
    }

    let mut classifiers = Vec::with_capacity(n);
    let mut cursor = CASCADE_HEADER_LEN;
    for &max_feature_idx in max_feature_idx_per_classifier.iter() {
        let (model, consumed) = parse_classifier_section(
            bytes,
            cursor,
            max_feature_idx,
        )?;
        cursor = cursor.checked_add(consumed).ok_or(XgbError::BadLength)?;
        classifiers.push(model);
    }
    if cursor != bytes.len() {
        return Err(XgbError::BadLength);
    }
    Ok(XgbCascade { classifiers })
}

fn parse_classifier_section(
    bytes: &[u8],
    start: usize,
    max_feature_idx: usize,
) -> Result<(XgbModel, usize), XgbError> {
    let header_end = start
        .checked_add(CLASSIFIER_HEADER_LEN)
        .ok_or(XgbError::BadLength)?;
    if bytes.len() < header_end {
        return Err(XgbError::TooShort);
    }
    let tree_count = read_u32_le(bytes, start + OFF_CLF_TREE_COUNT) as usize;
    let node_count = read_u32_le(bytes, start + OFF_CLF_NODE_COUNT) as usize;
    let n_classes = read_u16_le(bytes, start + OFF_CLF_N_CLASSES) as usize;
    if read_u16_le(bytes, start + OFF_CLF_RESERVED_U16) != 0
        || read_u32_le(bytes, start + OFF_CLF_RESERVED_U32) != 0
    {
        return Err(XgbError::NonZeroReserved);
    }
    if tree_count == 0 || node_count == 0 {
        return Err(XgbError::EmptyModel);
    }
    if tree_count > MAX_TREES_CASCADE {
        return Err(XgbError::TooManyTrees);
    }
    if node_count > MAX_NODES_CASCADE {
        return Err(XgbError::TooManyNodes);
    }
    if n_classes > MAX_LABEL_CLASSES {
        return Err(XgbError::TooManyLabels);
    }

    let roots_off = header_end;
    let roots_len = tree_count
        .checked_mul(4)
        .ok_or(XgbError::BadLength)?;
    let nodes_off = roots_off
        .checked_add(roots_len)
        .ok_or(XgbError::BadLength)?;
    let nodes_len = node_count
        .checked_mul(CASCADE_NODE_LEN_V1)
        .ok_or(XgbError::BadLength)?;
    let labels_off = nodes_off
        .checked_add(nodes_len)
        .ok_or(XgbError::BadLength)?;
    let labels_len = n_classes
        .checked_mul(4)
        .ok_or(XgbError::BadLength)?;
    let end = labels_off
        .checked_add(labels_len)
        .ok_or(XgbError::BadLength)?;
    if bytes.len() < end {
        return Err(XgbError::BadLength);
    }

    let roots = parse_roots_u32(bytes, roots_off, tree_count, node_count)?;
    let nodes = parse_nodes_u32(bytes, nodes_off, node_count, max_feature_idx)?;
    let mut label_classes = Vec::with_capacity(n_classes);
    for i in 0..n_classes {
        label_classes.push(read_i32_le(bytes, labels_off + i * 4));
    }

    let consumed = end - start;
    Ok((
        XgbModel {
            roots,
            nodes,
            label_classes,
        },
        consumed,
    ))
}

fn parse_roots_u16(
    bytes: &[u8],
    off: usize,
    tree_count: usize,
    node_count: usize,
) -> Result<Vec<u32>, XgbError> {
    let mut roots = vec![0u32; tree_count];
    let mut cursor = off;
    for root in roots.iter_mut() {
        let v = read_u16_le(bytes, cursor) as u32;
        cursor += 2;
        if v as usize >= node_count {
            return Err(XgbError::RootOutOfRange);
        }
        *root = v;
    }
    Ok(roots)
}

fn parse_roots_u32(
    bytes: &[u8],
    off: usize,
    tree_count: usize,
    node_count: usize,
) -> Result<Vec<u32>, XgbError> {
    let mut roots = vec![0u32; tree_count];
    let mut cursor = off;
    for root in roots.iter_mut() {
        let v = read_u32_le(bytes, cursor);
        cursor += 4;
        if v as usize >= node_count {
            return Err(XgbError::RootOutOfRange);
        }
        *root = v;
    }
    Ok(roots)
}

fn parse_nodes_u16(
    bytes: &[u8],
    off: usize,
    node_count: usize,
    max_feature_idx: usize,
) -> Result<Vec<Node>, XgbError> {
    let mut nodes = Vec::with_capacity(node_count);
    let mut cursor = off;
    for _ in 0..node_count {
        let node = Node {
            feature_idx: read_u16_le(bytes, cursor),
            flags: read_u16_le(bytes, cursor + 2),
            left_idx: read_u16_le(bytes, cursor + 4) as u32,
            right_idx: read_u16_le(bytes, cursor + 6) as u32,
            threshold: read_f32_le(bytes, cursor + 8),
            value: read_f32_le(bytes, cursor + 12),
        };
        cursor += NODE_LEN_V1;
        validate_node(&node, node_count, max_feature_idx)?;
        nodes.push(node);
    }
    Ok(nodes)
}

fn parse_nodes_u32(
    bytes: &[u8],
    off: usize,
    node_count: usize,
    max_feature_idx: usize,
) -> Result<Vec<Node>, XgbError> {
    let mut nodes = Vec::with_capacity(node_count);
    let mut cursor = off;
    for _ in 0..node_count {
        let node = Node {
            feature_idx: read_u16_le(bytes, cursor),
            flags: read_u16_le(bytes, cursor + 2),
            left_idx: read_u32_le(bytes, cursor + 4),
            right_idx: read_u32_le(bytes, cursor + 8),
            threshold: read_f32_le(bytes, cursor + 12),
            value: read_f32_le(bytes, cursor + 16),
        };
        cursor += CASCADE_NODE_LEN_V1;
        validate_node(&node, node_count, max_feature_idx)?;
        nodes.push(node);
    }
    Ok(nodes)
}

fn validate_node(
    node: &Node,
    node_count: usize,
    max_feature_idx: usize,
) -> Result<(), XgbError> {
    if (node.flags & FLAG_LEAF) != 0 {
        if !node.value.is_finite() {
            return Err(XgbError::InvalidLeafValue);
        }
    } else {
        if node.feature_idx as usize >= max_feature_idx {
            return Err(XgbError::InvalidFeatureIndex);
        }
        if node.left_idx as usize >= node_count
            || node.right_idx as usize >= node_count
        {
            return Err(XgbError::ChildOutOfRange);
        }
        if !node.threshold.is_finite() {
            return Err(XgbError::InvalidThreshold);
        }
    }
    Ok(())
}

impl XgbModel {
    pub fn n_trees(&self) -> usize {
        self.roots.len()
    }

    pub fn n_nodes(&self) -> usize {
        self.nodes.len()
    }

    pub fn label_classes(&self) -> &[i32] {
        &self.label_classes
    }

    /// Walk one tree from `root_idx`, returning the leaf value (or
    /// 0.0 if depth-capped — see [`MAX_TREE_DEPTH`]).
    ///
    /// `features` is indexed by `node.feature_idx`; the parser
    /// rejects feature indices ≥ the per-classifier `max_feature_idx`
    /// supplied at parse time, so passing a slice ≥ that bound is
    /// safe. A shorter slice would panic on indexing — `eval_tree`
    /// runs in IRQ context where a panic is fatal, so the loop
    /// degrades to the 0.0 fallback rather than panic when a feature
    /// index is out of slice range. Callers are still expected to
    /// pass the right-shape slice; this is defence-in-depth, not a
    /// substitute for shaping the input correctly.
    ///
    /// Indexing `self.nodes[idx]` is unchecked. Parser-built models
    /// are safe by construction: `validate_node` rejects any
    /// `left_idx`/`right_idx >= node_count`, and `parse_roots_*`
    /// rejects any root index that would put us out of range on the
    /// first iteration. Test-only callers using `from_parts_for_test`
    /// must preserve that invariant — keep all root/child indices
    /// strictly less than `nodes.len()` or this will panic.
    pub fn eval_tree(&self, root_idx: usize, features: &[f32]) -> f32 {
        let mut idx = root_idx;
        for _ in 0..MAX_TREE_DEPTH {
            let node = self.nodes[idx];
            if (node.flags & FLAG_LEAF) != 0 {
                return node.value;
            }
            let fi = node.feature_idx as usize;
            if fi >= features.len() {
                return 0.0_f32;
            }
            let f = features[fi];
            idx = if f < node.threshold {
                node.left_idx as usize
            } else {
                node.right_idx as usize
            };
        }
        0.0_f32
    }

    /// Binary-classifier convenience: sums every tree, applies sigmoid.
    /// Used by the eviction `XGBoostPolicy`.
    pub fn predict_sigmoid(&self, features: &[f32]) -> f32 {
        let mut sum = 0.0_f32;
        for &root in &self.roots {
            sum += self.eval_tree(root as usize, features);
        }
        1.0 / (1.0 + libm::expf(-sum))
    }

    /// Multiclass convenience: trees are interleaved one-per-class
    /// per boosting round (XGBoost's standard layout). Returns the
    /// argmax class index (0..n_classes), or 0 on empty.
    ///
    /// Stack-allocates the per-class score buffer (sized to
    /// `MAX_LABEL_CLASSES`) to avoid a heap allocation on every call
    /// — `predict_argmax` is called three times per AI-scheduler
    /// `assign_cpu` (one per cascade classifier), which runs from
    /// IRQ context on hardware-tick paths.
    pub fn predict_argmax(&self, features: &[f32], n_classes: usize) -> usize {
        if n_classes == 0 {
            return 0;
        }
        // Cap to the parse-time MAX_LABEL_CLASSES (parser rejects
        // n_classes > 64 so this clamp is defence-in-depth — won't
        // fire for blobs that came through `parse_cascade`).
        let n = if n_classes > MAX_LABEL_CLASSES {
            MAX_LABEL_CLASSES
        } else {
            n_classes
        };
        let mut scores = [0.0_f32; MAX_LABEL_CLASSES];
        for (i, &root) in self.roots.iter().enumerate() {
            let cls = i % n;
            scores[cls] += self.eval_tree(root as usize, features);
        }
        let mut best = 0usize;
        let mut best_v = scores[0];
        for i in 1..n {
            if scores[i] > best_v {
                best_v = scores[i];
                best = i;
            }
        }
        best
    }

    /// Convenience: argmax + label-class lookup. Returns the raw label
    /// (e.g. an action id) emitted by the trained classifier. Falls
    /// back to the argmax index itself if `label_classes` is empty.
    ///
    /// `n_classes == 2` routes through the binary path: XGBoost's
    /// `binary:logistic` objective stores one tree per boosting round
    /// (all contributing to a single class-1 margin), not two — so the
    /// multiclass `cls = i % n_classes` mapping in `predict_argmax`
    /// would split a single margin's trees alternately into the two
    /// class buckets. Sum all trees, threshold the margin at zero
    /// (`sigmoid(margin) >= 0.5`), then map through `label_classes`.
    /// `XGBClassifier` picks `binary:logistic` automatically for any
    /// 2-class fit, so the trigger matches what the trainer emits
    /// (see #920).
    pub fn predict_label(&self, features: &[f32], n_classes: usize) -> i32 {
        let idx = if n_classes == 2 {
            if self.roots.is_empty() {
                // Unreachable from `parse_*` (rejected as `EmptyModel`),
                // but `from_parts_for_test` can construct it. Without
                // this branch the sum loop below would leave `margin`
                // at the initial 0.0 and the `>= 0.0` threshold would
                // pick class 1, which is the multiclass fallback
                // (lowest-index argmax) inverted — return class 0
                // explicitly to match.
                0usize
            } else {
                let mut margin = 0.0_f32;
                for &root in &self.roots {
                    margin += self.eval_tree(root as usize, features);
                }
                // `NaN >= 0.0` is false in IEEE-754, so a NaN leaf
                // (only reachable via `from_parts_for_test`; the
                // parser rejects via `validate_node`) routes to class
                // 0. Treating it as "negative margin" matches the
                // class-0 fallback the empty-roots branch above
                // returns, so both degenerate inputs agree.
                if margin >= 0.0 { 1usize } else { 0usize }
            }
        } else {
            self.predict_argmax(features, n_classes)
        };
        if idx < self.label_classes.len() {
            self.label_classes[idx]
        } else {
            idx as i32
        }
    }

    /// Test-only constructor. Lets unit tests inject hand-built node
    /// graphs (e.g. the cyclic-tree fallback test inherited from
    /// `runtime_xgboost.rs`) without going through the parser's
    /// child-bounds check.
    #[cfg(test)]
    fn from_parts_for_test(
        roots: Vec<u32>,
        nodes: Vec<Node>,
        label_classes: Vec<i32>,
    ) -> Self {
        Self { roots, nodes, label_classes }
    }
}

#[cfg(any(test, feature = "ai_eviction"))]
pub(crate) fn build_test_single_first_feature_split(threshold: f32, left: f32, right: f32) -> Vec<u8> {
    // Single-classifier XGB1 payload, 1 tree, 3 nodes.
    let mut out = Vec::with_capacity(SINGLE_HEADER_LEN + 2 + 3 * NODE_LEN_V1);
    out.extend_from_slice(&PAYLOAD_MAGIC_SINGLE_V1);
    out.extend_from_slice(&PAYLOAD_VERSION_V1.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes()); // reserved
    out.extend_from_slice(&1u16.to_le_bytes()); // tree_count
    out.extend_from_slice(&3u16.to_le_bytes()); // node_count
    out.extend_from_slice(&0u32.to_le_bytes()); // reserved
    out.extend_from_slice(&0u16.to_le_bytes()); // root[0] = 0

    push_node(&mut out, 0, 0, 1, 2, threshold, 0.0_f32);
    push_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0_f32, left);
    push_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0_f32, right);
    out
}

#[cfg(any(test, feature = "ai_eviction"))]
fn push_node(
    out: &mut Vec<u8>,
    feature_idx: u16,
    flags: u16,
    left: u16,
    right: u16,
    threshold: f32,
    value: f32,
) {
    out.extend_from_slice(&feature_idx.to_le_bytes());
    out.extend_from_slice(&flags.to_le_bytes());
    out.extend_from_slice(&left.to_le_bytes());
    out.extend_from_slice(&right.to_le_bytes());
    out.extend_from_slice(&threshold.to_le_bytes());
    out.extend_from_slice(&value.to_le_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Inherited from `runtime_xgboost.rs` (PR #465). A
    /// malformed-but-checksum-valid blob with a cyclic tree (A→A) must
    /// not hang the kernel. `eval_tree` returns the 0.0 fallback after
    /// `MAX_TREE_DEPTH` iterations regardless of the input.
    #[test]
    fn eval_tree_breaks_cycle_with_fallback() {
        let leaf = Node {
            feature_idx: 0,
            flags: FLAG_LEAF,
            left_idx: 0,
            right_idx: 0,
            threshold: 0.0,
            value: 1.0,
        };
        let cyclic = Node {
            feature_idx: 0,
            flags: 0,
            left_idx: 0,
            right_idx: 0,
            threshold: 0.0,
            value: 0.0,
        };

        let leaf_only = XgbModel::from_parts_for_test(
            alloc::vec![0u32],
            alloc::vec![leaf],
            Vec::new(),
        );
        let features = [0.0_f32; 32];
        assert_eq!(leaf_only.eval_tree(0, &features), 1.0);

        let cyc = XgbModel::from_parts_for_test(
            alloc::vec![0u32],
            alloc::vec![cyclic],
            Vec::new(),
        );
        assert_eq!(cyc.eval_tree(0, &features), 0.0);
    }

    #[test]
    fn parse_single_round_trips_first_feature_split() {
        let bytes = build_test_single_first_feature_split(0.5, -1.0, 1.0);
        let model = parse_single(&bytes, 27).expect("parse");
        assert_eq!(model.n_trees(), 1);
        assert_eq!(model.n_nodes(), 3);

        let mut features = [0.0_f32; 27];
        features[0] = 0.0;
        // Sum is -1.0; sigmoid(-1) ~= 0.2689.
        let p_low = model.predict_sigmoid(&features);
        assert!((p_low - 0.26894143).abs() < 1e-5);

        features[0] = 1.0;
        let p_high = model.predict_sigmoid(&features);
        assert!((p_high - 0.7310586).abs() < 1e-5);
    }

    #[test]
    fn parse_single_rejects_bad_magic() {
        let mut bytes = build_test_single_first_feature_split(0.5, 0.0, 0.0);
        bytes[0] = b'X';
        bytes[1] = b'X';
        bytes[2] = b'X';
        bytes[3] = b'X';
        assert!(matches!(parse_single(&bytes, 27), Err(XgbError::BadMagic)));
    }

    #[test]
    fn parse_single_rejects_oob_feature() {
        let bytes = build_test_single_first_feature_split(0.5, 0.0, 0.0);
        // Tree splits on feature 0; with max_feature_idx=0 it must reject.
        assert!(matches!(
            parse_single(&bytes, 0),
            Err(XgbError::InvalidFeatureIndex)
        ));
    }

    fn build_cascade_two_classifiers() -> Vec<u8> {
        let mut out = Vec::new();
        // Cascade header (16 bytes).
        out.extend_from_slice(&PAYLOAD_MAGIC_CASCADE_V1);
        out.extend_from_slice(&PAYLOAD_VERSION_V1.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes()); // reserved
        out.extend_from_slice(&2u16.to_le_bytes()); // n_classifiers
        out.extend_from_slice(&0u16.to_le_bytes()); // reserved
        out.extend_from_slice(&0u32.to_le_bytes()); // reserved

        // Classifier 0: 2 classes, 2 trees, 4 nodes (2 used, 2 padding).
        // Tree i feeds class (i % 2). Each tree is a leaf — its score
        // is the leaf value; argmax produces a deterministic class.
        out.extend_from_slice(&2u32.to_le_bytes()); // n_trees
        out.extend_from_slice(&4u32.to_le_bytes()); // n_nodes
        out.extend_from_slice(&2u16.to_le_bytes()); // n_classes
        out.extend_from_slice(&0u16.to_le_bytes()); // reserved (u16)
        out.extend_from_slice(&0u32.to_le_bytes()); // reserved (u32)
        out.extend_from_slice(&0u32.to_le_bytes()); // root 0
        out.extend_from_slice(&1u32.to_le_bytes()); // root 1
        push_cascade_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0, 0.5); // class 0
        push_cascade_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0, 0.1); // class 1
        push_cascade_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0, 0.0); // unused
        push_cascade_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0, 0.0); // unused
        out.extend_from_slice(&7i32.to_le_bytes());
        out.extend_from_slice(&9i32.to_le_bytes());

        // Classifier 1: 2 classes, 2 trees, 2 nodes.
        out.extend_from_slice(&2u32.to_le_bytes());
        out.extend_from_slice(&2u32.to_le_bytes());
        out.extend_from_slice(&2u16.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&1u32.to_le_bytes());
        push_cascade_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0, 0.0); // class 0
        push_cascade_node(&mut out, 0, FLAG_LEAF, 0, 0, 0.0, 1.0); // class 1
        out.extend_from_slice(&100i32.to_le_bytes());
        out.extend_from_slice(&200i32.to_le_bytes());
        out
    }

    fn push_cascade_node(
        out: &mut Vec<u8>,
        feature_idx: u16,
        flags: u16,
        left: u32,
        right: u32,
        threshold: f32,
        value: f32,
    ) {
        out.extend_from_slice(&feature_idx.to_le_bytes());
        out.extend_from_slice(&flags.to_le_bytes());
        out.extend_from_slice(&left.to_le_bytes());
        out.extend_from_slice(&right.to_le_bytes());
        out.extend_from_slice(&threshold.to_le_bytes());
        out.extend_from_slice(&value.to_le_bytes());
    }

    #[test]
    fn parse_cascade_round_trips_two_classifiers() {
        // The cascade builder writes each classifier with n_classes == 2.
        // Per #920, that routes through the binary-logistic margin path:
        // sum all tree leaves, threshold at zero, then label_classes
        // lookup. The previous multiclass-style expectation was wrong.
        let bytes = build_cascade_two_classifiers();
        let cascade = parse_cascade(&bytes, &[1, 1]).expect("parse cascade");
        assert_eq!(cascade.classifiers.len(), 2);

        let features = [0.0_f32; 1];
        // Classifier 0: margin = 0.5 + 0.1 = 0.6 >= 0 → class 1 → label 9.
        assert_eq!(
            cascade.classifiers[0].predict_label(&features, 2),
            9
        );
        // Classifier 1: margin = 0.0 + 1.0 = 1.0 >= 0 → class 1 → label 200.
        assert_eq!(
            cascade.classifiers[1].predict_label(&features, 2),
            200
        );
    }

    #[test]
    fn parse_cascade_rejects_classifier_count_mismatch() {
        let bytes = build_cascade_two_classifiers();
        assert!(matches!(
            parse_cascade(&bytes, &[1]),
            Err(XgbError::BadLength)
        ));
    }

    /// XGBoost `binary:logistic` stores ONE tree per boosting round
    /// (all contribute to a single class-1 margin). `predict_label`
    /// with `n_classes == 2` must sum every tree's leaf value, threshold
    /// the margin at zero, and look up `label_classes[predicted]` —
    /// NOT route through `predict_argmax`'s `i % n_classes` split,
    /// which would alternate trees into separate buckets and produce
    /// noise. Regression for #920.
    #[test]
    fn predict_label_binary_uses_margin_threshold() {
        // 3 trees, each a single-leaf root contributing +0.4. Sum = +1.2 >= 0
        // → class 1 → label_classes[1] = 99.
        let leaf = |v: f32| Node {
            feature_idx: 0,
            flags: FLAG_LEAF,
            left_idx: 0,
            right_idx: 0,
            threshold: 0.0,
            value: v,
        };
        let model_pos = XgbModel::from_parts_for_test(
            alloc::vec![0u32, 1u32, 2u32],
            alloc::vec![leaf(0.4), leaf(0.4), leaf(0.4)],
            alloc::vec![7i32, 99i32],
        );
        let features = [0.0_f32; 1];
        assert_eq!(model_pos.predict_label(&features, 2), 99);

        // Same shape, leaves negative → margin -1.2 < 0 → class 0 → label 7.
        let model_neg = XgbModel::from_parts_for_test(
            alloc::vec![0u32, 1u32, 2u32],
            alloc::vec![leaf(-0.4), leaf(-0.4), leaf(-0.4)],
            alloc::vec![7i32, 99i32],
        );
        assert_eq!(model_neg.predict_label(&features, 2), 7);

        // Mixed signs where `predict_argmax` (multiclass-style i % 2)
        // and the binary margin disagree:
        //   - Trees 0 (cls 0 by i%2), 2 (cls 0): -0.6, -0.6 → scores[0] = -1.2
        //   - Tree 1 (cls 1 by i%2): +0.4 → scores[1] = +0.4
        //   - predict_argmax → class 1 → label 99
        //   - actual binary margin = -0.6 + 0.4 - 0.6 = -0.8 < 0 → class 0 → label 7
        // predict_label MUST follow the binary margin, not the argmax.
        let model_mixed = XgbModel::from_parts_for_test(
            alloc::vec![0u32, 1u32, 2u32],
            alloc::vec![leaf(-0.6), leaf(0.4), leaf(-0.6)],
            alloc::vec![7i32, 99i32],
        );
        assert_eq!(model_mixed.predict_label(&features, 2), 7);
        // Sanity check that predict_argmax does NOT match — confirms
        // the test is actually exercising the binary-specific path.
        assert_eq!(model_mixed.predict_argmax(&features, 2), 1);
    }
}
