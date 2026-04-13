/*
 * semihosting.c - ARM Semihosting Implementation
 *
 * Uses the ARM64 semihosting call interface via HLT #0xF000.
 * This is the A64 instruction for semihosting (different from A32's SVC/BKPT).
 */

#include "semihosting.h"

#if defined(PLATFORM_X86_64)

/*
 * x86-64: Use QEMU isa-debug-exit device for test exit.
 * Port 0x501, writing (code << 1) | 1. QEMU exits with that value.
 * QEMU sees exit_code=0 → writes 0x01 → QEMU exits with code 1.
 * QEMU sees exit_code=1 → writes 0x03 → QEMU exits with code 3.
 * The test harness accounts for this mapping.
 */
static inline void outb_x86(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void semihosting_exit(int exit_code)
{
    /* isa-debug-exit at port 0x501: value = (exit_code << 1) | 1 */
    outb_x86(0x501, (uint8_t)((exit_code << 1) | 1));
    /* Fallback: halt if isa-debug-exit not present */
    for (;;)
        __asm__ volatile("hlt");
}

int semihosting_available(void)
{
    return 1;  /* Available when QEMU has -device isa-debug-exit */
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
    /* ENABLE_SEMIHOSTING is defined by CMakeLists.txt only when PLATFORM=
     * QEMU_VIRT. On Pi 5 and Jetson the flag is absent, so this returns 0
     * and callers never issue the HLT #0xF000 probe that would trap on
     * real hardware without a semihosting host. */
#ifdef ENABLE_SEMIHOSTING
    return 1;
#else
    return 0;
#endif
}

#endif /* PLATFORM_X86_64 */
