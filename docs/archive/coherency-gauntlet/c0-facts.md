# C0: architecture and firmware facts (#1193)

Builder C0, coherency gauntlet. No hardware was touched and no code was changed.
Every register-behavior statement below cites an Arm document, TF-A source, Linux
source or QEMU source. SLM-OS code is cited only for what SLM-OS itself does, and
the SLM-OS lab record is cited only as an observation. Statements marked
**Inference** are not stated by any source and follow from the cited facts.

## Sources fetched for this component

All saved under `~/slmos-ref/` (README updated). Text extracts are `pdftotext -layout`
output next to each PDF; `.txt:<line>` citations point into those extracts.

| Short name | Document | Local file |
|---|---|---|
| A76 TRM | Arm Cortex-A76 Core TRM, 100798, r4p1, issue 0401-01 | `~/slmos-ref/arm/arm-cortex-a76-trm-100798-0401-01.{pdf,txt}` |
| A78AE TRM | Arm Cortex-A78AE Core TRM, 101779, r0p3, issue 0003-08 | `~/slmos-ref/arm/arm-cortex-a78ae-trm-101779-0003-08.{pdf,txt}` |
| A72 TRM | Arm Cortex-A72 MPCore TRM, 100095, r0p3, issue 0003-06 | `~/slmos-ref/arm/arm-cortex-a72-trm-100095-0003-06.{pdf,txt}` |
| DSU TRM | Arm DynamIQ Shared Unit TRM, 100453, r4p1, issue 0401-05 | `~/slmos-ref/arm/arm-dsu-trm-100453-0401-05.{pdf,txt}` |
| Arm ARM | Arm Architecture Reference Manual, DDI 0487, issue M.d | `~/slmos-ref/arm/arm-ddi0487-Md.{pdf,txt}` |
| PSCI | Power State Coordination Interface, DEN0022, issue F.b | `~/slmos-ref/arm/arm-den0022-psci-Fb.{pdf,txt}` |
| TF-A upstream (unpatched) | master snapshot 2024-05-09, Makefile says v2.10 | `~/slmos-ref/rpi/arm-trusted-firmware-master/` |
| TF-A upstream (patched) | v2.14.0 with SLM-OS IRQ patches 0001-0003 | `~/slmos-ref/tf-a/` |
| NVIDIA TF-A | L4T R36 public source, NVIDIA fork of v2.8, SLM-OS patch 0004 applied | `~/slmos-ref/nvidia/Linux_for_Tegra/source/arm-trusted-firmware/` (abbreviated `NV-ATF/` below) |
| QEMU | `target/arm/tcg/cpu64.c`, master, fetched 2026-11-08 | `~/slmos-ref/qemu/qemu-target-arm-tcg-cpu64.c` |
| Linux (Pi) | Raspberry Pi kernel 6.18.27 | `~/slmos-ref/rpi/linux-rpi-6.18.y/` |
| Linux (Jetson) | L4T kernel 5.15.185 (`kernel-jammy-src`), extracted from `~/slmos-ref/nvidia/Linux_for_Tegra/source/kernel_src.tbz2` | cited as `kernel_src.tbz2:kernel/kernel-jammy-src/<path>` |
| Linux (mainline) | 7.1.0 | `~/slmos-ref/linux/linux/` |

The SLM-OS TF-A patches (`tools/tfa-patches/0001` to `0004`) contain no ACTLR,
CPUECTLR or CLUSTERECTLR access (`grep -il 'actlr\|ectlr\|c15_c1_4' tools/tfa-patches/*`
returns nothing), so the patched trees are representative of stock for every
question here.

---

## Q1. CPUECTLR_EL1 on Cortex-A76 and Cortex-A78AE; is there an SMPEN field

**Short answer.** Neither core has an SMPEN field. On Cortex-A76, CPUECTLR_EL1 bit 6 is
RES0. On Cortex-A78AE, bit 6 is the low bit of PF_MODE[7:6] (prefetch aggressiveness).
No field is RES1 for coherency and no field must be set before enabling caches. Both
TRMs only say the register "can be written only when the system is idle" and recommend
writing it after power-up reset, before the MMU is enabled. SMPEN is a Cortex-A53/A57/A72
field at a different encoding (S3_1_C15_C2_1, bit 6). On Cortex-A72 even SMPEN only
controls receipt of broadcast instruction-cache and TLB maintenance; the A72 TRM states
that its L1 data cache and L2 are always coherent regardless of SMPEN.

**Cortex-A76 CPUECTLR_EL1** (A76 TRM §13.32; `arm-cortex-a76-trm-100798-0401-01.txt:7457-8008`)

- Encoding S3_0_C15_C1_4 (op0=3, op1=0, CRn=15, CRm=1, op2=4).
- Reset value 0x0000000961563000 (`.txt:7465`). Bit 6 is 0 at reset and is RES0 (`.txt:7906`).
- Fields with a cache or coherency effect:
  - L2_FLUSH [43]: allocation behavior of copybacks from L2 hardware flush and DC CISW (whether they allocate in L3).
  - CA_UCLEAN_EVICT_EN [35] (reset 1): send WriteEvict on the CHI interface for UniqueClean evictions.
  - CA_EVICT_DIS [34] (reset 0): disable Evict transactions for clean evictions (needed only when a downstream snoop filter wants notification).
  - ATOMIC_*_NEAR [32:29]: whether atomics to WB memory make a fill request (near) or go far.
  - WS_THR_L2/L3/L4/DRAM [25:18], WS_THR_DCZVA [17]: write-streaming thresholds.
  - PF_DIS [15], PF_SS_L2_DIST [13:12], PF_STI_DIS [8], PF_STS_DIS [7], RPF_DIS [5], RPF_LO_CONF [4], RPF_PHIT_EN [3]: prefetchers.
  - EXTLLC [0]: PMU accounting only.
  - The remaining fields are TLB, page-aggregation and max-power throttle controls.
- No field enables or disables coherency. Usage constraints: the register "can be written
  only when the system is idle", and Arm recommends writing it after a power-up reset,
  before the MMU is enabled (`.txt:7963-7965`).

**Cortex-A78AE CPUECTLR_EL1** (A78AE TRM §3.2.47; `arm-cortex-a78ae-trm-101779-0003-08.txt:9098-9675`)

The A78AE TRM calls it CPUECTLR_EL1, at the same encoding S3_0_C15_C1_4. TF-A names it
CORTEX_A78_CPUECTLR_EL1 (`NV-ATF/include/lib/cpus/aarch64/cortex_a78.h:20`,
aliased in `cortex_a78_ae.h:23`).

- Reset value 0xA000000B40543000 (`.txt:9105`). Bit 6 is 0 at reset.
- Bit 6 belongs to PF_MODE [7:6], reset 00 "Dynamic aggressiveness" (`.txt:9558`).
- Coherency-relevant fields match A76: L2_FLUSH [43], CA_UCLEAN_EVICT_EN [35],
  CA_EVICT_DIS [34], ATOMIC_LD_FORCE_NEAR [33], ATOMIC_*_NEAR [32:29], WS_THR_* [25:18],
  prefetch controls. A78AE-specific: CMC_MIN_WAYS [63:61], L2_INST_PART [60:58],
  L2_DATA_PART [57:55] (L2 way partitioning), RAS_RAZ [1] (RAS record reads return zero,
  for Lock-mode).
- Same "written only when the system is idle" constraint.

**Origin of the H1 belief** (Cortex-A53/A57/A72)

- A72 TRM §4.3.67, Table 4-79, bit [6] SMPEN: "Enables the processor to receive
  instruction cache and TLB maintenance operations broadcast from other processors in the
  cluster." The bit must be set before enabling the caches and MMU or performing any cache
  and TLB maintenance. Notes: "This bit has no impact on data cache
  maintenance operations" and "In the Cortex-A72 processor, the L1 data cache and L2 cache
  are always coherent, for shared or non-shared data, regardless of the value of the SMPEN
  bit." (`arm-cortex-a72-trm-100095-0003-06.txt:11803-11820`). §5.5 repeats the "must set
  before the caches and MMU are enabled" rule (`.txt:16310`).
- TF-A sets SMPEN at reset for these cores: `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a53.S:128-131`,
  `cortex_a72.S:133-141`, with `CORTEX_A5x/A72_ECTLR_EL1 = S3_1_C15_C2_1` and
  `ECTLR_SMP_BIT = 1 << 6` (`include/lib/cpus/aarch64/cortex_a53.h:27-29`,
  `cortex_a57.h:28-30`, `cortex_a72.h:21-23`).
