/*
 * main_x86.c - Minimal x86-64 kernel entry point
 *
 * This is a standalone kernel main for x86-64 boot testing.
 * Uses COM1 serial console for all output.
 */

#include <stdint.h>
#include <stddef.h>

/* x86 I/O port access */
static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* COM1 serial port */
#define COM1_PORT 0x3F8

static void serial_init(void)
{
    outb(COM1_PORT + 1, 0x00);    /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80);    /* Enable DLAB */
    outb(COM1_PORT + 0, 0x01);    /* Divisor low (115200 baud) */
    outb(COM1_PORT + 1, 0x00);    /* Divisor high */
    outb(COM1_PORT + 3, 0x03);    /* 8N1 */
    outb(COM1_PORT + 2, 0xC7);    /* Enable FIFO */
    outb(COM1_PORT + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static void serial_putc(char c)
{
    while ((inb(COM1_PORT + 5) & 0x20) == 0);
    outb(COM1_PORT, c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

static void serial_print_hex(uint64_t value)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 17; i >= 2; i--) {
        buf[i] = hex_chars[value & 0xF];
        value >>= 4;
    }
    buf[18] = '\0';
    serial_puts(buf);
}

/*
 * Kernel entry point (called from entry64.S)
 */
void kernel_main(uint32_t multiboot_info_addr)
{
    serial_init();
    serial_puts("\n[SLM-OS x86-64] Boot started\n");
    serial_puts("[SLM-OS x86-64] Multiboot info at: ");
    serial_print_hex(multiboot_info_addr);
    serial_puts("\n");

    /* Dump control registers to verify CPU state */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    serial_puts("[SLM-OS x86-64] CR0: ");
    serial_print_hex(cr0);
    serial_puts("\n");

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    serial_puts("[SLM-OS x86-64] CR4: ");
    serial_print_hex(cr4);
    serial_puts("\n");

    uint32_t efer_lo, efer_hi;
    __asm__ volatile("rdmsr" : "=a"(efer_lo), "=d"(efer_hi) : "c"(0xC0000080));
    serial_puts("[SLM-OS x86-64] EFER: ");
    serial_print_hex(efer_lo);
    serial_puts("\n");

    serial_puts("\n");
    serial_puts("================================================================================\n");
    serial_puts("                    SLM-OS x86-64 Boot Test\n");
    serial_puts("================================================================================\n");
    serial_puts("\n");
    serial_puts("x86-64 long mode active.\n");
    serial_puts("  Serial: COM1 @ 115200 baud\n");
    serial_puts("\n");
    serial_puts("System halted.\n");

    while (1) {
        __asm__ volatile("hlt");
    }
}
