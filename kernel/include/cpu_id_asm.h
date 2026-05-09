/*
 * cpu_id_asm.h - shared MPIDR -> logical CPU id lookup for asm sites
 *
 * Replaces the legacy fold (mpidr & 0xFF) | ((mpidr >> 8) & 0xFF), which
 * collides on Jetson Orin Nano's dual-cluster A78AE encoding (CPU 4
 * MPIDR 0x10200 -> 2, colliding with CPU 2; CPU 5 MPIDR 0x10300 -> 3,
 * colliding with CPU 3).
 *
 * The new lookup scans the cpu_logical_map[] table that smp_init
 * populates with each platform's actual MPIDR encoding, so it works
 * for any affinity layout. C-side fold sites use cpu_logical_id() in
 * <smp.h>; this header is the asm equivalent for sites that can't take
 * a function call (exception entry, the resched trampoline before its
 * register save is set up enough for AAPCS64).
 *
 * Single source of truth: cpu_logical_map[MAX_CPUS], populated by
 * smp_init() in kernel/sched/smp.c before any of these sites can fire.
 *
 * Closes #647.
 */

#ifndef CPU_ID_ASM_H
#define CPU_ID_ASM_H

/* MAX_CPUS is defined in <config.h> as a plain integer literal, but
 * config.h also contains C-only `_Static_assert` blocks that the GNU
 * assembler chokes on when run through cpp. Mirror the value here as
 * an asm-friendly literal and assert equality from C land
 * (kernel/sched/preempt.c) so a future drift surfaces at compile
 * time. */
#define ARM64_MAX_CPUS_LITERAL  8

#ifdef __ASSEMBLER__

/*
 * ARM64_GET_LOGICAL_CPU dst, s1, s2, s3
 *
 * Reads MPIDR_EL1, masks to AFF0|AFF1|AFF2|AFF3 = 0xFF00FFFFFF, scans
 * cpu_logical_map[] for a matching entry. On hit, dst holds the logical
 * CPU id (0..MAX_CPUS-1). On miss, dst = 0 (defensive — array indexing
 * stays in-bounds; preempt_check_cpu_mpidr surfaces real misconfig at
 * boot).
 *
 * Clobbers: dst, s1, s2, s3 (caller saves what it needs).
 *
 * Cost: 1 mrs + ~10 cycles of mask/setup + (MAX_CPUS x ~3 cycles)
 * worst-case loop. ~30 cycles for 6-CPU Jetson vs. 4 for the legacy
 * fold; acceptable at exception entry and the trampoline arm-up
 * (neither is a sub-microsecond hot path).
 */
.macro ARM64_GET_LOGICAL_CPU dst:req, s1:req, s2:req, s3:req
    mrs     \s1, mpidr_el1

    /* MPIDR_AFF_MASK = 0xFF00FFFFFF (Aff0|Aff1|Aff2|Aff3).
     * Aff3 = bits[39:32], Aff2 = bits[23:16], Aff1 = bits[15:8],
     * Aff0 = bits[7:0]. Bits 31:24 are reserved and zeroed by the
     * mask. */
    movz    \s2, #0xFFFF
    movk    \s2, #0xFF, lsl #16
    movk    \s2, #0xFF, lsl #32
    and     \s1, \s1, \s2

    /* Linear scan cpu_logical_map[0..MAX_CPUS-1]. Cacheable read is
     * fine — smp_init populated this before scheduler_init / first
     * timer IRQ, and the calling sites all run post-smp_init. */
    adrp    \s2, cpu_logical_map
    add     \s2, \s2, #:lo12:cpu_logical_map
    mov     \dst, #0
.Lslmos_cpu_id_loop\@:
    cmp     \dst, #ARM64_MAX_CPUS_LITERAL
    b.ge    .Lslmos_cpu_id_notfound\@
    ldr     \s3, [\s2, \dst, lsl #3]
    cmp     \s3, \s1
    b.eq    .Lslmos_cpu_id_done\@
    add     \dst, \dst, #1
    b       .Lslmos_cpu_id_loop\@
.Lslmos_cpu_id_notfound\@:
    mov     \dst, #0
.Lslmos_cpu_id_done\@:
.endm

#endif /* __ASSEMBLER__ */

#endif /* CPU_ID_ASM_H */
