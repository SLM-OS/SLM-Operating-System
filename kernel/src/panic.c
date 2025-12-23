/*
 * panic.c - Kernel panic handler for SLM-OS
 */

#include "uart.h"
#include "debug.h"
#include "task.h"
#include "smp.h"
#include <stdint.h>
#include <stdarg.h>

/*
 * Read ARM64 system registers for exception debugging.
 */
static inline uint64_t read_esr_el1(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(val));
    return val;
}

static inline uint64_t read_elr_el1(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, elr_el1" : "=r"(val));
    return val;
}

static inline uint64_t read_far_el1(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, far_el1" : "=r"(val));
    return val;
}

static inline uint64_t read_spsr_el1(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(val));
    return val;
}

static inline uint64_t read_sp(void)
{
    uint64_t val;
    __asm__ volatile("mov %0, sp" : "=r"(val));
    return val;
}

/*
 * Decode ESR_EL1 exception class for human-readable output.
 */
static const char *decode_exception_class(uint32_t ec)
{
    switch (ec) {
    case 0x00: return "Unknown reason";
    case 0x01: return "Trapped WFI/WFE";
    case 0x0E: return "Illegal execution state";
    case 0x15: return "SVC instruction (AArch64)";
    case 0x18: return "Trapped MSR/MRS (AArch64)";
    case 0x20: return "Instruction Abort (lower EL)";
    case 0x21: return "Instruction Abort (same EL)";
    case 0x22: return "PC alignment fault";
    case 0x24: return "Data Abort (lower EL)";
    case 0x25: return "Data Abort (same EL)";
    case 0x26: return "SP alignment fault";
    case 0x2C: return "Trapped FP exception";
    case 0x2F: return "SError interrupt";
    case 0x30: return "Breakpoint (lower EL)";
    case 0x31: return "Breakpoint (same EL)";
    case 0x32: return "Software Step (lower EL)";
    case 0x33: return "Software Step (same EL)";
    case 0x34: return "Watchpoint (lower EL)";
    case 0x35: return "Watchpoint (same EL)";
    case 0x3C: return "BRK instruction (AArch64)";
    default:   return "Reserved/Unknown";
    }
}

/*
 * Dump system registers and task context useful for debugging.
 */
static void dump_registers(void)
{
    uint64_t esr = read_esr_el1();
    uint64_t elr = read_elr_el1();
    uint64_t far = read_far_el1();
    uint64_t spsr = read_spsr_el1();
    uint64_t sp = read_sp();

    /* Extract exception class from ESR (bits 31:26) */
    uint32_t ec = (esr >> 26) & 0x3F;

    /* Get current task info */
    struct task *current = task_current();
    uint32_t task_id = current ? current->id : 0xFFFFFFFF;
    const char *task_name = current ? current->name : "<none>";
    uint32_t current_cpu = cpu_id();

    uart_puts("Task Context:\n");
    uart_printf("  Task ID:   %lu\n", task_id);
    uart_printf("  Task Name: %s\n", task_name);
    uart_printf("  CPU:       %lu\n", current_cpu);

    uart_puts("\nRegister Dump:\n");
    uart_printf("  SP:       0x%lx\n", sp);
    uart_printf("  ELR_EL1:  0x%lx  (return address)\n", elr);
    uart_printf("  SPSR_EL1: 0x%lx\n", spsr);
    uart_printf("  ESR_EL1:  0x%lx\n", esr);
    uart_printf("  EC:       0x%x (%s)\n", ec, decode_exception_class(ec));
    uart_printf("  FAR_EL1:  0x%lx  (fault address)\n", far);
}

/*
 * Panic - halt the system with an error message.
 *
 * Prints a panic message and register dump to UART, then halts.
 */
void panic(const char *fmt, ...)
{
    va_list args;

    /* Disable interrupts to prevent further issues */
    __asm__ volatile("msr daifset, #0xF");

    uart_puts("\n\n");
    uart_puts("*********************************\n");
    uart_puts("***      KERNEL PANIC         ***\n");
    uart_puts("*********************************\n\n");

    va_start(args, fmt);
    uart_vprintf(fmt, args);
    va_end(args);

    uart_puts("\n\n");
    dump_registers();

    uart_puts("\nSystem halted.\n");

    /* Infinite loop - system is dead */
    while (1) {
        __asm__ volatile("wfi");
    }
}
