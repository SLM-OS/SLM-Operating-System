# Jetson Orin Nano — Timer Preemption Investigation

**Date:** 2026-04-15
**Issue:** #134 (Hardware timer restoration)
**Platform:** Jetson Orin Nano (Tegra234, 6× Cortex-A78AE)
**Status:** All paths blocked. COOP_PREEMPT confirmed as the only viable mechanism.

---

## TL;DR

Jetson Orin Nano cannot deliver timer interrupts to NS EL2 through any
path accessible from kernel-space code. The GICv3 is fully locked down by
TF-A firmware: all interrupt groups (PPIs, SGIs, SPIs) are Group 0 or
Group 1 Secure, ICC_IGRPEN0 access is trapped to EL3, and SCR_EL3.FIQ=1
routes all FIQ to EL3. The same fundamental limitation as Pi 5 (documented
in `docs/pi5-preemption-resolution.md`), expressed through GICv3 mechanisms.

COOP_PREEMPT (cooperative preemption via CNTPCT_EL0 polling at schedule()
entry) remains the correct and only viable mechanism on both platforms.

---

## Hardware Configuration (from `timdiag` shell command)

```
Platform: Jetson Orin Nano (EL2 + VHE after kexec)
SCR_EL3:          0x3073d
  - NS=1    (Non-secure)
  - IRQ=0   (Physical IRQ stays at current EL — good)
  - FIQ=1   (Physical FIQ trapped to EL3 — blocked)
  - EA=1, HCE=1, RW=1

GICR_IGROUPR0:    0x00000000  (ALL PPIs/SGIs in Group 0)
GICR_IGRPMODR0:   0x00000000  (RAZ from NS — expected)
GICR_ISENABLER0:  0x40000000  (PPI 30 enabled, PPI 26/27 disabled)
GICR_ISPENDR0:    0x04000000  (PPI 26 pending — visible from NS anomalously)

GICD_IGROUPR[1]:  0x00000000  (ALL SPIs 32-63 in Group 0)
GICD_IGROUPR[2]:  0x00000000  (ALL SPIs 64-95 in Group 0)
GICD_IGROUPR[3]:  0x00000000  (ALL SPIs 96-127 in Group 0)
GICD_IGROUPR[4]:  0x00000000  (ALL SPIs 128-159 in Group 0)

ICC_SRE_EL1:      0x7    (System register access enabled)
ICC_PMR_EL1:      0xF0   (Priority mask)
ICC_IGRPEN0_EL1:  TRAPPED (read traps to EL3, EC=0x18)
ICC_IGRPEN1_EL1:  1      (Group 1 enabled — but nothing is Group 1 NS)

GICD_CTLR:        0x12   (ARE_NS=1, EN_G1=1, EN_G0=0)

CNTP_CTL_EL0:     0x5    (EN=1, IMASK=0, ISTATUS=1 — timer asserted)
CNTHP_CTL_EL2:    0x5    (EN=1, IMASK=0, ISTATUS=1 — also asserted)
CNTFRQ_EL0:       31250000 Hz (31.25 MHz)
```

---

## Paths Investigated

### 1. PPI 30 (Physical Timer) as Group 1 NS IRQ

**Hypothesis:** Write GICR_IGROUPR0 bit 30 = 1 to promote timer PPI to
Group 1 NS, making it generate IRQ (which reaches EL2 since SCR_EL3.IRQ=0).

**Result:** DEAD. NS writes to GICR_IGROUPR0 are silently ignored.
Readback stays 0x00000000. EL3 firmware owns the group configuration.

### 2. PPI 30 as FIQ (via ICC_IGRPEN0 + DAIF.F unmask)

**Hypothesis:** Enable Group 0 delivery (ICC_IGRPEN0_EL1=1), unmask FIQ
in DAIF, catch PPI 30 as FIQ in el1_fiq vector.

**Result:** DEAD. Two separate blockers:
- Writing ICC_IGRPEN0_EL1 from NS traps to EL3 (ESR_EL3 EC=0x18).
  Even **reading** ICC_IGRPEN0_EL1 traps. TF-A has configured the
  ICC system register trap mask to block all Group 0 register access from NS.
