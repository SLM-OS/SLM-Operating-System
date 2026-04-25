# Dynamic Policy Model Loading Plan

This document covers the work needed to support **runtime-loaded model
parameters** for eviction first, then scheduler, without depending on a
maintenance OS on the SD card.

This is a living plan. Update it as implementation lands so it remains
an accurate guide to what is done, what is partially done, and what
still needs design or validation.

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

Deployment assumptions for Pi 5 should preserve two board models:

- **SDWire-first boards**: fast host-driven kernel replacement remains
  the preferred iterative deploy path
- **No-SDWire boards**: Linux maintenance / dual-boot workflows remain
  valid deployment and recovery paths until native SLM-OS ingress is
  mature enough to replace them operationally

---

## Current State

Status legend:

- `✅ done` — implemented and validated enough to count as landed for this plan
- `✅ partial` — implemented and usable, but not yet complete or fully generalized
- `☐ pending` — intended work that has not reached a usable slice yet
- `☐🔗 gated` — next work depends on another phase landing first
- `⛔ blocked` — currently stopped by an external blocker

At-a-glance summary:

| Area | State | Notes |
|---|---|---|
| Eviction blob format + store | ✅ partial | Parser, staging, activate, rollback, and clear exist; formats are still first-cut |
| Eviction runtime policy use | ✅ partial | `xgboost`, `mlp`, and `cacheus_config` are live and hardware-validated on `pi-5-2` |
| Eviction shell / Lua control | ✅ done | Shell + `lua-admin` surfaces exist and are tested |
| Scheduler runtime models | ✅ partial | `mlp`, `ppo`, and `config` exist; dense-model + config behavior is hardware-validated |
| Scheduler live behavior validation | ✅ done | Deterministic runtime blobs affect real `ai_mlp` / `ai_ppo` decisions on `pi-5-2` |
| File ingress core transport | ✅ partial | `put`, `xput`, and `slm-put.py` are live; telnet + serial framed upload/resume are hardware-validated |
| Operator workflow wrapper | ✅ partial | `slm-modelctl.py` now defaults to subcommands, keeps legacy compatibility, and has a hardware-validated scheduler probe path |
| Persistence / autoload | ☐ pending | Intentionally deferred from the first milestone |
| HTTP / authenticated transport | ☐ pending | Still future work |

Milestone summary:

| Track | State | Notes |
|---|---|---|
| Eviction | ✅ partial | First usable runtime-loading path is in place end to end |
| Scheduler | ✅ partial | First reusable scheduler path is in place for dense models + config |
| Native ingress | ✅ partial | Practical shell-based ingress exists without removing the SD card |
| Maintenance-OS workflow | ✅ done | `pi-5-2` dual-boot + `--tryboot` wrapper path is hardware-validated |

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
- **Device-local filesystem storage** via VFS + LittleFS at `/mnt/files`
- **Shell file manipulation** on mounted filesystems (`ls`, `cat`,
  `write`, `cp`, `mv`, `mkdir`, `rm`, etc.)
- **Generic ONNX file-backed model loading** through `model load <path>`
- **Eviction blob header/parser prototype** in Rust:
  - versioned header with magic, kind, feature-schema version,
    payload length, and checksum
  - strict rejection of malformed blobs
  - self-tests for bad magic, bad checksum, bad schema, and bad length
- **RAM-backed eviction blob store backend** in Rust:
  - one staged slot, one active slot, and one rollback slot per blob kind
  - metadata/status reporting for staged/active/rollback state
  - store-level activate / rollback / clear operations
- **Shell admin controls for runtime eviction blobs**:
  - `eviction model status`
  - `eviction model load <kind> <path>`
  - `eviction model activate <kind>`
  - `eviction model rollback <kind>`
  - `eviction model clear <kind>`
  - hardware-validated on `pi-5-2` for `xgboost`, `mlp`, and
    `cacheus_config` using two distinct real blobs per kind, including
    staged, active, replacement, and rollback transitions
- **Shell-level binary file ingress primitive**:
  - `put <path> <hex...>` overwrites a file from hex-decoded bytes
  - `put -a <path> <hex...>` appends another binary chunk
  - works over any existing shell transport, including telnetd and UART