- SLM-OS `cpu_has_smpen()` reads S3_0_C15_C1_4 and tests bit 6
  (`kernel/include/cache.h:32-43`), that is, the A72 bit position at the A76 encoding.
  On A76 that bit is RES0 and on A78AE it is PF_MODE[0], reset 0, and no cached TF-A path
  for either board writes bit 6 (Q3). **Inference:** the boot messages "SMPEN NOT set"
  (`kernel/sched/smp.c:406`, `:749`) say nothing about coherency on either board. The bit
  has no coherency function whatever its value. It reads 0 unless EL3 code other than the
  cached BL31 sources wrote it; that part is unsourced (Q3, "EL3 state not written by
  BL31"). The lab record "Reading CPUECTLR_EL1 from EL1 works, confirmed
  SMPEN=0 on all 4 CPUs" (commit aedf6e1a) is this read.

---

## Q2. Who may read and write CPUECTLR_EL1; ACTLR_EL3.ECTLREN and ACTLR_EL2.ECTLREN

**Short answer.** Reads are permitted at NS-EL1, NS-EL2 and EL3 regardless of ECTLREN.
ECTLREN gates writes only. Separately, at NS-EL1 the architectural HCR_EL2.TIDCP trap
applies to reads and writes: when the EL2 software sets it, every NS-EL1 access to this
encoding traps to EL2. It never applies at EL2. ACTLR_EL3.ECTLREN (bit 1, reset 0) gates writes from EL2 and
Secure EL1; ACTLR_EL2.ECTLREN (bit 1, reset 0) additionally gates writes from Non-secure
EL1. The core TRMs say only "not write-accessible" and do not name the exception. The
DSU TRM, for CLUSTERECTLR, which is gated by the same ECTLREN bits, says: "If write access
is not possible, then Trap to the lowest Exception level that denied access (EL2 or EL3)."
A denied write from NS-EL2 is therefore a trap to EL3 (sourced for CLUSTERECTLR;
**Inference** for CPUECTLR, from the shared control bit, the QEMU model and the lab
record). What happens next depends on BL31 (Q3): NVIDIA's v2.8 fork panics at EL3,
TF-A v2.11 and later inject an UNDEFINED exception back into the lower EL.

Citations:

- A76 TRM §13.32 Accessibility table (`.txt:7990-8000`): CPUECTLR_EL1 is RW at EL1 and EL3
  for NS=0; RW at EL1, EL2 and EL3 for NS=1, TGE=0; RW at EL2 and EL3 for NS=1, TGE=1
  (EL1 n/a); EL0 "-" (not accessible). "Traps and enables: Access to this register depends
  on bit[1] of ACTLR_EL2 and ACTLR_EL3." (`.txt:8008`)
- A76 TRM §13.6 ACTLR_EL2, reset 0x0 (`.txt:5377`), ECTLREN [1]: "0 CPUECTLR and
  CLUSTERECTLR are not write-accessible from EL1 Non-secure. This is the reset value. 1 ...
  write-accessible from EL1 Non-secure if they are write-accessible from EL2."
- A76 TRM §13.7 ACTLR_EL3, reset 0x0 (`.txt:5539`), ECTLREN [1]: "0 CPUECTLR and CLUSTERECTLR
  are not write-accessible from EL2 and EL1 Secure. This is the reset value. 1 ...
  write-accessible from EL2 and EL1 Secure." (`.txt:5650`)
- Wording contrast in the same register: AMEN [4] says accesses "are trapped to EL3"; ECTLREN
  says only "not write-accessible". The core TRM does not state the exception for ECTLREN.
- A78AE TRM §3.2.6 ACTLR_EL2 and §3.2.7 ACTLR_EL3, ECTLREN [1]: identical text ("CPUECTLR*
  and CLUSTERECTLR"); §3.2.47 accessibility table and "depends on bit[1] of ACTLR_EL2 and
  ACTLR_EL3" (`arm-cortex-a78ae-trm-101779-0003-08.txt:6107`, `:6252`, `:9675`).
- DSU TRM §11.8 CLUSTERECTLR, Traps and enables: "This register is write accessible in EL1
  if ACTLR_EL3.ECTLREN is 1 and ACTLR_EL2.ECTLREN is 1, or ACTLR_EL3.ECTLR_EN is 1 and
  SCR.NS is 0. If write access is not possible, then Trap to the lowest Exception level
  that denied access (EL2 or EL3)." (`arm-dsu-trm-100453-0401-05.txt:5834-5839`). Every
  other DSU register gated by an ACTLR bit uses the same sentence (`.txt:6055`, `:6151`, ...).
- Cortex-A72 for contrast (A72 TRM §4.3.67, `.txt:11699-11722`): reads RW at all ELs, "Write
  access if ACTLR_EL3.CPUECTLR is 1 and ACTLR_EL2.CPUECTLR is 1, ..." Same read-open,
  write-gated model.
- QEMU (secondary source, an Arm-TRM-derived model): `access_actlr_w()` returns
  CP_ACCESS_OK for every read and CP_ACCESS_TRAP_EL2 / CP_ACCESS_TRAP_EL3 for writes below
  EL2 / EL3, "Because ACTLR_EL3 is constant 0, writes below EL3 trap to EL3."
  (`~/slmos-ref/qemu/qemu-target-arm-tcg-cpu64.c:544-559`, used for CPUECTLR_EL1 at
  `:599-602`). This model is attached to Neoverse-N1 and V1 only. The QEMU `cortex-a76`
  and `cortex-a78ae` models (`:340`, `:415`) define no IMPDEF registers, so in QEMU any
  CPUECTLR access is UNDEFINED. QEMU is not a reference for this register.
- EL0: Arm ARM D24.2.61 HCR_EL2.TIDCP (`arm-ddi0487-Md.txt:557480`): EL0 access to
  IMPLEMENTATION DEFINED encodings is UNDEFINED, performed, or trapped, IMPLEMENTATION
  DEFINED per encoding, when TIDCP is 0. The TRM table lists EL0 as "-".
- NS-EL1 and HCR_EL2.TIDCP (Arm ARM D24.2.61, `arm-ddi0487-Md.txt:557480-557492`): "Traps
  EL1 accesses to the encodings reserved for IMPLEMENTATION DEFINED functionality to EL2,
  when EL2 is enabled in the current Security state". In AArch64 this covers "IMPLEMENTATION
  DEFINED System registers, which are accessed using MRS and MSR with the
  S3_<op1>_<Cn>_<Cm>_<op2> register name" with CRn == {11, 15}, reported as EC 0x18. CPUECTLR_EL1
  is CRn=15, so with TIDCP=1 a **read** from NS-EL1 also traps to EL2. The TRM accessibility
  table describes the access once it is not trapped. TIDCP traps EL1 accesses only, so it
  never affects code running at EL2.
  What this means under Linux:
  - Linux at EL2 with VHE (the normal case on both boards, Q6): the kernel runs at EL2,
    TIDCP does not apply, and the read reaches the register.
  - Linux at EL1 (nVHE host): whether the read traps depends on the HCR_EL2 the Linux EL2
    stub or nVHE hypervisor programs. HCR_HOST_NVHE_FLAGS and
    HCR_HOST_NVHE_PROTECTED_FLAGS do not contain TIDCP
    (`~/slmos-ref/rpi/linux-rpi-6.18.y/arch/arm64/include/asm/kvm_arm.h:104-105`), so the
    read is not trapped.
  - KVM guest at EL1: `HCR_GUEST_FLAGS` contains TIDCP (`kvm_arm.h:100-103`), so the read
    traps to the host. Unknown registers get an injected UNDEF (`arch/arm64/kvm/sys_regs.c:4961-4977`).
  - SLM-OS at EL2 (E2H=1, TGE=1 after kexec on Jetson and on Pi 5; E2H=0 at UEFI-direct
    entry): TIDCP is irrelevant.
- Lab record (observation, not a register-behavior source): commit aedf6e1a, Pi 5:
  "writing CPUECTLR_EL1 from EL2 traps to EL3 (hangs). Reading CPUECTLR_EL1 from EL1
  works".

Which exception reaches NS software for a denied write:

- Jetson (NVIDIA TF-A v2.8 fork, stock image `tos-optee_t234.img.orig` reports
  "v2.8(release):e12e3fa93"): EC 0x18 from a lower EL goes to `sync_handler64`
  (`NV-ATF/bl31/aarch64/runtime_exceptions.S:197-198`), then `sysreg_handler64`
  (`:511-514`, `:581-597`). `handle_sysreg_trap()` handles only RNDR/RNDRRS and, if
  ENABLE_FEAT_IMPDEF_TRAP, IMPDEF encodings; otherwise it returns TRAP_RET_UNHANDLED = -1
  (`NV-ATF/bl31/bl31_traps.c:13-30`, `include/bl31/sync_handle.h:23`). The handler then
  takes `b.mi do_panic` (`runtime_exceptions.S:597`). ENABLE_FEAT_IMPDEF_TRAP defaults to 0
  (`NV-ATF/make_helpers/defaults.mk:180`) and is not set by t234. Result: EL3 panic, board
  stops until power cycle.
- Upstream TF-A v2.11.0 and later: "introduce UNDEF injection to lower EL"
  (`~/slmos-ref/tf-a/docs/change-log.md:3972`, in the 2.11.0 section). In v2.14 the
  unhandled EC 0x18 path calls `inject_undef64(ctx)` (`~/slmos-ref/tf-a/common/runtime_svc.c:251-302`).
  The 2024-05-09 master snapshot already has it (`~/slmos-ref/rpi/arm-trusted-firmware-master/bl31/aarch64/runtime_exceptions.S:469-505`).
  Before v2.9, EC 0x18 from a lower EL was treated as an external abort and reported as an
  unhandled exception (system register trap handler introduced in v2.9.0,
  `change-log.md:6400`). Result on Pi 5: depends on the BL31 version in the EEPROM (Q3).

---

## Q3. What TF-A writes on each board

**Short answer.** No cached TF-A source for either board sets ACTLR_EL3.ECTLREN or
ACTLR_EL2.ECTLREN.

- Pi 5, sourced for **upstream** TF-A only (`plat/rpi/rpi5`, A76 r4p1): it writes no
  CPUECTLR, ACTLR_EL3/EL2, CLUSTERECTLR or DSU register. The BL31 that actually runs on
  pi-5-1 is a Raspberry Pi build of undetermined version and source (see "Version of the
  Pi 5 stock BL31" below). **Inference:** the running BL31 behaves like upstream here.
- Jetson, sourced for the NVIDIA t234 tree that the running BL31 is built from: it writes
  CPUECTLR bit 46 (HPA_DIS), several CPUACTLR* errata bits, ACTLR_EL3.CLUSTERPMUEN (bit 12,
  non-production fuse only) and CLUSTERECTLR bits 0 and 14. None of these is a coherency
  enable.
- Both platforms declare `HW_ASSISTED_COHERENCY := 1`.
- BL31 is not the only EL3 code that may set these registers, and CPU 0 is not reset
  across kexec. "ECTLREN = 0" is therefore sourced for BL31's writes only. "EL3 state not
  written by BL31" below covers the rest and marks what has no source.

### Pi 5 (upstream TF-A `plat/rpi/rpi5`, Cortex-A76)

Two upstream trees are cited: `~/slmos-ref/tf-a/` (v2.14.0, SLM-OS IRQ patches applied)
and `~/slmos-ref/rpi/arm-trusted-firmware-master/` (unpatched master snapshot of
2024-05-09). Both have the same rpi5 platform lines quoted here. The original brief named
`~/slmos-ref/tf-a/arm-trusted-firmware-master/`, which does not exist.

| Location | Register | Write | Applies on Pi 5 |
|---|---|---|---|
| `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:489-532` (`cpu_reset_func_start`) | CPUACTLR2_EL1 bit 16 | DISABLE_LOAD_PASS_STORE, CVE-2018-3639, only under `DYNAMIC_WORKAROUND_CVE_2018_3639` and only if ID_AA64PFR1_EL1.SSBS is 0 (`:491-503`) | No: `DYNAMIC_WORKAROUND_CVE_2018_3639 ?=0` (`~/slmos-ref/tf-a/lib/cpus/cpu-ops.mk:32`), not set by rpi5 |
| `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:373-375` | CPUECTLR_EL1 WS_THR_L2 | erratum 1220197 | No: revisions up to r2p0 only (`:377`) |
| `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:391-393` | CPUECTLR_EL1 bit 51 | erratum 1262888 | No: up to r3p0 (`:395`) |
| `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:426-455` | CPUPSELR/CPUPOR/CPUPMR/CPUPCR_EL3 (S3_6_C15_C8_*) | erratum 1946160 instruction patch | Yes (`~/slmos-ref/tf-a/plat/rpi/rpi5/platform.mk:51`); EL3-only patch registers, not ECTLR |
| `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:538-549` | CPUPWRCTLR_EL1.CORE_PWRDN_EN | set on PSCI CPU_OFF / suspend path | Only on power-down; erratum 2743102 enabled (`~/slmos-ref/tf-a/plat/rpi/rpi5/platform.mk:52`) |
| `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:309-325` | CLUSTERACTLR_EL1 | DSU errata 798953 / 936184 | No: flags not set in `~/slmos-ref/tf-a/plat/rpi/rpi5/platform.mk`, default 0 |
| `~/slmos-ref/tf-a/plat/rpi/common/aarch64/plat_helpers.S:247-267` (`plat_reset_handler`) | CORTEX_A72_L2CTLR_EL1 | L2 latency | No: runs only when MIDR is A72 (RPi4) |

- `grep -i 'actlr\|ectlr'` over `~/slmos-ref/rpi/arm-trusted-firmware-master/plat/rpi/rpi5/*.c`,
  `plat/rpi/common/*.c` and `plat/rpi/common/aarch64/*.S` (snapshot tree) finds only the A72
  L2CTLR line. Generic TF-A code reads ACTLR_EL2 only
  to save EL2 context (`~/slmos-ref/tf-a/lib/el3_runtime/aarch64/context_mgmt.c:141-144`,
  `:1540`, `:1575`); it never sets ECTLREN.
- A76 revision r4p1: `~/slmos-ref/tf-a/plat/rpi/rpi5/platform.mk:50` ("Enable all errata workarounds for
  Cortex-A76 r4p1"). Not measured; a MIDR_EL1 read would confirm.
- `HW_ASSISTED_COHERENCY := 1` with the comment "Hardware-managed coherency"
  (`~/slmos-ref/rpi/arm-trusted-firmware-master/plat/rpi/rpi5/platform.mk:67`). TF-A doc:
  "newer systems exist where CPUs' entry to and exit from coherency is managed in hardware"
  (`~/slmos-ref/tf-a/docs/getting_started/build-options.rst:813-821`).
- BL33 is entered at EL2: `rpi3_get_spsr_for_bl33_entry()` returns `SPSR_64(MODE_EL2, ...)`
  (`~/slmos-ref/tf-a/plat/rpi/common/rpi3_common.c:207-216`).

**Version of the Pi 5 stock BL31.** pi-5-1 runs the BL31 embedded in the Raspberry Pi
EEPROM firmware (no `armstub=`, per the gauntlet log). The EEPROM image in the cache
(`~/slmos-ref/rpi/eeprom/pieeprom-2024-09-23.bin`, the version the lab pins to) has no
separate BL31 file: its sections are `bootmain` (compressed, magic 0x55aaf33f, offset
0x20000), `memsys0x.bin`, `mcb.bin`, `logo.bin`, `font.bin`, `qrcode.bin`, `pubkey.bin`,
`cacert.der`, `bootconf.*`; `strings` finds no TF-A banner. The version could not be
determined from cached material. Upstream rpi5 support first shipped in TF-A v2.11.0
(`~/slmos-ref/tf-a/docs/change-log.md:3856`, May 2024), after the Pi 5 launch.
**Inference:** the Pi 5 firmware BL31 comes from Raspberry Pi's own TF-A fork and may
predate v2.11. The upstream source is representative for "does BL31 set ECTLREN or touch
CPUECTLR" (the A76 CPU file and rpi platform code do neither on r4p1, in every upstream
version cached), and it is not representative for "what happens after a trapped write"
(panic before v2.11, UNDEF injection from v2.11). The lab record of a hang on a CPUECTLR
write from EL2 fits either a pre-v2.11 panic or an injected UNDEF taken at EL2 with no
usable vector at that point. Undetermined.

### Jetson (NVIDIA TF-A t234, Cortex-A78AE)

Build: `NV-ATF/plat/nvidia/tegra/soc/t234/platform_t234.mk` compiles only
`lib/cpus/aarch64/cortex_a78_ae.S` (not `cortex_a78.S`). Enabled errata: 2466780, 2743093,
2743229, 2821985, 2855379, 3604886. `HW_ASSISTED_COHERENCY := 1` (`:69`).
`WORKAROUND_CVE_2024_5660 ?= 1` (`NV-ATF/lib/cpus/cpu-ops.mk:28`).

| Location (`NV-ATF/...`) | Register | Bits and meaning | Active on t234 |
|---|---|---|---|
| `lib/cpus/aarch64/cortex_a78_ae.S:33-46` | CPUECTLR_EL1 | clear bit 8 (PF_STI_DIS), erratum 1941500 | No: `ERRATA_A78_AE_1941500 ?=0` (`cpu-ops.mk:367`) |
| `cortex_a78_ae.S:63-98` | S3_6_C15_C8_* | erratum 1951502 patch | No: default 0 (`cpu-ops.mk:371`) |
| `cortex_a78_ae.S:115-135` | CPUACTLR2_EL1 bit 0 | erratum 2376748 | No: default 0 (`cpu-ops.mk:375`) |
| `cortex_a78_ae.S:152-170` | CPUACTLR2_EL1 bit 40 | erratum 2395408 | No: default 0 (`cpu-ops.mk:379`) |
| `cortex_a78_ae.S:185-202` | CPUPSELR/CPUPOR/CPUPMR/CPUPCR_EL3 | erratum 2466780, flush MOP cache after SCR/HCR/SCTLR writes (r0p0-r0p2) | Yes |
| `cortex_a78_ae.S:220-233` | CPUACTLR5_EL1 bits [56:55]=01 | erratum 2743229 (r0p0-r0p2) | Yes |
| `cortex_a78_ae.S:249-258` | none (DSB in power-down) | erratum 2743093 | Yes |
| `cortex_a78_ae.S:275-286` | CPUACTLR5_EL1 bit 39 | erratum 2855379 | Yes |
| `cortex_a78_ae.S:303-314` | CPUACTLR3_EL1 bit 58 | erratum 3604886 | Yes |
| `cortex_a78_ae.S:349-360` | CPUECTLR_EL1 bit 46 | HPA_DIS, disable hardware page aggregation, CVE-2024-5660 | Yes (default 1) |
| `cortex_a78_ae.S:412-422` | ACTLR_EL3 / ACTLR_EL2 bit 30 | clear TAM (AMU trap) | Compiled out: guarded by `ENABLE_FEAT_AMU`, which defaults.mk does not define (it defines `ENABLE_FEAT_AMUv1 := 0`, `:137`). **Inference** that it is 0 |
| `cortex_a78_ae.S:449-465` | CPUPWRCTLR_EL1 bit 0 | CORE_PWRDN_EN on power-down | Power-down path only |
| `plat/nvidia/tegra/common/aarch64/tegra_helpers.S:59-103` (`cpu_init_common`, from `plat_reset_handler` `:272`) | ACTLR_EL3 and ACTLR_EL2 bits 0,1,4,5,6 | CPUACTLR, CPUECTLR, L2CTLR, L2ECTLR, L2ACTLR NS access | No: guarded by `cmp x0, #MIDR_PN_CORTEX_A57; b.ne 1f` (`:66-71`); A78AE skips to `1:` |
| `plat/nvidia/tegra/soc/t234/plat_psci_handlers.c:323-338`, call `:380-386` | ACTLR_EL3 bit 12 | CLUSTERPMUEN, write access to CLUSTERPM* from EL2 | Only if fuse SECURITY_MODE == 0 (non-production) |
| `plat/nvidia/tegra/soc/t234/plat_setup.c:254-263` (cold boot) and `plat_psci_handlers.c:359-369` (CPU_ON finish) | CLUSTERECTLR_EL1 bits 0 and 14 | bit 0: interleave Normal Non-cacheable transactions across master interfaces "like Cacheable transactions"; bit 14: WriteEvict on UniqueClean evictions (DSU TRM §11.8) | Yes |
| `plat/nvidia/tegra/soc/t234/plat_setup.c:513-514` | CPUECTLR_EL1 (read) | asserts write streaming is not fully disabled | Read only |
| `plat/nvidia/tegra/soc/t234/plat_setup.c:232`, `:546-549`; `drivers/mce/ari.c:383-405` | MCE ARI "CCPLEX cache clean" | "SCF flush - Clean and invalidate caches" at cold boot, before entering the NS payload, and for SC7 | Yes |

- NVIDIA's own comment: "Linux kernel is in EL2, so ACTLR_EL3.CLUSTERPMUEN bit needs to be
  set" (`plat_psci_handlers.c:328-331`). NVIDIA chose to open CLUSTERPMUEN only, and only
  on unfused boards; ECTLREN stays 0.
- Stock vs patched BL31: jetson-nano-2 runs a BL31 built from this tree plus SLM-OS patch
  0004 (IRQ routing only). For every write listed above the two are identical.

### EL3 state not written by BL31

ACTLR_EL3, ACTLR_EL2 and CPUECTLR_EL1 are per-core registers. A register's value at the
time SLM-OS runs is its reset value plus every write since the last reset of that core,
from any EL3 code, not only BL31.

- **Jetson, EL3 code before BL31.** NVIDIA: "MB2 runs on CCPLEX" (Jetson Developer Guide
  r36.4, Jetson Orin Series Boot Flow, section MB2;
  `~/slmos-ref/nvidia/DevGuide/web/nvidia-r36.4-JetsonOrinSeriesBootFlow.html`). The same
  page says MB1 (on BPMP) "initializes certain parts of the SoC, including the CPU". BL31 is
  built `RESET_TO_BL31 := 1`, `COLD_BOOT_SINGLE_CPU := 1` (`NV-ATF/plat/nvidia/tegra/soc/t234/platform_t234.mk:24-26`)
  and takes its parameters from the previous bootloader (`NV-ATF/plat/nvidia/tegra/common/tegra_bl31_setup.c:135-145`,
  `:174-176`). The page does not give MB2's exception level. **Unsourced:** whether MB2 (closed
  source) runs at EL3 on CPU 0 and whether it writes ACTLR_EL3, ACTLR_EL2 or CPUECTLR_EL1.
  **Inference:** MB2 runs at EL3, because BL31 is entered after it on the same core.
- **Jetson CPU 0 across kexec and UEFI-direct.** kexec does not reset CPU 0, and neither
  does the UEFI-direct path. CPU 0's ACTLR_EL3 and CPUECTLR_EL1 therefore hold whatever
  MB2 and BL31 left at cold boot, plus any EL3 writes made at runtime (none found in BL31
  for ECTLREN). Linux at EL2 cannot write ACTLR_EL3. Only BL31's part of this is sourced.
- **Jetson secondaries.** CPU_ON goes to the MCE (`MCE_CMD_ONLINE_CORE`,
  `NV-ATF/plat/nvidia/tegra/soc/t234/plat_psci_handlers.c:315`) and BL31's reset handler
  runs again on the woken core. A core that Linux offlined before kexec went through
  CPU_OFF, which is the Off mode (A78AE TRM §2.4.6.2: "all core state is lost").
  **Inference:** a secondary brought up by CPU_ON comes out of a core reset, so its
  ACTLR_EL3 starts at 0 and only BL31's warm-boot writes apply; MB2 does not run on it.
- **Pi 5, EL3 code before BL31.** `RESET_TO_BL31 := 1` and "All CPUs enter armstub8.bin"
  (`COLD_BOOT_SINGLE_CPU := 0`) (`~/slmos-ref/tf-a/plat/rpi/rpi5/platform.mk:33-36`): in the
  upstream design BL31 is the first code the A76 cores execute. **Unsourced:** whether the
  Raspberry Pi firmware runs any other code on the A76 cores at EL3 before its BL31 and
  what its own BL31 build writes (Q3 version note).
- **Pi 5 secondaries are not reset by CPU_ON** in upstream TF-A. All four cores enter BL31
  at cold reset. Secondaries park in `plat_wait_for_warm_boot`, a WFE loop on a hold
  mailbox (`~/slmos-ref/tf-a/plat/rpi/common/aarch64/plat_helpers.S:83-124`).
  `rpi3_pwr_domain_on()` only writes GO to the mailbox and issues SEV
  (`~/slmos-ref/tf-a/plat/rpi/common/rpi3_pm.c:146-166`). The core then jumps to the warm-boot
  entry point. Its ACTLR_EL3 and CPUECTLR_EL1 are cold-reset values plus BL31 writes. Its
  caches were invalidated at cold reset, not at CPU_ON, and it has executed BL31 code
  since then. PSCI still requires the caches to be clean and coherent from the caller's
  view at entry (PSCI DEN0022 F.b §6.4.3.4).
  **Inference:** the running Raspberry Pi BL31 uses the same holding-pen scheme.
- **Effect on the SLM-OS "SMPEN" read.** On A76, bit 6 of CPUECTLR_EL1 is RES0. A RES0 bit
  in a read/write register returns the last value written, or the reset value if never
  written, and "must have no effect on the operation of the PE" (Arm ARM DDI 0487 M.d,
  Glossary, RES0, `arm-ddi0487-Md.txt:1091430-1091445`). On A78AE, bit 6 is PF_MODE[0],
  which only affects prefetch. The value SLM-OS reads is therefore 0 unless some EL3 code
  wrote bit 6, and either way it has no bearing on coherency. The value itself is sourced
  only for BL31.

---

## Q4. DSU coherency, what removes a core from the domain, and Tegra234 cross-cluster coherency

**Short answer.** Within a DynamIQ cluster, coherency between the private L1/L2 caches and
the shared L3 is maintained in hardware by the SCU and snoop filter; a core joins the
coherency domain at power-up with no software steps and leaves it automatically on the
transition to Off. The software-visible trigger for leaving is
CPUPWRCTLR_EL1.CORE_PWRDN_EN=1, ISB, WFI (issued by TF-A on CPU_OFF). Re-entry is
reset, which PSCI CPU_ON causes on Jetson; on Pi 5 a core is reset only at cold boot
(Q3). There is no software coherency-enable bit. Cross-cluster coherency on Tegra234:
**inference** that NVIDIA's CCPLEX fabric (the "SCF" named in TF-A) provides it in
hardware. No public primary document describing it was reachable. Only firmware
configuration and Linux behavior support it.

Citations:

- DSU TRM §2.1: "The Snoop Control Unit (SCU) maintains coherency between caches in the
  cores and L3. The SCU includes a snoop filter to optimize coherency maintenance
  operations." Figure 2-1 shows each core with private L1 and L2 (`.txt:690-755`).
- DSU TRM §5.9: "The cluster is taken out of coherence with the interconnect automatically
  when it is powered down. No software sequence is required." §5.9.1: "The cluster enables
  coherency during powerup and disables it during powerdown", using SYSCOREQ/SYSCOACK
  (`.txt:2697-2760`). The §5.9 note qualifies the automatic part: "To support automated
  removal from coherency, the interconnect must support the SYSCOREQ and SYSCOACK handshake
  protocol. If not, the SoC is responsible for programming the interconnect to remove the
  cluster from coherency." (`.txt:2706-2709`). §5.9.1 adds that if the interconnect lacks
  these signals, Arm recommends connecting them to the power controller, which "must take
  any actions necessary to make the transition" (`.txt:2722-2736`).
- DSU TRM Appendix B.2.10, BROADCASTOUTER: "Within the cluster, Inner Shareable and Outer
  Shareable memory are treated identically." (`.txt:12715-12722`)
- A76 TRM §5.9 "Core powerup and powerdown sequences": powerdown is (1) save state,
  (2) reroute GIC, (3) set CPUPWRCTLR.CORE_PWRDN_EN, (4) ISB, (5) WFI; "All L1 and L2 cache
  disabling, L1 and L2 cache flushing, and communication with the L3 memory system is
  performed in hardware after the WFI is executed". "Core powerup: To bring a core into
  coherence after reset, no software steps are required." (`.txt:2142-2183`)
- A76 TRM §5.6.2 Off mode: "The L1 and L2 caches are disabled, flushed and the core is
  removed from coherency automatically on transition to Off mode." (`.txt:1956-1970`)
- A76 TRM §7.2.5: "To maintain data coherency between multiple cores, the Cortex-A76 core
  uses the Modified Exclusive Shared Invalid (MESI) protocol." (`.txt:2811`)
- A78AE TRM §2.4.6.2 Off mode and §2.4.9 Core powerup and powerdown sequences: same text
  ("To bring a core into coherence after reset, no software steps are required",
  `.txt:2289`). §2.6.2.5: MESI.
- TF-A issues the powerdown: `~/slmos-ref/tf-a/lib/cpus/aarch64/cortex_a76.S:538-549`,
  `NV-ATF/lib/cpus/aarch64/cortex_a78_ae.S:449-465`. CPUPWRCTLR is not write-accessible from
  NS-EL2 at reset (ACTLR_EL3.PWREN [7], reset 0, A76 TRM §13.7), so SLM-OS cannot remove a
  core from coherency by accident through this register.
- PSCI DEN0022 F.b §5.4.8: the PSCI implementation "needs to perform invalidation of caches
  on booting, unless this is automatically supported by the hardware, and to manage
  coherency" (`arm-den0022-psci-Fb.txt:2540-2547`).

Tegra234 (two populated clusters, 4 + 2):

- Linux DT: CPUs 0-3 have `next-level-cache` L2s chained to `l3c0`, CPUs 4-5 to `l3c1`;
  each L3 is 2 MB (`~/slmos-ref/linux/linux/arch/arm64/boot/dts/nvidia/tegra234.dtsi:5342-5440`,
  `:5625-5760`). This is one DSU (with L3) per cluster. The CCPLEX node
  (`compatible = "nvidia,tegra234-ccplex-cluster"`, `:4075-4079`) is the cpufreq
  interface; the DT does not describe the inter-cluster fabric.
- NVIDIA TF-A: `HW_ASSISTED_COHERENCY := 1` (`platform_t234.mk:69`); MCE ARI commands
  "Initiate an SCF level Cache Clean" / "Cache invalidate and Clean" (`drivers/mce/ari.c:383-405`),
  used at cold boot and before the NS hand-off (`plat_setup.c:232`, `:546-549`);
  `PLATFORM_CLUSTER_COUNT := 3`. RAS has an SCF error source (`plat_ras.c:416`).
  **Inference:** a coherent fabric (SCF) with its own cache level sits above the per-cluster
  DSU L3s and keeps clusters coherent in hardware; the name expansion and topology are not
  in any cached source.
- Relation to DSU §5.9. Each Tegra234 cluster's DSU must enter and leave coherency with
  that fabric, either through SYSCOREQ/SYSCOACK or with the SoC (power controller)
  programming the interconnect. In NVIDIA BL31, cluster and core power transitions are
  requests to the MCE firmware (`MCE_CMD_ONLINE_CORE`, `plat_psci_handlers.c:315`; C-state
  updates `mce_update_cstate_info`). No BL31 code programs an interconnect coherency
  register directly; the only fabric-level operation is the SCF clean and invalidate.
  **Unsourced:** whether the Tegra234 fabric implements the SYSCOREQ/SYSCOACK handshake or
  whether the MCE or another controller performs the "SoC is responsible" step.
  **Inference:** cluster coherency entry and exit on Tegra234 are handled by hardware and
  NVIDIA firmware below NS software; nothing in SLM-OS or Linux takes part, and Linux
  operates both clusters coherently after CPU_ON.
- Linux runs all six CPUs as one SMP system with Inner Shareable mappings
  (`PTE_SHARED ... inner shareable`, `~/slmos-ref/rpi/linux-rpi-6.18.y/arch/arm64/include/asm/pgtable-hwdef.h:169`)
  and its boot protocol requires: "All CPUs to be booted by the kernel must be part of the
  same coherency domain on entry to the kernel. This may require IMPLEMENTATION DEFINED
  initialisation to enable the receiving of maintenance operations on each CPU."
  (`~/slmos-ref/rpi/linux-rpi-6.18.y/Documentation/arch/arm64/booting.rst:200-204`).
  The second sentence describes the A53/A57/A72 SMPEN bit almost word for word ("Enables
  the processor to receive instruction cache and TLB maintenance operations broadcast",
  A72 TRM §4.3.67). **Inference:** it is a plausible origin of the H1 belief. On A76 and
  A78AE no such initialisation exists. CPUECTLR has no receive-enable field (Q1), and the
  cores join coherency with no software steps (A76 TRM §5.9, A78AE TRM §2.4.9). The
  sentence is also addressed to firmware, which must deliver the CPUs to the kernel already
  in one domain. Linux itself never performs such a step (Q6). Even where SMPEN exists, it
  governs broadcast I-cache and TLB maintenance, not data coherency (A72 TRM §4.3.67 note).
  Arm ARM B2.10.1.1: "This architecture assumes that all PEs that use the same operating
  system or hypervisor are in the same Inner Shareable shareability domain."
- Not reachable: the Orin TRM (login required; local copy listed in CLAUDE.md is on a
  Windows drive not mounted here). No primary NVIDIA hardware document for CCPLEX/SCF
  coherency was available. Cross-cluster coherency is sourced only indirectly (firmware
  configuration plus Linux operating the CPUs as one IS domain).

---

## Q5. How the cores treat Non-shareable Normal and Normal Non-cacheable memory; mismatched attributes

**Short answer.**

- Cacheability: A76 and A78AE cache a page only if both Inner and Outer attributes are
  Write-Back; everything else (including Inner WT, Outer WT, Outer NC) is downgraded to
  Normal Non-cacheable.
- Caches disabled (SCTLR_ELx.C=0): loads and stores do not access the L1, L2 or DSU L3
  arrays at all, and are incoherent with every cache in the cluster.
- NC accesses with the MMU on: the TRMs say only "downgraded to Non-cacheable"; they do not
  say whether an NC access looks up the caches. At the external interface the DSU issues
  NC loads as ReadNoSnoop and NC stores as WriteNoSnoop.
- Non-shareable WB: neither core TRM nor the DSU TRM says how it is handled inside the
  cluster. The DSU says only that Inner Shareable and Outer Shareable are treated
  identically inside the cluster (Non-shareable is not mentioned there), and that
  Non-shareable linefills and evictions use ReadNoSnoop/WriteNoSnoop on the external
  interface. The Arm ARM explicitly forbids relying on Non-shareable to produce incoherence.
- Mismatched aliases: the Arm ARM says loss of coherency "might" occur. A cacheable
  writer with an NC reader is not architecturally guaranteed to read stale data.

Citations:

- A76 TRM §6.6.3: "In the Cortex-A76 core, a page is cacheable only if the Inner and Outer
  memory attributes are Write-Back. In all other cases, all pages are downgraded to
  Non-cacheable Normal memory." (`.txt:2609-2610`). §8.3 Support for memory types (`.txt:3916-3923`).
- A78AE TRM §2.5.6.4 (`.txt:2720-2722`) and §2.7.3 (`.txt:4152-4160`), same rules.
- A76 TRM §7.2.3 Data cache disabled behavior: "If the data cache is disabled, load and
  store instructions do not access any of the L1 data, L2 cache, and, if present, the
  DynamIQ Shared Unit (DSU) L3 cache arrays." "All load and store instructions to cacheable
  memory are treated as if they were Non-cacheable and are incoherent with the caches in
  both this core and other cores in the cluster." "Data cache maintenance operations are
  an exception and will execute normally." (`.txt:2780-2796`)
- A78AE TRM §2.6.2.3: same text (`.txt:2883-2898`).
- DSU TRM §7.6 Table 7-8 ACE transactions: ReadNoSnoop = "Non-cacheable loads or
  instruction fetches. Linefills of non-shareable cache lines into L1, L2, or L3 caches";
  WriteNoSnoop = "Non-cacheable store instructions. Evictions of non-shareable cache lines
  from L1, L2, and L3 caches." (`.txt:3910-3936`). The CHI table (§8) has the equivalent.
  These describe the master interface out of the cluster, not the SCU's internal handling.
- Arm ARM B2.10.1.1 Note: "software developers must not assume that specifying a memory
  location as Non-shareable permits software to make assumptions about the incoherency of
  the location between different PEs in a shared memory system. ... Any multiprocessing
  implementation might implement caches that are shared, inherently, between different
  processing elements." Also: "Because all data accesses to Non-cacheable locations are
  data coherent to all observers, Non-cacheable locations are always treated as Outer
  Shareable." (`arm-ddi0487-Md.txt:20906-20937`)
- Arm ARM B2.10.1.2: Non-shareable "does not require the hardware to make data accesses by
  different observers coherent, unless the memory is Non-cacheable" (`.txt:20979-20984`).
  "Does not require" is permission, not a guarantee of incoherence.
- Arm ARM B2.11 Mismatched memory attributes (`.txt:21571-21705`): mismatch covers memory
  type, shareability and cacheability. Effects are "one or more of": loss of uniprocessor
  semantics, "There might be a loss of coherency when multiple agents attempt to access a
  memory Location", UNKNOWN exclusive monitor state, and "Bytes written without the
  Write-Back cacheable attribute within the same Write-Back granule as bytes written with
  the Write-Back cacheable attribute might have their values reverted to the old values as
  a result of cache Write-Back." Coherent access through mismatched aliases requires either
  all writes through NC/WT aliases or software cache maintenance (clean after WB write,
  invalidate before cacheable read, DMB between attribute changes).
- Arm ARM D7.5.6: "If an implementation permits cache hits when the Cacheability control
  fields force all memory locations to be treated as Non-cacheable then the cache
  initialization routine must ..." (`.txt:475662-475680`). The A76/A78AE TRM §7.2.3 /
  §2.6.2.3 statement settles that these cores do not hit when C=0.

**Inference for the negative control.** How the SCU treats Non-shareable WB lines inside
a cluster is not documented. DSU Appendix B.2.10 states identical treatment only for Inner
Shareable and Outer Shareable, and the only documented Non-shareable behavior is the
external transaction type (ReadNoSnoop/WriteNoSnoop). The result of a Non-shareable mapping
inside one cluster is therefore unknown in advance: the SCU may still keep it coherent (the
Arm ARM B2.10.1.1 note warns that implementations may), or it may not. Across Tegra234
clusters, Non-shareable linefills use ReadNoSnoop, so cross-cluster staleness is possible
but not documented. Arm ARM B2.10.1.1 forbids relying on it either way. A Non-shareable
mapping is not a reliable negative control on either board. A run that shows staleness
would show only that this implementation does not keep Non-shareable lines coherent; a run
that does not show it proves nothing.

---

## Q6. Secondary entry from PSCI CPU_ON; what Linux does; which EL Linux runs at

**Short answer.** PSCI guarantees entry with SCTLR_ELx.{I,C,M} = {0,0,0} at the caller's
EL and that "from the point of view of the caller the caches must be clean and coherent
with main memory". The architecture leaves cache state at reset IMPLEMENTATION DEFINED;
A76 and A78AE invalidate their caches automatically at reset deassertion (unless the core
was initialised to Debug recovery mode) and join coherency with no software steps. On Pi 5,
upstream TF-A's CPU_ON releases a holding pen rather than resetting the core, so that reset
was the cold boot (Q3, "EL3 state not written by BL31"). The PSCI clean-and-coherent
guarantee covers that case. The OS does not need to invalidate data caches before enabling
them. Linux's secondary path
(`secondary_entry` -> `init_kernel_el` -> `__cpu_setup` -> `__enable_mmu`) performs a local
TLB invalidate and no data-cache invalidate, no set/way operation, and no CPUECTLR or
ACTLR access, in both 6.18 and the Jetson 5.15 kernel. Linux enters at EL2 on both boards
and runs with VHE when the core supports it (both do). For the questions here the EL does
not matter: reads of CPUECTLR are permitted at EL1 and EL2, writes are blocked at both.

Citations:

- PSCI DEN0022 F.b §6.4.3.4 "MMU, cache and branch predictor enables": "execution must
  start with the stage 1 MMU and caches disabled ... SCTLR_ELx.{I, C, M} bits must be set to
  {0, 0, 0}. ... On start-up, from the point of view of the caller the caches must be clean
  and coherent with main memory." (`arm-den0022-psci-Fb.txt:4068-4077`). §5.4.8 puts boot-time
  invalidation on the PSCI implementation "unless this is automatically supported by the
  hardware".
- Arm ARM D7.5.6 Behavior of caches at reset: "All caches reset to IMPLEMENTATION DEFINED
  states that might be UNKNOWN." Cacheability controls reset to force Non-cacheable. An
  implementation can require an IMPLEMENTATION DEFINED initialization routine
  (`arm-ddi0487-Md.txt:475659-475683`).
- A76 TRM §7.2: "At reset the instruction and data caches are disabled and both caches are
  automatically invalidated." and "Caches in the core are invalidated automatically at reset
  deassertion unless the core power mode is initialized to Debug recovery mode."
  (`.txt:2725-2733`); also §8.2 (`.txt:3909`); §5.6.5 Debug recovery (`.txt:2028-2040`).
- A78AE TRM §2.5.2.3 note, §2.6.2 and §2.7.2 note: same (`.txt:2465`, `:2831`, `:4145`).
- A76 TRM §7.4 "Data cache invalidate on reset" note: the architecture has no whole-cache
  invalidate; software that needs one must iterate set/way (`.txt:2965-2968`). Not needed on
  these cores given the automatic invalidate.
- Linux 6.18: `~/slmos-ref/rpi/linux-rpi-6.18.y/arch/arm64/kernel/head.S:355-359`
  (`secondary_entry`), `:361-379` (`secondary_startup`: `__cpu_setup`, `__enable_mmu`),
  `:382-406` (`__secondary_switched`, `finalise_el2`); `arch/arm64/mm/proc.S:473-476`
  (`__cpu_setup` starts with `tlbi vmalle1; dsb nsh`).
- Linux 5.15 (Jetson L4T): `kernel_src.tbz2:kernel/kernel-jammy-src/arch/arm64/kernel/head.S:627-645`
  (`secondary_entry` -> `secondary_startup`: `switch_to_vhe`, `__cpu_setup`, `__enable_mmu`).
- No set/way and no CPUECTLR: `grep -rnE '\bdc\s+(c?isw|csw)\b'` and
  `grep -rni 's3_0_c15_c1_4\|cpuectlr'` over `arch/arm64` of the 6.18 tree return nothing;
  the same greps over the 5.15 `head.S` and `proc.S` return nothing.
- What Linux does do with MMU-off data, by VA only:
  - `update_early_cpu_boot_status`: a secondary with the MMU off does `str; dmb sy; dc ivac`
    "Invalidate potentially stale cache line" (6.18 `head.S:437-443`; 5.15 `head.S:686`).
  - Primary: "If the page tables have been populated with non-cacheable accesses (MMU
    disabled), invalidate those tables again to remove any speculatively loaded cache
    lines." followed by `dcache_inval_poc` (6.18 `head.S:96-105`).
  - Boot protocol: "The address range corresponding to the loaded kernel image must be
    cleaned to the PoC. ... this will typically require cache maintenance by VA rather than
    set/way operations." (`Documentation/arch/arm64/booting.rst:183-187`)
- Linux EL:
  - Pi 5: BL31 enters BL33 at EL2 (`~/slmos-ref/tf-a/plat/rpi/common/rpi3_common.c:214`);
    Linux 6.18 `init_kernel_el` takes the `init_el2` path (`head.S:270-331`) and
    `__finalise_el2` switches to VHE when ID_AA64MMFR1_EL1.VH allows
    (`arch/arm64/kernel/hyp-stub.S:76-92`).
  - Jetson: NVIDIA states "Linux kernel is in EL2" (`NV-ATF/plat/nvidia/tegra/soc/t234/plat_psci_handlers.c:330`);
    the 5.15 secondary path calls `switch_to_vhe` (`head.S:637`), and `hyp-stub.S:76-102`
    ("Engage the VHE magic!", HCR_HOST_VHE_FLAGS).
  - **Inference:** Linux runs at EL2 with VHE on both boards unless VHE is disabled on the
    command line; this was not measured here (C1 owns the Linux reference bar).
  - VHE host HCR_EL2 = RW|TGE|E2H|AMO|IMO|FMO, no TIDCP, no TSW
    (`~/slmos-ref/rpi/linux-rpi-6.18.y/arch/arm64/include/asm/kvm_arm.h:104-106`). KVM guests
    get TIDCP and TSW (`:100-103`): guest IMPDEF accesses and guest set/way trap to the host.
    Unknown trapped guest sysregs get an injected UNDEF (`arch/arm64/kvm/sys_regs.c:4961-4977`).

---

## Q7. Set/way after MMU enable on a live SMP system; data written with SCTLR.C=0 read later as cacheable

**Short answer.** Set/way operations are local to the executing PE, are not atomic with
respect to other PEs, and cannot prevent lines migrating between caches; Arm "strongly
discourages the use of set/way instructions to manage coherency in coherent systems" and
limits their expected use to cache power-up and power-down. Architecturally DC ISW is an
invalidate that can discard dirty data. On A76 and A78AE, DC ISW is implemented as clean
and invalidate, so on these cores it does not lose data, but it gives no cross-core
guarantee either. Data written with SCTLR.C=0 is a Non-cacheable write; reading it later
through a cacheable mapping is a mismatched-attribute access, and stale cached copies
(anywhere in the coherency domain) or a later write-back of a dirty line in the same
granule can hide or revert it. The documented remedy is maintenance by VA on the affected
range (invalidate before the cacheable read; clean before an MMU-off reader reads
cacheably-written data), as Linux does.

Citations:

- Arm ARM D7.5.9.12: "The DC set/way instructions apply only to the caches of the PE that
  performs the instruction." The note adds that system caches might not be maintainable by
  set/way (`arm-ddi0487-Md.txt:476569-476585`).
- Arm ARM D7.5.9.16.1: "unless all PEs attached to the caches regard all memory locations as
  Non-cacheable, it is not possible to prevent locations being allocated into the cache
  during such a sequence"; "it is possible for a cache line to migrate from an entry in the
  cache of one PE to the cache of a different PE in a way that means the line is not
  affected by set/way based cache maintenance. Therefore, Arm strongly discourages the use
  of set/way instructions to manage coherency in coherent systems. The expected use of the
  cache maintenance instructions that operate by set/way is limited to the cache maintenance
  associated with the powerdown and powerup of caches, if this is required by the
  implementation." (`.txt:476970-476997`)
- Arm ARM D7.5.8.2.2: "An invalidate instruction invalidates only at the level specified."
  (`.txt:476051-476058`)
- A76 TRM §7.2.4: "DC ISW instructions in AArch64, perform both a clean and invalidate of the
  target set/way. The values of HCR.SWIO and HCR_EL2.SWIO have no effect." Also "DC IVAC
  operations in AArch64 state are treated as DC CIVAC except for permission checking and
  watchpoint matching." (`.txt:2798-2808`)
- A78AE TRM §2.6.2.4: same, including "In Non-secure state, ... DC ISW operations in AArch64
  state, perform both a clean and invalidate" (`.txt:2900-2913`).
- Linux KVM rationale for trapping guest set/way: "S/W ops are local to a CPU (not
  broadcast); We have line migration behind our back (speculation); System caches don't
  support S/W at all" (`~/slmos-ref/rpi/linux-rpi-6.18.y/arch/arm64/kvm/mmu.c:2391-2409`).
