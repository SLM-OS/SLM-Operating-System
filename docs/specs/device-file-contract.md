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

The kernel creates these standard directories at boot on the mounted
LittleFS instance:

- `/mnt/files/policies`
- `/mnt/files/models`
- `/mnt/files/autoload`
- `/mnt/files/help`

## Current Limitation

`/mnt/files` itself is still a boot-session workspace, not yet the
finished persistent general-purpose store.

For policy replacement, that is no longer the critical limitation:

- authoritative managed autoload blobs and `blob_autoload.conf` now
  persist on the boot FAT volume under `0:/slmstore/` when boot media is
  available
- `/mnt/files` remains the standard writable ingress root for uploads,
  staging, and host-tool flows

So the device-local contract remains stable for operators, while the
authoritative persistence path for boot-managed runtime blobs is now
separate and persistent.
