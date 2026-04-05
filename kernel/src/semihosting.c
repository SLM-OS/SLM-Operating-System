/*
 * semihosting.c - ARM Semihosting Implementation
 *
 * Uses the ARM64 semihosting call interface via HLT #0xF000.
 * This is the A64 instruction for semihosting (different from A32's SVC/BKPT).
 */

#include "semihosting.h"

#if defined(PLATFORM_X86_64)

/* x86-64: no semihosting support */
void semihosting_exit(int exit_code)
{
    (void)exit_code;
    for (;;)
        __asm__ volatile("hlt");
}

int semihosting_available(void)
{
    return 0;
}

#else /* ARM64 */

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

    block.reason = (exit_code == 0) ?
        ADP_Stopped_ApplicationExit :
        ADP_Stopped_RunTimeErrorUnknown;
    block.subcode = (uint64_t)exit_code;

    semihosting_call(SYS_EXIT_EXTENDED, &block);

    semihosting_call(SYS_EXIT, (void *)ADP_Stopped_ApplicationExit);

    for (;;) {
        __asm__ volatile("wfi");
    }
}

int semihosting_available(void)
{
#ifdef ENABLE_SEMIHOSTING
    return 1;
#else
    return 0;
#endif
}

#endif /* PLATFORM_X86_64 */
