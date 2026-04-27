# Dev Environment Setup (Ubuntu)

Step-by-step guide for standing up an SLM-OS development environment on
Ubuntu. Ubuntu 24.04 LTS is the reference distribution; 22.04 LTS is
known to work. Other Linux distributions are not covered — the tools
below are available on most of them, but package names and paths vary.

For anything beyond *installing* the toolchains — how to build, how to
run, how to debug — see `docs/getting-started.md`.

---

## Prerequisites

- Ubuntu 24.04 LTS (or 22.04 LTS)
- `sudo` access
- Internet connection
- ~5 GB of free disk space for toolchains and build artifacts

---

## 1. System Packages

Install the build tools, host compilers, emulators, and helpers needed
by every SLM-OS target:

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    curl wget \
    gdb-multiarch \
    qemu-system-arm \
    qemu-system-x86 \
    grub-pc-bin \
    grub-common \
    xorriso \
    mtools \
    gdisk
```

What each piece provides:

| Package | Role in SLM-OS |
|---|---|
| `build-essential`, `cmake`, `ninja-build` | Host build tools. CMake 3.20+ is required; Ubuntu 24.04 ships 3.28. |
| `gdb-multiarch` | Multi-arch GDB for remote debugging kernels under QEMU. |
| `qemu-system-arm` | `qemu-system-aarch64` — primary ARM64 dev target. |
| `qemu-system-x86` | `qemu-system-x86_64` — x86-64 QEMU target. |
| `grub-pc-bin`, `grub-common`, `xorriso` | `grub-mkrescue` — builds the x86-64 test ISO. |
| `mtools`, `gdisk` | `mformat`/`mcopy`/`sgdisk` — build the UEFI disk image for bare-metal x86-64 boots. |

The host `gcc` from `build-essential` is also SLM-OS's x86-64 kernel
cross-compiler (the kernel uses `x86_64-linux-gnu-gcc` in freestanding
mode — no extra package needed).

---

## 2. ARM64 Bare-Metal Toolchain

ARM64 kernel targets (`QEMU_VIRT`, `RASPI5`, `JETSON_ORIN_NANO`) build
with `aarch64-none-elf-gcc`, the ARM-provided **bare-metal** cross-
compiler. Ubuntu's packaged `gcc-aarch64-linux-gnu` is **not**
suitable — it targets Linux userspace, not bare metal.

Download the latest `x86_64` Linux tarball for the **AArch64 bare-metal
target (`aarch64-none-elf`)** from ARM's release page:

https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads

Install to `/opt/arm-gnu-toolchain`:

```bash
cd /tmp
TARBALL=arm-gnu-toolchain-14.2.rel1-x86_64-aarch64-none-elf.tar.xz  # adjust to current release
wget https://developer.arm.com/-/media/Files/downloads/gnu/14.2.rel1/binrel/${TARBALL}
sudo mkdir -p /opt/arm-gnu-toolchain
sudo tar -xf ${TARBALL} -C /opt/arm-gnu-toolchain --strip-components=1
```

PATH configuration is handled in §4.

---

## 3. Rust Toolchain

SLM-OS uses a stable Rust toolchain with two bare-metal targets and the
`rust-src` component.

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
source "$HOME/.cargo/env"

rustup target add aarch64-unknown-none
rustup target add x86_64-unknown-none
rustup component add rust-src
```

`aarch64-unknown-none` and `x86_64-unknown-none` provide the `core` and
`alloc` crates precompiled for each target triple. `rust-src` lets
Cargo rebuild them when the project uses `build-std` features.

---

## 4. PATH Configuration

Append both toolchains to the shell PATH. For interactive Bash shells:

