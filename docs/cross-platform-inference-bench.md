# Cross-Platform Inference Benchmark

Capstone deliverable (Jetson execution plan §G5). Reproducible
inference numbers across the four targets SLM-OS supports today —
QEMU ARM64, Raspberry Pi 5, Jetson Orin Nano, and x86-64.

**Last updated:** 2026-04-14.

---

## Purpose

SLM-OS is positioned as a small-language-model *operating system*,
not a model library. The G-track kernels (G1 NEON MatMul, G2 NEON
Conv, G3 Softmax / LayerNorm / RMSNorm / GELU, G4 FP16 + INT8) land
the actual math that inference workloads need, so the capstone has a
single measurable number to report: *how fast does SLM-OS run the
same tensor kernel on each platform?*

This doc is the single source of truth for that answer. The x86-64
plan's Phase C3 and the Pi 5 plan's Phase F both feed their numbers
into the tables below rather than producing parallel benchmark docs.

## Methodology

All measurements use the built-in `bench` shell command, implemented
in `kernel/src/shell_sys.c` and backed by Rust FFI functions in
`runtime/src/lib.rs`:

| Shell verb | Rust FFI | What it runs |
|---|---|---|
| `bench matmul [iters]` | `rust_matmul_bench_fp32` | 128×128×128 FP32 matmul |
| `bench conv [iters]` | `rust_conv_bench_fp32` | 1×4×28×28 Conv2D, 8×4×3×3 kernel, pad=1 stride=1 |
| `bench quant [iters]` | FP32 → FP16 → INT8 at 128×128×128 | Three back-to-back matmuls for quantization comparison |

Timing source is the ARM generic timer (`CNTPCT_EL0`) on ARM64 and
the x86 TSC on x86-64, both reported in nanoseconds after scaling by
the detected counter frequency. Each iteration is wall-clock, so L1
warmup from the previous iteration is counted — the first iteration
is typically visible in the `max` column but not separated out.

Units:
- **MFLOPS / GFLOPS** for FP32 and FP16 (2·M·K·N floating-point ops
  per matmul).
- **GOPS** for INT8 (integer multiply-accumulates). Listed separately
  because INT8 throughput is not directly comparable to FP32 GFLOPS
  — the operations do less work per op on 8-bit inputs.

## Platform Summary

| Platform | CPU | Cores | Clock | RAM | Status |
|---|---|---|---|---|---|
| QEMU ARM64 (virt) | Cortex-A76 (emulated) | 4 | host-dependent | 1 GB | ✅ captured (this doc) |
| Raspberry Pi 5 | BCM2712 Cortex-A76 | 4 | 2.4 GHz | 4 GB | ⏳ hardware capture pending (see §Pi 5) |
| Jetson Orin Nano | Cortex-A78AE | 6 | 1.5 GHz | 8 GB | ⏳ hardware capture pending (see §Jetson) |
| x86-64 (QEMU) | max | 4 | host-dependent | 256 MB | ❌ build blocked by #141 |

## MatMul — 128×128×128

FLOPs / call: 2·128³ = **4.19 M** (FP32/FP16) or equivalent INT8 ops.

| Platform | Mode | Iters | Avg latency | Avg throughput |
|---|---|---|---|---|
| QEMU ARM64 | FP32 | 20 | 54.3 ms | 0.077 GFLOPS |
| QEMU ARM64 | FP16 (B half) | 20 | 116.5 ms | 0.035 GFLOPS |
| QEMU ARM64 | INT8 (A+B) | 20 | 8.6 ms | 0.488 GOPS |
| Pi 5 | FP32 | — | TBD | TBD |
| Pi 5 | FP16 | — | TBD | TBD |
| Pi 5 | INT8 | — | TBD | TBD |
| Jetson | FP32 | — | TBD | TBD |
| Jetson | FP16 | — | TBD | TBD |
| Jetson | INT8 | — | TBD | TBD |
| x86-64 | any | — | blocked by #141 | — |

**QEMU ARM64 observations:**
- FP32 path lands the NEON 4×4 FMA tiled kernel from G1. Emulated NEON
  is not representative of real silicon — QEMU's TCG lowering of
  `vfmaq_f32` gives a small fraction of hardware throughput, so the
  sub-0.1 GFLOPS figure is the TCG tax, not the NEON ceiling.
- FP16 path is slower than FP32 because the current implementation
  dequantizes row-by-row via the lock-protected `FP16_BUF` scratch in
  `ops::matmul` before entering the same FP32 tile loop. A real FP16
  NEON kernel (using `vfmlalq_low_f16`/`vfmlalq_high_f16`, ARMv8.2
  FEAT_FP16) would halve the memory traffic; that's future work.
- INT8 is ~6× faster than FP32 on QEMU. The INT32 accumulator loop is
  scalar today, but with 1-byte inputs the memory bandwidth advantage
  dominates. On real silicon the speedup will likely be smaller but
  still significant.

