# Jetson CBB Firewall: Comprehensive Report

Consolidates everything SLM-OS has learned about the Tegra234 Control
Backbone (CBB) firewall: what it is, what it blocks, what has been
worked around, and what would close the gaps "officially." Pulls
together material previously scattered across `docs/jetson-el2-bringup.md`,
`docs/jetson-nvidia-support.md`, `docs/capstone-feature-status.md`,
`docs/jetson-capstone-handoff.md`, and issues #9 / #24 / #25 / #31 / #258.

**Audience:** Future SLM-OS developers and anyone evaluating how much
Jetson hardware is addressable from a bare-metal kernel at NS EL2.

**Last updated:** 17 April 2026 (Phase 7 retest retracted the GPU doorbell finding)

---

## 1. Executive Summary

The CBB is a Tegra234 hardware access-control fabric that sits between
every bus master (CPU, GPU, BPMP, NVENC, etc.) and every MMIO
peripheral. The bootloader programs the CBB with a permissions table
during secure boot; every MMIO access is checked against this table
at runtime. Non-matching accesses produce a RAS Uncorrectable error
that EL3 firmware handles by powering off the offending CPU core.

SLM-OS landed on Jetson Orin Nano via a **partial bypass**: running at
NS EL2 with VHE (rather than the EL1 the CBB table was built for) gives
access to a useful subset of peripherals — enough to boot, run SMP,
drive serial, use the GIC and timer, talk to DRAM, and inspect the GPU
via non-engine register space. The **hard blockers that
remain** are all either:

- **Peripheral apertures the EL2-NS table has never been programmed to
  permit** (UARTA on the 40-pin header; OP-TEE secure carveout at
  `0xBE000000–0xC2000000`; INA3221 power telemetry).

  **Previously listed here:** the GPU's PFIFO/CHRAM/NV_USERMODE
  apertures. An April-17 retest on jetson-nano-2 (peek 0x17BB0000
  from EL2 returned 0x0000C561, matching Linux `/dev/mem`; poke of
  the correct token advanced GP_GET) proved those apertures are
  actually accessible from EL2 — the earlier map was probing the
  wrong offset (0x17800000) and misreading the GPU's "no register
  here" `0xbadf1100`-family responses as aborts. What remains
  kernel-owned is the nvgpu *ioctl surface* (kernel-mode bookkeeping
  for channel creation), not the MMIO itself.
- **Privileged-write endpoints that require a bus master the CBB
  considers "trusted"** — handful of SMC-gated registers accessible
  only via EL3 firmware.

There is no software-only fix for either class from NS EL2. Getting
"officially" past them requires one of four avenues described in §6:
CBB reconfiguration via the bootloader boot-config-table (BCT),
entering the NVIDIA secure boot chain, running under a Linux/KVM
hypervisor with device passthrough, or an EL3 SMC service exposed via
a custom TF-A.

---

## 2. What the CBB Actually Is

The Control Backbone ("CBB") is an NVIDIA-proprietary on-chip
interconnect fabric that arbitrates every transaction on the SoC.
Every read or write from a CPU core, DMA engine, or accelerator to an
MMIO register traverses the CBB. In front of each destination the CBB
has a **firewall** — a lookup of `(source master, security state,
target address range)` tuples against a permissions table programmed
by early boot firmware.

### Configuration path

```
BootROM → MB1 → MB2 → UEFI → TF-A (BL31) → OP-TEE → L4T/Linux
            │     │
            │     └─ Loads the CBB permissions table from the
            │        MB2 BCT ("Boot Configuration Table"), a set of
            │        .dts-compiled binaries specifying per-master,
            │        per-peripheral allow/deny rules.
            │
            └────── Executes the BCT to program the CBB firewall
                    registers for every slave on the fabric.
```

