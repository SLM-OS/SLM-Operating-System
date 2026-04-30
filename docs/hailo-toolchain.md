# Hailo HEF Compilation Toolchain

Compiling a model into Hailo's HEF (Hailo Executable Format) is a one-time
step performed on the developer workstation. The resulting `.hef` file is
then embedded into the SLM-OS kernel at build time via `USER_HEF_BLOB=` or
`SCHEDULER_HEF_BLOB=`. The Hailo Dataflow Compiler (DFC) is **not** required
to build SLM-OS — only to produce or update a HEF.

## Prerequisites

### 1. Hailo Developer Account

HEF compilation requires the Hailo Dataflow Compiler, which is proprietary
and only available through the Hailo developer portal:

- <https://hailo.ai/developer-zone/>

Register for an account (free for non-commercial use), then download:

- `hailo_dataflow_compiler-3.33.1-py3-none-linux_x86_64.whl`
- Optionally `hailo_model_zoo-2.18.0-py3-none-any.whl` (prebuilt recipes for
  MobileNet, ResNet, YOLO, etc.)

**Version pinning matters.** For the Hailo-8 and Hailo-8L chips (including
the AI HAT+), use **DFC 3.33.1**. The DFC 5.x line targets Hailo-10H and
produces HEFs incompatible with our firmware (v4.23).

### 2. Host Environment

- Linux x86-64 (DFC does not run on ARM)
- Python 3.10 (DFC 3.33.1 is built against this exact version)
- ~5 GB disk space for the venv (TensorFlow / CUDA runtime dependencies)
- 8+ GB RAM for compilation of models larger than a few MB

### 3. Python venv

```bash
python3.10 -m venv venv
source venv/bin/activate
pip install /path/to/hailo_dataflow_compiler-3.33.1-py3-none-linux_x86_64.whl
# Optional, if you want to use Hailo Model Zoo recipes:
pip install /path/to/hailo_model_zoo-2.18.0-py3-none-any.whl
```

Verify the install:

```bash
source venv/bin/activate
hailo --version
# expected: some lines starting [info] and a version like 3.33.1
```

## The Compilation Pipeline

DFC compiles a model in three phases. `scripts/hailo/compile_hef.sh` wraps
all three:

1. **Parser** — `hailo parser onnx model.onnx` → `model.har`
   Reads the ONNX graph and produces a frozen intermediate representation
   (HAR = Hailo Archive).

2. **Optimize** — `hailo optimize model.har --calib-set-path calib.npy`
   → `model_optimized.har`
   Runs calibration inference on a representative dataset to compute
   per-layer scale/zero-point values, then quantizes the graph to INT8.
   This phase dominates compile time (can take minutes for big models).

3. **Compile** — `hailo compiler model_optimized.har --hw-arch hailo8l`
   → `model.hef`
   Emits the final binary: quantized weights as CCW (compressed weight
   stream) bytes + per-context action sequences that drive the NPU during
   inference.

### Calibration Data

A calibration dataset is required — without it, `hailo optimize` defaults
to synthetic random data that degrades accuracy significantly. The dataset
is a `.npy` file whose shape matches the model's input tensor:

- **MNIST**: shape `[N, 1, 28, 28]` or `[N, 28, 28, 1]` (NCHW vs NHWC),
  dtype `float32` or `uint8`. 64-1024 samples from the MNIST training set
  is typical.
- **Scheduler MLP**: shape `[N, 108]`, dtype `float32`. Samples of
  recently-observed scheduler state vectors.
- **ResNet/MobileNet/YOLO**: shape `[N, 224, 224, 3]` (or 3×224×224),
  dtype `uint8`. 64+ samples from ImageNet validation.

Helpers in the repo:

- `scripts/hailo/generate_calibration_data.py` — scheduler MLP state
  vectors. Pi 5 / Jetson variants.
- `scripts/hailo/generate_mnist_calibration.py` — MNIST image samples.

Quantization is sensitive to the calibration set. Input that doesn't reflect
the inference-time distribution causes systematic accuracy loss that no
amount of downstream tuning can recover.

## Running `compile_hef.sh`

The generalized wrapper accepts any ONNX path + calibration dataset:

```bash
source venv/bin/activate
scripts/hailo/compile_hef.sh \
    --onnx models/test/mnist.onnx \
    --calib build/hailo/mnist_calib.npy
```

Flags:

| Flag | Required | Default | Purpose |
|---|---|---|---|
| `--onnx <path>` | yes | — | ONNX model file |
| `--calib <path>` | yes | — | Calibration dataset `.npy` |
| `--name <str>` | no | ONNX basename | HEF filename + network-id |
| `--arch <str>` | no | `hailo8l` | Target chip (`hailo8` or `hailo8l`) |
| `--outdir <path>` | no | `build/hailo` | Where the HEF lands |
| `--keep-intermediates` | no | off | Retain `.har` files (useful for debug) |