```bash
echo 'export PATH="/opt/arm-gnu-toolchain/bin:$HOME/.cargo/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

For zsh, use `~/.zshrc`. For login-shell coverage, append the same line
to `~/.profile`.

---

## 5. Verify

The project's Makefile has a `check-tools` target that validates every
tool the build system expects:

```bash
cd /path/to/CS-496-Capstone-SLM-Operating-System
make check-tools
```

Any missing tool is reported with its expected command name. A clean
pass looks like:

```
All required tools found.
```

Individual version spot-checks, if needed:

```bash
aarch64-none-elf-gcc --version   # ARM cross-compiler
gcc --version                    # host / x86-64 cross-compiler
rustc --version                  # Rust ≥ 1.80 recommended
cmake --version                  # must be ≥ 3.20
qemu-system-aarch64 --version    # ≥ 7.0 recommended
qemu-system-x86_64 --version
grub-mkrescue --version
```

---

## 6. First Build

A quick sanity build confirms the toolchain is wired correctly:

```bash
make kernel          # QEMU_VIRT (ARM64), default
make run             # Build and boot to shell in QEMU
```

Ctrl+A, X exits QEMU.

For all other platforms and targets — Pi 5, Jetson Orin Nano, x86-64
bare-metal, the test suite, AI scheduler builds — see
`docs/getting-started.md`.

---

## Hardware Lab Access (Optional)

Pi 5 and Jetson hardware workflows run through **labctl**
(`../Embedded-Lab-Control`), a separate project that owns SD-card
flashing, power cycling, and serial capture. Ubuntu setup for labctl is
documented in that repository's README; it is not needed to work on
SLM-OS under QEMU.

---

## Hailo Toolchain (Optional — Phase 6 / Pi 5 AI HAT+)

Needed only when rebuilding the scheduler MLP as a `.hef` for Hailo-8/8L
inference, or compiling any other ONNX model for the AI HAT+. Skip this
section for QEMU-only work.

### 1. System packages (Python 3.10 + native build deps)

The Hailo Dataflow Compiler (DFC) 3.33.1 is pinned to **Python 3.10** —
3.12 (Ubuntu 24.04 default) and 3.11 are not supported. Install 3.10
alongside the system Python, plus the C headers `pygraphviz` needs:

```bash
sudo apt install -y \
    python3.10 \
    python3.10-venv \
    python3.10-dev \
    libgraphviz-dev \
    graphviz \
    pkg-config
```

Ubuntu 22.04 ships Python 3.10 as its default `python3` — no extra repo
needed. On Ubuntu 24.04 (default `python3` = 3.12), Python 3.10 comes
from the `deadsnakes` PPA: `sudo add-apt-repository ppa:deadsnakes/ppa`
first.

### 2. Get the Hailo wheels

Register at [Hailo Developer Zone](https://hailo.ai/developer-zone/) (free for
non-commercial use) and download **from the Hailo-8/8L track**:

- `hailo_dataflow_compiler-3.33.1-py3-none-linux_x86_64.whl` (AMD64 / x86-64)
- `hailo_model_zoo-*.whl` (same track; depends on DFC)

Do **not** use the 5.x DFC track — that's for Hailo-10H and produces
binaries incompatible with the AI HAT+ silicon.

### 3. Create the venv + install

From the SLM-OS repo root (a `venv/` at the root is gitignored):

```bash
python3.10 -m venv venv
source venv/bin/activate
pip install --upgrade pip
pip install /path/to/hailo_dataflow_compiler-3.33.1-py3-none-linux_x86_64.whl
pip install /path/to/hailo_model_zoo-*.whl
```

Verify:

```bash
hailo --version     # expect "Hailo DFC Version: 3.33.1"
```

The GPU-driver / CUDNN warnings on first run are harmless for MLP-sized
models — CPU compilation is plenty fast.

### 4. Compile the scheduler MLP

With the venv active:

```bash
python3 scripts/hailo/export_scheduler_mlp_onnx.py     # ai_weights_mlp.c -> .onnx
python3 scripts/hailo/generate_calibration_data.py     # synthesize quantization set
bash   scripts/hailo/compile_hef.sh --arch hailo8 --variant pi5
# Output: build/hailo/scheduler_mlp_pi5.hef
```

For Jetson (42-action): `--variant jetson`. For Hailo-8L silicon:
`--arch hailo8l`.

To wipe all Hailo toolchain artifacts (`.onnx`, `.npy`, `.har`, `.hef`,
DFC logs) out of `build/hailo/`:

```bash
make hailo-clean
```

### 5. Embed the `.hef` in the kernel (Phase 6.2)

To make the compiled scheduler MLP reachable by the in-kernel `hailo
load <path> sched` path, pass both the Hailo firmware blob and the
compiled `.hef` when building the kernel:

```bash
make kernel AI_SCHED=ON PLATFORM=RASPI5 \
    HAILO_FW_BLOB=/path/to/hailo8_fw.bin \
    SCHEDULER_HEF_BLOB=$(pwd)/build/hailo/scheduler_mlp_pi5.hef
