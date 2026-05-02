/*
 * panic.c - Kernel panic handler for SLM-OS
 */

#include "uart.h"
#include "debug.h"
#include "task.h"
#include "smp.h"
#include <stdint.h>
#include <stdarg.h>

#if defined(PLATFORM_X86_64)
/*
 * x86-64 register dump for panic diagnostics.
 */
static void dump_registers(void)
{
    uint64_t rsp, cr2;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    struct task *current = task_current();
    uint32_t task_id = current ? current->id : 0xFFFFFFFF;
    const char *task_name = current ? current->name : "<none>";

    uart_puts_unlocked("Task Context:\n");
    uart_printf_unlocked("  Task ID:   %lu\n", task_id);
    uart_printf_unlocked("  Task Name: %s\n", task_name);
    uart_printf_unlocked("  CPU:       0\n");

    uart_puts_unlocked("\nRegister Dump:\n");
    uart_printf_unlocked("  RSP:  0x%lx\n", rsp);
    uart_printf_unlocked("  CR2:  0x%lx  (page fault address)\n", cr2);
}

#else /* ARM64 */
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

    uart_puts_unlocked("Task Context:\n");
    uart_printf_unlocked("  Task ID:   %lu\n", task_id);
    uart_printf_unlocked("  Task Name: %s\n", task_name);
    uart_printf_unlocked("  CPU:       %lu\n", current_cpu);

    uart_puts_unlocked("\nRegister Dump:\n");
    uart_printf_unlocked("  SP:       0x%lx\n", sp);
    uart_printf_unlocked("  ELR_EL1:  0x%lx  (return address)\n", elr);
    uart_printf_unlocked("  SPSR_EL1: 0x%lx\n", spsr);
    uart_printf_unlocked("  ESR_EL1:  0x%lx\n", esr);
    uart_printf_unlocked("  EC:       0x%x (%s)\n", ec, decode_exception_class(ec));
    uart_printf_unlocked("  FAR_EL1:  0x%lx  (fault address)\n", far);
}
#endif /* PLATFORM_X86_64 */

/*
 * Panic - halt the system with an error message.
 *
 * Prints a panic message and register dump to UART, then halts.
 */
void panic(const char *fmt, ...)
{
    va_list args;

    /* Disable interrupts to prevent further issues */
#if defined(PLATFORM_X86_64)
    __asm__ volatile("cli");
#else
    __asm__ volatile("msr daifset, #0xF");
#endif

    uart_puts_unlocked("\n\n");
    uart_puts_unlocked("*********************************\n");
    uart_puts_unlocked("***      KERNEL PANIC         ***\n");
    uart_puts_unlocked("*********************************\n\n");

    va_start(args, fmt);
    uart_vprintf(fmt, args);  /* vprintf is already unlocked */
    va_end(args);

    uart_puts_unlocked("\n\n");
    dump_registers();

    /* #601 Bug B diagnostic: dump task stack canary state + the
     * stack contents around the current SP. If any task's canary
     * is broken, the offset + corrupted bytes get logged. Even if
     * canaries are intact, the SP-region dump shows what's living
     * on the stack at the moment of the panic — a corrupt return
     * address there will be visible alongside its surrounding
     * stack-frame context. */
    {
        extern int task_canary_check_all(void);
        uart_puts_unlocked("\nStack canary check:\n");
        int broken = task_canary_check_all();
        if (broken == 0) {
            uart_puts_unlocked("  All task canaries intact.\n");
        }

        /* Stack dump near SP. On AArch64 we can read SP via mrs
         * then dump 256 bytes (32 quadwords) around it. Stack grows
         * down, so dump from SP-64 to SP+192 to capture the
         * immediately-active frames + a bit below for canary visibility. */
#if !defined(PLATFORM_X86_64)
        {
            uint64_t sp;
            __asm__ volatile("mov %0, sp" : "=r"(sp));
            uart_printf_unlocked("\nStack dump around SP=0x%lx:\n", sp);
            uint64_t start = (sp - 64) & ~0x7ULL;
            const uint64_t *p = (const uint64_t *)(uintptr_t)start;
            for (int row = 0; row < 32; row++) {
                uint64_t addr = start + row * 8;
                uint64_t v = p[row];
                /* ASCII view of the 8 bytes for spotting strings. */
                char ascii[9];
                for (int j = 0; j < 8; j++) {
                    uint8_t b = (uint8_t)(v >> (j * 8));
                    ascii[j] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
                }
                ascii[8] = '\0';
                uart_printf_unlocked("  0x%lx: 0x%016lx  \"%s\"%s\n",
                                     (unsigned long)addr,
                                     (unsigned long)v, ascii,
                                     (addr == sp) ? "  <- SP" : "");
            }
        }
#endif
    }

    uart_puts_unlocked("\nSystem halted.\n");

    /* Infinite loop - system is dead */
    while (1) {
#if defined(PLATFORM_X86_64)
        __asm__ volatile("hlt");
#else
        __asm__ volatile("wfi");
#endif
    }
}