## Conv2D — MNIST shape

Input 1×4×28×28, weight 8×4×3×3, pad=1 stride=1 → output 1×8×28×28.
FLOPs / call ≈ **451 k**.

| Platform | Iters | Avg latency | Avg throughput |
|---|---|---|---|
| QEMU ARM64 | 50 | 4.52 ms | 0.099 MFLOPS |
| Pi 5 | — | TBD | TBD |
| Jetson | — | TBD | TBD |
| x86-64 | — | blocked by #141 | — |

This is the same NEON matmul kernel reached via im2col, so the
QEMU-ARM64-vs-Pi-5 gap here will mirror the matmul gap. The conv row
is cheap enough to include as a sanity check that the im2col path
actually runs end-to-end, not as the load-bearing number.

## Hardware capture procedure

### Pi 5

```
make kernel PLATFORM=RASPI5
labctl sdwire_update kernel build/kernel/slmos.bin
labctl power_cycle pi5-1
labctl serial_capture pi5-1 --pattern "slmos>" --timeout 30
labctl serial_send pi5-1 "bench matmul 50"
labctl serial_send pi5-1 "bench conv 200"
labctl serial_send pi5-1 "bench quant 50"
```

Capture the three output blocks and drop them into the tables above,
one row per platform × mode. The `iters` column stays as the real
count so min/max aren't misread.

### Jetson Orin Nano

```
make kernel PLATFORM=JETSON_ORIN_NANO
scp build/kernel/slmos.elf jetson-nano-2:/tmp/
ssh jetson-nano-2 sudo slmos-kexec /tmp/slmos.elf  # GSP suspend + kexec
# Serial console on USB-C:
labctl serial_send jetson-nano-2 "bench matmul 50"
labctl serial_send jetson-nano-2 "bench conv 200"
labctl serial_send jetson-nano-2 "bench quant 50"
```

The Cortex-A78AE at 1.5 GHz should land between Pi 5 and a modern
laptop — the Orin Nano GPU is not in play here because the CBB
firewall still blocks direct GPU register access at EL2, so GPU rows
are deliberately *not* part of this benchmark.

### Energy (Jetson only)

The Orin Nano carrier board has INA3221 power rails accessible as
`/sys/bus/i2c/devices/.../ina3221*` **on the Linux host**, which is
where the energy samples get taken — capture before `slmos-kexec` as
the idle baseline and after running `bench quant` for 1 minute. The
INA3221 is also behind the CBB firewall, so bare-metal SLM-OS cannot
read it directly.

### x86-64

Blocked on #141 (libm 0.2 f16 intrinsics + rustc 1.92 x86_64-unknown-none
backend). When unblocked, the procedure will be:

```
make kernel PLATFORM=X86_64
# Boots via GRUB ISO under QEMU; bench commands identical.
```

## How G1-G4 map to the numbers

- **G1 (NEON MatMul FP32):** sets the FP32 row of the matmul table.
- **G2 (NEON Conv):** sets the Conv2D row.
- **G3 (Softmax / LayerNorm / RMSNorm / GELU):** not directly
  benchmarked here — these are element-wise / reduction kernels that
  don't dominate transformer runtime unless the hidden dim is small.
  Regression tests (`make test`) validate them.
- **G4 (FP16 + INT8):** sets the FP16 and INT8 rows. The INT8 path
  today is scalar INT32 accumulate; a real NEON `sdot` (ARMv8.4
  FEAT_DotProd) kernel is future work.

## Known gaps / future work

- x86-64 path blocked on #141. Until resolved the x86-64 row stays
  empty.
- FP16 on QEMU is slower than FP32 — this is not a regression, it's
  the cost of dequant-then-FP32-matmul. Fix requires a genuine FP16
  NEON kernel.
- INT8 uses scalar accumulate. Real NEON `sdot` should give another
  2-4× on A78.
- Conv benchmark shape is MNIST (tiny). A real transformer-layer-shape
  conv (e.g. input 1×64×32×32, 128-channel 3×3 kernel) would be more
  representative; not added yet because the shape lands on the same
  NEON matmul inner loop and doesn't change the conclusion.

## Related docs

- `docs/benchmarks.md` — kernel benchmarks (context switch, IRQ
  latency, IPC). Platform-agnostic infrastructure numbers, not
  inference-specific.
- `docs/jetson-capstone-execution-plan.md` §G5 — the capstone
  deliverable definition this doc fulfills.
- `docs/x86-64-capstone-closure-plan.md` §C3 — x86-64 benchmark
  track that will contribute x86-64 rows once #141 is resolved.
- `docs/pi5-preemption-plan.md` — Pi 5 track; Phase F will drop Pi 5
  numbers into the tables above.
