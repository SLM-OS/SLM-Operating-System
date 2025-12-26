/*
 * semihosting.c - ARM Semihosting Implementation
 *
 * Uses the ARM64 semihosting call interface via HLT #0xF000.
 * This is the A64 instruction for semihosting (different from A32's SVC/BKPT).
 */

#include "semihosting.h"

/*
 * Structure for SYS_EXIT_EXTENDED operation.
 */
struct exit_block {
    uint64_t reason;    /* ADP_Stopped_* code */
    uint64_t subcode;   /* Exit code (0 = success) */
};

/*
 * Make a semihosting call.
 *
 * ARM64 semihosting uses: HLT #0xF000
 * - x0 = operation number
 * - x1 = pointer to parameter block (or parameter itself)
 * - Returns result in x0
 */
static inline uint64_t semihosting_call(uint64_t operation, void *arg)
{
    register uint64_t op __asm__("x0") = operation;
    register uint64_t param __asm__("x1") = (uint64_t)arg;

    __asm__ volatile(
        "hlt #0xF000"
        : "+r"(op)
        : "r"(param)
        : "memory"
    );

    return op;
}

void semihosting_exit(int exit_code)
{
    struct exit_block block;

    /*
     * Use SYS_EXIT_EXTENDED for AArch64.
     * This allows specifying both a reason code and an exit subcode.
     */
    block.reason = (exit_code == 0) ?
        ADP_Stopped_ApplicationExit :
        ADP_Stopped_RunTimeErrorUnknown;
    block.subcode = (uint64_t)exit_code;

    semihosting_call(SYS_EXIT_EXTENDED, &block);

    /*
     * If semihosting call returns (shouldn't happen with SYS_EXIT),
     * try the simple SYS_EXIT as fallback.
     */
    semihosting_call(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit);

    /* If we're still here, loop forever */
    for (;;) {
        __asm__ volatile("wfi");
    }
}

int semihosting_available(void)
{
    /*
     * There's no reliable way to detect semihosting availability
     * without potentially causing a fault. For now, assume it's
     * available when ENABLE_SEMIHOSTING is defined at compile time.
     *
     * On real hardware, the HLT instruction will cause an exception.
     */
#ifdef ENABLE_SEMIHOSTING
    return 1;
#else
    return 0;
#endif
}
