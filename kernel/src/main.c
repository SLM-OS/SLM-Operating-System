/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"
#include "uart.h"
#include "debug.h"
#include "pmm.h"
#include "vmm.h"
#include "ncmem.h"
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
#if defined(PLATFORM_JETSON_ORIN_NANO)
extern const struct gpu_driver gpu_nvidia_driver;
extern void nvidia_gpu_set_mmio_base(uintptr_t base);
#endif

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
     * Disable hardware watchdog timer.
     *
     * Linux starts a hardware watchdog (Tegra WDT) with a 120-second
     * timeout. After kexec, the watchdog continues running and will
     * reset the system unless disabled. Do this early — before any
     * time-consuming initialization like SMP boot.
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
     * Raspberry Pi 5: Blink LED to confirm kernel reached C code.
     * BCM2712 GPIO2 controls ACT LED (bit 9).
     */
    {
        volatile uint32_t *gpio2_data = (volatile uint32_t *)0x107D517C04ULL;
        for (int i = 0; i < 3; i++) {
            *gpio2_data |= (1 << 9);   /* LED ON */
            for (volatile int d = 0; d < 500000; d++);
            *gpio2_data &= ~(1 << 9);  /* LED OFF */
            for (volatile int d = 0; d < 500000; d++);
        }
    }
#endif

    /* Initialize UART for debug output */
    uart_init();

    /* Parse Device Tree (must be done early, before using platform values) */
    fdt_info_t fdt_info = {0};
    int dtb_ret = dtb_parse(dtb, &fdt_info);

    /* Results stored globally, accessible via dtb_get_info() */

#if defined(PLATFORM_RASPI5)
    /*
     * Pi 5 DTB recovery: the firmware places the DTB near the top of RAM
     * (typically around 0x2efec000). When using an armstub, the DTB
     * pointer in x0 may not be preserved through the EL3→EL2 transition.
     * Scan backwards from the top of low RAM to find it.
     */
    if (!dtb) {
        /* FDT magic is 0xD00DFEED (big-endian) */
        for (uint64_t addr = 0x2FFF0000; addr >= 0x2E000000; addr -= 0x1000) {
            uint32_t *p = (uint32_t *)addr;
            if (*p == 0xEDFE0DD0) {  /* 0xD00DFEED in little-endian */
                dtb = (void *)addr;
                break;
            }
        }
        if (dtb) {
            uart_printf("[BOOT] DTB found by scan at %p\n", dtb);
        }
    }
#endif

    /* Banner */
    uart_puts("\n");
    uart_puts("========================================\n");
    uart_puts("  SLM-OS v0.1.0\n");
    uart_puts("  Small Language Model Operating System\n");
    uart_puts("========================================\n\n");

    INFO("Boot successful");
#if defined(PLATFORM_JETSON_ORIN_NANO)
    INFO("Running at EL2 (VHE) on %s", PLATFORM_NAME);
#else
    INFO("Running at EL1 on %s", PLATFORM_NAME);
#endif

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

    /* Initialize non-cacheable shared memory region (Pi 5 only) */
    ncmem_init();

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

#if defined(PLATFORM_RASPI5)
    /* Enable UART RX interrupts (after GIC + SMP init) */
    uart_irq_init();
#endif

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
#if defined(PLATFORM_JETSON_ORIN_NANO)
    nvidia_gpu_set_mmio_base(GPU_BASE);
    gpu_register_driver(&gpu_nvidia_driver);
#else
    gpu_register_driver(&gpu_stub_driver);
#endif
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

    /*
     * Start scheduler - this selects the first task, starts the timer,
     * enables interrupts, and does not return.
     *
     * Timer and interrupts must be enabled AFTER the first task is selected,
     * otherwise a timer IRQ could fire before task_current() is set,
     * causing schedule() to dereference NULL.
     */
    INFO("Starting scheduler...");
    scheduler_start();

    /* Should never reach here */
    panic("scheduler_start returned!");
}
