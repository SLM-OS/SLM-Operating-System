/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"
#include "uart.h"
#include "debug.h"
#include "pmm.h"
#include "vmm.h"
#include "task.h"
#include "sched.h"
#include "gic.h"
#include "timer.h"
#include "smp.h"
#include "ipc.h"
#include "slm_ffi.h"
#include "test_harness.h"
#include "gpu.h"
#include "shell.h"
#include "dtb.h"
#include "bpmp.h"
#include "vfs.h"
#include "component.h"
#include "blkdev.h"
#include "ramdisk.h"
#include "littlefs_slm.h"
#include "littlefs_vfs.h"
#include "help.h"
#if defined(PLATFORM_QEMU_VIRT)
#include "net.h"
#endif
#include <stdint.h>
#include <stdbool.h>

/* External GPU drivers */
extern const struct gpu_driver gpu_stub_driver;

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

/* ============================================================================
 * Main Task
 * ============================================================================ */

/*
 * Main task - runs tests or does nothing (shell handles interaction)
 */
static void main_task_func(void *arg)
{
    (void)arg;

#ifdef ENABLE_BOOT_TESTS
    /* Run all test suites (Unity + integration + Rust FFI) */
    test_harness_init();
    test_harness_run_all();  /* Exits via semihosting on completion */
#endif

    /* Shell mode - shell task handles everything, just exit */
    task_exit();
}

/*
 * kernel_main - Main kernel entry point
 *
 * Called from boot.S after basic hardware initialization.
 * @param dtb  Pointer to Device Tree Blob from bootloader (may be NULL)
 * This function should not return.
 */
