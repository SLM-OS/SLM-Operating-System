# Runtime Blob Formats

This document specifies the current first-cut runtime blob formats used
by dynamic eviction and scheduler model loading.

These formats are intentionally narrow:

- policy code stays compiled into SLM-OS
- blobs carry model parameters or tuning/config data only
- files are validated before stage/activate

The current implementation is versioned and usable, but still first-cut.
This spec exists so future changes can be deliberate instead of inferred
from scattered builders and parser code.

## Compatibility Policy

The current runtime blob policy is intentionally conservative:

- unknown outer-wrapper versions are rejected
- unknown inner payload versions are rejected
- non-zero reserved fields are rejected
- there is no negotiated forward-compatibility path yet
- compatibility is currently defined as:
  - exact outer-wrapper version match
  - exact subsystem schema version match
  - exact inner payload-version match
  - exact compiled feature/action contract match where that payload kind
    carries feature/action metadata

In other words, the current system prefers fail-closed validation over
best-effort compatibility. Future generalization work can add a richer
compatibility policy, but the current milestone intentionally does not
attempt partial decoding or field-level downgrade behavior.

## Common Outer Wrapper

All current runtime blobs use the same 24-byte outer wrapper:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `SEMB` |
| `4` | 2 | blob version | currently `1` |
| `6` | 2 | kind id | eviction or scheduler kind |
| `8` | 2 | schema version | currently `1` |
| `10` | 2 | reserved | must be `0` |
| `12` | 4 | payload length | little-endian bytes |
| `16` | 4 | checksum | FNV-1a 32-bit over payload bytes |
| `20` | 4 | reserved | must be `0` |

Validation rules:

- magic must be exactly `SEMB`
- version must be the current supported version
- schema version must match the expected subsystem schema
- payload length must exactly match the file length minus 24
- checksum must match the payload bytes
- all reserved bytes must be zero

Current kind ids:

- Eviction:
  - `1` = `xgboost`
  - `2` = `mlp`
  - `3` = `cacheus_config`
- Scheduler:
  - `0x1001` = `sched_mlp`
  - `0x1002` = `sched_ppo`
  - `0x1003` = `sched_config`
  - `0x1004` = `sched_thresholds`
  - `0x1005` = `sched_rebalance`
  - `0x1006` = `sched_xgboost` (cascade — see below)

## Eviction Payloads

### `xgboost`

Inner magic: `XGB1`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `XGB1` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | reserved | must be `0` |
| `8` | 2 | tree count | must be non-zero |
| `10` | 2 | node count | must be non-zero |
| `12` | 4 | reserved | must be `0` |

Then:

- `tree_count` root indices as `u16`
- `node_count` nodes, each 16 bytes

Node layout:

| Offset | Size | Field |
|---|---:|---|
| `0` | 2 | feature index |
| `2` | 2 | flags |
| `4` | 2 | left child index |
| `6` | 2 | right child index |
| `8` | 4 | threshold (`f32`) |
| `12` | 4 | value (`f32`) |

Current flag bits:

- bit `0` = leaf

Validation rules:

- root indices must be in range
- non-leaf feature indices must be `< 27`
- non-leaf child indices must be in range
- thresholds must be finite
- leaf values must be finite

### `mlp`

Inner magic: `MLP1`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `MLP1` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | reserved | must be `0` |

Payload body is a packed float32 tensor list for the fixed network:

- input: `27`
- hidden layers: `64`, `32`, `16`
- output: `1`

Current tensor order:

1. `W_L1` shape `64 x 27`
2. `B_L1` shape `64`
3. `W_L2` shape `32 x 64`
4. `B_L2` shape `32`
5. `W_L3` shape `16 x 32`
6. `B_L3` shape `16`
7. `W_OUT` shape `1 x 16`
8. `B_OUT` shape `1`

Validation rules:

- exact payload length match only
- no shape metadata is carried; shape is implied by version
- reserved bytes must be zero

Current compiled coupling:

- input feature count is fixed at `27`
- hidden/output shapes are implied entirely by payload version `1`
- a future format revision should move this toward an explicit tensor
  table instead of hard-coded tensor order and shape inference

### `cacheus_config`

Inner magic: `CCFG`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `CCFG` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | reserved | must be `0` |
| `8` | 4 | expert pool id | `0=ml_only`, `1=all_5` |
| `12` | 4 | learning rate | `f32` |
| `16` | 4 | window size | `u32` |
| `20` | 4 | min weight | `f32` |

Validation rules:

- expert pool id must be known
- learning rate must be finite and in `[0, 1]`
- window size must be non-zero
- min weight must be finite and in `[0, 1]`

## Scheduler Payloads

Scheduler blobs use the same outer wrapper but different kind ids.

### `sched_mlp` and `sched_ppo`

Inner magic: `SML1`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `SML1` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | feature version | currently `1` |
| `8` | 2 | action-space version | currently `1` |
| `10` | 2 | action count | must match `AI_SCHED_N_ACTIONS` |

Body is a packed float32 tensor list for the fixed scheduler dense net:

1. `W0`
2. `B0`
3. `W1`
4. `B1`
5. `W2`
6. `B2`
7. `W3`
8. `B3`

Validation rules:

- feature version must match the current scheduler feature vector
- action-space version must match the current action mapping
- action count must match the current compiled scheduler action count
- exact payload length match only

Current compiled coupling:

- tensor order and tensor shapes are implied by payload version `1`
- scheduler feature layout is coupled to the compiled feature extractor
- action count is coupled to the compiled scheduler action map
- these blobs are therefore portable only across builds that share the
  same scheduler feature/action contract

### `sched_config`

Inner magic: `SCF1`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `SCF1` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | feature version | currently `1` |
| `8` | 2 | action-space version | currently `1` |
| `10` | 2 | reserved | must be `0` |

Body:

| Offset | Size | Field |
|---|---:|---|
| `12` | 4 | `enabled` |
| `16` | 4 | `min_target_ready` |
| `20` | 4 | `min_active_cpus` |
| `24` | 4 | `imbalance_num` |
| `28` | 4 | `imbalance_den` |

Validation rules:

- feature version must match
- action-space version must match
- reserved bytes must be zero
- `enabled` must be `0` or `1`
- `min_active_cpus` must be non-zero
- `imbalance_num` and `imbalance_den` must be non-zero

### `sched_thresholds`

Inner magic: `STH1`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `STH1` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | feature version | currently `1` |
| `8` | 2 | action-space version | currently `1` |
| `10` | 2 | reserved | must be `0` |

Body:

| Offset | Size | Field |
|---|---:|---|
| `12` | 8 | `critical_ns` |
| `20` | 8 | `high_ns` |
| `28` | 8 | `boost_ns` |

Validation rules:

- feature version must match
- action-space version must match
- reserved bytes must be zero
- all thresholds must be non-zero
- thresholds must be strictly ordered:
  - `critical_ns < high_ns < boost_ns`

### `sched_rebalance`

Inner magic: `SRB1`

Header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `SRB1` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | feature version | currently `1` |
| `8` | 2 | action-space version | currently `1` |
| `10` | 2 | reserved | must be `0` |

Body:

| Offset | Size | Field |
|---|---:|---|
| `12` | 4 | `enabled` |
| `16` | 4 | `interval_ticks` |
| `20` | 4 | `imbalance_min` |

Validation rules:

- feature version must match
- action-space version must match
- reserved bytes must be zero
- `enabled` must be `0` or `1`
- `interval_ticks` must be non-zero
- `imbalance_min` must be non-zero

### `sched_xgboost`

Inner magic: `XGBC` (cascade — see "Why two XGBoost formats" below).

The scheduler XGBoost cascade is the only blob whose **storage lives
Rust-side** (`runtime/src/sched/xgb.rs`) rather than the static dense
pool used by every other scheduler kind. The trained cascade is ~9 MB
on the shipping model — too large for the per-slot pool — so the C
side forwards `SCHED_MODEL_KIND_XGBOOST` calls to the Rust FFI in
`runtime/src/sched/xgb.rs` and the bytes are owned by Rust heap from
stage onward.