- Arm ARM D7.5.5: when SCTLR_ELx.C is 0, "All data accesses to Normal memory using the EL2
  translation regime are Non-cacheable" (and the EL1&0 and EL2&0 equivalents); the closing
  note says that differing cacheability must be handled with the mismatched-attribute
  measures (`.txt:475532-475655`).
- Arm ARM B2.11 (Q5): the hazards and the clean/invalidate/DMB remedies.
- Hazard specific to these cores (**Inference** from A76 TRM §7.2.4): because DC IVAC is
  executed as DC CIVAC, an "invalidate" by VA of an NC-written range writes back any dirty
  cached copy of that line first, which would overwrite the NC-written bytes in DRAM. The
  Linux pattern is safe only because no cache holds those lines dirty.
- SLM-OS context: `kernel/arch/arm64/smp_boot.S:200-265` runs DC ISW over L1 and L2 on each
  Pi 5 and Jetson secondary after setting SCTLR M|C|I. Its comment says pre-MMU reads
  "entered L1/L2 as Non-Shareable lines". A76 TRM §7.2.3 and A78AE TRM §2.6.2.3 say loads
  with the data cache disabled do not access the L1, L2 or L3 arrays, so they cannot have
  allocated lines. On these cores the loop is a local clean and invalidate of a cache that
  was invalidated at reset and has been enabled for only a few instructions. **Inference:**
  it is harmless for data integrity on these cores, it is not portable, and it cannot be
  the mechanism that fixed a cross-CPU visibility problem.

