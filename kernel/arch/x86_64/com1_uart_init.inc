/*
 * com1_uart_init.inc — reusable 16550-compatible UART init snippet
 * for the COM1 console. Configures COM1 (0x3F8) for 115200 8N1,
 * enables the TX/RX FIFO, and asserts DTR/RTS+OUT2. Leaves %dx
 * pointing at the THR (0x3F8) so the caller can immediately emit
 * bytes with `mov imm8,%al; out %al,%dx`.
 *
 * Included from trampoline32.S (Multiboot2 / 32-bit entry) and from
 * bzimage_entry.S (Linux bzImage / 64-bit entry) so the exact same
 * UART bring-up sequence runs on every boot path. The instruction
 * encodings (`mov imm8/16,%reg`, `out %al,%dx`) are identical in 32-
 * and 64-bit modes, so this file works unmodified from both sites.
 *
 * Callers must `#include` this via the C preprocessor (CMake passes
 * .S files through cpp before GAS).
 *
 * Required preconditions:
 *   - CPL=0 (kernel mode) so IN/OUT is permitted.
 *   - COM1 is at I/O port 0x3F8 with the standard 1.8432 MHz baud
 *     clock.
 */

#ifndef COM1_UART_INIT_INC
#define COM1_UART_INIT_INC

.macro COM1_UART_REINIT
    /* IER: no interrupts */
    mov $0x3F9, %dx
    mov $0x00, %al
    out %al, %dx
    /* LCR: set DLAB */
    mov $0x3FB, %dx
    mov $0x80, %al
    out %al, %dx
    /* DLL = 1 → 115200 baud @ 1.8432 MHz / 16 */
    mov $0x3F8, %dx
    mov $0x01, %al
    out %al, %dx
    /* DLM = 0 */
    mov $0x3F9, %dx
    mov $0x00, %al
    out %al, %dx
    /* LCR: 8N1, DLAB=0 */
    mov $0x3FB, %dx
    mov $0x03, %al
    out %al, %dx
    /* FCR: enable + clear FIFO, 14-byte RX trigger */
    mov $0x3FA, %dx
    mov $0xC7, %al
    out %al, %dx
    /* MCR: DTR + RTS + OUT2 */
    mov $0x3FC, %dx
    mov $0x0B, %al
    out %al, %dx
    /* Leave %dx pointing at THR so caller can emit bytes via
     *   mov $'C', %al ; out %al, %dx
     * without re-setting %dx. */
    mov $0x3F8, %dx
.endm

#endif  /* COM1_UART_INIT_INC */
