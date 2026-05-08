# docs/reference/ — relocated

The third-party reference material previously kept here (NVIDIA L4T nvgpu,
Mesa NVK, nouveau, HailoRT, Linux kernel, RPi firmware, ARM Trusted Firmware,
etc.) now lives in a local-only reference library at `~/slmos-ref/`.

## Why

The reference material is third-party source mirrored for offline lookup
during SLM-OS development. Keeping it in a public repository was
unnecessary — it inflates clone size, complicates licensing posture, and
isn't useful to anyone outside the SLM-OS development workflow. It also
no longer needs version control, since it's only ever read.

## How to use

The cache is expected at `~/slmos-ref/` on the developer's machine. It is
**not** a git repo — just a flat reference tree maintained outside the
public source.

In-tree citations use the form
`~/slmos-ref/<vendor>/<filename>:<line>`.

Vendor folders in the cache: `nvidia/`, `nouveau/`, `mesa/`, `hailo/`,
`tegra-l4t/`, `linux/`, `rpi/`, `circle/`, `uboot/`, `kexec/`, `tf-a/`.
SLM-OS-authored investigation notes and lab traces live under
`derivatives/notes/`, `derivatives/hailort-traces/`, and
`derivatives/shaders/`.

See `~/slmos-ref/README.md` for the full layout and contents.

## For CC agents

See the top-level `CLAUDE.md` "Reference File Cache" section for the
fetch-cache-first rule and naming conventions when adding new upstream
material.
