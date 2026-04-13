/*
 * idt.c - x86-64 IDT setup and exception handling
 *
 * Sets up the 64-bit Interrupt Descriptor Table with handlers for
 * CPU exceptions (0-31) and PIC IRQs (32-47).
 */

#include <stdint.h>

/* Serial output wrappers — thin adapters onto uart.h. */
#include "uart.h"
static void serial_puts(const char *s) { uart_puts(s); }
static void serial_print_hex(uint64_t v) { uart_printf("0x%lx", v); }

/* ISR stubs from idt.S */
extern void isr_0(void);
extern void isr_1(void);
extern void isr_2(void);
extern void isr_3(void);
extern void isr_4(void);
extern void isr_5(void);
extern void isr_6(void);
extern void isr_7(void);
extern void isr_8(void);
extern void isr_9(void);
extern void isr_10(void);
extern void isr_11(void);
extern void isr_12(void);
extern void isr_13(void);
extern void isr_14(void);
extern void isr_15(void);
extern void isr_16(void);
extern void isr_17(void);
extern void isr_18(void);
extern void isr_19(void);
extern void isr_20(void);
extern void isr_21(void);
extern void isr_22(void);
extern void isr_23(void);
extern void isr_24(void);
extern void isr_25(void);
extern void isr_26(void);
extern void isr_27(void);
extern void isr_28(void);
extern void isr_29(void);
extern void isr_30(void);
extern void isr_31(void);
extern void isr_32(void);
extern void isr_33(void);
extern void isr_34(void);
extern void isr_35(void);
extern void isr_36(void);
extern void isr_37(void);
extern void isr_38(void);
extern void isr_39(void);
extern void isr_40(void);
extern void isr_41(void);
extern void isr_42(void);
extern void isr_43(void);
extern void isr_44(void);
extern void isr_45(void);
extern void isr_46(void);
extern void isr_47(void);
extern void isr_48(void);
extern void isr_49(void);
extern void isr_50(void);
extern void isr_51(void);
extern void isr_52(void);
extern void isr_53(void);
extern void isr_54(void);
extern void isr_55(void);
extern void isr_56(void);
extern void isr_57(void);
extern void isr_58(void);
extern void isr_59(void);
extern void isr_60(void);
extern void isr_61(void);
extern void isr_62(void);
extern void isr_63(void);

/*
 * IDT gate descriptor (16 bytes on x86-64)
 */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;           /* IST index (0 = no IST) */
    uint8_t  type_attr;     /* type + DPL + present */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_ENTRIES 64
static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtr;

/* Type: 64-bit interrupt gate, DPL=0, present */
#define IDT_INTERRUPT_GATE 0x8E
/* Type: 64-bit trap gate, DPL=0, present */
#define IDT_TRAP_GATE 0x8F

static void idt_set_gate(int vector, void (*handler)(void), uint8_t type)
{
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = addr & 0xFFFF;
    idt[vector].selector    = 0x08;             /* kernel code segment */
    idt[vector].ist         = 0;
    idt[vector].type_attr   = type;
    idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].reserved    = 0;
}

/*
 * Register state pushed by ISR stubs + CPU.
 * Must match the push order in idt.S.
 */