- **Scheduler runtime dense-model blob path (first cut)**:
  - scheduler runtime blob kinds now include `mlp`, `ppo`, and
    `config`
  - shell: `sched model status`
  - shell: `sched model load <kind> <path>`
  - shell: `sched model activate <kind>`
  - shell: `sched model rollback <kind>`
  - shell: `sched model clear <kind>`
  - Lua admin: `slm.sched_model_status/load/activate/rollback/clear`
  - `ai_mlp` and `ai_ppo` now prefer active runtime payloads over
    compiled-in weights when one is installed
  - hardware-validated on `pi-5-2` with two distinct real `sched_mlp`
    blobs and two distinct real `sched_ppo` blobs covering staged,
    active, replacement, rollback, and clear transitions
  - policy-level behavior also validated on `pi-5-2` with
    deterministic runtime blobs: `ai_mlp` produced raw action `7` and
    `ai_ppo` produced raw action `13` on live tasks, proving the active
    runtime payloads affect real scheduling decisions
  - Lua admin bindings smoke-tested on `pi-5-2` via `lua-admin`
- **Lua admin controls for runtime eviction blobs**:
  - `slm.eviction_model_status(kind)`
  - `slm.eviction_model_load(kind, path)`
  - `slm.eviction_model_activate(kind)`
  - `slm.eviction_model_rollback(kind)`
  - `slm.eviction_model_clear(kind)`

Partially implemented:

- **Eviction runtime blob format (E1):** parser and validation exist,
  and blobs can now be staged into the runtime store
- **Eviction staging and activation backend (E2/E3):** the in-memory
  store exists and is exposed through shell commands, and active blobs
  are now consumed by MLP / XGBoost / CACHEUS runtime paths; the
  `xgboost`, `mlp`, and `cacheus_config` shell lifecycles have been
  validated on hardware
- **Eviction policy integration (E4):** MLP and XGBoost can now consume
  active runtime payloads, and CACHEUS can consume active runtime
  config payloads, but the payload formats are still first-cut
- **Filesystem side of file ingress:** the running OS can already read
  and write files under `/mnt/files`, but there is no dedicated
  host-to-running-device upload transport yet
- **Host-to-device file ingress:** a binary-safe shell primitive now
  exists, and a first-cut telnet host push tool now exists in
  `scripts/tools/slm-put.py`; it now supports a framed shell upload
  path via `xput`, auto-probing between framed and legacy modes,
  resume-from-partial, and reconnect-and-continue recovery
  - direct-run host-side functional coverage now exists in
    `scripts/tools/test_slm_tooling.py`
- **Operator wrapper for runtime blob workflows:** `scripts/tools/slm-modelctl.py`
  now supports explicit `apply`, `load`, `activate`, `rollback`,
  `clear`, and `status` subcommands
  - direct-run host-side functional coverage now exists in
    `scripts/tools/test_slm_tooling.py`
  - hardware-validated on `pi-5-2` for the live `eviction cacheus_config`
    lifecycle across explicit subcommands
  - hardware-validated on `pi-5-2` for both telnet and serial wrapper use
  - the maintenance-OS `--tryboot` path has also been hardware-validated
  - scheduler `probe` is now hardware-validated on `pi-5-2` for:
    - `ai_mlp` with deterministic raw action `7`
    - `ai_ppo` with deterministic raw action `13`
  - one-shot `apply --probe-raw` is also hardware-validated on
    `pi-5-2` for `sched mlp`
- **Scheduler runtime model loading:** first-cut dense-network runtime
  loading now exists through the shell and Lua admin for both
  `sched_mlp` and `sched_ppo`, and both lifecycles have now been
  validated on hardware
- **Scheduler runtime config loading:** first-cut scheduler config
  blobs now exist for proactive load-balance override tuning, reusing
  the same stage / activate / rollback / clear lifecycle as scheduler
  dense models
  - shell/Lua lifecycle is implemented for `sched model ... config`
  - live hardware validation on `pi-5-2` now passes on a clean
    `WORK_STEALING=OFF` build: with an active runtime config blob
    setting `enabled=0`, a deterministic `sched_mlp` blob forcing core
    `0` lands on CPU `0` with `fallbacks=0`
  - earlier contrary results were a validation artifact, not a config
    parser/activation bug: the kernel build directory was silently
    reusing a stale CMake cache with work stealing still enabled, and
    immediate steals could move probe tasks within a few hundred
    microseconds

What is missing:

- a more complete host-to-running-device file upload protocol on top of
  the new shell-level `put` primitive
- runtime-loaded scheduler payloads beyond the first-cut `sched_mlp`
- broader runtime-loaded scheduler payload families beyond dense models
  plus the first-cut proactive-balance config

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