---

## Hypothesis table

| Hypothesis | Pi 5 (BCM2712, 4x A76, one DSU) | Jetson (Tegra234, 6x A78AE, 2 clusters) |
|---|---|---|
| H1: TF-A does not set CPUECTLR_EL1.SMPEN, so private L2s are incoherent; the bit is writable only at EL3 | **INCONSISTENT.** There is no SMPEN: A76 TRM 100798 r4p1 §13.32 (bit 6 RES0). Private L2s are coherent in hardware with no software step: A76 TRM §5.9 "Core powerup", §7.2.5 (MESI), DSU TRM 100453 r4p1 §2.1 (SCU). Sub-claim "writable only at EL3": ACTLR_EL3.ECTLREN resets to 0 (A76 TRM §13.7) and upstream TF-A rpi5 code never sets it (Q3). That it stays 0 under the Raspberry Pi BL31 actually running is **inference** (BL31 version and source undetermined; consistent with the lab record of a hang on an EL2 write) | **INCONSISTENT.** No SMPEN: A78AE TRM 101779 r0p3 §3.2.47 (bit 6 is PF_MODE[0]). Coherent at power-up: A78AE TRM §2.4.9, §2.6.2.5; DSU TRM §2.1. Sub-claim "writable only at EL3": ECTLREN reset 0 (A78AE TRM §3.2.7), NVIDIA TF-A sets ECTLR access only for A57 (`NV-ATF/plat/nvidia/tegra/common/aarch64/tegra_helpers.S:66-103`). Writes by MB2 on CPU 0 (not reset across kexec) are unsourced (Q3, "EL3 state not written by BL31") |
| H2: no SMPEN bit on A76; the DSU keeps coherency automatically; writing CPUECTLR_EL1 from EL2 traps to EL3 | **CONSISTENT.** A76 TRM §13.32; DSU TRM §2.1, §5.9.1; A76 TRM §5.9. Trap to EL3: DSU TRM §11.8 states the trap for the same ECTLREN control (CLUSTERECTLR); for CPUECTLR the trap target is inference (A76 TRM §13.7 says only "not write-accessible"), consistent with QEMU `cpu64.c:544-559` and the lab record | **CONSISTENT** within each cluster (A78AE TRM §3.2.47, §2.4.9; DSU TRM §2.1, §11.8). Cross-cluster coherency: no primary hardware source; consistent with `HW_ASSISTED_COHERENCY := 1` (`platform_t234.mk:69`) and Linux running 6 CPUs in one IS domain (inference). Trapped write then panics in NVIDIA BL31 (`NV-ATF/bl31/aarch64/runtime_exceptions.S:581-597`, `bl31_traps.c:13-30`) |
| H3: pre-MMU reads (SCTLR.C=0) left stale lines in L1/L2 not coherent with later cacheable writes; set/way invalidate after MMU enable fixed it | **INCONSISTENT.** With the data cache disabled, loads and stores do not access L1/L2/L3 arrays: A76 TRM §7.2.3. Caches are invalidated at reset: A76 TRM §7.2. DC ISW is local and discouraged for coherency: Arm ARM DDI 0487 M.d D7.5.9.16.1. The documented mechanism does not exist; what the observed "fix" was is not determinable from documents | **INCONSISTENT.** A78AE TRM §2.6.2.3 (same text), §2.6.2 reset invalidate; Arm ARM D7.5.9.16.1 |
| H4 (`ncmem.h` comment): L2s incoherent "despite SMPEN being set" | **INCONSISTENT.** No SMPEN field exists (A76 TRM §13.32); the SLM-OS check reads RES0 bit 6 (`kernel/include/cache.h:41-42`) | **INCONSISTENT.** A78AE TRM §3.2.47 (bit 6 = PF_MODE[0], reset 0) |

