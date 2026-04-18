/*
 * Jetson EL1 smoke test — determines whether the CBB firewall gates
 * PCIe + XHCI MMIO by Exception Level (EL2 blocked, EL1 allowed) or
 * by kernel signature (EL1 also blocked for unsigned code).
 *
 * Invoked at the very end of boot.S primary_cpu (after VHE setup,
 * before the branch to kernel_main). Never returns. Prints results
 * over UARTC and halts.
 *
 * Gated on -DJETSON_EL1_SMOKE (CMake option JETSON_EL1_SMOKE).
 *
 * Expected serial output on success:
 *     === EL SMOKE TEST ===
 *     EL2:APPL=0xffffffff
 *     EL2:XHCI=0xffffffff
 *     EL1:REACHED
 *     EL1:APPL=0xXXXXXXXX
 *     EL1:XHCI=0xXXXXXXXX
 *     EL1:DONE
 *
 *     * If EL1:APPL and EL1:XHCI show real values (e.g. 0x009490e0,
 *       0x01200020) then CBB firewall gates by EL → Path 1 is viable.
 *     * If EL1:APPL and EL1:XHCI are also 0xffffffff, CBB gates by
 *       kernel signature or secure state → Path 1 is dead.
 *     * If we never see EL1:REACHED, UARTC itself is blocked at EL1
 *       (or the eret crashed).
 */

#ifdef JETSON_EL1_SMOKE

#define UARTC_THR    0x0C280000    /* NS16550 THR, 4-byte spaced */
#define UARTC_LSR    0x0C280014    /* NS16550 LSR (5 << 2) */
#define LSR_THRE     (1 << 5)      /* Transmit Holding Register Empty */

#define APPL_CTRL    0x140A0004    /* Tegra PCIe C8 APPL_CTRL */
#define XHCI_HCD     0x03610000    /* Tegra XHCI CAPLENGTH|HCIVER */

/* Delay constants for the FIFO-drain busy-loops below. */

/* Per-character delay between raw writes. ~20k cycles ≈ 14 µs at
 * 1.5 GHz — enough spacing that UARTC's FIFO never backs up while
 * emitting the "ABCD" breadcrumb without LSR polling. */
#define CHAR_DELAY           20000

/* FIFO fully-drain delay. ~1.04 M cycles ≈ 0.7 ms — comfortably
 * larger than one full 16-byte FIFO drain at 115200 baud (≈1.4 ms
 * per char total, but only queued chars need to drain). */
#define FIFO_DRAIN_DELAY     0x000FF424

/* Per-hex-digit delay used while emitting the 8-digit APPL hex
 * readout. Shorter than CHAR_DELAY because the FIFO was already
 * fully drained by FIFO_DRAIN_DELAY just before this loop — we
 * only need enough spacing that the FIFO doesn't backfill
 * mid-digit. ~5k cycles ≈ 3.3 µs at 1.5 GHz. */
#define HEX_CHAR_DELAY       5000

.section ".text"

/* =========================================================================
 * jetson_el1_smoke — entry point, called from boot.S before kernel_main
 * =========================================================================
 *   State on entry: EL2 + VHE (E2H=TGE=1), MMU off, DAIF masked, stack set
 *   State on exit:  never returns (halt loop)
 */