That does **not** mean existing deploy models should be collapsed into
one path in the meantime. The project should preserve both:

- SDWire-first kernel deployment where the hardware supports it
- maintenance-OS / dual-boot deployment where SDWire is absent

Recommended order:

1. **filesystem-backed on-device loader API**
2. **policy blob staging / activation / rollback**
3. **native SLM-OS file ingress**
4. **scheduler reuse of the same blob framework**

Pi OS remains a temporary convenience / recovery path in the long-term
architecture, but it is still a supported deploy model on boards
without SDWire while native ingress is incomplete.

---

## Eviction Plan

### Phase E1 — Blob format + parser

Add a versioned binary blob format for eviction model data.

Status: `✅ partial`

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

Implementation notes:

- current code lives in `runtime/src/mm/eviction/blob.rs`
- current scope is header validation only; payload interpretation is
  still policy-specific follow-on work
- current checksum is FNV-1a 32-bit, which is acceptable for accidental
  corruption detection in this first cut

### Phase E2 — Staging store

Add an eviction-model staging layer in the Rust runtime.

Status: `✅ done`

Requirements:

- one inactive staging slot per model kind
- one active slot per model kind
- metadata visible to shell/UI
- no partial publish

Implementation notes:

- current code lives in `runtime/src/mm/eviction/store.rs`
- the runtime backend exposes stage/status/clear primitives now
- shell surfacing exists through `eviction model ...`
- Lua admin surfacing exists through `slm.eviction_model_*`

State machine:

- `empty`
- `staged`
- `active`
- `rolled_back`

### Phase E3 — Activation + rollback

Add atomic activation of staged blobs.

Status: `✅ done`

Requirements:

- validate before publish
- swap active pointer under the eviction registry lock
- preserve previous active payload until activation succeeds
- explicit rollback to prior payload
- safe fallback to compiled-in tables / stubs when no runtime payload is active
- first milestone is **RAM-backed activation only**

Implementation notes:

- the blob store already supports activate / rollback / clear semantics
- the active blob path now changes eviction behavior for MLP, XGBoost,
  and CACHEUS config
- remaining work is mostly around surfacing, format hardening, and
  scheduler reuse

### Phase E4 — Policy integration

Wire the existing policies to consult runtime payloads.

Status: `✅ partial`

Expected behavior:

- if active runtime payload exists, use it
- else if compiled-in trained payload exists, use it
- else fall back to stub behavior

This applies to:

- `XGBoostPolicy`
- `MlpPolicy`
- `CacheusSelector` expert weights / tuning constants

Implementation notes:

- `MlpPolicy` now prefers an active runtime payload when present and
  falls back to the compiled-in/stub predictor otherwise
- the current runtime MLP payload is a fixed-shape float32 payload for
  the existing 27 → 64 → 32 → 16 → 1 network
- `XGBoostPolicy` now prefers an active runtime tree-ensemble payload
  when present and falls back to the compiled-in/stub predictor
- `CacheusSelector` now consumes an active runtime config payload for
  expert-pool selection and tuning constants

### Phase E5 — Shell / Lua admin controls

Add admin-only control surface:

Status: `✅ done`

- `eviction model status`
- `eviction model load <kind> <path>`
- `eviction model activate <kind>`
- `eviction model rollback <kind>`
- `eviction model clear <kind>`

Implementation notes:

- shell path is implemented
- Lua admin path is implemented on the `lua-admin` surface
- current coverage includes shell tests, FFI tests, runtime self-tests,
  and Lua binding coverage for status/load/activate/rollback/clear

### Phase E6 — Persistence

First cut:

- RAM activation only
- explicit reload after reboot

Optional follow-up:

- persist selected runtime payloads to filesystem
- boot-time autoload from configured paths

Status: `☐ pending`

Rationale:

- the first milestone should prove correctness of validation, staging,
  activation, rollback, and policy fallback without also taking on
  persistence semantics
- persistence adds a larger design surface: authoritative path
  selection, boot-time discovery/autoload policy, corruption handling,
  and update atomicity across power loss

---

## Scheduler Follow-On Plan

Scheduler reuses the same model-blob architecture after eviction is stable.

| Phase | State | Notes |
|---|---|---|
| S1 — Scheduler blob format | ✅ partial | Dense models + `config` exist; the format family is not complete yet |
| S2 — Scheduler model staging | ✅ done | Stage / activate / rollback / clear exist for current kinds |
| S3 — Scheduler runtime integration | ✅ partial | `ai_mlp`, `ai_ppo`, and proactive-balance config are live |
| S4 — Scheduler control surface | ✅ partial | Shell + Lua exist; richer operator tooling is still pending |