```

At boot, `kernel/src/sched_hef_init.c` writes the embedded bytes into
`/mnt/files/scheduler_mlp.hef`. Then from the SLM-OS shell:

```
slmos> hailo probe
slmos> hailo boot
slmos> hailo load /mnt/files/scheduler_mlp.hef sched
slmos> sched policy ai_hailo
```

Without `SCHEDULER_HEF_BLOB` set, the stub `sched_hef_init()` is a
no-op — zero kernel-image impact for builds that don't embed. The
firmware blob (`hailo8_fw.bin`) is distributed with the Linux HailoRT
driver and is open-redistribution licensed.

---

## Jetson SD-Card Layout

When SLM-OS boots on a Jetson Orin Nano (Super Dev Kit) the LittleFS
partition mounted at `/mnt/files` carries every blob the kernel needs
beyond the kernel image itself. Standard layout:

```
/mnt/files/
├── blob_autoload.conf                          # registry of blobs to autoload
├── scheduler_mlp.hef                           # AI scheduler weights (Hailo-compiled)
├── models/
│   └── *.onnx                                  # Phase-5 ONNX vision models
└── qwen2.5-1.5b-instruct-q4_k_m.gguf           # SLM weights — Qwen2.5-1.5B Q4_K_M (~1.0 GB)
```

The Qwen GGUF is the M5 demo target for `slm load /mnt/files/...`. It
is **not** embedded in the kernel image (kernel-image budget is 32 MB;
the GGUF is ~1 GB). Staging is done through labctl rather than
direct `mount`/`cp`/`umount` (see `CLAUDE.md` "labctl is the only
hardware interface"). The current high-level flow:

```bash
# Fetch + verify on the dev host
scripts/fetch-slm.sh

# Switch the SDWire to the labctl host, place the GGUF onto the
# LittleFS partition mounted at /mnt/files via the appropriate
# labctl sdwire subcommand (see docs/lab-operations.md for the
# current syntax — sdwire_update / sdwire_to_host / sdwire_to_dut),
# then return the SD card to the DUT and reboot:
labctl power cycle jetson-nano-1
```

For background on the GGUF format and how SLM-OS parses it, see
`docs/tutorials/slm-models.md`. For the `model_mem` pool sizing on
Jetson (2 GB weight + 256 MB workspace), see
`docs/specs/slm-integration.md` §"Memory Plan".

---

## Troubleshooting

**`aarch64-none-elf-gcc: command not found`** — the ARM toolchain is
installed but not on PATH. Confirm `/opt/arm-gnu-toolchain/bin/aarch64-none-elf-gcc`
exists, then re-check §4.

**`cmake: command not found`** — the `cmake` package was not installed,
or the shell has not been reopened since installing it. Re-run the
`apt install` in §1 and open a fresh shell.

**`rustup: command not found` after install** — `~/.cargo/bin` is not on
PATH. Either restart the shell or `source ~/.cargo/env`.

**`error: failed to run custom build command for core`** — `rust-src`
component is missing. Re-run `rustup component add rust-src`.

**`grub-mkrescue: error: ... /usr/lib/grub/i386-pc not found`** — the
`grub-pc-bin` package was skipped. It is required even on 64-bit hosts
because the x86-64 ISO uses a BIOS-compatible GRUB stage.

**Old toolchain versions cause build failures** — the project tracks
modern compilers. If Ubuntu's packages are too old, upgrade the
distribution rather than pinning older SLM-OS commits.

**Hailo `pip install` fails with `Python.h: No such file or directory`** —
the `pygraphviz` C extension needs Python headers. Install
`python3.10-dev` (see §Hailo Toolchain step 1). `libgraphviz-dev` is
needed for the graphviz wrapper itself.

**Hailo `pip install` fails on Python 3.12** — DFC 3.33.1 is pinned to
Python 3.10. Recreate the venv with `python3.10 -m venv venv`.

**`hailo` CLI errors out with `unrecognized arguments`** — DFC 5.x uses
different CLI flags than 3.x. Confirm `hailo --version` reports 3.33.1;
if it reports 5.x, the wrong wheel track was installed (5.x is
Hailo-10H, not Hailo-8/8L).

---

*Last updated: 27 April 2026*