- Even if we could enable Group 0 delivery, SCR_EL3.FIQ=1 routes
  FIQ to EL3 before it reaches our EL2 FIQ vector.

**Evidence:** First timdiag run triggered "Unhandled Exception from EL2"
crash dump from TF-A. ESR_EL3 = 0x623c3319 decodes to:
EC=0x18 (trapped MRS/MSR), register = S3_0_C12_C12_6 (ICC_IGRPEN0_EL1).

### 3. PPI 26 (Hypervisor Physical Timer, CNTHP)

**Hypothesis:** The EL2 hypervisor timer uses a different PPI (26) that
might be in a different GIC group.

**Result:** DEAD. GICR_IGROUPR0 = 0x0 means ALL PPIs (including 26) are
Group 0. Same routing to EL3 via FIQ.

### 4. SPI via Peripheral Timer (Tegra TMR)

**Hypothesis:** Use a Tegra-specific timer peripheral that generates an
SPI (not PPI). SPIs might be Group 1 NS.

**Result:** DEAD. GICD_IGROUPR for all SPI banks reads 0x00000000 from NS.
ALL SPIs are also in Group 0 / Group 1 Secure. TF-A has locked down the
entire interrupt namespace, not just PPIs.

### 5. SGI Self-IPI

**Hypothesis:** Send a software-generated interrupt to self via
ICC_SGI1R_EL1 to trigger an IRQ.

**Result:** DEAD. SGIs (bits 0-15 of GICR_IGROUPR0) are also Group 0.
ICC_SGI1R generates Group 1 SGIs, but there are no Group 1 NS SGIs.
ICC_SGI0R would generate Group 0 SGIs → FIQ → trapped to EL3.

### 6. WFI Wake-up on Timer

**Hypothesis:** Even without GIC-delivered interrupts, WFI might wake when
the timer asserts its output (ARM spec says WFI wakes on pending interrupts
regardless of DAIF mask).

**Result:** DEAD. WFI hangs indefinitely with both DAIF.I and DAIF.F masked.
The GIC never asserts IRQ or FIQ to the CPU because:
- ICC_IGRPEN0 is disabled (and inaccessible from NS)
- No Group 1 NS interrupts exist
- SCR_EL3.FIQ=1 means FIQ assertion goes to EL3, not our WFI context

### 7. Custom TF-A Build

**Status:** Not attempted. NVIDIA provides TF-A source as part of the
Jetson Linux BSP. Rebuilding with modified GIC configuration is technically
possible but out of capstone scope.

---

## Why This Is Locked Down

NVIDIA's Jetson platform uses a TrustZone-enabled security architecture:
- TF-A (BL31) at EL3 manages the GIC for Secure world services
- OP-TEE runs as a Trusted OS in Secure EL1
- All interrupts are configured as Group 0 (Secure) by default
- The Non-secure world (Linux, or SLM-OS after kexec) only receives
  interrupts that EL3 explicitly forwards

On a stock Linux system, interrupt delivery works because Linux's GIC
driver negotiates with TF-A during boot. After kexec, SLM-OS inherits
whatever GIC state Linux had, but the Group registers are reset by
TF-A's warm-boot path.

---

## Relation to Pi 5

Pi 5 has the exact same fundamental limitation but through GICv2:
- GICD_IGROUPR writes from non-secure are silently ignored
- Timer PPI stays in Group 0 → FIQ → EL3
- armstub8 approach breaks PSCI handoff

Both platforms resolve to the same outcome: COOP_PREEMPT is the only
viable preemption mechanism without firmware modification.

---

## What Would Fix It

1. **Custom TF-A with Group 1 NS timer PPI.** Modify TF-A's GIC
   initialization to place PPI 30 (and optionally PPI 26) in Group 1 NS.
   With SCR_EL3.IRQ=0, the timer IRQ would reach EL2 directly.

2. **TF-A interrupt forwarding.** Configure TF-A's FIQ handler to
   forward the timer interrupt to the NS world via a virtual interrupt
   injection (ICC_HPPIR or virtual interrupt mechanism).

3. **NVIDIA BSP update.** If NVIDIA's TF-A were updated to support
   a "pass-through" mode for the timer PPI, all downstream OSes would
   benefit.

*Last updated: 2026-04-15*