### Phase S1 — Scheduler blob format

Status: `✅ partial`

Extend the blob format with scheduler kinds:

- `sched_mlp`
- `sched_ppo`
- future `sched_thresholds` / `sched_config`

Scheduler payloads must also carry:

- action-space version
- feature-vector version
- CPU-count / platform constraints if needed

Implementation notes:

- the current implementation covers `sched_mlp` and `sched_ppo`
- the current payload carries feature-vector version, action-space
  version, and action-count validation
- the current outer blob reuses the same `SEMB` wrapper shape as the
  eviction path, but uses a scheduler-specific kind id

### Phase S2 — Scheduler model staging

Status: `✅ done`

Add staging and activation semantics equivalent to eviction:

- inactive slot
- validate
- atomic publish
- rollback

### Phase S3 — Scheduler runtime integration

Status: `✅ partial`

The scheduler already hot-swaps among compiled-in policies. This work
only changes how the AI policies source their model parameters.

Requirements:

- policy code remains compiled in
- model tables become replaceable at runtime
- activation must not violate scheduler invariants

Implementation notes:

- `ai_mlp` now consults the active runtime model before falling back to
  compiled-in weights
- `ai_ppo` now consults the active runtime model before falling back to
  compiled-in weights
- the first hardware pass exposed a real bug: `sched_model_stage_blob()`
  was parsing a full runtime MLP model into a stack-local temporary,
  which was large enough to wedge the live shell on replacement load;
  this was fixed by moving parse staging into a static scratch slot
- the runtime store now swaps staged / active / rollback slot roles
  instead of copying active model bytes, and inference acquires a
  reader token so it does not hold the scheduler model lock across the
  forward pass
- the first reader-token implementation had a token-encoding bug where
  one valid active-slot combination collapsed to token `0`, leaking a
  reader count and blocking `sched model clear`; this was fixed and
  re-validated on `pi-5-2`

### Phase S4 — Scheduler control surface

Status: `✅ partial`

Shell:

- `sched model status`
- `sched model load <kind> <path>`
- `sched model activate <kind>`
- `sched model rollback <kind>`

Current scope:

- shell support exists for `mlp` and `ppo`
- `sched model clear <kind>` also exists for operator cleanup
- Lua admin support now exists for `mlp` and `ppo` through
  `slm.sched_model_status/load/activate/rollback/clear`
- the shell lifecycles for `sched_mlp` and `sched_ppo` are
  hardware-validated on `pi-5-2`
- the Lua admin surface has been smoke-tested on `pi-5-2`
- `scripts/tools/slm-modelctl.py` now exposes explicit lifecycle
  subcommands on top of the same shell path
- top-level `--help` now shows the subcommand-oriented CLI by default,
  while the old positional upload form still maps to `apply`
- the wrapper lifecycle flow is hardware-validated on `pi-5-2` over
  both telnet and serial, and through the maintenance-OS `--tryboot`
  path
- the new multi-step scheduler `probe` helper is now hardware-validated
  on `pi-5-2` for both current AI scheduler policies:
  - `ai_mlp` with a deterministic runtime blob forcing raw action `7`
  - `ai_ppo` with a deterministic runtime blob forcing raw action `13`
- one-shot `apply --probe-raw` is also hardware-validated on
  `pi-5-2` for `sched mlp`
- remaining work on `probe` is operator polish rather than basic
  policy coverage

Do not implement this before eviction activation is proven out.

---

## Native File Ingress Plan

The loader needs files on the running device. Long term, SLM-OS should
support that without Pi OS.

Current status:

- the on-device filesystem contract already exists (`/mnt/files`)
- shell-side file operations already exist on the running OS
- the missing piece is a transport that gets files from the host onto a
  running device without depending on a maintenance OS workflow
- until that transport exists, both Pi 5 deploy models stay supported:
  SDWire-first where available, maintenance-OS / dual boot where not

| Phase | State | Notes |
|---|---|---|
| F1 — Minimal device-local contract | ✅ partial | `/mnt/files` exists, but path conventions are not yet formalized |
| F2 — Small native upload path | ✅ partial | `put`, `xput`, `slm-put.py`, and `slm-modelctl.py` are live; shell-line-aware chunking and resume are in place |
| F3 — Serial upload fallback | ✅ partial | Same framed path works over serial and is hardware-validated |
| F4 — HTTP client integration | ☐ pending | Not started |
| F5 — Stronger authenticated transport | ☐ pending | Not started |

