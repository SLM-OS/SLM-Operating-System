# HailoRT-in-VM capture environment

Task 0.3 of [#795](https://github.com/SLM-OS/SLM-Operating-System/issues/795). Stands up a reproducible QEMU/KVM VM that runs Hailo's proprietary HailoRT userspace runtime (`libhailort.so` + `hailortcli`) against an emulated PCIe device, so every BAR R/W operation HailoRT issues during device probe / IDENTIFY / Configure() / Activate() can be recorded into the corpus format defined in [`docs/hailo-re-corpus-format.md`](../../docs/hailo-re-corpus-format.md).

This directory contains environment + automation. No SLM-OS or HailoRT source lives here; the HailoRT `.deb` packages must be obtained separately from Hailo's developer portal — see [§ Obtaining HailoRT](#obtaining-hailort).

## Status

Phase 1 integrated 2026-05-12. The custom QEMU built by `build-qemu.sh` now
registers two devices:

- `-device hailo-stub-stub` — the original throwaway placeholder. Read traffic
  goes through `launch-capture.sh` + `tools/qemu-trace-to-corpus.py` as the
  determinism-check artifact path. Kept to keep that artifact reproducible.
- `-device hailo8,corpus=<path>` — Task 0.2's real corpus-driven stub
  (sources at `../qemu-hailo8-stub/`). Used by Phase 1's
  `launch-bootstrap.sh`, which is the launcher the Task 0.5 driver script
  invokes via `HAILO_RE_QEMU_LAUNCHER`. This is the path that closes the
  capture-replay loop end-to-end against real pi-5-1 hardware.

The historical "stub-stub" mode below is retained for reference; the
corpus-driven path is what Phase 1+ actually uses.

Why a separate stub-stub instead of just using the QEMU `edu` device: `edu` exposes only BAR0. The Hailo driver hard-requires BAR0 + BAR2 + BAR4 to be present and non-zero-size — it fails probe with `Invalid PCIe BAR 2` against any device with fewer BARs. Hence the minimal patch.

## Layout

| Path | Purpose |
|---|---|
| `README.md` | This file |
| `build-qemu.sh` | Fetches QEMU 8.2 source, applies the hailo-stub-stub patch, builds a custom `qemu-system-x86_64` binary. Output: `~/slmos-ref/derivatives/hailort-vm-qemu/qemu-system-x86_64` |
| `build-vm-image.sh` | Builds a customized Ubuntu 24.04 qcow2 via cloud-init seed ISO. Output: `~/slmos-ref/derivatives/hailort-vm-images/`, **not in this repo** |
| `launch-capture.sh` | Boots the qcow2 with the custom QEMU + `-device hailo-stub-stub`, captures MMIO trace, emits a corpus JSONL. Legacy stub-stub path. |
| `launch-bootstrap.sh` | Phase 1+ launcher. Boots the qcow2 with `-device hailo8,corpus=<path>` and propagates the corpus-driven stub's `HAILO_RE_CORPUS_*` stdout. Wired into the Task 0.5 driver via `HAILO_RE_QEMU_LAUNCHER`. |
| `verify-determinism.sh` | Runs `launch-capture.sh` twice from a cold snapshot, diffs the traces, reports match / first divergence |
| `qemu-patch/hw/misc/hailo-stub-stub.c` | Minimal QEMU PCI device with Hailo-8 IDs (0x1e60:0x2864) and three BARs (4 KB / 16 KB / 1 MB). Reads return 0; writes are discarded. Throwaway placeholder for Task 0.2's real stub |
| `qemu-patch/apply.sh` | Copies hailo-stub-stub.c into a QEMU source tree and registers it in `hw/misc/meson.build` |
| `cloud-init/user-data` | First-boot cloud-init config: user account, package install, dpkg of the HailoRT `.deb`s, determinism tweaks |
| `cloud-init/meta-data` | cloud-init NoCloud meta-data (instance-id, hostname) |
| `guest-scripts/00-determinism.sh` | Disables ASLR, NTP, telemetry, irqbalance, tuned, etc. Inside the guest |
| `guest-scripts/10-bind-stub.sh` | Loads `hailo_pci`; the kernel auto-probes the hailo-stub-stub device via its Hailo IDs (`new_id` only needed in the edu-fallback path) |
| `guest-scripts/20-capture-run.sh` | Runs `taskset -c 0 hailortcli fw-control identify` (and exits cleanly so QEMU snapshot ends) |
| `tools/qemu-trace-to-corpus.py` | Parses QEMU `--trace memory_region_ops_read,memory_region_ops_write` output into the corpus JSONL format |

## Prerequisites (host)

- Ubuntu 22.04+ / Debian 12+ host with `qemu-system-x86_64` ≥ 8.0, `qemu-img`, `genisoimage` (or `xorriso`), `curl`, `python3` ≥ 3.10.
- KVM enabled and `/dev/kvm` accessible by the invoking user. Verify with `qemu-system-x86_64 -accel help | grep kvm`. The build runs in TCG if KVM is unavailable but is ~10× slower.
- For building the custom QEMU: `build-essential`, `ninja-build`, `pkg-config`, `meson`, `libglib2.0-dev`, `libpixman-1-dev`, `libslirp-dev`, `python3-pip`.
- ~6 GB free in `~/slmos-ref/derivatives/hailort-vm-images/` (Ubuntu cloud image + working qcow2).
- ~3 GB free in `/var/tmp/qemu-stub-build/` for the QEMU build tree (override with `QEMU_BUILD_ROOT`; must be a path **without spaces** — QEMU's configure script can't handle them).

## Obtaining HailoRT

The HailoRT `.deb` packages are not freely redistributable. Download them from Hailo's developer portal (login required) and place them at:

```
~/Downloads/hailort_4.23.0_amd64.deb            # libhailort + hailortcli (userspace)
~/Downloads/hailort-pcie-driver_4.23.0_all.deb  # DKMS source for hailo_pci.ko
```

Or set `HAILORT_DEB_DIR=<path>` to override the location. `build-vm-image.sh` will sha256-check these and fail loudly if they don't match the pinned values.

**Pinned versions (HailoRT v4.23.0, 2026-05-12):**

| File | sha256 | Notes |
|---|---|---|
| `hailort_4.23.0_amd64.deb` | _TBD on first build — script will print observed sha and persist it_ | Must match the version running on pi-5-1 — see CLAUDE.md / memory `hailo_fw_v4_23_struct_sizes.md` |
| `hailort-pcie-driver_4.23.0_all.deb` | `36e308eb492808db9db7c64046e3fdfb3e4e02fdfd67550db678f068869c1fbf` | Arch-independent DKMS package — same file used on pi-5-1 |

The arm64 build (`hailort_4.23.0_arm64.deb`, sha256 `344c7432e8240b666f7817226e5bdface02902380904432caa639fdb09e67417`) is what pi-5-1 has installed. The amd64 build of the **same upstream release** is what we need for the x86_64 VM; Hailo distributes both side-by-side on their portal.

## Quick start

```bash
# One-time: build the custom QEMU with the 3-BAR stub-stub device (~10-20 min).
./build-qemu.sh

# One-time: build the VM image (~5 minutes on first run, cached afterwards).
./build-vm-image.sh

# Run a single capture.
./launch-capture.sh ~/slmos-ref/derivatives/hailo-re-corpora/test.jsonl

# Run the determinism check (two captures + byte-for-byte diff).
./verify-determinism.sh
```

The determinism check is the load-bearing deliverable of Task 0.3 — see [§ Determinism](#determinism).

## How capture works

```
┌─────────────────────────────────────────────────────────────────────┐
│ host (this machine)                                                 │
│                                                                     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │ qemu-system-x86_64 (custom build with hailo-stub-stub)       │   │
│  │  --trace memory_region_ops_{read,write}                      │   │
│  │  -device hailo-stub-stub     ← Hailo IDs, BAR0/2/4, reads=0  │   │
│  │  -serial file:...            ← guest-side trace markers      │   │
│  │                                                              │   │
│  │  ┌──────────────────── guest (Ubuntu 24.04) ──────────────┐  │   │
│  │  │  cloud-init has already:                               │  │   │
│  │  │   - installed HailoRT v4.23 (.deb)                     │  │   │
│  │  │   - built hailo_pci.ko via DKMS                        │  │   │
│  │  │   - disabled ASLR, NTP, irqbalance, ...                │  │   │
│  │  │                                                        │  │   │
│  │  │  20-capture-run.sh runs once per capture boot:         │  │   │
│  │  │   1. modprobe hailo_pci  (auto-binds to Hailo IDs)     │  │   │
│  │  │   2. taskset -c 0 hailortcli fw-control identify       │  │   │
│  │  │   3. shutdown -h now                                   │  │   │
│  │  └────────────────────────────────────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                          │                                          │
│                          ▼                                          │
│                  qemu-trace.log                                     │
│                          │                                          │
│            tools/qemu-trace-to-corpus.py                            │
│                          │                                          │
│                          ▼                                          │
│   ~/slmos-ref/derivatives/hailo-re-corpora/...jsonl                 │
└─────────────────────────────────────────────────────────────────────┘
```

`memory_region_ops_read` and `memory_region_ops_write` fire on **every** MMIO trap regardless of accelerator (KVM dispatches MMIO faults to QEMU, where the device's `MemoryRegionOps` callbacks run and the trace points fire). The captured stream is complete for the stub-stub device. Once Task 0.2 lands a corpus-driven stub, this same trace machinery applies to its BAR0/2/4 regions — `qemu-trace-to-corpus.py`'s `BAR_NAME_MAP` already accepts both names.

## Determinism

The single most important question Task 0.3 answers: **is HailoRT's BAR write sequence deterministic across runs?** If not, the entire RE plan in #795 is unviable (capture would record a different protocol every iteration).

Mitigations applied (all enforced via `cloud-init/user-data` + `guest-scripts/00-determinism.sh`):

| Source of non-determinism | Mitigation |
|---|---|
| Address-space layout randomization | `echo 0 > /proc/sys/kernel/randomize_va_space` |
| Scheduler placement across cores | `taskset -c 0 hailortcli ...` |
| Background daemons issuing parallel I/O | Mask `systemd-timesyncd`, `unattended-upgrades`, `irqbalance`, `tuned`, `snapd`, `cron`, `apt-daily*.timer` |
| Network/DNS lookups during HailoRT init | Guest has no NIC except hostfwd; no resolvers reachable |
| Kernel-side IRQ ordering | Pin all IRQs to CPU 0 (`/proc/irq/default_smp_affinity = 1`) |
| Telemetry / hailo-monitor | Not installed in the .deb chain we use; `systemctl mask hailo-monitor.service` defensively |
| HailoRT internal async paths | TBD: `HAILORT_LOGGER_PATH` + `HAILORT_CONSOLE_LOGGER_LEVEL=trace` to inspect post-capture; if async pre-fetch is observed, set `HAILO_DISABLE_ASYNC=1` (or whatever the v4.23 env knob is — discover during capture) |
| QEMU CPU-mode jitter | `-cpu host,migratable=no` under KVM; `-smp 1` for the capture VM |
| Wall-clock dependent code paths | Fixed RTC base via `-rtc base=2026-05-12T00:00:00,clock=vm` (guest's `time()` returns a deterministic starting point each boot) |

`verify-determinism.sh` runs two captures back-to-back from the same cold qcow2 snapshot and diffs the trace files. Expected result: byte-for-byte match. Any divergence is documented in this README under [§ Observed determinism](#observed-determinism) below.

### Observed determinism

| Date | HailoRT version | Run-A vs Run-B | First divergence (seq, offset) | Notes |
|---|---|---|---|---|
| 2026-05-12 | 4.23.0 | **MATCH** — 19/19 op entries identical byte-for-byte | none | First end-to-end run. `hailortcli fw-control identify` against the hailo-stub-stub device. All 19 ops on BAR0; mix of reads (probe + vendor/fw-loaded magic) + writes (driver init + FW_CONTROL ioctl). Driver eventually returns `Failed writing fw control to pcie` because the stub doesn't fake any of the fw_control request/response handshake — but the **write sequence up to that failure point reproduces exactly**. That is the capture invariant the rest of #795 needs. |

**Implication for #795**: the strict single-step + corpus discipline in [`docs/hailo-re-corpus-format.md`](../../docs/hailo-re-corpus-format.md) is viable. Task 0.2 can build its corpus-driven stub knowing that re-running HailoRT against the same stub responses will produce the same write sequence — no race-driven non-determinism observed at this stage. Re-validate once Task 0.2's stub lands (the MMIO surface will be larger; this run only covers the first 19 ops).

## Hand-off to Task 0.2

The stub-stub QEMU patch is throwaway. Once Task 0.2 lands a corpus-driven Hailo stub:

1. Replace `-device hailo-stub-stub` in `launch-capture.sh` with `-device hailo-stub,corpus=<path>` (or whichever invocation Task 0.2 ships).
2. `qemu-patch/hw/misc/hailo-stub-stub.c` becomes redundant — Task 0.2's device replaces it. Delete `build-qemu.sh` and the patch tree.
3. `guest-scripts/10-bind-stub.sh`'s fallback branch (for the edu device) becomes dead code; keep it or trim.
4. Either keep `tools/qemu-trace-to-corpus.py` as the trace post-processor or switch to Task 0.2's direct-emit path (the stub writes corpus entries itself, no post-processing needed).

The VM image, cloud-init, and determinism harness all stay the same. Integration cost is ~30 minutes once 0.2 has a working stub.

## Workflow rules

- **No HailoRT source in this repo.** The local reference cache at `~/slmos-ref/hailo/v4.23.0/` is read-only. Don't copy contents into this directory.
- **HailoRT `.deb` packages stay out of git.** The README directs the user to obtain them manually; the build script verifies sha256s.
- **VM disk images stay out of git.** They land in `~/slmos-ref/derivatives/hailort-vm-images/`.
- **Corpora stay out of git.** They land in `~/slmos-ref/derivatives/hailo-re-corpora/`.
- Per top-level `CLAUDE.md`: no emoji; branch + PR; ask before commit; second-person avoided.

## References

- Issue #795 — full RE plan, parallelization layout, Phase 0 task table.
- `docs/hailo-re-corpus-format.md` — the on-disk schema this environment feeds.
- `docs/hailo-protocol-architecture.md` — context: what HailoRT does on a real MNIST load (84,003 inferences, 27 fw_control calls, all IDENTIFY).
- Memory pointers: `hailo_re_plan_qemu_capture.md`, `hailort_protocol_architecture.md`.
- HailoRT v4.23 reference cache: `~/slmos-ref/hailo/v4.23.0/` (kernel-side driver only — no userspace source).
