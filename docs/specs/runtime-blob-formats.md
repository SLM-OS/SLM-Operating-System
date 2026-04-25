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
