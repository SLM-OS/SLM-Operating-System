# docs/reference/ — relocated

The third-party reference material previously kept here (NVIDIA L4T nvgpu,
Mesa NVK, nouveau, HailoRT, Linux kernel, RPi firmware, etc.) has moved
to a **private companion repo**: `SLM-OS/slmos-reference-cache`.

## Why

The reference material is third-party source mirrored for offline lookup
during SLM-OS development. Keeping it in a public repository was
unnecessary — it inflates clone size, complicates licensing posture, and
isn't useful to anyone outside the SLM-OS development workflow.

## How to use

Clone the private repo as a sibling directory next to this one:

```
parent-dir/
├── SLM-Operating-System/      ← this repo (public)
└── slmos-reference-cache/     ← private companion
```

In-tree citations use the form
`../slmos-reference-cache/<vendor>/<filename>:<line>`.
They resolve correctly when both repos are siblings.

Vendor folders in the cache: `nvidia/`, `nouveau/`, `mesa/`, `hailo/`,
`tegra-l4t/`, `linux/`, `rpi/`, `circle/`, `uboot/`, `kexec/`.
SLM-OS-authored investigation notes and lab traces live under
`derivatives/notes/`, `derivatives/hailort-traces/`, and
`derivatives/shaders/`.

See `../slmos-reference-cache/README.md` for the full layout and contents.

## For CC agents

See the top-level `CLAUDE.md` "Reference File Cache" section for the
fetch-cache-first rule and naming conventions when adding new upstream
material.