### Ingress principle

Keep transport separate from activation.

Transport writes a file to the filesystem.
Activation validates and installs it.

### Recommended order

#### Phase F1 — Minimal device-local contract

Standardize on destination paths, for example:

- `/mnt/files/policies/*.blob`
- `/mnt/files/models/*`

Status: `✅ partial`

Notes:

- `/mnt/files` already exists and is the right first destination root
- this phase should primarily formalize path conventions and shell/admin
  expectations, not invent a new storage abstraction

All future ingress methods target files first, not direct activation.

#### Phase F2 — Small native upload path

Best near-term choice:

- small SLM-OS-native upload protocol over the existing network stack
- host-side push tool
- write directly to VFS

Status: `✅ partial`

Why this first:

- smaller than SSH/SCP
- lower implementation cost than a full remote shell/file subsystem
- enough for model blobs and general files

Implementation notes:

- the running OS now exposes:
  - `put <path> <hex...>` and `put -a <path> <hex...>` as legacy
    binary-safe chunk writes to mounted filesystems
  - `xput begin|chunk|status|finish|abort` as a framed shell upload
    session
- `scripts/tools/slm-put.py` now stages a local file over `telnetd`
  by auto-probing `xput status`, preferring framed `xput` when it is
  available, and verifying final size with `stat`
- the `slm-put.py` direct telnet path, `--labctl` telnet path, and
  `--transport serial` fallback have all been hardware-validated on
  `pi-5-2` against a refreshed SLM-OS image, including end-to-end
  write and content verification under `/mnt/files`
- `slm-put.py` now resumes from an existing partial remote file or
  active framed session by default and can recover from per-chunk
  transport loss by reconnecting and continuing from the confirmed
  remote offset
- both framed upload and framed resume-from-partial have now been
  hardware-validated on `pi-5-2` over both the telnet and serial paths
- remaining work is better operator ergonomics and eventually a
  transport that is not hex-over-shell
- `scripts/tools/slm-modelctl.py` now provides explicit lifecycle
  subcommands on top of the same transport
- `scripts/tools/slm-put.py` now auto-caps chunk sizes to stay within
  shell line-length limits while still allowing larger requested chunk
  sizes
- both the live SLM-side wrapper path and the maintenance-OS
  `--tryboot` wrapper path have now been hardware-validated on
  `pi-5-2`

#### Phase F3 — Serial upload fallback

Provide the same capability over UART for recovery / bring-up.

Status: `✅ partial`

Implementation notes:

- the same `put` shell primitive can already be driven over the UART
  shell as a crude serial fallback
- serial now shares the same shell-level protocol selection and
  resume/recovery semantics as telnet through `slm-put.py`
- framed serial upload and framed serial resume have now been
  hardware-validated on `pi-5-2`

#### Phase F4 — HTTP client integration

Status: `☐ pending`

Long-term win:

- pull files directly from an HTTP endpoint
- useful for model distribution and general device provisioning

This should be added after the local file-loader contract exists so the
HTTP path is just another transport feeding the same activation commands.

#### Phase F5 — Stronger authenticated transport

Status: `☐ pending`

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
put /mnt/files/policies/xgb.blob 0123abcd...
put -a /mnt/files/policies/xgb.blob deadbeef...
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
- confusion between "filesystem exists on-device" and "host upload
  transport exists"

Mitigations:

- explicit schema version in every blob
- checksum + strict length validation
- inactive-slot staging only
- activation under existing subsystem lock
- rollback path from day one
- explicitly treat file transport as a separate workstream from
  activation

---

## Immediate Next Steps

| Step | State | Notes |
|---|---|---|
| Harden and document runtime payload formats | ☐ pending | `mlp`, `xgboost`, `cacheus_config`, `sched_mlp`, `sched_ppo`, and `sched_config` are still first-cut formats |
| Polish `slm-modelctl.py` ergonomics | ✅ partial | Subcommand help and `apply --probe-raw` are in place; remaining work is UX convenience rather than core viability |
| Validate both Pi 5 deploy models where practical | ✅ partial | Maintenance-OS path is validated on `pi-5-2`; keep the SDWire-first path in play too |
| Reuse the architecture for more scheduler payloads | ☐ pending | Next likely slice is new scheduler blob families, not more dense-model mechanics |