The BCT files live in NVIDIA's Jetson Linux release under
`bootloader/tegra234-mb2-bct-scr-*.dts.in` (confirmed by forum threads
and NVIDIA's tegraflash tooling). Each peripheral has a block of
READ_CTL / WRITE_CTL / CTL_SETTING registers that encode which bus
masters may access it.

### Error manifestation

A blocked access generates a CBB interface error, captured by the SoC's
RAS fabric:

```
ERROR:   RAS Uncorrectable Error in IOB, base=0xe010000:
ERROR:      Status = 0xec00030d
ERROR:   SERR = Illegal address (software fault): 0xd
ERROR:      IERR = CBB Interface Error: 0x6
ERROR:      ADDR = 0x8000000003100000        ← the blocked address
ERROR:   Powering off core
```

EL3 firmware (TF-A's BL31) handles the RAS error and **powers off the
offending CPU core**, which is why a single blocked access takes down
one of SLM-OS's CPUs and is effectively a crash.

### Why it exists

The CBB enforces the Tegra security model so that, for example, a
compromised non-secure kernel cannot poke the BPMP's secrets, the
fuse controller, the GPU's secure microcode loader, or OP-TEE memory.
It is not a bug — it's the mechanism by which the platform enforces
who can touch what. The problem for a bare-metal OS is that the
permissions table is built around "Linux at NS EL1," not around
SLM-OS, and NVIDIA does not publish the syntax for editing it.

### Per-peripheral *and* per-master granularity

Two facts matter for the rest of this report:

1. **It's per-peripheral, not global.** UARTC and UARTA have
   independent rules — one is reachable from NS EL2, the other is
   not, on the same core at the same privilege.
2. **It's per-master as well.** Some MMIO apertures that EL2-NS
   cannot reach from SLM-OS *can* be reached by Linux's kernel-mode
   drivers because they write through a different, trusted bus-master
   path the CBB allows. When SLM-OS's read of a masked aperture
   returns `0xbadf1100` (PRI poison) it indicates a masked-out
   behavior, not a bus abort. **Caution:** `0xbadf1100` is also the
   normal response for "no register at this offset" on any GA10x GPU
   — see §5 below for how misreading that pattern as an abort led to
   the wrong conclusion about `NV_USERMODE` being blocked.

---

## 3. The EL2 + VHE Partial Bypass (What Works Today)

Per `docs/jetson-el2-bringup.md`, SLM-OS runs at **NS EL2 with VHE
(HCR_EL2.E2H=1, TGE=1, RW=1)** after kexec from Linux. Linux itself
runs at EL2-VHE (verified via `CurrentEL` probe), and kexec's
`HVC_SOFT_RESTART` preserves the privilege level.

Why this matters: the CBB's permissions table allows EL2-NS access to
a wider peripheral set than EL1-NS, because NVIDIA's stock Linux uses
EL2-VHE itself. SLM-OS "inherits" that permission profile by running
at the same EL as Linux was running at when kexec handed off.

### Peripherals reachable from NS EL2

| Peripheral | Address | EL2 access | Used by SLM-OS |
|---|---|---|---|
| UARTC (TX) | `0x0C280000` | ✅ | Shell console via TCU |
| TCU RX mailbox | `0x03C10000` | ✅ | Shell input (SPE routes USB-C input) |
| GIC Distributor | `0x0F400000` | ✅ | GICv3 for all IRQs |
| GIC Redistributor | `0x0F440000+` | ✅ | 6 CPUs, dual-cluster |
| ARM Generic Timer | system regs | ✅ | 100 Hz scheduler tick (cooperative) |
| Watchdog | `0x02190000` | ✅ | Disabled at boot |
| DRAM | entire 8 GB | ✅ | ~6.9 GB usable across 3 regions |
| GPU PMC regs | `0x17000000` (BAR0) | ✅ (read/write) | ID (GA10B), HWCFG, FECS, runlist status |
| FECS method push | `0x409500`/`0x409504` | ✅ | GPU method submission (verified) |
| GPU USERMODE doorbell | BAR0 + `0xBB0090` (phys `0x17BB0090`) | ✅ (R/W) | Phase 7 submission kick (verified) |
| HSP mailboxes (TCU) | `0x03C00000+` | ✅ | Serial input routing |
| PSCI calls | SMC | ✅ | CPU power, SYSTEM_OFF |
| XUSB pad controller | `0x03520000` | ✅ | USB networking UPHY config (#266 Phase 0) |
| Tegra XHCI host | `0x03610000` | ⚠ MMIO readable, DMA blocked | #266 Phase 3A mothballed. Capability probe works post-kexec once `slmos-kexec` holds the `xusb_*` clocks on. But `USBCMD.RUN=1` wedges the aperture because `arm-smmu` drops the xusb stream's translations during Linux's kexec path (#285, closed). See `docs/jetson-usb-networking-plan.md` §8. |
| Tegra XUDC device | `0x03550000` | ✅ (clock-dark) | USB networking Option B fallback (#266 Phase 0); expected to hit the same SMMU-at-kexec DMA blocker as XHCI if attempted. |

### Peripherals blocked even from NS EL2

| Peripheral | Address | Used by | What we lose |
|---|---|---|---|
| UARTA | `0x03100000` | 40-pin header UART | Secondary serial console; diagnostic uplink when USB-C is busy |
| OP-TEE carveout | `0xBE000000`–`0xC2000000` | OP-TEE secure world | 64 MB of DRAM (unusable; we skip it) |
| INA3221 telemetry | I²C behind CBB | Power/energy measurement | Energy/power readings have to be captured pre-kexec from Linux |
| BPMP IVC | `0x0C168000` | Clock, power domain control | Cannot reconfigure any clock post-kexec |
| Fuse controller | varies | Secure boot identity | Cannot inspect platform fuses |
| QSPI flash | behind firewall | Firmware storage | Cannot update boot chain from SLM-OS |

### Secondary consequence: BPMP is unreachable

Because BPMP IVC is behind the CBB, SLM-OS cannot talk to the Boot and
Power Management Processor after kexec. Practical effects:

- Cannot enable/disable any peripheral clock (UART clocks, EQOS clock,
  GPU clock, etc.) — must inherit whatever Linux left enabled.
- Cannot DVFS — CPU frequency is whatever Linux was running at.
- Cannot query thermals from BPMP — must read thermal sensors directly
  (and at least some of those are also behind the CBB).

The effect is that SLM-OS runs *within the operating envelope Linux set
up before kexec*, and any feature that needs dynamic clock or power
configuration has to arrange it on the Linux side first.

---

## 4. Feature-Level Impact

Cross-walked to the five tracked features (see
`docs/capstone-feature-status.md`):

| Feature | Status on Jetson | CBB impact |
|---|---|---|
| **SMP / cross-CPU dispatch** | ✅ 6 cores online, `bench smp` passes | No impact — GIC and CPU power are reachable. |
| **Preemptive multitasking** | 🟡 Cooperative (`COOP_PREEMPT`) | Separate blocker: GIC Group config is locked by TF-A, not CBB. Timer IRQs don't deliver to NS EL2 regardless of CBB. See `kernel/CLAUDE.md` §"ARM64 Hardware Timer IRQs." |
| **GPU inference** | 🟢 Detection + FECS gateway + Phase 7 NOP dispatch working | No CBB wall in the submit path. Channel *creation* still requires the Linux-side helper (kernel-mode nvgpu ioctl surface), but once the channel exists, SLM-OS writes GP_PUT in DRAM and rings the USERMODE doorbell at BAR0+0xBB0090 directly from EL2. #258 closed 2026-04-17. |
| **AI scheduler** | ✅ Running | No CBB dependency — pure CPU/NEON path. |
| **AI page eviction** | ✅ Running | No CBB dependency. |
| **Networking** | ❌ Not wired yet (#25) | Tentative impact. EQOS MAC is at `0x02310000`; need to verify EL2 reachability (§6.A first experiment). If blocked, Jetson networking is a hard no-go without one of the permanent fixes in §6. |
| **USB** | ⛔ Mothballed (#266, #285) | Not CBB-blocked — Phase 0 (2026-04-17) confirmed XHCI + XUDC + UPHY padctl are all NS-EL2-reachable. Clock-gate state fixed by `scripts/jetson-kexec-slmos.sh` holding `xusb_*` clocks + `xusba`/`xusbc` powergates through kexec (commit `66b7ad9`). Phase 3A reached working capability probe but blocked at `USBCMD.RUN=1`: Linux's kexec path disables `arm-smmu` translations for the xusb stream (dmesg `arm-smmu … disabling translation` immediately before `kexec_core: Starting new kernel`), and the controller's first DMA fault on RUN=1 bricks the MMIO aperture. Option A mothballed 2026-04-18 after four variants tested (#285, closed); #286 tracks the long-term standalone-firmware-load alternative. Full investigation in `docs/jetson-usb-networking-plan.md` §8. |

The CBB is the *root blocker* for two features (GPU inference full
pipeline, and possibly networking) and several smaller items (UARTA,
energy telemetry, USB).

---

## 5. Where GPU Bring-Up Hit the CBB Wall

The GPU story is instructive because it shows what CBB bypass by
*privilege escalation* (EL2) can and cannot buy.

### What works from NS EL2

- **Identify** the GPU: `BOOT_0 = 0xB7B000A1` (GA10B), `BOOT_42`,
  HWCFG — all readable.
- **Read** FECS/GPCCS mailbox state — can confirm Linux's microcode is
  loaded and healthy (ga10b_bringup inherit path, PR #256).
- **Submit** a method through the FECS push registers
  (`0x409500`/`0x409504`) and read the reply back — verified
  end-to-end: FECS returned the 513,280-byte GR context image size in
  response to `DISCOVER_IMAGE_SIZE`.
- **Read** runlist status and FIFO_USER doorbell on the first five
  channels.
- **Access** USERD, GPFIFO rings, and pushbuffer memory in DRAM
  directly.

### What's blocked

- **Create** a new channel *from SLM-OS alone* — the nvgpu ioctl
  surface (TSG open, ALLOC_AS, SETUP_BIND, nvmap, runlist programming)
  is a Linux-kernel driver that we haven't ported. This is a software
  scope limit, not a CBB block; SLM-OS works around it with the
  Linux-side helper.

### Historical correction (April 17)

Earlier revisions of this report listed channel *creation* and the
USERMODE *doorbell* as CBB-blocked from EL2. Both conclusions were
wrong:

- **The doorbell isn't at `0x800000`.** GA10B inherits the TU104
  usermode layout; the actual doorbell register is at `BAR0+0xBB0090`
  (physical `0x17BB0090`), not `BAR0+0x800000`. The old map probed the
  pre-Turing `FIFO_USER` aperture and declared "blocked."
- **`0xbadf1100` isn't always a firewall mask.** On GA10x GPUs the
  value is *also* the PRI poison for "no register at this offset."
  Since the old probe was hitting unused offsets (0x17800000,
  0x17002000, etc.), the "blocked" readings were the GPU's normal
  response for empty space, not CBB refusals.

On April 17, `peek 0x17BB0000` from SLM-OS at EL2 returned
`0x0000C561` — identical to what Linux `/dev/mem` and CUDA's USERMODE
mmap see. `poke 0x17BB0090 <work_submit_token>` advanced GP_GET on a
Linux-primed channel from the SLM-OS shell. The updated
`ga10b_bringup_smoke_test` then completed the full submit path
end-to-end (GP_PUT → doorbell → PBDMA consume → GP_GET advance),
reaching `METHOD_ACCEPTED` state. Issue #258 was closed as the result
of this retest.

### Why the inherit approach is still needed

The inherit path (PR #256) is still the right pattern for Jetson,
but for a different reason than previously claimed. Channel
*creation* goes through nvgpu's kernel-mode ioctls — roughly 10 of
them, plus SMMU programming and runlist management — which is a
Linux kernel driver SLM-OS hasn't ported. The Linux-side helper
reuses that code path, then hands off the completed channel's
addresses + `work_submit_token` to SLM-OS through a handoff block
in DRAM (wire v2). After kexec, SLM-OS writes GP_PUT in DRAM and
the doorbell in BAR0 — both directly from EL2 without any CBB
involvement.

---

## 6. Paths to "Officially" Past CBB

Four mutually non-exclusive paths. Ranked roughly by effort and by
what they would unlock.

### A. Bootloader BCT Reconfiguration (MB2-BCT-SCR)

**What it is:** Edit the CBB permissions table at the source —
`tegra234-mb2-bct-scr-*-override.dts` — to grant NS EL2 access to
the specific peripherals SLM-OS needs, rebuild the bootloader with
`tegraflash`, and reflash MB2 / BCT partitions. This is the "right"
answer for bare-metal peripheral access on Jetson.

**What it unlocks:**
- UARTA on the 40-pin header.
- INA3221 for bare-metal energy telemetry.
- Potentially EQOS and Tegra XUSB, depending on what they actually
  need (TBD).
- *(The GPU's PFIFO/CHRAM/NV_USERMODE apertures are no longer on this
  list — §5 retest showed they're already reachable from EL2.)*

**What's required:**
- NVIDIA Jetson Linux Tegra flash host environment installed.
- Access to the BCT source files (shipped with L4T under the
  bootloader directory).
- Successfully identify the `reg@XXXX` register IDs for each
  target peripheral — NVIDIA has **never published the decode** for
  these IDs, and the forum thread linked in
  `docs/jetson-nvidia-support.md` closed without an answer. Reverse
  engineering this mapping is the bulk of the work.
- Willingness to reflash MB2/BCT, which is reversible but has some
  brick risk on the dev kit if the replacement BCT is malformed.

**Open question:** whether unsigned / locally-built BCT files are
accepted by a Jetson with default fuse state. On the Orin Nano Dev Kit
(fuses unburned), the answer is probably yes. On a production-fused
Jetson, full secure boot would also need to be satisfied — making this
option (A) collapse into (B).

**Effort:** 2–4 weeks focused reverse-engineering work, with high
variance depending on how much of the BCT tooling documentation is
actually usable.

### B. Secure Boot Chain Integration

**What it is:** Sign SLM-OS with the platform's PKC/SBK keys, register
it as a legitimate payload in the Jetson secure boot chain, and flash
it as the primary OS. Done correctly, SLM-OS is no longer "untrusted
kexec'd code" — it boots directly and gets whatever CBB permissions
the chain is configured to grant.

**What it unlocks:**
- Full CBB permissions (if the BCT is configured for them).
- Direct UEFI boot, eliminates the Linux-kexec indirection.
- Makes the platform support narrative cleaner for any real deployment.

**What's required:**
- Full `l4t_sign_image.sh` toolchain.
- Possibly fuse-burning for PKC/SBK (one-way, **permanent**, do not do
  on shared lab hardware unless you own the board).
- A full reflash for *every* kernel change — iteration loop measured
  in minutes, not seconds.

**Issues:** Tracked as #31. Even after doing this work, we might still
need (A) to update the BCT permissions for the apertures SLM-OS wants
— secure boot is an identity mechanism, not an authorization one.

**Effort:** Weeks of flash-tooling integration; fuse-burning is a
one-line operation with forever consequences.

### C. Linux Hypervisor + Device Passthrough

**What it is:** Keep Linux as the host, run SLM-OS as a KVM or Xen
guest with select peripherals IOMMU-passed-through. Linux owns the
CBB; SLM-OS operates inside a virtualization boundary where those
permissions are inherited by the guest.

**What it unlocks:**
- Arbitrary peripheral access within what Linux chooses to grant.
- Reusable across all Tegra boards, not Orin Nano-specific.

**What's required:**
- KVM on Jetson (Linux config).
- IOMMU / SMMU configuration for each passed-through device —
  non-trivial on Tegra.
- An SLM-OS virtio-mmio path for disk/net, or real passthrough for
  GPU / NIC.
- Measurably higher latency than bare-metal.

**Issue:** It's no longer "bare-metal." The project's framing would
have to change accordingly.

**Effort:** 4–8 weeks.

### D. EL3 SMC Service via a Custom TF-A

**What it is:** NVIDIA's TF-A (BL31 at EL3) is open source. A custom
build can add a Silicon Partner (SiP) SMC handler that performs
specific CBB-blocked MMIO writes on SLM-OS's behalf — SLM-OS at EL2
issues `SMC`, the modified BL31 at EL3 does the write, returns.

**What it unlocks:**
- Surgical access to specific CBB-locked apertures (GPU doorbell,
  UARTA) via well-defined SMC calls — SLM-OS gains no generic
  peripheral access, only the exact operations the SMC service is
  coded to perform.

**What's required:**
- Building a modified TF-A and replacing the `bl31.bin` partition
  (secure boot identity needs to allow it; on an unfused dev kit this
  is fine).
- Writing the SMC handler (~200 LoC of TF-A code).
- Flashing the modified BL31.

**Issues:** Still depends on the CBB permissions granted to EL3.
Whatever needs EL3 to do it on our behalf becomes two context
switches per operation, which is fine for cold paths but not for
hot MMIO. As of April 17, this path is *no longer needed* for the
GPU doorbell — that write works directly from NS EL2 (§5). Path D
is now only a fallback for apertures still blocked at EL2 (e.g.,
UARTA, BPMP IVC).

**Effort:** 2–3 weeks per SMC handler. Cheap for each additional
aperture once the first is working.

---

## 7. Recommended Next Steps

For SLM-OS specifically, the paths that buy the most for the least
effort, in rough priority order:

1. **First experiment (1 day):** probe EQOS at `0x02310000` from NS
   EL2. If it's CBB-blocked, Jetson networking is gated on path A/B/C.
   If it's reachable, proceed with the EQOS driver (#25) without
   touching CBB — this is the cheapest thing to verify next.

2. *(Was "build custom TF-A SMC for NV_USERMODE doorbell." Removed
   April 17: the doorbell write works directly from NS EL2; #258 is
   closed. What remains blocking full GPU compute is encoding real
   methods — NOP dispatch is verified; next step is a
   SEMAPHORE_RELEASE and then compute-class QMD dispatch.)*

3. **If broad bare-metal on Jetson becomes strategic (4 weeks+):**
   invest in path A (BCT reconfig). Start by reverse-engineering
   the `reg@XXXX` decoding by diffing two BCT builds: one stock,
   one with a single peripheral's rule swapped, observed via a
   CBB readback probe from Linux. This is tedious but mechanical.
   Skip path B (secure boot) unless the project ever needs to ship
   on locked-down hardware — for development, an unfused dev kit
   is sufficient.

4. **Do not pursue path C (Linux hypervisor)** unless the bare-metal
   framing is explicitly dropped. It's technically valid but changes
   the nature of the project.

### A note on "officially"

None of paths A–D are what NVIDIA would call "officially supported" —
NVIDIA's position is that kexec / bare-metal-on-Jetson is
**not a validated workflow** (see the linked forum threads in
`docs/jetson-nvidia-support.md` §"NVIDIA Forum Research"). "Officially
past CBB" for this project means "reaching a state where blocked
apertures are no longer blocked by construction, and the fix is
documented and reproducible," not "NVIDIA blessed us."

---

## 8. Open GitHub Issues Related to CBB

| # | Title | Relevance |
|---|---|---|
| #258 | Jetson GPU Phase 7: PBDMA doesn't consume GPFIFO entries — doorbell mechanism blocked from EL2 | Closed 2026-04-17 as misdiagnosis; actual doorbell is at BAR0+0xBB0090 (TU104 layout) and is reachable from EL2. |
| #31 | Secure Boot Chain (Jetson fuse-based signature verification) | Path B of §6 |
| #25 | Jetson Ethernet driver (EQOS controller) | Assumes EQOS is reachable at EL2 — first experiment in §7 |
| #24 | Jetson USB Serial Console (TinyUSB + Tegra XUSB) | Blocked if XUSB needs clocks SLM-OS can't enable (BPMP unreachable) |

Not tracked as an issue but worth filing once any of paths A/B/C/D is
seriously on the roadmap: "Design doc for Jetson CBB reconfiguration
approach," so the reverse-engineering work described in §6.A is not
lost if it is repeated.

---

## 9. References

### Internal docs (authoritative detail for each topic)

- `docs/jetson-el2-bringup.md` — The EL2+VHE bypass and the peripheral
  reachability matrix.
- `docs/jetson-nvidia-support.md` — Investigation history, NVIDIA
  forum citations, signing toolchain notes.
- `docs/capstone-feature-status.md` §"GPU-Based Inference" —
  per-aperture EL2 reachability for the GPU specifically.
- `docs/jetson-capstone-handoff.md` — Session-level handoff state
  including the April 17 channel-inherit work.
- `docs/archive/investigations/jetson-nvgpu-bringup-research.md` —
  `nvgpu.ko` reference trace (blob 17 of 17 cached).
- `kernel/CLAUDE.md` §"ARM64 Hardware Timer IRQs" — GIC Group-config
  blocker (separate from CBB, but often confused with it).

### NVIDIA forums (read-only references)

- [kexec on the Jetson not working (CBB errors)](https://forums.developer.nvidia.com/t/kexec-on-the-jetson-not-working-cbb-errors/275207)
- [How to disable OEM firewall on Orin?](https://forums.developer.nvidia.com/t/how-to-disable-oem-firewall-on-orin/304878)
- [Direct register access to Timer](https://forums.developer.nvidia.com/t/direct-register-access-to-timer/286298) (EL2 success story that prompted our bypass)
- [Clarification on Jetson Orin Hypervisor Support](https://forums.developer.nvidia.com/t/clarification-on-jetson-orin-hypervisor-support-hardware-lock-or-only-unsupported/348348) (NVIDIA: EL2 is software-disabled, not hardware-locked)

### NVIDIA developer documentation

- [Orin Series SoC TRM](https://developer.nvidia.com/orin-series-soc-technical-reference-manual) — covers the CBB fabric chapter (dev login required).
- [Jetson Linux Developer Guide — UEFI](https://docs.nvidia.com/jetson/archives/r36.2/DeveloperGuide/SD/Bootloader/UEFI.html)
- [Jetson Security Documentation](https://docs.nvidia.com/jetson/archives/r36.4/DeveloperGuide/SD/Security/index.html)

---

*Report compiled: 17 April 2026.*