Outer cascade header:

| Offset | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | magic | ASCII `XGBC` |
| `4` | 2 | payload version | currently `1` |
| `6` | 2 | reserved | must be `0` |
| `8` | 2 | classifier count | must be 3 for `ai_xgb` |
| `10` | 2 | reserved | must be `0` |
| `12` | 4 | reserved | must be `0` |

Then **N back-to-back classifier sections**, each starting with a
16-byte header:

| Offset (rel.) | Size | Field | Notes |
|---|---:|---|---|
| `0` | 4 | tree count (`u32`) | must be non-zero |
| `4` | 4 | node count (`u32`) | must be non-zero |
| `8` | 2 | label-class count (`u16`) | bounded by `MAX_LABEL_CLASSES = 64` |
| `10` | 2 | reserved | must be `0` |
| `12` | 4 | reserved | must be `0` |

Followed by, in order:

- `tree_count` root indices as `u32` (widened from XGB1's `u16` —
  see below)
- `node_count` nodes, each **20 bytes** (widened from XGB1's 16
  bytes)
- `n_classes` label values as `i32`

Cascade node layout (XGBC, 20 bytes):

| Offset | Size | Field |
|---|---:|---|
| `0` | 2 | feature index (`u16`) |
| `2` | 2 | flags (`u16`) |
| `4` | 4 | left child index (`u32`) |
| `8` | 4 | right child index (`u32`) |
| `12` | 4 | threshold (`f32`) |
| `16` | 4 | value (`f32`) |

Validation rules:

- root and child indices must be in range
- non-leaf feature indices must be `< CASCADE_MAX_FEATURE_IDX[i]` for
  classifier `i` (113, 114, 115 for the shipping `ai_xgb` cascade —
  each classifier appends the prior stage's prediction to the input
  vector, widening it by one)
- thresholds must be finite
- leaf values must be finite
- entire blob must be `≤ MAX_CASCADE_PAYLOAD_BYTES = 128 MB`
- absolute caps: `MAX_CLASSIFIERS = 8`, `MAX_TREES_CASCADE = 16384`,
  `MAX_NODES_CASCADE = 2_000_000`. The 3-classifier requirement is
  `ai_xgb`-specific; the XGBC wire format itself is a generic
  N-classifier cascade up to the 8-classifier engine cap, so a future
  scheduler kind could reuse it for a deeper or shallower cascade
  without bumping the format version.

#### Why two XGBoost formats

The eviction `xgboost` kind uses **XGB1** (16-byte node, `u16`
children, `MAX_NODES_SINGLE = 65535`); the scheduler `sched_xgboost`
kind uses **XGBC** (20-byte node, `u32` children, `MAX_NODES_CASCADE
= 2_000_000`). The split exists because the shipping scheduler
`core_clf` flattens to ~450 K nodes — well past the `u16` ceiling
that XGB1 imposes. Eviction models stay small enough that the
narrower XGB1 layout is correct. The shared in-memory `Node`
representation in `runtime/src/ml/xgb_tree.rs` holds children as
`u32` regardless of wire width; only the parsers differ.

## Current Limits

The current formats are still intentionally narrow:

- no embedded signer or trust policy
- no manifest chaining
- no anti-rollback generation counter
- no variable-shape dense models
- no platform-constraint metadata beyond current feature/action versioning
- no self-describing tensor table

Those are follow-on hardening/generalization tasks, not part of the
first usable runtime-loading milestone.

## Generalization Targets

When these formats grow beyond the current first-cut milestone, the next
intended improvements are:

- explicit forward-compatibility policy instead of exact-version-only
  acceptance
- self-describing dense-model tensor tables instead of fixed tensor
  order by version alone
- clearer platform/CPU-topology constraint metadata for scheduler blobs
- explicit statement of which fields are safe to extend in-place versus
  requiring a new payload version
