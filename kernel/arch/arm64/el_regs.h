/*
 * el_regs.h — exception-state register name macros for the EL the
 * kernel actually runs at.
 *
 * Background: ARM VHE (Virtualization Host Extensions, HCR_EL2.E2H=1)
 * silently redirects most _EL1 register names to _EL2 equivalents
 * (SCTLR, TTBR0/1, TCR, MAIR, ESR, FAR, VBAR, CPACR→CPTR, CONTEXTIDR,
 * CNTKCTL, AFSR0/1). ELR and SPSR are NOT in that set: at EL2h the
 * live exception state lives in ELR_EL2 / SPSR_EL2, and EL1-named
 * accesses target the dormant EL1 registers (silently — no fault).
 *
 * The kernel runs at:
 *   - EL2h with VHE on PLATFORM_RASPI5 (issue #683 — Pi 5 NS-EL1 IRQ
 *     delivery is broken on BCM2712 / GIC-400 firmware regardless of
 *     PPI; running at EL2 routes IRQs through VBAR_EL2 the way Linux
 *     and Pi firmware actually validate).
 *   - EL2h with VHE on PLATFORM_JETSON_ORIN_NANO (CBB firewall path
 *     forces EL2 entry; smp_boot.S confirms secondaries also stay at
 *     EL2). Cooperative-only preemption today, so vector-entry paths
 *     that read ELR/SPSR aren't currently exercised on Jetson
 *     hardware — but the macro selects the right operand for the day
 *     hardware IRQs come back, and matches what KVM-host kernels do.
 *   - EL1h on every other ARM64 platform (QEMU virt direct boot).
 *
 * Sites that explicitly mrs/msr ELR or SPSR (vector save/restore,
 * resched_trampoline, panic register dump, user_entry) must use
 * KERN_ELR / KERN_SPSR (assembly) or KERN_ELR_NAME / KERN_SPSR_NAME
 * (C inline asm) so the same source compiles correctly for both
 * runtime ELs.
 */
#ifndef KERNEL_ARCH_ARM64_EL_REGS_H
#define KERNEL_ARCH_ARM64_EL_REGS_H

#if defined(PLATFORM_RASPI5) || defined(PLATFORM_JETSON_ORIN_NANO)
#  define KERN_ELR        elr_el2
#  define KERN_SPSR       spsr_el2
#  define KERN_ELR_NAME   "elr_el2"
#  define KERN_SPSR_NAME  "spsr_el2"
#else
#  define KERN_ELR        elr_el1
#  define KERN_SPSR       spsr_el1
#  define KERN_ELR_NAME   "elr_el1"
#  define KERN_SPSR_NAME  "spsr_el1"
#endif

#endif /* KERNEL_ARCH_ARM64_EL_REGS_H */