---

## Implications for experiments

### (a) Is reading CPUECTLR_EL1 safe?

- **NS-EL2, Pi 5: safe for reads.** A76 TRM §13.32 lists RW at EL2 for NS=1 (both TGE
  values); ACTLR_EL3.ECTLREN gates writes only (§13.7). SLM-OS runs at EL2 with E2H=1,
  TGE=1 (`kernel/arch/arm64/boot.S:402-455`, `:664-665`). Observation: SLM-OS already
  executes this MRS from EL2 on CPU 0 and every secondary at every boot
  (`kernel/include/cache.h:41`, `kernel/sched/smp.c:403`, `:746`, called from
  `kernel/src/main.c:479`) and pi-5-1 boots to the shell.
- **NS-EL2, Jetson: safe for reads.** A78AE TRM §3.2.47 table; ECTLREN gates writes only
  (§3.2.7). Same SLM-OS read path, compiled for Jetson (`cache.h:36-43`), and jetson-nano-2
  boots to the shell. Both kexec (EL2+VHE) and UEFI-direct (EL2) entry are NS-EL2.
- **NS-EL1 under Linux, both boards: safe for reads.** Linux normally runs its kernel at
  EL2 with VHE on both boards (Q6), so a kernel module read is an EL2 read. If Linux is
  booted nVHE, the kernel is at NS-EL1 with TGE=0, where the TRM tables list RW, and the
  nVHE host HCR_EL2 has no TIDCP (`kvm_arm.h:104`). Inside a KVM guest, HCR_EL2.TIDCP=1
  traps the read to the KVM host, which injects UNDEF into the guest (`sys_regs.c:4961-4977`);
  that is a guest exception, not a board hang. EL0 must not attempt it (UNDEFINED or IMPDEF,
  Arm ARM D24.2.61).
