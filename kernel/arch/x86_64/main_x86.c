/*
 * main_x86.c - x86-64 kernel entry point
 *
 * Standalone kernel for x86-64 bring-up. Initializes serial console,
 * IDT, PIC, PIT timer, and parses the Multiboot2 memory map.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- I/O port access ---- */

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

static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* ---- Serial console (COM1) ---- */

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

void serial_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

void serial_print_hex(uint64_t value)
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

static void serial_print_dec(uint64_t value)
{
    char buf[21];
    int i = 0;

    if (value == 0) {
        serial_puts("0");
        return;
    }
    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }
    for (int j = i - 1; j >= 0; j--)
        serial_putc(buf[j]);
}

/* ---- PIC (8259) ---- */

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

static void pic_remap(void)
{
    /* Save masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: begin init sequence (cascade, ICW4 needed) */
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();

    /* ICW2: vector offsets — remap IRQ 0-7 to 32-39, IRQ 8-15 to 40-47 */
    outb(PIC1_DATA, 32);  io_wait();
    outb(PIC2_DATA, 40);  io_wait();

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04); io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* slave identity */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Restore masks (all masked for now) */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

static void pic_unmask(uint8_t irq)
{
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    outb(port, inb(port) & ~(1 << irq));
}

/* ---- PIT (8254) timer ---- */

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_FREQ  1193182
#define TARGET_HZ 100

volatile uint64_t pit_ticks;

static void pit_irq_handler(uint8_t irq)
{
    (void)irq;
    pit_ticks++;
}

static void pit_init(void)
{
    uint16_t divisor = PIT_FREQ / TARGET_HZ;

    /* Channel 0, lo/hi byte, rate generator (mode 2) */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, (divisor >> 8) & 0xFF);
}

/* ---- Multiboot2 parsing ---- */

static void parse_multiboot2(uint32_t mb_addr)
{
    uint8_t *ptr = (uint8_t *)(uintptr_t)mb_addr;
    uint32_t total_size = *(uint32_t *)ptr;

    serial_puts("[SLM-OS x86-64] Multiboot2 info: ");
    serial_print_dec(total_size);
    serial_puts(" bytes\n");

    ptr += 8;  /* Skip size + reserved */
    uint8_t *end = (uint8_t *)(uintptr_t)mb_addr + total_size;

    while (ptr < end) {
        uint32_t tag_type = *(uint32_t *)ptr;
        uint32_t tag_size = *(uint32_t *)(ptr + 4);

        if (tag_type == 0)
            break;

        switch (tag_type) {
        case 1: {
            /* Boot command line */
            const char *cmdline = (const char *)(ptr + 8);
            serial_puts("  Boot cmdline: ");
            serial_puts(cmdline);
            serial_puts("\n");
            break;
        }
        case 2: {
            /* Boot loader name */
            const char *name = (const char *)(ptr + 8);
            serial_puts("  Bootloader: ");
            serial_puts(name);
            serial_puts("\n");
            break;
        }
        case 6: {
            /* Memory map */
            uint32_t entry_size = *(uint32_t *)(ptr + 8);
            uint32_t entry_ver  = *(uint32_t *)(ptr + 12);
            (void)entry_ver;

            serial_puts("  Memory map:\n");

            uint64_t total_usable = 0;
            uint8_t *entry = ptr + 16;
            while (entry < ptr + tag_size) {
                uint64_t base = *(uint64_t *)entry;
                uint64_t len  = *(uint64_t *)(entry + 8);
                uint32_t type = *(uint32_t *)(entry + 16);

                const char *type_str;
                switch (type) {
                case 1:  type_str = "Available";  total_usable += len; break;
                case 2:  type_str = "Reserved";   break;
                case 3:  type_str = "ACPI Recl";  break;
                case 4:  type_str = "ACPI NVS";   break;
                case 5:  type_str = "Bad RAM";    break;
                default: type_str = "Unknown";    break;
                }

                serial_puts("    ");
                serial_print_hex(base);
                serial_puts(" - ");
                serial_print_hex(base + len - 1);
                serial_puts("  ");
                serial_print_dec(len >> 20);
                serial_puts(" MB  ");
                serial_puts(type_str);
                serial_puts("\n");

                entry += entry_size;
            }

            serial_puts("  Total usable: ");
            serial_print_dec(total_usable >> 20);
            serial_puts(" MB\n");
            break;
        }
        default:
            break;
        }

        /* Next tag (8-byte aligned) */
        ptr += (tag_size + 7) & ~7;
    }
}

/* ---- IDT and IRQ from idt.c ---- */

extern void idt_init(void);
extern void irq_register(uint8_t irq, void (*handler)(uint8_t));

/* ---- Kernel entry ---- */

void kernel_main(uint32_t multiboot_info_addr)
{
    serial_init();
    serial_puts("\n[SLM-OS x86-64] Boot started\n");

    /* CPU state */
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    serial_puts("[SLM-OS x86-64] CR0: ");
    serial_print_hex(cr0);
    serial_puts("\n");

    /* IDT */
    serial_puts("[SLM-OS x86-64] Loading IDT...\n");
    idt_init();
    serial_puts("[SLM-OS x86-64] IDT loaded (48 vectors)\n");

    /* PIC */
    serial_puts("[SLM-OS x86-64] Remapping PIC...\n");
    pic_remap();
    serial_puts("[SLM-OS x86-64] PIC remapped (IRQ 0-15 -> vectors 32-47)\n");

    /* PIT timer */
    serial_puts("[SLM-OS x86-64] Initializing PIT @ 100 Hz...\n");
    pit_init();
    irq_register(0, pit_irq_handler);
    pic_unmask(0);  /* Unmask IRQ 0 (timer) */
    serial_puts("[SLM-OS x86-64] PIT running\n");

    /* Enable interrupts */
    serial_puts("[SLM-OS x86-64] Enabling interrupts...\n");
    __asm__ volatile("sti");
    serial_puts("[SLM-OS x86-64] Interrupts enabled\n");

    /* Parse Multiboot2 info */
    serial_puts("\n");
    parse_multiboot2(multiboot_info_addr);

    /* Banner */
    serial_puts("\n");
    serial_puts("================================================================================\n");
    serial_puts("                    SLM-OS x86-64\n");
    serial_puts("================================================================================\n");
    serial_puts("\n");

    /* Wait for some timer ticks to confirm interrupts are working */
    serial_puts("Waiting for timer ticks... ");
    uint64_t start = pit_ticks;
    while (pit_ticks < start + 100)
        __asm__ volatile("hlt");
    serial_puts("done (");
    serial_print_dec(pit_ticks);
    serial_puts(" ticks = ~1 second)\n");

    serial_puts("\nSystem halted.\n");

    while (1) {
        __asm__ volatile("hlt");
    }
}
