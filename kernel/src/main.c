/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"
#include "uart.h"
#include "debug.h"
#include "pmm.h"
#include "task.h"
#include "sched.h"
#include "gic.h"
#include "timer.h"
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
 * Simple delay loop (busy wait)
 */
static void delay(volatile uint32_t count)
{
    while (count--) {
        __asm__ volatile("nop");
    }
}

/*
 * Test task A - loops with delay (preempted by timer)
 */
static void task_a_func(void *arg)
{
    int count = (int)(uintptr_t)arg;

    for (int i = 0; i < count; i++) {
        uart_printf("[Task A] iteration %d/%d\n", i + 1, count);
        delay(500000);
    }

    uart_puts("[Task A] Done!\n");
}

/*
 * Test task B - loops with delay (preempted by timer)
 */
static void task_b_func(void *arg)
{
    int count = (int)(uintptr_t)arg;

    for (int i = 0; i < count; i++) {
        uart_printf("[Task B] iteration %d/%d\n", i + 1, count);
        delay(500000);
    }

    uart_puts("[Task B] Done!\n");
}

/*
 * Test task C - loops with delay (preempted by timer)
 */
static void task_c_func(void *arg)
{
    int count = (int)(uintptr_t)arg;

    for (int i = 0; i < count; i++) {
        uart_printf("[Task C] iteration %d/%d\n", i + 1, count);
        delay(500000);
    }

    uart_puts("[Task C] Done!\n");
}

/*
 * Main task - runs after scheduler starts
 */
static void main_task_func(void *arg)
{
    (void)arg;

    uart_puts("\n[Main] Creating test tasks...\n\n");

    /* Create test tasks */
    struct task *task_a = task_create("task_a", task_a_func, (void *)5);
    struct task *task_b = task_create("task_b", task_b_func, (void *)5);
    struct task *task_c = task_create("task_c", task_c_func, (void *)5);

    if (!task_a || !task_b || !task_c) {
        ERROR("Failed to create test tasks");
        return;
    }

    /* Add to scheduler */
    scheduler_add_task(task_a);
    scheduler_add_task(task_b);
    scheduler_add_task(task_c);

    uart_puts("[Main] Tasks created and added to scheduler\n");
    scheduler_dump();
    uart_puts("\n[Main] Starting preemptive scheduling test...\n\n");

    /*
     * Wait for tasks to complete via timer preemption.
     * The timer interrupt will call scheduler_tick() which
     * will preempt tasks and switch between them.
     */
    while (task_a->state != TASK_TERMINATED ||
           task_b->state != TASK_TERMINATED ||
           task_c->state != TASK_TERMINATED) {
        /* Busy wait - timer will preempt us */
        delay(100000);
    }

    /* Show final stats */
    uart_puts("\n[Main] All tasks completed\n");
    scheduler_dump();
    pmm_dump_stats();

    uart_puts("\n");
    INFO("Preemptive scheduler test complete");

    /* Trigger an exception to demonstrate exception handling */
    INFO("Triggering intentional fault (undefined instruction)...");
    __asm__ volatile(".word 0x00000000");  /* Undefined instruction */

    /* Should never reach here */
    INFO("Halting");
    while (1) {
        __asm__ volatile("wfi");
    }
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

    /* Initialize physical memory manager */
    uart_puts("\n");
    pmm_init();
    pmm_dump_stats();

    /* Initialize interrupt controller */
    uart_puts("\n");
    INFO("Initializing GIC...");
    gic_init();

    /* Initialize timer (but don't start yet) */
    INFO("Initializing timer...");
    timer_init();

    /* Initialize scheduler */
    uart_puts("\n");
    scheduler_init();

    /* Create main task */
    struct task *main_task = task_create("main", main_task_func, NULL);
    if (!main_task) {
        panic("Failed to create main task");
    }
    scheduler_add_task(main_task);

    /* Start timer - will generate periodic interrupts */
    INFO("Starting timer (100 Hz)...");
    timer_start();

    /* Enable interrupts */
    INFO("Enabling interrupts...");
    __asm__ volatile("msr daifclr, #0x2");  /* Clear IRQ mask */

    /* Start scheduler - this does not return */
    INFO("Starting scheduler...");
    scheduler_start();

    /* Should never reach here */
    panic("scheduler_start returned!");
}