- **Writes are not safe on either board, from NS-EL2 or NS-EL1.** ACTLR_EL3.ECTLREN = 0 on
  both as far as BL31 is concerned (Q3). Other EL3 code is unsourced, and on Pi 5 the running
  BL31 is not the upstream source. That uncertainty is a further reason not to write: a
  write is safe only if ECTLREN is known to be 1. The denied write traps to EL3 (DSU TRM §11.8 for the shared control; inference
  for CPUECTLR). On Jetson, NVIDIA BL31 v2.8 panics at EL3: the board stops until power
  cycle. On Pi 5 the outcome depends on the undetermined EEPROM BL31 version (panic before
  TF-A v2.11, UNDEF injected into EL2 from v2.11); the lab record says it hung. The same
  applies to CLUSTERECTLR (ECTLREN), CPUACTLR*/CLUSTERACTLR (ACTLREN), CPUPWRCTLR and
  CLUSTERPWR* (PWREN): all reset 0 in ACTLR_EL3 and none set by either BL31, except
  CLUSTERPM* on unfused Jetsons.
- **Inference:** reading CLUSTERECTLR_EL1 (S3_0_C15_C3_4) from NS-EL2 is also permitted on
  both boards (DSU TRM §11.8 accessibility table, RW at EL2, writes gated). On Jetson it
  should read back bits 0 and 14 set (`plat_setup.c:262-263`), which would be a cheap
  check that the expected BL31 configuration is live.

