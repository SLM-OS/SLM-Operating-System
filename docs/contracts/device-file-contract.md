# Device-Local File Contract

This document defines the standard writable layout under `/mnt/files`
for host tooling, runtime blob activation, and boot-managed copies.

`/mnt/files` remains the single device-local ingress root.

## Standard Paths

| Path | Purpose | Producer | Consumer |
|---|---|---|---|
| `/mnt/files/policies/` | Operator-supplied runtime blob files | `slm-put.py`, `slm-modelctl.py`, shell `put` / `xput`, shell `http get` | `eviction model load ...`, `sched model load ...` |
| `/mnt/files/models/` | General model artifacts that are not directly policy blobs | Shell tooling, future host tooling | model- or accelerator-specific loaders |
| `/mnt/files/autoload/` | Fallback system-managed autoload blob area when persistent boot FAT storage is unavailable | `eviction model autoload set ...`, `sched model autoload set ...` | boot-time autoload replay fallback |
| `/mnt/files/help/` | Boot-generated help text | kernel `help_init()` | shell `help` |

## Contract Rules

- Host-side blob uploads should default to `/mnt/files/policies/<name>`.
- Runtime activation commands consume files; they do not invent their own
  storage location.
- Autoload does not point at arbitrary operator files after `autoload
  set`. The kernel snapshots the validated source blob into a
  system-managed canonical store and records that canonical path plus
  the managed copy's size/checksum identity in `blob_autoload.conf`.
- When boot FAT storage is available, the authoritative managed store is
  persistent and lives under `0:/slmstore/autoload/` with the config at
  `0:/slmstore/blob_autoload.conf`.
- `/mnt/files/autoload/` remains reserved as a system-managed fallback
  area. Operators should treat both it and `0:/slmstore/autoload/` as
  implementation detail, not as primary upload targets.
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

The kernel mounts `/mnt/files` as LittleFS:

- preferably from the persistent boot-FAT-backed image
  `0:/slmstore/files.lfs`
- otherwise from the legacy RAM-backed fallback when boot media is
  unavailable or the persistent store cannot be mounted

On the mounted LittleFS instance, the kernel creates these standard
directories at boot:

- `/mnt/files/policies`
- `/mnt/files/models`
- `/mnt/files/autoload`
- `/mnt/files/help`

## Persistence Semantics

When boot FAT storage is available, ordinary files written under
`/mnt/files` now persist through the LittleFS image at
`0:/slmstore/files.lfs`.

When boot FAT storage is not available, `/mnt/files` falls back to the
legacy RAM-backed LittleFS path and changes do not persist across
reboot.

The authoritative policy-autoload store remains separate:

- managed autoload blobs and `blob_autoload.conf` persist under
  `0:/slmstore/`
- `/mnt/files` remains the standard writable ingress root for uploads,
  staging, and host-tool flows

So the device-local contract remains stable for operators, while the
authoritative persistence path for boot-managed runtime blobs is now
separate and persistent.
