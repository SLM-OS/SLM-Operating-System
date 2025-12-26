/*
 * semihosting.h - ARM Semihosting Interface
 *
 * ARM semihosting allows the target to communicate with the debugger/emulator.
 * Used primarily for:
 * - Clean QEMU exit after tests
 * - Debug output during early boot
 *
 * Only available when running under a debugger or emulator with semihosting
 * support (e.g., QEMU with -semihosting flag).
 */

#ifndef SEMIHOSTING_H
#define SEMIHOSTING_H

#include <stdint.h>

/*
 * ARM Semihosting operation codes.
 */
#define SYS_EXIT            0x18
#define SYS_EXIT_EXTENDED   0x20
#define SYS_WRITEC          0x03
#define SYS_WRITE0          0x04

/*
 * Exit codes for SYS_EXIT.
 * ADP_Stopped_ApplicationExit = 0x20026
 */
#define ADP_Stopped_ApplicationExit     0x20026
#define ADP_Stopped_RunTimeErrorUnknown 0x20023

/*
 * Exit QEMU/debugger with specified exit code.
 *
 * This function does not return when semihosting is available.
 * When semihosting is not available (real hardware), the HLT
 * instruction will cause an exception - the caller should handle this.
 *
 * @exit_code: 0 for success, non-zero for failure
 */
void semihosting_exit(int exit_code);

/*
 * Check if semihosting is available.
 *
 * Returns true if running under QEMU/debugger with semihosting.
 * Note: This may cause a fault on real hardware without semihosting.
 */
int semihosting_available(void);

#endif /* SEMIHOSTING_H */
