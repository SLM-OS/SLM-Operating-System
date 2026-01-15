/*
 * main_x86.c - Minimal x86-64 kernel entry point for boot testing
 *
 * This is a temporary standalone main.c for testing x86-64 boot.
 * Once verified working, this will be integrated with the main kernel.
 */

#include <stdint.h>
#include <stddef.h>
#include "../../include/fb_console.h"

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

/* COM1 serial port (for debug output) */
#define COM1_PORT 0x3F8

static void serial_init(void)
{
    outb(COM1_PORT + 1, 0x00);    /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80);    /* Enable DLAB */
    outb(COM1_PORT + 0, 0x03);    /* Divisor low (38400 baud) */
    outb(COM1_PORT + 1, 0x00);    /* Divisor high */
    outb(COM1_PORT + 3, 0x03);    /* 8N1 */
    outb(COM1_PORT + 2, 0xC7);    /* Enable FIFO */
    outb(COM1_PORT + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static void serial_putc(char c)
{
    while ((inb(COM1_PORT + 5) & 0x20) == 0);  /* Wait for THR empty */
    outb(COM1_PORT, c);
}

static void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

/* String functions (minimal implementations for testing) */
static int strlen(const char *s)
{
    int len = 0;
    while (*s++) len++;
    return len;
}

/* Simple number to string for hex output */
static void print_hex(uint64_t value)
{
    static const char hex_chars[] = "0123456789ABCDEF";
    char buf[17];
    int i;

    for (i = 15; i >= 0; i--) {
        buf[i] = hex_chars[value & 0xF];
        value >>= 4;
    }
    buf[16] = '\0';

    fb_console_puts("0x");
    fb_console_puts(buf);
}

/*
 * Kernel entry point (called from boot.S)
 * multiboot_info points to the Multiboot2 information structure
 */
void kernel_main(uint32_t multiboot_info_addr)
{
    /* Initialize serial debug output first */
    serial_init();
    serial_puts("\n[SLM-OS x86-64] Boot started\n");

    /* Initialize framebuffer console from Multiboot2 info */
    void *multiboot_info = (void *)(uintptr_t)multiboot_info_addr;
    serial_puts("[SLM-OS x86-64] Initializing framebuffer...\n");
    fb_console_init(multiboot_info);
    serial_puts("[SLM-OS x86-64] Framebuffer initialized\n");

    /* Print banner */
    fb_console_puts("\n");
    fb_console_puts("================================================================================\n");
    fb_console_puts("                    SLM-OS x86-64 Boot Test\n");
    fb_console_puts("================================================================================\n");
    fb_console_puts("\n");

    fb_console_puts("Multiboot2 info at: ");
    print_hex((uint64_t)multiboot_info_addr);
    fb_console_puts("\n\n");

    /* Get framebuffer info */
    uint32_t width, height, cols, rows;
    fb_console_get_info(&width, &height, &cols, &rows);

    fb_console_puts("Framebuffer:\n");
    fb_console_puts("  Resolution: ");
    /* Print width */
    char buf[16];
    int i = 0;
    uint32_t w = width;
    if (w == 0) {
        buf[i++] = '0';
    } else {
        while (w > 0) {
            buf[i++] = '0' + (w % 10);
            w /= 10;
        }
    }
    for (int j = i - 1; j >= 0; j--) {
        fb_console_putc(buf[j]);
    }
    fb_console_putc('x');
    i = 0;
    uint32_t h = height;
    if (h == 0) {
        buf[i++] = '0';
    } else {
        while (h > 0) {
            buf[i++] = '0' + (h % 10);
            h /= 10;
        }
    }
    for (int j = i - 1; j >= 0; j--) {
        fb_console_putc(buf[j]);
    }
    fb_console_puts("\n");

    fb_console_puts("  Console: ");
    i = 0;
    uint32_t c = cols;
    if (c == 0) {
        buf[i++] = '0';
    } else {
        while (c > 0) {
            buf[i++] = '0' + (c % 10);
            c /= 10;
        }
    }
    for (int j = i - 1; j >= 0; j--) {
        fb_console_putc(buf[j]);
    }
    fb_console_putc('x');
    i = 0;
    uint32_t r = rows;
    if (r == 0) {
        buf[i++] = '0';
    } else {
        while (r > 0) {
            buf[i++] = '0' + (r % 10);
            r /= 10;
        }
    }
    for (int j = i - 1; j >= 0; j--) {
        fb_console_putc(buf[j]);
    }
    fb_console_puts(" characters\n");

    fb_console_puts("\n");
    fb_console_puts("x86-64 boot successful!\n");
    fb_console_puts("\n");
    fb_console_puts("System halted.\n");

    serial_puts("[SLM-OS x86-64] Boot successful! System halted.\n");

    /* Halt */
    while (1) {
        __asm__ volatile("hlt");
    }
}