### (b) What a correct secondary bring-up sequence must contain

From PSCI DEN0022 F.b §6.4.3.4, A76 TRM §5.9 and §7.2, A78AE TRM §2.4.9 and §2.6.2,
Arm ARM B2.11, D7.5.9.16.1, and the Linux 6.18 / 5.15 secondary path:

1. Primary, before CPU_ON: clean to the PoC by VA (DC CVAC, DSB) every location the
   secondary will read while its MMU or data cache is off: the entry trampoline's data
   (TTBR, MAIR, TCR, stack pointer, per-CPU arguments) and the page tables if the secondary
   walks them before the primary's last cacheable write is visible. Reason: MMU-off reads
   are Non-cacheable and do not look up any cache (A76 TRM §7.2.3), so a dirty line in the
   primary's cache is invisible to them (booting.rst:183-187 states the same for the image).
2. PSCI CPU_ON. The secondary enters at NS-EL2 with SCTLR.{I,C,M}=0 and caches that are
   clean and coherent from the caller's view (PSCI §6.4.3.4), and already in the coherency
   domain (A76 §5.9 "no software steps are required"). The caches were invalidated by
   hardware at the core's last reset (A76 §7.2, A78AE §2.6.2). On Jetson that is the CPU_ON
   itself (inference, Q3). On Pi 5 it is cold boot, because upstream TF-A releases
   secondaries from a holding pen without a reset (Q3); the PSCI guarantee is what applies
   there.