void kernel_main(void *dtb)
{
#if defined(PLATFORM_JETSON_ORIN_NANO) && defined(JETSON_REBOOT_CHECKPOINT)
    /*
     * CHECKPOINT TEST: Trigger immediate reboot via PSCI.
     * If the Jetson reboots back to Linux, the kernel reached kernel_main().
     * Enable by adding -DJETSON_REBOOT_CHECKPOINT to CFLAGS.
     */
    {
        register uint64_t x0 __asm__("x0") = 0x84000009;  /* PSCI SYSTEM_RESET */
        __asm__ volatile("smc #0" : "+r"(x0) :: "memory");
        while(1) __asm__ volatile("wfi");
    }
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * After kexec, the ARM64 exclusive monitor and event flags may be in
     * undefined states. Clear them before using any spinlocks.
     * - CLREX clears the exclusive monitor (prevents stale exclusive access)
     * - SEVL sets event locally (ensures first WFE in spinlock doesn't hang)
     *
     * This is safe to do even in UEFI context.
     */
    __asm__ volatile(
        "clrex\n"       /* Clear exclusive monitor */
        "sevl\n"        /* Set event locally */
        "wfe\n"         /* Consume the event we just set */
        ::: "memory"
    );

    /*
     * Early device access (watchdog, UART) is skipped for direct UEFI boot.
     * UEFI doesn't have these device registers mapped. After vmm_init()
     * sets up our own page tables, these devices will be accessible.
     *
     * For kexec boot, define JETSON_KEXEC_BOOT to enable early device access.
     */
#if defined(JETSON_KEXEC_BOOT)
    /*
     * Disable hardware watchdog timer.
     *
     * Linux starts a watchdog with a 120 second timeout. After kexec, the
     * watchdog continues running and will reset the system unless disabled.
     * We must do this BEFORE any time-consuming initialization.
     */
    {
        volatile uint32_t *wdt_unlock = (volatile uint32_t *)(WDT_BASE + WDT_UNLOCK);
        volatile uint32_t *wdt_cmd = (volatile uint32_t *)(WDT_BASE + WDT_CMD);

        /* Unlock the watchdog registers */
        *wdt_unlock = WDT_UNLOCK_PATTERN;
        __asm__ volatile("dsb sy" ::: "memory");

        /* Disable the watchdog counter */
        *wdt_cmd = WDT_CMD_DISABLE;
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /*
     * EARLY DEBUG: Write directly to UART before uart_init()
     * This tests if UART hardware is accessible after kexec.
     * UARTA is at 0x03100000, THR is at offset 0 (reg-shift=2).
     *
     * Don't wait for THRE - if UART clock is off, we'd hang forever.
     * Just blast characters and add delays.
     */
    {
        volatile uint32_t *uart_thr = (volatile uint32_t *)0x03100000;

        /* Send "SLM" without waiting (in case UART clock is off) */
        for (int i = 0; i < 100000; i++) __asm__ volatile("nop");
        *uart_thr = 'S';
        for (int i = 0; i < 100000; i++) __asm__ volatile("nop");
        *uart_thr = 'L';
        for (int i = 0; i < 100000; i++) __asm__ volatile("nop");
        *uart_thr = 'M';
        for (int i = 0; i < 100000; i++) __asm__ volatile("nop");
        *uart_thr = '\r';
        for (int i = 0; i < 100000; i++) __asm__ volatile("nop");
        *uart_thr = '\n';
        __asm__ volatile("dsb sy" ::: "memory");
    }
#endif /* JETSON_KEXEC_BOOT */
#endif /* PLATFORM_JETSON_ORIN_NANO */

    /*
     * JETSON HARDWARE BRING-UP
     *
     * When JETSON_EARLY_UART_TEST is enabled (set to 1), this code runs a
     * minimal UART test before full kernel initialization. This is useful
     * for hardware bring-up when debugging boot issues.
     *
     * Requirements:
     * - USB-serial adapter connected to 40-pin header pins 8/10 (UARTA)
     * - BPMP clock enable for UARTA (handled below)
     *
     * See docs/jetson-tcu.md for notes on why TCU (USB-C debug) doesn't work
     * after kexec - it requires SPE firmware cooperation.
     */
#define JETSON_EARLY_UART_TEST 0  /* Disabled - testing uart_init */

#if defined(PLATFORM_RASPI5)
    /*
     * Pi 5 RP1 UART0 initialization with LED validation.
     * GPIO14 = TXD (alt function 4), GPIO15 = RXD (alt function 4)
     */
    {
        volatile uint32_t *gpio2_data = (volatile uint32_t *)0x107D517C04ULL;

        /* RP1 GPIO registers for pin muxing */
        volatile uint32_t *gpio14_ctrl = (volatile uint32_t *)0x1F000D0074ULL;
        volatile uint32_t *gpio15_ctrl = (volatile uint32_t *)0x1F000D007CULL;
        volatile uint32_t *gpio14_pads = (volatile uint32_t *)0x1F000F003CULL;
        volatile uint32_t *gpio15_pads = (volatile uint32_t *)0x1F000F0040ULL;

        /* RP1 PL011 UART0 registers */
        volatile uint32_t *uart_dr   = (volatile uint32_t *)0x1F00030000ULL;
        volatile uint32_t *uart_ibrd = (volatile uint32_t *)0x1F00030024ULL;
        volatile uint32_t *uart_fbrd = (volatile uint32_t *)0x1F00030028ULL;
        volatile uint32_t *uart_lcrh = (volatile uint32_t *)0x1F0003002CULL;
        volatile uint32_t *uart_cr   = (volatile uint32_t *)0x1F00030030ULL;

        /* Brute-force test: cycle through alt functions 0-8 */
        for (int alt = 0; alt <= 8; alt++) {
            /* Blink (alt+1) times to identify iteration */
            for (int b = 0; b <= alt; b++) {
                *gpio2_data |= (1 << 9);
                for (volatile int d = 0; d < 300000; d++);
                *gpio2_data &= ~(1 << 9);
                for (volatile int d = 0; d < 300000; d++);
            }

            /* Configure GPIO14 with current alt function */
            *gpio14_pads = (1 << 6) | (2 << 4);  /* IE=1, drive=8mA */
            *gpio14_ctrl = alt;  /* Try this FUNCSEL */

            /* Configure GPIO15 same way */
            *gpio15_pads = (1 << 6) | (1 << 3);  /* IE=1, pull-up */
            *gpio15_ctrl = alt;

            /* Initialize PL011 UART */
            *uart_cr = 0;
            *uart_ibrd = 23;
            *uart_fbrd = 56;
            *uart_lcrh = (3 << 5);
            *uart_cr = (1 << 0) | (1 << 8);

            /* Send "SLM" multiple times */
            for (int r = 0; r < 10; r++) {
                *uart_dr = 'S';
                *uart_dr = 'L';
                *uart_dr = 'M';
                for (volatile int d = 0; d < 100000; d++);
            }

            /* 1 second pause before next iteration */
            for (volatile int d = 0; d < 50000000; d++);
        }

        /* Done - long blink */
        *gpio2_data |= (1 << 9);
        for (volatile int d = 0; d < 50000000; d++);
        *gpio2_data &= ~(1 << 9);

        /* Hang */
        while(1);
    }
#endif

    /* Initialize UART for debug output */
    uart_init();

    /* Parse Device Tree (must be done early, before using platform values) */
    fdt_info_t fdt_info = {0};
    int dtb_ret = dtb_parse(dtb, &fdt_info);

    /* Results stored globally, accessible via dtb_get_info() */

    /* Banner */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("  SLM-OS v0.1.0\n");
    uart_puts("  Small Language Model Operating System\n");
    uart_puts("========================================\n\n");

    INFO("Boot successful");
    INFO("Running at EL1 on %s", PLATFORM_NAME);

    /* Show DTB parsing results */
    if (dtb_ret == FDT_OK) {
        INFO("DTB parsed successfully at %p", dtb);
        dtb_print_info(&fdt_info);
    } else {
        WARN("DTB parsing failed (code=%d), using platform defaults", dtb_ret);
    }

    /* Show memory layout */
    uart_puts("\n");
    print_memory_info();

    /* Initialize physical memory manager */
    uart_puts("\n");
    pmm_init();
    pmm_dump_stats();

    /* Initialize virtual memory manager and enable MMU */
    uart_puts("\n");
    vmm_init();

    /* Initialize interrupt controller */
    uart_puts("\n");
    INFO("Initializing GIC...");
    gic_init();

    /* Initialize timer (but don't start yet) */
    INFO("Initializing timer...");
    timer_init();

    /* Initialize SMP and boot secondary CPUs */
    uart_puts("\n");
    smp_init();

    /* Initialize scheduler */
    uart_puts("\n");
    scheduler_init();

    /* Initialize IPC subsystem */
    ipc_init();

    /* Initialize Virtual Filesystem */
    INFO("Initializing VFS...");
    vfs_init();

    /* Initialize Block Device Subsystem and LittleFS */
    INFO("Initializing filesystem subsystems...");
    blkdev_init();
    littlefs_init();

    /* Create RAM disk for file storage (1 MB) */
    struct blkdev *ramdisk = ramdisk_create_default("ramdisk0");
    if (ramdisk) {
        if (blkdev_register(ramdisk) == BLKDEV_OK) {
            /* Mount LittleFS at /mnt/files */
            struct lfs_mount *lfs_mnt = littlefs_mount_at("/mnt/files", ramdisk, true);
            if (lfs_mnt) {
                INFO("  LittleFS mounted at /mnt/files (1 MB)");

                /* Create a welcome file for testing */
                int f = littlefs_file_open(lfs_mnt, "/hello.txt",
                                           LFS_O_WRONLY | LFS_O_CREAT);
                if (f >= 0) {
                    const char *msg = "Hello from SLM-OS LittleFS!\n";
                    littlefs_file_write(lfs_mnt, f, msg, 28);
                    littlefs_file_close(lfs_mnt, f);
                }

                /* Create a readme file */
                f = littlefs_file_open(lfs_mnt, "/readme.txt",
                                       LFS_O_WRONLY | LFS_O_CREAT);
                if (f >= 0) {
                    const char *readme =
                        "SLM-OS LittleFS File System\n"
                        "===========================\n"
                        "This is a RAM-backed filesystem for testing.\n"
                        "Files will not persist across reboots.\n";
                    littlefs_file_write(lfs_mnt, f, readme, 127);
                    littlefs_file_close(lfs_mnt, f);
                }

                /* Initialize file-driven help system */
                if (help_init() == 0) {
                    INFO("  Help system initialized (/mnt/files/help/)");
                }
            } else {
                WARN("Failed to mount LittleFS");
            }
        } else {
            WARN("Failed to register RAM disk");
        }
    } else {
        WARN("Failed to create RAM disk");
    }

    /* Initialize Rust runtime */
    INFO("Initializing Rust runtime...");

    /* Allocate heap for Rust (1MB = 256 pages) */
    void *rust_heap = pmm_alloc_pages(256);
    if (!rust_heap) {
        panic("Failed to allocate Rust heap");
    }
    rust_heap_init(rust_heap, 256 * 4096);
    INFO("  Rust heap: %p (%u KB)", rust_heap, (256 * 4096) / 1024);

    /* Call Rust init and verify */
    int magic = rust_init();
    if (magic != 42) {
        panic("Rust init failed (expected 42, got %d)", magic);
    }
    INFO("  Rust init: OK (magic=%d)", magic);

    /* Say hello from Rust */
    rust_hello();

    /* Initialize component system */
    INFO("Initializing component system...");
    if (component_system_init() != 0) {
        panic("Component system init failed");
    }

    /* Initialize model memory pools */
    INFO("Initializing model memory...");
    int model_init = rust_model_mem_init();
    if (model_init != 0) {
        WARN("Model memory init failed (code=%d)", model_init);
    } else {
        INFO("  Model memory: OK (16 MB weights, 8 MB workspace)");
    }

    /* Initialize GPU subsystem */
    INFO("Initializing GPU...");
    gpu_register_driver(&gpu_stub_driver);  /* QEMU uses stub driver */
    int gpu_ret = gpu_init();
    if (gpu_ret != GPU_OK) {
        WARN("GPU init failed (code=%d)", gpu_ret);
    }

    /* Create main task (runs tests) */
    struct task *main_task = task_create("main", main_task_func, NULL);
    if (!main_task) {
        panic("Failed to create main task");
    }
    scheduler_add_task(main_task);

    /* Start shell task (interactive debug console) */
    shell_start();

    /* Note: Network initialization is done via 'net init' shell command
     * because VirtIO MMIO needs to be mapped first. See cmd_net in shell.c */

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
