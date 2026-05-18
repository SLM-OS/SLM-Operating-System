# Jetson — channel-inherit helper setup

The `kexec` path from Linux to SLM-OS on Jetson Orin Nano requires a userspace **helper binary** running on Linux that owns a GPU channel, then "hands off" that channel to SLM-OS by publishing a magic-tagged dmabuf containing the channel's physical addresses. See `docs/deploy/jetson-kexec.md` for the overall flow.

This document covers the per-Jetson helper-directory setup that `slmos-kexec` expects.

## What lives in the helper directory

`scripts/jetson-kexec-slmos.sh` starts up to two helpers immediately before `kexec -e`:

- `gpu-kernel-mnist` — owns the GPU channel that runs the MNIST conv pipeline post-kexec (the primary smoke test for `nvgpu launch-kernel`).
- `gpu-kernel-sched-mlp` — owns a second channel for the AI-scheduler MLP policy (optional; absent on dev kits without sched weights).

Both helpers are passed `--preserve-for-kexec`, which makes them publish a `struct ga10b_channel_handoff` to DRAM at a kind-tagged magic address that SLM-OS's DRAM scanner picks up after kexec.

`slmos-kexec` looks for them in `$SLMOS_HELPER_DIR`, defaulting to **`/root/gpu-mnist`** on the Jetson. The expected layout:

```
/root/gpu-mnist/
├── gpu-kernel-mnist                          (compiled helper binary)
├── gpu-kernel-sched-mlp                      (optional)
├── conv2d_fp32_direct_shader.sass            \
├── add_bias_relu_fp32_shader.sass            |  MNIST shaders, loaded
├── maxpool2d_fp32_shader.sass                |  at runtime by the helper
├── gemm_fp32_shader.sass                     |  via --shader-dir
├── gemm_hmma_fp32a_fp16w_shader.sass         /
├── mnist-weights/                            (W_conv1.bin, etc.)
└── sched-weights/                            (optional, MLP weights)
```

## Step 1: Build the SASS shader blobs

The MNIST helper opens five `*_shader.sass` files at runtime — these are raw GA10B SASS `.text` segments extracted from cubins. They're produced by compiling the matching `.cu` sources in `scripts/cuda/` with `nvcc -arch=sm_87`, then carving the SASS bytes out via `cuobjdump --extract-elf` + `readelf -SW`.

These files are deliberately **not committed** to the repo (see `.gitignore`); regenerate them whenever a `.cu` source changes.

### Option A — Cross-build on the dev machine (preferred)

Any x86_64 Linux box with the NVIDIA CUDA Toolkit installed produces SASS identical to a Jetson-native build. One toolkit install services every Jetson in the lab.

```bash
# One-time toolkit install on the dev machine:
sudo apt install nvidia-cuda-toolkit
# OR: official .deb from https://developer.nvidia.com/cuda-downloads
#     (pick "x86_64 Linux" — cross-builds to sm_87 just fine)

# Build the shaders:
scripts/cuda/build-mnist-shaders.sh
# → build/cuda/mnist-shaders/*.sass
```

The script auto-detects `nvcc` / `cuobjdump` from `$PATH` or `/usr/local/cuda/bin`. Override the output directory by passing it as `$1`. `ARCH=sm_xx` env var overrides the target (default `sm_87` = Orin Nano/NX/AGX).

### Option B — Native build on the Jetson

L4T provisions `/usr/local/cuda` already, so the same script runs unchanged. Useful when a dev machine isn't around or to verify reproducibility.

```bash
ssh root@<JETSON_IP>
git clone ... slmos
slmos/scripts/cuda/build-mnist-shaders.sh /root/gpu-mnist
```

## Step 2: Build the helper binary

`gpu-kernel-mnist` and `gpu-kernel-sched-mlp` are C programs that link against `libnvgpu`-style userspace API exposed by L4T's nvgpu driver. They need to be built **on a Jetson** (or against an L4T sysroot) because they `#include <linux/types.h>` from the nvgpu uapi headers and call NVGPU-specific ioctls.

```bash
ssh root@<JETSON_IP>
cd slmos/scripts
gcc -O2 -o /root/gpu-mnist/gpu-kernel-mnist \
    gpu-kernel-mnist.c gpu-launch-common.c -lpthread
gcc -O2 -o /root/gpu-mnist/gpu-kernel-sched-mlp \
    gpu-kernel-sched-mlp.c gpu-launch-common.c -lpthread
```

