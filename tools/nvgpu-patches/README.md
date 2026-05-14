# Patches against NVIDIA L4T `nvgpu.ko`

Local modifications to NVIDIA's out-of-tree `nvgpu.ko` (L4T R36.4.x)
that SLM-OS depends on for Jetson Orin Nano operation. Each patch
here describes a single behavior change and ships its own short
rationale at the top of the diff.

The patches are NOT upstreamed — they expose internal allocator
state that NVIDIA wouldn't ship to production users. They're
required only for SLM-OS's bare-metal kexec-handoff path; vanilla
Linux usage of the GPU doesn't need them.

## Patches

### `0001-expose-gr-ctx-phys-via-debugfs.patch`

Adds three debugfs files exposing the physical addresses of nvgpu's
GR-related buffers, so SLM-OS can reserve those pages from its PMM
*before* booting and avoid clobbering them when the inherited GPU
channel runs post-kexec. See the in-tree consumer at
`kernel/gpu/nvidia/ga10b_handoff_reserve.c` (the
`reserve_gr_ctx_extents` helper) for what SLM-OS does with the
addresses, and `#788` / `#823` for the wedge this works around.

The patch adds:

| Debugfs file | Contents |
|---|---|
| `/sys/kernel/debug/gpu.0/fifo/slmos_gr_ctx_phys` | Per-channel GR ctx buffer phys ranges (`chid tsgid idx phys size`) |
| `/sys/kernel/debug/gpu.0/fifo/slmos_global_ctx_phys` | Per-GPU global GR ctx buffers (golden image, attribute cb, priv access map, RTV circular, FECS trace) |
| `/sys/kernel/debug/gpu.0/fifo/slmos_falcon_ucode_phys` | FECS+GPCCS ctxsw ucode mirror (64 KB) |

It also adds one accessor in `drivers/gpu/nvgpu/common/gr/gr_falcon.c`
(plus its declaration in
`drivers/gpu/nvgpu/include/nvgpu/gr/gr_falcon.h`) that returns the
`nvgpu_mem *` for the GR falcon's ucode surface, so the new debugfs
file can read its phys.

Without this patch installed, SLM-OS's per-attempt kexec-handoff
PASS rate regresses from ~60% to the v6 baseline of ~17%.

## Building

The patches target NVIDIA's L4T R36.4.x kernel-OOT-modules tarball
(`kernel_oot_modules_src.tbz2`, ~13 MB), distributed as part of the
Linux for Tegra source bundle.

Run as root on the Jetson:

```bash
cd /root && mkdir -p oot-build && cd oot-build
tar -xjf /path/to/kernel_oot_modules_src.tbz2

# Apply this patch (only changes three files; round-trip-tested
# clean on the R36.4.x tarball).
patch -p1 -d nvgpu < /path/to/0001-expose-gr-ctx-phys-via-debugfs.patch

# Generate conftest.h — required even though the patch doesn't
# touch nvidia-oot, because nvgpu's Kbuild rules read from it.
mkdir -p out/nvidia-conftest/nvidia
cp -av nvidia-oot/scripts/conftest/* out/nvidia-conftest/nvidia/
make -j$(nproc) ARCH=arm64 \
    src=$PWD/out/nvidia-conftest/nvidia obj=$PWD/out/nvidia-conftest/nvidia \
    NV_KERNEL_SOURCES=/lib/modules/$(uname -r)/build \
    NV_KERNEL_OUTPUT=/lib/modules/$(uname -r)/build \
    -f $PWD/out/nvidia-conftest/nvidia/Makefile

# Reuse the system's nvidia-oot Module.symvers rather than
# building all of nvidia-oot.
cp /usr/src/nvidia/nvidia-oot/Module.symvers nvidia-oot/Module.symvers

# Build nvgpu only.
make -j$(nproc) ARCH=arm64 \
    -C /lib/modules/$(uname -r)/build \
    M=$PWD/nvgpu/drivers/gpu/nvgpu \
    CONFIG_TEGRA_OOT_MODULE=m \
    srctree.nvgpu=$PWD/nvgpu \
    srctree.nvidia=$PWD/nvidia-oot \
    srctree.nvidia-oot=$PWD/nvidia-oot \
    srctree.nvconftest=$PWD/out/nvidia-conftest \
    KBUILD_EXTRA_SYMBOLS=$PWD/nvidia-oot/Module.symvers \
    modules

# Install + reboot. Stock module preserved as `.stock` sibling.
cp /lib/modules/$(uname -r)/updates/nvgpu.ko \
   /lib/modules/$(uname -r)/updates/nvgpu.ko.stock
cp nvgpu/drivers/gpu/nvgpu/nvgpu.ko \
   /lib/modules/$(uname -r)/updates/nvgpu.ko
reboot
```

After reboot, verify the patched module is loaded:

```bash
cat /sys/kernel/debug/gpu.0/fifo/slmos_gr_ctx_phys
# Should print: "chid tsgid idx phys size" + per-channel rows.
# If the file doesn't exist, the stock module is still active.
```

## Gotchas

- **Do NOT pass `srctree.nvmap=...`.** The default
  (`srctree.nvidia/drivers/video/tegra/nvmap`) is what nvgpu's
  Kbuild expects; overriding it breaks with
  `nvmap_exports.h: No such file or directory`.
- **`nvsched` is a sibling of `drivers/`, not a child.** If
  conftest fails on `nvsched/Makefile.sources`, extract the
  top-level `nvsched/` directory from the tarball too.
- **Use the system-installed `Module.symvers`** at
  `/usr/src/nvidia/nvidia-oot/Module.symvers` (~117 KB). Rebuilding
  all of nvidia-oot is slow and unnecessary.

## Install status across the lab

| Host | Status | Verified |
|------|--------|----------|
| `jetson-nano-2` | Patched | yes (used by every PR #823 hardware run) |
| `jetson-nano-1` | Unknown | check before relying on the 60% PASS baseline |

Without the patched module, SLM-OS still boots and reaches its
shell, but `nvgpu launch-kernel` per-attempt PASS regresses to the
~17% v6 baseline. The retry orchestrator
(`scripts/jetson-deploy-retry.sh`) will hit its 4-retry budget
roughly half the time at that rate.

## Long-term plan

This patch + the consumer reservation logic in
`kernel/gpu/nvidia/ga10b_handoff_reserve.c` are a workaround. The
permanent fix is tracked in **#819** ("Jetson kexec dispatch
reliability: replace retry orchestrator with deterministic GR-ctx
handoff") — when that lands, both this patch and the retry
orchestrator become obsolete.
