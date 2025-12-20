/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"
#include "uart.h"
#include "debug.h"
#include <stdint.h>

/* External symbols from linker script */
extern char __text_start, __text_end;
extern char __data_start, __data_end;
extern char __bss_start, __bss_end;
extern char __kernel_end;

/*
 * Print kernel memory layout information.
 */
static void print_memory_info(void)
{
    uart_printf("Memory Layout:\n");
    uart_printf("  .text:   %p - %p\n", &__text_start, &__text_end);
    uart_printf("  .data:   %p - %p\n", &__data_start, &__data_end);
    uart_printf("  .bss:    %p - %p\n", &__bss_start, &__bss_end);
    uart_printf("  End:     %p\n", &__kernel_end);
    uart_printf("  RAM:     %p - %p (%u MB)\n",
                (void *)RAM_BASE,
                (void *)(RAM_BASE + RAM_SIZE),
                RAM_SIZE / (1024 * 1024));
}

/*
 * kernel_main - Main kernel entry point
 *
 * Called from boot.S after basic hardware initialization.
 * This function should not return.
 */
void kernel_main(void)
{
    /* Initialize UART for debug output */
    uart_init();

    /* Banner */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("  SLM-OS v0.1.0\n");
    uart_puts("  Small Language Model Operating System\n");
    uart_puts("========================================\n\n");

    INFO("Boot successful");
    INFO("Running at EL1 on %s", "QEMU virt");

    /* Show memory layout */
    uart_puts("\n");
    print_memory_info();

    /* Test printf format specifiers */
    uart_puts("\nPrintf test:\n");
    uart_printf("  Decimal:     %d, %d\n", 42, -123);
    uart_printf("  Unsigned:    %u\n", 4294967295U);
    uart_printf("  Hex:         0x%x, 0x%X\n", 0xDEAD, 0xBEEF);
    uart_printf("  Long hex:    0x%lx\n", 0x123456789ABCDEF0UL);
    uart_printf("  Pointer:     %p\n", (void *)kernel_main);
    uart_printf("  String:      %s\n", "Hello, SLM-OS!");
    uart_printf("  Char:        %c\n", 'X');
    uart_printf("  Percent:     100%%\n");

    /* Debug macro test */
    uart_puts("\nDebug macro test:\n");
    DEBUG_PRINT("This is a debug message with value: %d", 42);
    INFO("System initialized");
    WARN("This is a warning");

    /* Assert test (should pass) */
    ASSERT(1 == 1);
    DEBUG_PRINT("Assert passed");

    /* Test panic with register dump (uncomment to test) */
    /* panic("Test panic: value=%d, ptr=%p", 42, (void *)0xDEADBEEF); */

    /* Done - halt */
    uart_puts("\n");
    INFO("Kernel initialization complete");
    INFO("Halting");

    while (1) {
        __asm__ volatile("wfi");
    }
}