.global jetson_el1_smoke
.type jetson_el1_smoke, @function
jetson_el1_smoke:
    /* x20 = preserve LR (we never return, but we call puts_raw etc.) */
    mov     x20, x30

    /* Print banner */
    adr     x0, banner
    bl      puts_raw

    /* EL2:APPL= */
    adr     x0, str_el2_appl
    bl      puts_raw
    ldr     x1, =APPL_CTRL
    ldr     w0, [x1]
    bl      put_hex32_nl

    /* EL2:XHCI= */
    adr     x0, str_el2_xhci
    bl      puts_raw
    ldr     x1, =XHCI_HCD
    ldr     w0, [x1]
    bl      put_hex32_nl

    /* ---- Prepare to drop to EL1 ---- */

    /* Install EL1 vector table (under VHE this writes VBAR_EL2; we
     * also write it again after clearing E2H for the real EL1 reg). */
    adr     x0, el1_vectors
    msr     vbar_el1, x0
    isb

    /* Clear HCR_EL2.E2H (bit 34) and TGE (bit 27). This takes the
     * core out of VHE. All subsequent EL1 sysreg accesses now hit
     * the real EL1 regs instead of being redirected to EL2. */
    mrs     x0, hcr_el2
    mov     x1, #1
    lsl     x1, x1, #34
    bic     x0, x0, x1
    mov     x1, #1
    lsl     x1, x1, #27
    bic     x0, x0, x1
    /* Keep RW (bit 31) — EL1 must be AArch64 */
    mov     x1, #1
    lsl     x1, x1, #31
    orr     x0, x0, x1
    msr     hcr_el2, x0
    isb

    /* Now at EL2 non-VHE. Re-install EL1 state in the real EL1
     * regs (the previous writes under VHE hit EL2 regs). */
    adr     x0, el1_vectors
    msr     vbar_el1, x0
    /* SCTLR_EL1 = minimal (MMU off, caches off — reset value) */
    ldr     x0, =0x30C50838         /* RES1 bits per ARM ARM */
    msr     sctlr_el1, x0
    msr     ttbr0_el1, xzr
    msr     ttbr1_el1, xzr
    msr     tcr_el1, xzr
    msr     mair_el1, xzr
    msr     cpacr_el1, xzr
    isb

    /* SPSR_EL2 to target EL1h (M=0b00101), DAIF all masked (bits [9:6]=1) */
    mov     x0, #0x3C5
    msr     spsr_el2, x0
    adr     x0, el1_entry
    msr     elr_el2, x0

    /* Breadcrumb: last EL2 print before eret */
    adr     x0, str_pre_eret
    bl      puts_raw

    eret                            /* → EL1 */


/* =========================================================================
 * el1_entry — execution continues here at EL1
 * =========================================================================
 *   State: EL1h, MMU off, DAIF masked
 */
el1_entry:
    /*
     * Write 8 distinct chars with busy-loop spacing to avoid FIFO
     * overrun. Kept inline instead of using `puts_raw` because the
     * investigation needed to distinguish "EL1 can write UARTC" from
     * "EL1 can read UARTC_LSR" — the helper does both, which would
     * have confounded the signal if LSR had turned out to be
     * unreadable at EL1. EL1 reads ended up working too, so this
     * block reads redundantly now but stays as the historical proof.
     */
    ldr     x4, =UARTC_THR
    ldr     w5, =CHAR_DELAY

    mov     w2, #'A'
    str     w2, [x4]
    mov     w3, w5
1:  subs    w3, w3, #1
    b.ne    1b

    mov     w2, #'B'
    str     w2, [x4]
    mov     w3, w5
1:  subs    w3, w3, #1
    b.ne    1b

    mov     w2, #'C'
    str     w2, [x4]
    mov     w3, w5
1:  subs    w3, w3, #1
    b.ne    1b

    mov     w2, #'D'
    str     w2, [x4]
    mov     w3, w5
1:  subs    w3, w3, #1
    b.ne    1b

    mov     w2, #'\r'
    str     w2, [x4]
    mov     w3, w5
1:  subs    w3, w3, #1
    b.ne    1b

    mov     w2, #'\n'
    str     w2, [x4]
    mov     w3, w5
1:  subs    w3, w3, #1
    b.ne    1b

    /* If we see "ABCD" above, EL1 writes work — now try LSR read. */
    mov     w2, #'L'
    str     w2, [x4]
    ldr     x4, =UARTC_LSR
    ldr     w6, [x4]                /* does this abort? */
    ldr     x4, =UARTC_THR
    mov     w2, #'S'
    str     w2, [x4]
    mov     w2, #'R'
    str     w2, [x4]
    mov     w2, #'='
    str     w2, [x4]
    /* Print low nibble of w6 */
    and     w7, w6, #0xF
    cmp     w7, #10
    b.lt    .Ll1
    add     w7, w7, #('a' - 10)
    b       .Ll2
.Ll1:
    add     w7, w7, #'0'
.Ll2:
    str     w7, [x4]
    mov     w2, #'\r'
    str     w2, [x4]
    mov     w2, #'\n'
    str     w2, [x4]

    /* Long delay to let FIFO drain */
    ldr     w3, =FIFO_DRAIN_DELAY
2:  subs    w3, w3, #1
    b.ne    2b

    /* Now read APPL and print raw 8 hex digits */
    ldr     x4, =APPL_CTRL
    ldr     w0, [x4]                /* APPL value */
    ldr     x4, =UARTC_THR
    mov     w2, #'P'
    str     w2, [x4]
    mov     w2, #'='
    str     w2, [x4]
    mov     w6, #28
.Lappl_loop:
    lsr     w2, w0, w6
    and     w2, w2, #0xF
    cmp     w2, #10
    b.lt    .Lappl_lo
    add     w2, w2, #('a' - 10)
    b       .Lappl_emit
