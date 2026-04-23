# Dynamic Policy Model Loading Plan

This document covers the work needed to support **runtime-loaded model
parameters** for eviction first, then scheduler, without depending on a
maintenance OS on the SD card.

Non-goals for the first iteration:

- loading arbitrary new policy code at runtime
- making the scheduler dynamically loadable before the eviction path is stable
- depending on Pi OS for updates once the device workflow is mature
- selecting a specific wire protocol now (FTP/ZMODEM/etc.)

The intended architecture is:

- keep eviction/scheduler **policy code** compiled into SLM-OS
- make **model parameters / expert weights / tuning tables** dynamically loadable
- separate **file ingress** from **validation + activation**
- require **atomic activation** and **safe fallback**

---

## Current State

Already implemented:

- **Scheduler policy hot-swap** among compiled-in policies:
  - shell: `sched policy <name>`
  - Lua admin: `slm.sched_set_policy(name)`
- **Eviction policy hot-swap** among compiled-in policies:
  - shell: `eviction policy <name>`
  - Lua admin: `slm.eviction_set_policy(name)`
- **Eviction enabled by default**
- **Classical eviction policies available without trained models**
- **Trained eviction models as build-time payloads** via `EVICTION_MODELS=ON`
- **AI scheduler build-time inclusion** via `AI_SCHED=ON`

What is missing:

- runtime-loaded eviction model payloads
- atomic staging / activation / rollback of those payloads
- native SLM-OS file ingress for blobs and general files
- runtime-loaded scheduler model payloads

---

## Architectural Direction

### Core principle

Do **not** dynamically load new code first.

Instead:

1. compile the policy implementation into the kernel
2. load validated model payloads into kernel-managed memory
3. atomically switch the active model for that policy

That gives most of the operational value with much lower correctness
and security risk than runtime-loaded code.

### Why eviction first

Eviction is the better proving ground because:

- it already sits behind a clean policy abstraction in the Rust runtime
- its hot path is simpler than the task scheduler
- mistakes are easier to isolate and recover from
- model activation can be tested under synthetic pressure without
  destabilizing the core dispatcher

### Long-term file-ingress direction

The stable end-state should not depend on Pi OS being present on the
card. That means SLM-OS needs a native file-ingress path.

Recommended order:

1. **filesystem-backed on-device loader API**
2. **native SLM-OS file ingress**
3. **policy blob activation / rollback**
4. **scheduler reuse of the same blob framework**

Pi OS remains a temporary convenience / recovery path, not part of the
long-term architecture.

---

## Eviction Plan

### Phase E1 — Blob format + parser

Add a versioned binary blob format for eviction model data.

Requirements:

- stable magic
- schema version
- model kind
- feature-schema version
- payload length
- checksum
- architecture-independent integer encoding

Kinds in scope:

- `xgboost`
- `mlp`
- `cacheus_config`

Deliverables:

- Rust parser/validator
- strict rejection of malformed/incompatible payloads
- self-tests for header validation and checksum mismatch

### Phase E2 — Staging store

Add an eviction-model staging layer in the Rust runtime.

Requirements:

- one inactive staging slot per model kind
- one active slot per model kind
- metadata visible to shell/UI
- no partial publish

State machine:

- `empty`
- `staged`
- `active`
- `rolled_back`

### Phase E3 — Activation + rollback

Add atomic activation of staged blobs.

Requirements:

- validate before publish
- swap active pointer under the eviction registry lock
- preserve previous active payload until activation succeeds
- explicit rollback to prior payload
- safe fallback to compiled-in tables / stubs when no runtime payload is active

### Phase E4 — Policy integration

Wire the existing policies to consult runtime payloads.

Expected behavior:

- if active runtime payload exists, use it
- else if compiled-in trained payload exists, use it
- else fall back to stub behavior

This applies to:

- `XGBoostPolicy`
- `MlpPolicy`
- `CacheusSelector` expert weights / tuning constants

### Phase E5 — Shell / Lua admin controls

Add admin-only control surface:

- `eviction model status`
- `eviction model load <kind> <path>`
- `eviction model activate <kind>`
- `eviction model rollback <kind>`
- `eviction model clear <kind>`

Lua admin equivalents can come after the shell path stabilizes.

### Phase E6 — Persistence

First cut:

- RAM activation only
- explicit reload after reboot

Optional follow-up:

- persist selected runtime payloads to filesystem
- boot-time autoload from configured paths

---

## Scheduler Follow-On Plan

Scheduler reuses the same model-blob architecture after eviction is stable.

### Phase S1 — Scheduler blob format

Extend the blob format with scheduler kinds:

- `sched_mlp`
- `sched_ppo`
- future `sched_thresholds` / `sched_config`

Scheduler payloads must also carry:

- action-space version
- feature-vector version
- CPU-count / platform constraints if needed

### Phase S2 — Scheduler model staging

Add staging and activation semantics equivalent to eviction:

- inactive slot
- validate
- atomic publish
- rollback

### Phase S3 — Scheduler runtime integration

The scheduler already hot-swaps among compiled-in policies. This work
only changes how the AI policies source their model parameters.

Requirements:

- policy code remains compiled in
- model tables become replaceable at runtime
- activation must not violate scheduler invariants

### Phase S4 — Scheduler control surface

Shell:

- `sched model status`
- `sched model load <kind> <path>`
- `sched model activate <kind>`
- `sched model rollback <kind>`

Do not implement this before eviction activation is proven out.

---

## Native File Ingress Plan

The loader needs files on the running device. Long term, SLM-OS should
support that without Pi OS.

### Ingress principle

Keep transport separate from activation.

Transport writes a file to the filesystem.
Activation validates and installs it.

### Recommended order

#### Phase F1 — Minimal device-local contract

Standardize on destination paths, for example:

- `/mnt/files/policies/*.blob`
- `/mnt/files/models/*`

All future ingress methods target files first, not direct activation.

#### Phase F2 — Small native upload path

Best near-term choice:

- small SLM-OS-native upload protocol over the existing network stack
- host-side push tool
- write directly to VFS

Why this first:

- smaller than SSH/SCP
- lower implementation cost than a full remote shell/file subsystem
- enough for model blobs and general files

#### Phase F3 — Serial upload fallback

Provide the same capability over UART for recovery / bring-up.

#### Phase F4 — HTTP client integration

Long-term win:

- pull files directly from an HTTP endpoint
- useful for model distribution and general device provisioning

This should be added after the local file-loader contract exists so the
HTTP path is just another transport feeding the same activation commands.

#### Phase F5 — Stronger authenticated transport

Potential later work:

- SSH/SCP
- HTTPS/TLS
- signed manifests / integrity policy

Not required for the first dynamic-model milestone.

---

## Recommended Command Shape

Eviction:

```text
eviction model status
eviction model load xgboost /mnt/files/policies/xgb.blob
eviction model activate xgboost
eviction model rollback xgboost
```

Scheduler:

```text
sched model status
sched model load mlp /mnt/files/policies/sched_mlp.blob
sched model activate mlp
sched model rollback mlp
```

Future file ingress examples:

```text
put /mnt/files/policies/xgb.blob
recv /mnt/files/policies/xgb.blob
fetch http://host/policies/xgb.blob /mnt/files/policies/xgb.blob
```

---

## Risks

Main risks:

- feature-schema mismatch between training exports and runtime extractor
- partial activation leaving policy state inconsistent
- unbounded memory growth from staged blobs
- scheduler-side activation races if applied too early

Mitigations:

- explicit schema version in every blob
- checksum + strict length validation
- inactive-slot staging only
- activation under existing subsystem lock
- rollback path from day one

---

## Immediate Next Steps

1. Add eviction blob header/parser/validator.
2. Add an eviction staging store for inactive/active payloads.
3. Add shell-visible status for staged/runtime eviction blobs.
4. Add activation + rollback for eviction MLP.
5. Extend to XGBoost and CACHEUS config.
6. Add native file-ingress path for copying blobs onto a running SLM-OS device.
7. Reuse the architecture for scheduler models.