Output lands at `<outdir>/<name>.hef`.

## Consuming the HEF in a SLM-OS Build

Embed at build time via one of two flags. These are independent; either or
both can be set per build.

### Per-run ad-hoc HEF — `USER_HEF_BLOB`

```bash
make kernel PLATFORM=RASPI5 \
    USER_HEF_BLOB=build/hailo/mnist.hef \
    HAILO_FW_BLOB=/usr/lib/firmware/hailo/hailo8_fw.4.23.0.bin
```

The bytes land at `/mnt/files/user.hef` at boot. Accessible via:

- Shell: `hailo load /mnt/files/user.hef sched`
- Lua: `slm.hailo.load("/mnt/files/user.hef")`

This is the right slot for experimentation and the Phase 8 demo. Swap the
HEF just by rebuilding with a different path.

### Scheduler MLP HEF — `SCHEDULER_HEF_BLOB`

```bash
make kernel PLATFORM=RASPI5 AI_SCHED=ON \
    SCHEDULER_HEF_BLOB=build/hailo/scheduler_mlp_pi5.hef \
    HAILO_FW_BLOB=/usr/lib/firmware/hailo/hailo8_fw.4.23.0.bin
```

The bytes land at `/mnt/files/scheduler_mlp.hef` at boot. The AI scheduler
policy `ai_hailo` loads this automatically on policy activation so
scheduling decisions route through the NPU.

## End-to-End Example: MNIST

```bash
source venv/bin/activate

# 1. Produce a calibration dataset from MNIST
python3 scripts/hailo/generate_mnist_calibration.py \
    --output build/hailo/mnist_calib.npy \
    --samples 128

# 2. Compile
scripts/hailo/compile_hef.sh \
    --onnx models/test/mnist.onnx \
    --calib build/hailo/mnist_calib.npy

# 3. Build SLM-OS with the HEF embedded
make kernel-clean
make kernel PLATFORM=RASPI5 \
    USER_HEF_BLOB=build/hailo/mnist.hef \
    HAILO_FW_BLOB=/usr/lib/firmware/hailo/hailo8_fw.4.23.0.bin

# 4. Deploy + run (shell commands once pi-5-1 is up)
#    slm> hailo probe
#    slm> hailo boot
#    slm> hailo load /mnt/files/user.hef sched
#    slm> lua /mnt/files/demo_hailo.lua /mnt/files/user.hef 100
```

## Troubleshooting

### `hailo parser onnx` fails with "unsupported op"

DFC supports a subset of ONNX ops. Common unsupported ops:
- Custom ops not in the mainstream opset.
- Dynamic shapes. Input must be fully static.
- Opset version newer than DFC supports (3.33.1 ≈ opset 17).

Fix: re-export the model with a lower opset
(`torch.onnx.export(..., opset_version=13)`), or rewrite the offending op in
terms of supported primitives.

### `hailo optimize` degrades accuracy

Calibration dataset likely doesn't match the inference distribution. Try:
- More samples (128-1024).
- Samples from the actual target task, not random noise.
- Dtype matches what the runtime will pass (usually `uint8` for images, not
  `float32`).

### `hailo compiler` fails with "resources exhausted"

Model is too large for the target chip at the requested optimization level.
Options:
- Drop `--performance-mode`, or use `--compression-level 2` or 3.
- Split the model (manual layer partitioning — outside the scope of this
  doc).

### HEF loads on pi-5-1 but inference returns garbage

Quantization error. Verify by running the `_compiled.har` artifact back
through DFC's emulator (`hailo-runner`) and comparing outputs against the
reference ONNX.

### DFC version mismatch vs firmware

Our embedded Hailo-8 firmware is v4.23 (`/usr/lib/firmware/hailo/
hailo8_fw.4.23.0.bin`). HEFs compiled with DFC versions outside the 3.x
track may target a different firmware ABI and will be rejected at load.
Stick to DFC 3.33.1 for Hailo-8/8L.

## Further Reading

- HailoRT source reference (cached): `../slmos-reference-cache/hailo/hailort-*.cpp`,
  `../slmos-reference-cache/hailo/hailo-hef-parser-head.cpp`
- v4.23 wire-format capture: `../slmos-reference-cache/derivatives/hailort-traces/hailort-v4.23.0-wire-capture-
  mobilenet.txt`
- Phase 8 / multi-context blocker: #347
- Pi 5 AI HAT+ phase plan: `docs/pi5-ai-hat-plan.md`
