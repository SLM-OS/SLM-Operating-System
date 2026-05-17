# Graduating an XGBoost Eviction Model

Tracking ticket: [#952](https://github.com/SLM-OS/SLM-Operating-System/issues/952).

This document walks through promoting a candidate XGBoost page-eviction
model from the runtime **blob path** (dynamically loaded, experiment-
ready) to the **baked path** (compiled into the kernel image, the
default that ships when `EVICTION_MODELS=ON`).

The two paths coexist by design — see [Two paths, one trained model](#two-paths-one-trained-model)
below. The graduation procedure is what to do when an experimental
blob-path model has proven good enough to become the official baked-in
SLM-OS choice.

## Audience

This is a developer-procedural document. Read it end-to-end before
graduating a model for the first time. Reading source code to figure
out the workflow is exactly what this document exists to avoid.

## Two paths, one trained model

The `XGBoostPolicy` in `runtime/src/mm/eviction/xgboost.rs` has two
prediction sources for a single trained model:

| Path | Storage | Loaded when | Use case |
|------|---------|-------------|----------|
| **Baked** | `runtime/src/mm/eviction/generated/xgb_policy_generated.rs` | Compile time, gated on `EVICTION_MODELS=ON` | Default ship config; no SD-card files needed |
| **Blob** | SEMB outer + XGB1 payload (`evict.smb`) loaded via `eviction model load xgboost <path>` and activated | Runtime, via shell verb or boot-time autoload | Experimentation; ship different models without rebuilding the kernel |

`XGBoostPolicy::score_row` checks for a staged blob first and falls back
to the baked function when no blob is present. So a kernel with both a
baked model AND a blob staged will use the blob; clearing the blob
(`eviction model clear xgboost`) falls back to the baked default.

Both sources consume the same `[f32; 27]` feature vector and produce
the same `sigmoid(margin)` output. They're interchangeable from
`select_victim`'s perspective.

## When to graduate

Graduate a blob-path model into the baked path when **all** of the
following are true:

- The candidate has demonstrated improved hit-rate on your target
  workloads vs the current baked model.
- The candidate has passed
  [`bench xgb-equiv-evict`](benchmarks.md#eviction-equivalence-verification-bench-xgb-equiv-evict)
  on hardware (the blob's predictions match the trained Python
  model within tolerance).
- You want the model to ship as the default — i.e. it should be
  used on a fresh SD card / kexec with no `evict.smb` staged.

Don't graduate just to "lock in" a checkpoint; the blob path is the
right venue for ongoing experimentation. Graduation is a one-way
commitment to ship-as-default.

## Procedure

### Step 1: Re-train (if you've changed anything)

If you've changed the training data, feature definitions, or
hyper-parameters since the candidate blob was exported, re-train and
regenerate everything from scratch. The blob you tested needs to come
from the same `xgb_model.json` checkpoint you're about to graduate.

```bash
cd ~/projects/slm-os-page-sim
python scripts/train_xgb.py --output data/models/xgb_model.json
```

(Or whatever your training command is — see `slm-os-page-sim`'s own
docs for the canonical flow.)

### Step 2: Verify the blob path one more time

Before committing to graduation, prove the trained model still matches
the blob's predictions on hardware:

```bash
# Re-export the blob + verification corpus from the trained model.
python ~/projects/slm-os-page-eviction/scripts/export_to_slmos.py \
    --xgb-model ~/projects/slm-os-page-sim/data/models/xgb_model.json \
    --smb-output-dir /tmp/evict-fixtures \
    --smb-corpus-size 1000

# Stage to the Pi 5 / Jetson via labctl and run on hardware.
# (See docs/benchmarks.md §"Eviction equivalence verification" for the
# full shell-side recipe.)
```

If this fails, **stop here**. A blob that doesn't match its source
model has a real bug; graduating it would bake that bug into the
kernel image.

### Step 3: Regenerate the baked Rust file

The page-sim exporter writes the `xgb_policy_generated.rs` if-else
chain alongside the blob:

```bash
cd ~/projects/slm-os-page-sim
python scripts/export_to_slmos.py
# Default output: data/export/xgb_policy_generated.rs
```

### Step 4: Import into SLM-OS

`scripts/import_eviction_weights.sh` copies the file from
`~/projects/slm-os-page-sim/data/export/` into
`runtime/src/mm/eviction/generated/`, rewriting the `use` path and the
`(expr).exp()` → `libm::expf(expr)` substitution:

```bash
cd ~/projects/CS-496-Capstone-SLM-Operating-System
./scripts/import_eviction_weights.sh
```

### Step 5: Build, then verify the baked path on-device

```bash
make kernel-test PLATFORM=RASPI5 AI_SCHED=ON EVICTION_MODELS=ON
# Deploy via labctl sdwire_update + power_cycle as usual.
```

Once booted to the shell, run the baked-path equivalence verb:

```
bench xgb-equiv-evict --baked /slmstore/test_vectors_xgb_evict.bin \
                              /slmstore/expected_evict.bin
```

The `--baked` flag routes the comparison through
`generated::xgb_predict()` (the just-imported function) instead of the
runtime blob. **This is the load-bearing step**: it verifies that the
ARM64 / x86_64 compiler's f32 codegen actually preserves the
predictions, not just that the Python-vs-Python round-trip matches.
See [Gotcha #5](#gotcha-5-baked-path-needs-its-own-on-device-equivalence-check).

If this passes — N/N match within tolerance — graduation is complete.

### Step 6: Commit + ship

```bash
git add runtime/src/mm/eviction/generated/xgb_policy_generated.rs
git commit -m "eviction: graduate XGBoost model trained on <dataset> (vN)"
# Open a PR with the bench output captured in the description.
```

The `import_eviction_weights.sh` second-line provenance comment in the
generated file records the import timestamp; the commit message should
record the training dataset / version. Together they make
"which model is in this kernel?" answerable from `git log`.

## Gotchas

These are the items that bit early graduators or that surface as
quiet failures rather than loud errors. Read all of them.

### Gotcha 1: There was no documented procedure

Until this document existed, a first-time graduator had to
reverse-engineer the steps from `scripts/import_eviction_weights.sh`'s
header comment, intuition about the sibling repos, and trial and
error. If any step below feels "obvious" to you now, it wasn't
yesterday. Update this document when you discover new sharp edges.

### Gotcha 2: Feature ordering drift is silent

Both `RuntimeXGBoostModel` (`FEATURE_COUNT = 27`) and the generated
`xgb_predict()` hardcode the feature *layout*. The if-else chain
indexes features positionally with name-only comments:

```rust
if features[13] /* predicted_reuse_dist */ < 0.00700000022_f32 { ... }
```

If `slm-os-page-sim`'s `FeatureConfig` ever re-orders features or
inserts a new one without updating SLM-OS's `BlockFeatures` /
`features.rs::extract_features`, **the baked code silently uses the
wrong index at every node** — every prediction becomes garbage.
Nothing in the build catches this today.

[Phase 2 of #952](https://github.com/SLM-OS/SLM-Operating-System/issues/952#issue)
adds a generator-emitted feature-name header constant and a runtime
`const_assert!` to catch ordering drift at compile time. Until that
lands, **diff `feature_names` in the sibling's `FeatureConfig`
against `BlockFeatures` field comments by hand** every time you
regenerate.

(Feature *count* drift is explicitly out of scope per maintainer
call — that's a much bigger schema change than a graduation.)

### Gotcha 3: Compile-flag boundary is undocumented

`xgb_policy_generated.rs` is full of `f32` literals
(`0.199489996_f32`). Eviction code isn't built under
`-mgeneral-regs-only` today (that's shell-only), but the boundary
isn't pinned anywhere. If a future change pulls eviction into a
no-FP-literals build path (e.g., to make the eviction policy reachable
from an IRQ handler), `xgb_policy_generated.rs` will silently stop
compiling at graduation time, not at flag-flip time.

If you're touching the build path for any eviction-adjacent code,
verify `EVICTION_MODELS=ON` still builds. The boundary lives
implicitly in `CMakeLists.txt:2222` (`ENABLE_EVICTION_MODELS` adds
`CONFIG_AI_EVICTION_MODELS=1` to compile defs but no flag change).

### Gotcha 4: Hardcoded-output tests break on graduation

Tests under `kernel/tests/test_eviction*.c` and
`runtime/src/mm/eviction/tests/` may assert specific scores against
specific inputs. A new baked model gives new scores — those
assertions break.

Today there's no script to regenerate test fixtures. If a test fails
after Step 4, inspect the assertion: if it's a "this model predicts X
for input Y" check, regenerate the fixture from the new model rather
than skipping the test. If it's a structural check (output is finite,
within `[0, 1]`, signs are correct), the test should pass and the
failure indicates a real graduation problem.

### Gotcha 5: Baked path needs its own on-device equivalence check

`slm-os-page-eviction`'s `verify_predictions` cross-checks Python
predictions against the *generated Rust code's logic*, executed by a
Python interpreter that walks the if-else tree. That's an important
check but it's **Python comparing Python**.

It does NOT verify that the actual ARM64 / x86_64 baked code — with
whatever FP rounding the kernel compiler applied — matches the source
model on real hardware. A regression in compiler tree-walk or
`f32` precision (from a toolchain upgrade, an inadvertent
`-ffast-math`, an LTO interaction) would slip through.

Step 5 above (`bench xgb-equiv-evict --baked`) is the on-device check
that catches this class of bug. **Do not skip it.** A blob-path PASS
followed by a baked-path FAIL means the model was preserved through
Python export but not through native compilation — which is exactly
the class of regression that surfaces only on hardware.

## Feature schema invariants

The SLM-OS-side source-of-truth for what features the model sees is:

| Element | Pinned by | Where |
|---------|-----------|-------|
| **Feature count** (27) | `BlockFeatures = [f32; 27]` | `runtime/src/mm/eviction/policy.rs:45` |
| **Feature names + order** | `extract_features()` field-by-field order | `runtime/src/mm/eviction/features.rs` |
| **Feature normalization** | `FeatureNormalizer` in the sibling repo | `slm-os-page-sim/src/features/normalizer.py` (mirror of what SLM-OS expects) |

`slm-os-page-sim`'s `FeatureConfig` must match these three things
exactly. If it doesn't, the model was trained on differently-shaped
data than SLM-OS will feed it, and predictions will be garbage even
if every other step succeeds.

Until [Phase 2 of #952](https://github.com/SLM-OS/SLM-Operating-System/issues/952#issue)
lands the compile-time guard, the consistency check is manual:
diff `FeatureConfig.feature_names` (sibling) against the field comments
in `extract_features` (SLM-OS) every time you regenerate.

## Related

- [`docs/eviction.md`](eviction.md) — runtime layout, policy list,
  build flags, and the existing
  [Importing Trained Weights](eviction.md#importing-trained-weights)
  section that this document supersedes for the graduation use case.
- [`docs/benchmarks.md` §Eviction equivalence verification](benchmarks.md#eviction-equivalence-verification-bench-xgb-equiv-evict) —
  the blob-path equivalence verb that gates Step 2.
- [#932](https://github.com/SLM-OS/SLM-Operating-System/issues/932) —
  the equivalence verb itself (closed by PR #959).
- [#920](https://github.com/SLM-OS/SLM-Operating-System/issues/920) —
  scheduler-side equivalence verb (template the eviction side mirrors).
- [#448](https://github.com/SLM-OS/SLM-Operating-System/issues/448) —
  envelope generalization (closed; the blob format used here).
- [#953](https://github.com/SLM-OS/SLM-Operating-System/issues/953) —
  decision gate on flipping XGBoost from opt-in to the default
  eviction policy.
- [#961](https://github.com/SLM-OS/SLM-Operating-System/issues/961) —
  XGBoost latency at ~4-22 µs/select_victim on Pi 5 / Jetson misses
  the < 1 µs hot-path target; affects whether to graduate at all.