3. Do not invalidate or clean by set/way, and do not touch CPUECTLR, CLUSTERECTLR, CPUACTLR*
   or ACTLR_EL2 for coherency: none is needed (Q1, Q4), set/way is discouraged on live SMP
   (Arm ARM D7.5.9.16.1), and writes trap (Q2). Linux does none of these.
4. If the secondary stores to shared memory while its MMU or cache is off (for example a
   "booted" flag), follow the store with DMB SY and DC IVAC on that line, as Linux's
   `update_early_cpu_boot_status` does (6.18 `head.S:437-443`), and make sure no cache holds
   that line dirty (on these cores DC IVAC behaves as DC CIVAC, A76 TRM §7.2.4).
5. Program MAIR (Normal Inner/Outer WB RA/WA), TCR with cacheable Inner Shareable walks
   (IRGN/ORGN WB, SH0/SH1 = Inner Shareable), TTBRs, then TLBI (local, as `__cpu_setup`
   does with `tlbi vmalle1; dsb nsh`), DSB, ISB, then set SCTLR.M|C|I and ISB.
6. Map every piece of shared memory with the same attributes on every CPU (Inner
   Shareable, Normal WB). Any NC alias of WB memory, including an NC window used for the
   scheduler, is a mismatched alias and needs the B2.11 maintenance rules on every access
   that crosses the alias.

### (c) Which negative control is guaranteed to show staleness on this hardware

**No negative control is guaranteed by any source.** The strongest one the TRMs support is
a reader with its data cache off. The value that reader returns is inference.

- **Not reliable: a Non-shareable WB mapping.** Arm ARM B2.10.1.1 forbids relying on it,
  and the SCU's handling of Non-shareable lines inside a cluster is undocumented (Q5). A
  result showing no staleness proves nothing.
- **Not reliable: a cacheable-writer / NC-reader alias with the MMU on.** B2.11 says
  coherency "might" be lost, and the TRMs do not state whether an NC access with
  SCTLR.C=1 looks up the caches.
- **What the TRMs document for a reader with the data cache off** (SCTLR_EL2.C=0, or a
  secondary that has not yet enabled its MMU). A76 TRM §7.2.3 / A78AE TRM §2.6.2.3 say its
  loads "do not access any of the L1 data, L2 cache, and ... DSU L3 cache arrays" and "are
  incoherent with the caches in both this core and other cores in the cluster". That
  documents incoherence. It does not document which value the load returns.
- **Inference: which value the reader sees** depends on where the newest copy of the line
  is when the NC read is served, and on what serves it:
  1. Dirty in the writer's L1/L2, or in the DSU L3 of the writer's cluster. The
     caches-off load does not look in any of these arrays, so the newest value is not
     returned from them. The read goes out of the cluster (ReadNoSnoop for NC loads, DSU
     TRM §7.6) and returns whatever the next level holds.
  2. Evicted out of the cluster into an outer cache. Jetson has a fabric-level cache: the
     MCE "SCF level Cache Clean" and "Cache invalidate and Clean" commands
     (`NV-ATF/plat/nvidia/tegra/soc/t234/drivers/mce/ari.c:383-405`) are issued at cold boot
     and before the NS hand-off (`plat_setup.c:232`, `:546-549`). A write-back from the
     cluster could land there. Whether a ReadNoSnoop from a cluster is served from that
     cache is undocumented. If it is, the reader sees the new value with no coherency
     involved. For BCM2712, no cached source says whether any cache exists downstream of
     the DSU; the same caveat applies. An eviction therefore can make the reader return the
     new value. A write-back to DRAM is not the only way the control can fail.
  3. Jetson reader cluster. A reader in the same cluster as the writer (CPUs 0-3 together,
     or 4-5 together) fits case 1 directly: its own DSU L3 and the writer's L1/L2 are both
     arrays it does not access. A reader in the other cluster sends its NC read across the
     fabric. Whether the fabric snoops the writer's cluster for an NC read, or serves it
     from the SCF-level cache, is undocumented. Same-cluster and cross-cluster runs are two
     different experiments and must be reported separately.
- **Recipe.** Use one 64-byte line L, at a PA used for nothing else, mapped WB Inner
  Shareable for the writer.
  1. Writer, caches on: store P0 to L, DC CIVAC L, DSB SY. P0 is now at the PoC and no
     cache holds L.
  2. Writer: store P1 to L, DSB SY, no cache maintenance. The writer then waits (WFE) and
     touches no other memory, to keep eviction pressure low.
  3. Reader, data cache off: read back SCTLR_EL2 to confirm C=0 (an MMU-off secondary
     before enable is the natural case). Load L, giving V1. Record the time since step 2.
  4. Writer: DC CVAC L, DSB SY, then signal. Reader loads L again, giving V2.
  5. Repeat with: (i) a reader in the writer's cluster and, on Jetson, in the other
     cluster; (ii) a longer delay before step 3; (iii) the writer streaming through more
     than L2 plus L3 (Pi 5: 512 KB L2 per core and 2 MB L3,
     `~/slmos-ref/rpi/rpi-linux-bcm2712.dtsi:818-869`; Jetson: 256 KB L2 per core and 2 MB L3
     per cluster, `~/slmos-ref/linux/linux/arch/arm64/boot/dts/nvidia/tegra234.dtsi:5625-5760`)
     between steps 2 and 3, to force the line out of the cluster.
- **Interpreting results** (inference):
  - V1 = P0 and V2 = P1: staleness of a dirty line with respect to a caches-off reader is
    shown, and step 4 shows that cleaning to the PoC makes P1 visible. This is the expected
    result for case 1.
  - V1 = P1 (V2 = P1): **inconclusive.** Possible causes are a write-back before the read,
    an outer cache serving the read (Jetson SCF level, or an undocumented BCM2712 cache),
    fabric snooping (cross-cluster case), the reader's cache not actually off, or the two
    accesses not reaching the same PA. This result does not show coherency or the
    absence of a defect.
  - V2 = P0: the setup is invalid (wrong PA or alias, or the clean did not complete). The
    run must be discarded.
  - Variant (iii) is expected to move results from the first case towards the inconclusive
    one. It tests the eviction caveat, not coherency.
- The same mechanism in reverse (writer with caches off, reader with a cached copy) depends
  on the reader holding a cached copy and on the SCU not snooping NC writes, which no source
  states; it is a weaker control.

---

## Could not be sourced

- The BL31 version embedded in the Pi 5 EEPROM firmware on pi-5-1 (determines panic versus
  UNDEF injection after a trapped write). The EEPROM image stores it inside the compressed
  `bootmain` section.
- The exact exception taken by a denied CPUECTLR_EL1 write: the A76 and A78AE TRMs say only
  "not write-accessible"; the trap-to-EL3 statement is in the DSU TRM for CLUSTERECTLR,
  which is gated by the same ECTLREN bit.
- Tegra234 cross-cluster coherency hardware (CCPLEX/SCF): the Orin TRM was not reachable
  (login; the local copy named in CLAUDE.md is on an unmounted Windows drive). Only
  firmware configuration and Linux behavior support "coherent across clusters".
- Whether an access to a page mapped Normal Non-cacheable with SCTLR.C=1 looks up the
  caches on A76/A78AE: the TRMs describe only the caches-disabled case.
- How the DSU SCU handles Non-shareable WB lines inside the cluster: only the external
  transaction type is documented.
- A76 and A78AE revision numbers on the actual boards (A76 r4p1 is from the TF-A rpi5
  makefile comment; neither MIDR was read).
- EL3 writes to ACTLR_EL3, ACTLR_EL2 or CPUECTLR_EL1 by code other than the cached BL31
  sources: MB2 on Jetson CPU 0 (closed source; its exception level is not stated in the
  NVIDIA boot-flow page), any Raspberry Pi firmware code on the A76 cores before or instead
  of upstream BL31. Jetson CPU 0 keeps these values across kexec and UEFI-direct boot.
- Whether the Tegra234 fabric implements SYSCOREQ/SYSCOACK, or whether the MCE or another
  controller performs the DSU §5.9 "SoC is responsible" step.
- Whether an NC read leaving a cluster is served from the Tegra SCF-level cache or snooped
  in the other cluster, and whether BCM2712 has any cache downstream of the DSU L3. These
  decide whether the caches-off negative control can return the new value after an eviction.