struct interrupt_frame {
    /* Pushed by isr_common (reverse order) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* Pushed by ISR stub */
    uint64_t vector;
    uint64_t error_code;
    /* Pushed by CPU */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

static const char *exception_names[] = {
    "#DE Divide Error",
    "#DB Debug",
    "NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range",
    "#UD Invalid Opcode",
    "#NM No FPU",
    "#DF Double Fault",
    "Coprocessor Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack Fault",
    "#GP General Protection",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 FPU Error",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Exception",
    "#VE Virtualization",
    "#CP Control Protection",
};

/* IRQ handler callback table (vectors 32-63 → index 0-31) */
typedef void (*irq_handler_t)(uint8_t irq);
static irq_handler_t irq_handlers[32];

void irq_register(uint8_t irq, irq_handler_t handler)
{
    if (irq < 32)
        irq_handlers[irq] = handler;
}

/* gic_end_interrupt for LAPIC EOI — provided by pic.c in integrated build.
 * Weak default for standalone test kernel (no LAPIC). */
__attribute__((weak)) void gic_end_interrupt(uint32_t irq) { (void)irq; }

/* I/O port access for PIC test in test suite */
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * C exception/interrupt handler — called from isr_common in idt.S.
 */
void exception_handler(struct interrupt_frame *frame)
{
    uint64_t vec = frame->vector;

    if (vec < 32) {
        /* CPU exception */
        serial_puts("\n*** EXCEPTION: ");
        if (vec < sizeof(exception_names) / sizeof(exception_names[0]))
            serial_puts(exception_names[vec]);
        else
            serial_puts("Unknown");
        serial_puts(" (vector ");
        serial_print_hex(vec);
        serial_puts(")\n");

        serial_puts("  Error code: ");
        serial_print_hex(frame->error_code);
        serial_puts("\n");

        serial_puts("  RIP: ");
        serial_print_hex(frame->rip);
        serial_puts("  CS:  ");
        serial_print_hex(frame->cs);
        serial_puts("\n");

        serial_puts("  RSP: ");
        serial_print_hex(frame->rsp);
        serial_puts("  SS:  ");
        serial_print_hex(frame->ss);
        serial_puts("\n");

        serial_puts("  RFLAGS: ");
        serial_print_hex(frame->rflags);
        serial_puts("\n");

        serial_puts("  RAX: ");
        serial_print_hex(frame->rax);
        serial_puts("  RBX: ");
        serial_print_hex(frame->rbx);
        serial_puts("\n");

        serial_puts("  RCX: ");
        serial_print_hex(frame->rcx);
        serial_puts("  RDX: ");
        serial_print_hex(frame->rdx);
        serial_puts("\n");

        serial_puts("  RSI: ");
        serial_print_hex(frame->rsi);
        serial_puts("  RDI: ");
        serial_print_hex(frame->rdi);
        serial_puts("\n");

        serial_puts("  RBP: ");
        serial_print_hex(frame->rbp);
        serial_puts("\n");

        if (vec == 14) {
            /* Page fault: CR2 has faulting address */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            serial_puts("  CR2 (fault addr): ");
            serial_print_hex(cr2);
            serial_puts("\n");
        }

        serial_puts("*** HALT ***\n");
        while (1)
            __asm__ volatile("hlt");
    } else if (vec >= 32 && vec < 64) {
        /* Hardware IRQ (IOAPIC vectors 32-47, LAPIC vectors 48-63).
         * Send EOI before handler — timer_handler may context switch
         * via schedule() and never return here. */
        uint8_t irq = vec - 32;
        gic_end_interrupt(vec);  /* LAPIC EOI for all APIC-delivered interrupts */
        if (irq < 32 && irq_handlers[irq])
            irq_handlers[irq](irq);
    }
}

/*
 * Initialize the IDT and load it.
 */
void idt_init(void)
{
    /* Set up exception handlers (trap gates — don't clear IF) */
    idt_set_gate(0,  isr_0,  IDT_TRAP_GATE);
    idt_set_gate(1,  isr_1,  IDT_TRAP_GATE);
    idt_set_gate(2,  isr_2,  IDT_INTERRUPT_GATE);  /* NMI */
    idt_set_gate(3,  isr_3,  IDT_TRAP_GATE);
    idt_set_gate(4,  isr_4,  IDT_TRAP_GATE);
    idt_set_gate(5,  isr_5,  IDT_TRAP_GATE);
    idt_set_gate(6,  isr_6,  IDT_TRAP_GATE);
    idt_set_gate(7,  isr_7,  IDT_TRAP_GATE);
    idt_set_gate(8,  isr_8,  IDT_TRAP_GATE);
    idt_set_gate(9,  isr_9,  IDT_TRAP_GATE);
    idt_set_gate(10, isr_10, IDT_TRAP_GATE);
    idt_set_gate(11, isr_11, IDT_TRAP_GATE);
    idt_set_gate(12, isr_12, IDT_TRAP_GATE);
    idt_set_gate(13, isr_13, IDT_TRAP_GATE);
    idt_set_gate(14, isr_14, IDT_TRAP_GATE);
    idt_set_gate(15, isr_15, IDT_TRAP_GATE);
    idt_set_gate(16, isr_16, IDT_TRAP_GATE);
    idt_set_gate(17, isr_17, IDT_TRAP_GATE);
    idt_set_gate(18, isr_18, IDT_TRAP_GATE);
    idt_set_gate(19, isr_19, IDT_TRAP_GATE);
    idt_set_gate(20, isr_20, IDT_TRAP_GATE);
    idt_set_gate(21, isr_21, IDT_TRAP_GATE);
    idt_set_gate(22, isr_22, IDT_TRAP_GATE);
    idt_set_gate(23, isr_23, IDT_TRAP_GATE);
    idt_set_gate(24, isr_24, IDT_TRAP_GATE);
    idt_set_gate(25, isr_25, IDT_TRAP_GATE);
    idt_set_gate(26, isr_26, IDT_TRAP_GATE);
    idt_set_gate(27, isr_27, IDT_TRAP_GATE);
    idt_set_gate(28, isr_28, IDT_TRAP_GATE);
    idt_set_gate(29, isr_29, IDT_TRAP_GATE);
    idt_set_gate(30, isr_30, IDT_TRAP_GATE);
    idt_set_gate(31, isr_31, IDT_TRAP_GATE);

    /* Set up IRQ handlers (interrupt gates — clear IF) */
    idt_set_gate(32, isr_32, IDT_INTERRUPT_GATE);
    idt_set_gate(33, isr_33, IDT_INTERRUPT_GATE);
    idt_set_gate(34, isr_34, IDT_INTERRUPT_GATE);
    idt_set_gate(35, isr_35, IDT_INTERRUPT_GATE);
    idt_set_gate(36, isr_36, IDT_INTERRUPT_GATE);
    idt_set_gate(37, isr_37, IDT_INTERRUPT_GATE);
    idt_set_gate(38, isr_38, IDT_INTERRUPT_GATE);
    idt_set_gate(39, isr_39, IDT_INTERRUPT_GATE);
    idt_set_gate(40, isr_40, IDT_INTERRUPT_GATE);
    idt_set_gate(41, isr_41, IDT_INTERRUPT_GATE);
    idt_set_gate(42, isr_42, IDT_INTERRUPT_GATE);
    idt_set_gate(43, isr_43, IDT_INTERRUPT_GATE);
    idt_set_gate(44, isr_44, IDT_INTERRUPT_GATE);
    idt_set_gate(45, isr_45, IDT_INTERRUPT_GATE);
    idt_set_gate(46, isr_46, IDT_INTERRUPT_GATE);
    idt_set_gate(47, isr_47, IDT_INTERRUPT_GATE);

    /* Extended vectors (48-63) for LAPIC timer, IPIs, etc. */
    idt_set_gate(48, isr_48, IDT_INTERRUPT_GATE);
    idt_set_gate(49, isr_49, IDT_INTERRUPT_GATE);
    idt_set_gate(50, isr_50, IDT_INTERRUPT_GATE);
    idt_set_gate(51, isr_51, IDT_INTERRUPT_GATE);
    idt_set_gate(52, isr_52, IDT_INTERRUPT_GATE);
    idt_set_gate(53, isr_53, IDT_INTERRUPT_GATE);
    idt_set_gate(54, isr_54, IDT_INTERRUPT_GATE);
    idt_set_gate(55, isr_55, IDT_INTERRUPT_GATE);
    idt_set_gate(56, isr_56, IDT_INTERRUPT_GATE);
    idt_set_gate(57, isr_57, IDT_INTERRUPT_GATE);
    idt_set_gate(58, isr_58, IDT_INTERRUPT_GATE);
    idt_set_gate(59, isr_59, IDT_INTERRUPT_GATE);
    idt_set_gate(60, isr_60, IDT_INTERRUPT_GATE);
    idt_set_gate(61, isr_61, IDT_INTERRUPT_GATE);
    idt_set_gate(62, isr_62, IDT_INTERRUPT_GATE);
    idt_set_gate(63, isr_63, IDT_INTERRUPT_GATE);

    /* Load IDT */
    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));
}
