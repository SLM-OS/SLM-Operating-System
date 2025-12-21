/*
 * platform.h - Hardware configuration for SLM-OS
 *
 * NOTE: These are hardcoded values for QEMU virt machine.
 * Replace with DTB parsing when adding Jetson Orin Nano support (Month 2-3).
 */

#ifndef PLATFORM_H
#define PLATFORM_H

/* Platform selection */
#define PLATFORM_QEMU_VIRT  1

#if PLATFORM_QEMU_VIRT

/* Memory layout */
#define RAM_BASE        0x40000000UL
#define RAM_SIZE        0x08000000UL    /* 128 MB default */

/* UART - PL011 on QEMU virt */
#define UART_TYPE_PL011
#define UART_BASE       0x09000000UL
#define UART_CLOCK      24000000UL      /* 24 MHz (QEMU default) */

/* GIC (Generic Interrupt Controller) */
#define GIC_DIST_BASE   0x08000000UL
#define GIC_CPU_BASE    0x08010000UL

/* Timer */
#define TIMER_IRQ       30

/* Kernel stack */
#define STACK_SIZE      0x4000UL        /* 16 KB per stack */

#endif /* PLATFORM_QEMU_VIRT */

#endif /* PLATFORM_H */
