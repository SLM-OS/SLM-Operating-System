# Device-Local File Contract

This document defines the standard writable layout under `/mnt/files`
for host tooling, runtime blob activation, and boot-managed copies.

`/mnt/files` remains the single device-local ingress root.

## Standard Paths

| Path | Purpose | Producer | Consumer |
|---|---|---|---|
| `/mnt/files/policies/` | Operator-supplied runtime blob files | `slm-put.py`, `slm-modelctl.py`, shell `put` / `xput`, shell `http get` | `eviction model load ...`, `sched model load ...` |
| `/mnt/files/models/` | General model artifacts that are not directly policy blobs | Shell tooling, future host tooling | model- or accelerator-specific loaders |
| `/mnt/files/autoload/` | System-managed canonical autoload blob copies | `eviction model autoload set ...`, `sched model autoload set ...` | boot-time autoload replay |
| `/mnt/files/help/` | Boot-generated help text | kernel `help_init()` | shell `help` |

## Contract Rules

- Host-side blob uploads should default to `/mnt/files/policies/<name>`.
- Runtime activation commands consume files; they do not invent their own
  storage location.
- Autoload does not point at arbitrary operator files after `autoload
  set`. The kernel snapshots the validated source blob into the managed
  `/mnt/files/autoload/` area and records that canonical path plus the
  managed copy's size/checksum identity in `blob_autoload.conf`.
- `/mnt/files/autoload/` is system-managed. Operators should treat its
  contents as implementation detail, not as the primary place to upload
  or edit blobs.
- `scripts/tools/slm-modelctl.py` now enforces this contract for its
  managed blob flows:
  - `load`, `apply`, and `autoload-set` accept operator-managed blob
    paths only under `/mnt/files/policies/` or `/mnt/files/models/`
  - `/mnt/files/autoload/` is intentionally rejected there because it
    is reserved for kernel-managed canonical autoload copies
- Existing demo/test assets may still live elsewhere under `/mnt/files`
  for compatibility. This contract defines the standard destinations for
  new ingress and runtime-loading workflows.

## Current Boot Behavior

The kernel creates these standard directories at boot on the mounted
LittleFS instance:

- `/mnt/files/policies`
- `/mnt/files/models`
- `/mnt/files/autoload`
- `/mnt/files/help`

## Current Limitation

On current builds, `/mnt/files` is backed by a RAM disk, so this layout
is stable within a booted session but not yet non-volatile across full
reboots.