.Lappl_lo:
    add     w2, w2, #'0'
.Lappl_emit:
    str     w2, [x4]
    ldr     w3, =HEX_CHAR_DELAY
3:  subs    w3, w3, #1
    b.ne    3b
    subs    w6, w6, #4
    b.ge    .Lappl_loop
    mov     w2, #'\r'
    str     w2, [x4]
    mov     w2, #'\n'
    str     w2, [x4]

    /* First thing: did UARTC work at EL1? Print marker. */
    adr     x0, str_el1_ok
    bl      puts_raw

    /* EL1:APPL= */
    adr     x0, str_el1_appl
    bl      puts_raw
    ldr     x1, =APPL_CTRL
    ldr     w0, [x1]
    bl      put_hex32_nl

    /* EL1:XHCI= */
    adr     x0, str_el1_xhci
    bl      puts_raw
    ldr     x1, =XHCI_HCD
    ldr     w0, [x1]
    bl      put_hex32_nl

    /* EL1:DONE */
    adr     x0, str_el1_done
    bl      puts_raw

halt_loop:
    wfe
    b       halt_loop


/* =========================================================================
 * puts_raw(x0 = zero-terminated string) — blocking UARTC putc loop
 * =========================================================================
 * Clobbers x1..x5. Preserves x0 on return (restored from saved x3).
 */
puts_raw:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    mov     x3, x0
1:
    ldrb    w2, [x3], #1
    cbz     w2, 2f
    /* Wait for LSR.THRE */
    ldr     x4, =UARTC_LSR
3:
    ldr     w5, [x4]
    tbz     w5, #5, 3b
    ldr     x4, =UARTC_THR
    str     w2, [x4]
    b       1b
2:
    ldp     x29, x30, [sp], #16
    ret


/* =========================================================================
 * put_hex32_nl(w0 = value) — prints "0xXXXXXXXX\r\n"
 * =========================================================================
 */
put_hex32_nl:
    stp     x29, x30, [sp, #-32]!
    stp     x21, x22, [sp, #16]
    mov     x29, sp
    mov     w21, w0

    adr     x0, str_hex_prefix
    bl      puts_raw

    mov     w22, #28                /* bit shift (MSB first) */
1:
    lsr     w0, w21, w22
    and     w0, w0, #0xF
    cmp     w0, #10
    b.lt    2f
    add     w0, w0, #('a' - 10)
    b       3f
2:
    add     w0, w0, #'0'
3:
    /* Emit single char in w0 */
    ldr     x4, =UARTC_LSR
4:
    ldr     w5, [x4]
    tbz     w5, #5, 4b
    ldr     x4, =UARTC_THR
    str     w0, [x4]
    subs    w22, w22, #4
    b.ge    1b

    adr     x0, str_newline
    bl      puts_raw

    ldp     x21, x22, [sp, #16]
    ldp     x29, x30, [sp], #32
    ret


/* =========================================================================
 * EL1 vector table — any exception = print + halt
 * =========================================================================
 * Each vector is 128 bytes (0x80), table must be 2 KB-aligned.
 *
 * The `bl puts_raw` inside each vector handler will double-fault
 * if the exception was itself caused by stack corruption (puts_raw
 * needs a valid sp to save x29/x30). For this one-shot smoke test
 * that's acceptable — we're halting anyway, and any cascade still
 * stops at the second exception. A production exception handler
 * would print via a stack-less path.
 */
.align 11
el1_vectors:
    .rept 16
    adr     x0, str_el1_vect
    bl      puts_raw
    b       .
    .align  7
    .endr


/* ========================================================================= */
.section ".rodata"
.align 3
banner:         .asciz "\r\n=== EL SMOKE TEST ===\r\n"
str_el2_appl:   .asciz "EL2:APPL="
str_el2_xhci:   .asciz "EL2:XHCI="
str_el1_ok:     .asciz "EL1:REACHED\r\n"
str_el1_appl:   .asciz "EL1:APPL="
str_el1_xhci:   .asciz "EL1:XHCI="
str_el1_done:   .asciz "EL1:DONE\r\n"
str_hex_prefix: .asciz "0x"
str_newline:    .asciz "\r\n"
str_el1_vect:   .asciz "EL1:VECTOR-HIT\r\n"
str_pre_eret:   .asciz "EL2:PRE-ERET\r\n"

#endif /* JETSON_EL1_SMOKE */