`gpu-launch-common.c` ships the helper-side handoff publishing code (the v7 / v8 / v9 / v10 struct writers and the magic-tagged dmabuf scanner). Match the .c sources against `kernel/gpu/nvidia/ga10b_channel_handoff.h` in this repo — they share that header verbatim (the helper copies it into its build directory).

## Step 3: Stage weights

Both helpers load model weights from on-disk `.bin` files in their respective sub-directories. These are produced by the Python `extract-weights` scripts in `scripts/` against trained PyTorch checkpoints.

```bash
# On the dev machine, generate per-tensor .bin files:
python3 scripts/mnist-extract-weights.py --out /tmp/mnist-weights/
scp -r /tmp/mnist-weights root@<JETSON_IP>:/root/gpu-mnist/

# Optional — sched MLP weights:
python3 scripts/sched-mlp-extract-weights.py --out /tmp/sched-weights/
scp -r /tmp/sched-weights root@<JETSON_IP>:/root/gpu-mnist/
```

`gpu-kernel-mnist` falls back to an empty channel (no compute pre-staged) if `mnist-weights/` is missing — SLM-OS's `nvgpu launch-kernel` smoke test still works on the channel infrastructure alone. But running the full MNIST forward pass needs the weights present.

## Step 4: scp the SASS blobs

```bash
# From the dev machine after Step 1:
scp build/cuda/mnist-shaders/*.sass \
    root@<JETSON_IP>:/root/gpu-mnist/
```

## Step 5: Verify

```bash
ssh root@<JETSON_IP> 'ls -la /root/gpu-mnist/'
# Expect: gpu-kernel-mnist (executable), 5 *_shader.sass files,
#         mnist-weights/ (optional), sched-weights/ (optional)
```

Then `slmos-kexec /root/slmos.elf` from Linux will pick up the helpers and publish their channel handoffs before kexec'ing into SLM-OS.

## Patched nvgpu.ko (separate but related)

For v10 handoff publishing (per-channel GR context buffer phys ranges — needed to stop SLM-OS PMM from clobbering FECS save buffers post-kexec), the helper reads from `/sys/kernel/debug/gpu.0/fifo/slmos_gr_ctx_phys`. That debugfs file is exposed by a patched `nvgpu.ko` — see `tools/nvgpu-patches/README.md` for the patch + install recipe.

A helper running against a stock `nvgpu.ko` falls back to a v9 publish (weights extents only), which still works but leaves the GR-ctx-clobber wedge in place. See `#788` for the wedge symptoms.

Per-host install state for the patched module is tracked in the lab notes; check there before assuming a Jetson has it.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `./conv2d_fp32_direct_shader.sass: No such file or directory` (helper log) | Step 1 not done; missing shader blobs in helper directory |
| `slmos.elf` kexec'd cleanly, but `nvgpu inherit` reports `no handoff found` | Helper wasn't running at kexec time, or it crashed before publishing. `tail /tmp/gpu-kernel-mnist.log` on Linux side |
| `nvgpu inherit` finds handoff, but `nvgpu oplib stage` fails with `no handoff and FECS_CURRENT_CTX read failed` | Old helper binary with `inst_block_phys=0` (FECS read failure). Rebuild against current `gpu-launch-common.c` (Step 2) |
| Helper crashes with `Failed to read /sys/kernel/debug/gpu.0/fifo/slmos_gr_ctx_phys` | Patched nvgpu.ko not installed — see `tools/nvgpu-patches/README.md` |

## Reference: directory contents on the canonical `jetson-nano-2`

For comparison when bringing up a fresh Jetson:

```
$ ls /root/gpu-mnist/ | grep -vE '\.(cubin|bak|prev|prev2|c|h|cu)$'
add_bias_relu_fp32
add_bias_relu_fp32_shader.sass
build-shaders.sh        # legacy local copy — superseded by scripts/cuda/build-mnist-shaders.sh
conv2d_fp32_direct
conv2d_fp32_direct_shader.sass
cuda/                   # symlink or copy of repo's scripts/cuda/
gemm_fp32
gemm_fp32_shader.sass
gemm_hmma_fp32a_fp16w_shader.sass
gpu-kernel-mnist
gpu-kernel-sched-mlp
maxpool2d_fp32
maxpool2d_fp32_shader.sass
mnist-weights/
sched-weights/
```

The `add_bias_relu_fp32`, `conv2d_fp32_direct`, etc. without an extension are the host-stub binaries `nvcc` emits — they aren't consumed at runtime and can be deleted after the `_shader.sass` files are extracted.
