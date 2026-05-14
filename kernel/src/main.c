/*
 * main.c - SLM-OS kernel main entry point
 */

#include "platform.h"
#include "build_info.h"
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
#include "cpu_supervisor.h"
#include "ipc.h"
#include "slm_ffi.h"
#include "test_harness.h"
#include "gpu.h"
#include "shell.h"
#include "dtb.h"
#if defined(PLATFORM_JETSON_ORIN_NANO)
#include "../gpu/nvidia/ga10b_handoff_reserve.h"
#endif
#if !defined(PLATFORM_X86_64)
#include "hailo_trace.h"
#endif
#include "bpmp.h"
#include "vfs.h"
#include "component.h"
#include "blkdev.h"
#include "persistent_lfs_store.h"
#include "ramdisk.h"
#include "littlefs_slm.h"
#include "littlefs_vfs.h"
#include "blob_autoload.h"
#include "boot_media.h"
#include "help.h"
#include "oplib_pool.h"
#if defined(ENABLE_NETWORKING)
#include "net.h"
#include "net_driver.h"
#if defined(PLATFORM_QEMU_VIRT)
#include "virtio_net.h"
#endif
#endif
#include <stdint.h>
#include <stdbool.h>

/* SLMOS_BUILD_STAMP is YYYYMMDDhhmmss UTC — fixed 14 chars, string-sortable. */
static_assert(sizeof(SLMOS_BUILD_STAMP) - 1 == 14,
              "SLMOS_BUILD_STAMP must be exactly 14 characters (YYYYMMDDhhmmss UTC)");

/* External GPU drivers */
extern const struct gpu_driver gpu_stub_driver;
#if defined(PLATFORM_JETSON_ORIN_NANO)
extern const struct gpu_driver gpu_nvidia_driver;
extern void nvidia_gpu_set_mmio_base(uintptr_t base);
extern void jetson_gsp_platform_install(void);
extern int  gsp_init(void);
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
     *
     * Pre-MMU MMIO must use the literal physical address — vmm hasn't
     * mapped GPIO2_BASE yet at this point. Named constant matches the
     * naming convention used by WDT_BASE / WDT_UNLOCK below so a
     * future copy-paste error is easier to catch.
     */
    {
        #define BCM2712_GPIO2_DATA_REG  0x107D517C04ULL
        volatile uint32_t *gpio2_data =
            (volatile uint32_t *)BCM2712_GPIO2_DATA_REG;
        for (int i = 0; i < 3; i++) {
            *gpio2_data |= (1 << 9);   /* LED ON */
            for (volatile int d = 0; d < 500000; d++);
            *gpio2_data &= ~(1 << 9);  /* LED OFF */
            for (volatile int d = 0; d < 500000; d++);
        }
        #undef BCM2712_GPIO2_DATA_REG
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
        /* FDT magic is 0xD00DFEED (big-endian).
         *
         * Bound the scan against the platform's actual RAM window
         * (RAM_BASE..RAM_BASE+RAM_SIZE). The hardcoded scan top
         * 0x2FFF0000 is just below the end of Pi 5's low 1 GB
         * region; if a future firmware version hands off a smaller
         * RAM window the unguarded scan could fault before VMM
         * init. Clamp explicitly.
         *
         * `ram_end > 0x1000` guard prevents an underflow if a
         * future platform.h regression sets RAM_BASE+RAM_SIZE to a
         * value where the addition wraps to ≤ 0x1000 — without it,
         * `ram_end - 0x1000` would underflow to ~SIZE_MAX and the
         * scan would walk wild memory. */
        uint64_t scan_top = 0x2FFF0000;
        uint64_t scan_bot = 0x2E000000;
        const uint64_t ram_end = (uint64_t)RAM_BASE + (uint64_t)RAM_SIZE;
        if (scan_top > ram_end) {
            scan_top = (ram_end > 0x1000) ? ram_end - 0x1000 : 0;
        }
#if RAM_BASE != 0
        /* Skip on platforms where RAM_BASE is 0 (Pi 5) — the
         * comparison would be `scan_bot < 0` which `-Werror=type-
         * limits` flags as always-false on unsigned. The clamp is
         * a no-op there: scan_bot (0x2E000000) is already > 0. */
        if (scan_bot < (uint64_t)RAM_BASE) scan_bot = (uint64_t)RAM_BASE;
#endif
        for (uint64_t addr = scan_top; addr >= scan_bot; addr -= 0x1000) {
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
    uart_puts("  SLM-OS v" SLMOS_VERSION
              " build " SLMOS_BUILD_STAMP
              " (" SLMOS_BUILD_SHA ")\n");
    uart_puts("  Small Language Model Operating System\n");
    uart_puts("========================================\n\n");

    INFO("Boot successful");
#if defined(PLATFORM_X86_64)
    INFO("Running in Ring 0 on %s", PLATFORM_NAME);
#else
    {
        /* Read live CurrentEL so the print reflects the EL the
         * kernel actually ended up at, not what the platform header
         * assumes. With VHE, post-E2H=1 on Pi 5/Jetson, the value
         * reads 0xC (EL2h). Without VHE on QEMU/other, it reads
         * 0x4 (EL1h). When at EL2, also probe HCR_EL2.E2H to
         * differentiate "EL2 with VHE" from "EL2 without VHE". */
        uint64_t current_el;
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
        unsigned el = (unsigned)((current_el >> 2) & 0x3);
        if (el == 2) {
            uint64_t hcr;
            __asm__ volatile("mrs %0, hcr_el2" : "=r"(hcr));
            const char *vhe = (hcr & (1ull << 34)) ? " (VHE)" : "";
            INFO("Running at EL2%s on %s", vhe, PLATFORM_NAME);
        } else {
            INFO("Running at EL%u on %s", el, PLATFORM_NAME);
        }
    }
#endif

    /* Show DTB parsing results */
    if (dtb_ret == FDT_OK) {
        INFO("DTB parsed successfully at %p", dtb);
        dtb_print_info(&fdt_info);

#if defined(ENABLE_NETWORKING)
        /* Seed the lwIP RNG with firmware-supplied entropy from
         * /chosen/{rng-seed,kaslr-seed}. Folds in up to 80 bytes of
         * per-boot entropy that's otherwise discarded. Effect: TCP
         * initial-sequence numbers and ephemeral-port choices vary
         * across reboots instead of starting from a hardcoded LCG
         * state. The underlying RNG is still non-cryptographic.
         *
         * Gated on ENABLE_NETWORKING because lwip_rand_seed lives in
         * kernel/net/sys_arch.c, which is only compiled with
         * networking enabled. */
        const dtb_chosen_t *ch = dtb_get_chosen();
        if (ch->rng_seed_len > 0) {
            lwip_rand_seed(ch->rng_seed, ch->rng_seed_len);
        }
        if (ch->kaslr_seed_len > 0) {
            lwip_rand_seed(ch->kaslr_seed, ch->kaslr_seed_len);
        }
#endif
    } else {
        WARN("DTB parsing failed (code=%d), using platform defaults", dtb_ret);
    }

    /* Show memory layout */
    uart_puts("\n");
    print_memory_info();

#if defined(PLATFORM_X86_64)
    /* x86-64: extend page tables BEFORE PMM so all RAM is accessible */
    uart_puts("\n");
    vmm_init();

    /* Initialize physical memory manager */
    uart_puts("\n");
    pmm_init();
    pmm_dump_stats();
#else
    /*
     * ARM64: vmm_init MUST run before pmm_init.
     *
     * On Tegra234 (Jetson Orin) the SCF L4 cache (4 MiB, 16-way, 8
     * slices — Orin TRM §5.2.1.2.1) sits between CCPLEX and DRAM
     * and isn't enumerated by CLIDR_EL1, so explicit set/way
     * maintenance can't reach it. After kexec it still holds
     * Linux's dirty data for the addresses we're about to use as
     * free-list block heads. With MMU off + SCTLR.C=0, pmm_init's
     * pointer writes go directly to DRAM as Device-nGnRnE; once
     * mmu_enable later activates the data cache, the next load
     * fills L1 from L4's stale shadow, clobbering our free-list
     * pointers and producing the order-19 page fault from #608.
     *
     * Running pmm_init AFTER mmu_enable routes the pointer writes
     * through the now-active coherent fabric, naturally evicting
     * the stale L4 entries via SCF write-allocate at the same
     * cache lines. vmm_init touches only static page-table arrays
     * in BSS, so it has no PMM dependency — safe to run first.
     * QEMU and Pi 5 boot cleanly under either ordering, but the
     * vmm_init→pmm_init sequence is correct on every ARM64
     * platform and removes a class of cache-coherency latent
     * bugs, so the swap is unconditional on every ARM64 target.
     */
    uart_puts("\n");
    vmm_init();

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* #788 root-cause fix: register the kexec'd nvgpu channel's
     * inst block as a PMM user-reserve BEFORE pmm_init publishes
     * pages. The MMU is up (vmm_init just ran) so reading the GPU
     * BAR0 MMIO and the inst-block DRAM page is safe. The reservation
     * feeds into the same /memreserve/ carve path firmware DTB entries
     * use, so kernel allocations naturally steer around the channel's
     * pages and the post-kexec wedge (inst block clobbered with
     * SLM-OS kernel code or model bytes — see PR #797 diagnostic)
     * doesn't fire. Safe to call on non-kexec boots: the function
     * early-returns when FECS_CURRENT_CTX is zero / target=0 / a
     * poisoned-MMIO pattern. */
    ga10b_kexec_handoff_register_reserves();
#endif

    uart_puts("\n");
    pmm_init();
    pmm_dump_stats();
#endif

    /* Initialize non-cacheable shared memory region (Pi 5 only) */
    ncmem_init();

    /* Parse the embedded operator-library blob (#714). Pure CPU side
     * — no PMM / no GPU resources required, just a .rodata read. The
     * default build embeds a 32-byte stub (op_count=0); pass
     * -DOPLIB_BLOB=path to embed a real library produced by
     * scripts/build-operator-library.py. */
    (void)oplib_pool_init();

    /* Move UART lock to NC memory for cross-CPU safety */
    {
        extern void kprintf_init_nc_lock(void);
        kprintf_init_nc_lock();
    }

    /* Initialize interrupt controller */
    uart_puts("\n");
    INFO("Initializing GIC...");
    gic_init();

    /* Initialize timer (but don't start yet) */
    INFO("Initializing timer...");
    timer_init();

#if defined(PLATFORM_HAS_NC_MEMORY)
    /* Zero the NC scheduler init flag BEFORE booting secondary CPUs.
     * Must be after vmm_init() so the write targets NC memory (DRAM),
     * not the cacheable identity map from boot.S. Secondary CPUs will
     * poll this flag after booting — it must be 0 until scheduler_init
     * sets it to 1. */
    {
        extern void nc_zero_sched_init_flag(void);
        nc_zero_sched_init_flag();
    }
#endif

    /* Initialize SMP and boot secondary CPUs */
    uart_puts("\n");
    smp_init();

#if defined(PLATFORM_RASPI5)
    /* Enable UART RX interrupts (after GIC + SMP init) */
    uart_irq_init();
#endif

    /* Initialize Virtual Filesystem and perform all initial PMM allocations
     * BEFORE scheduler_init(). On Pi 5 without SMPEN, PMM spinlock doesn't
     * provide cross-CPU mutual exclusion. Secondary CPUs will allocate
     * idle task stacks after scheduler_init(), so CPU 0 must finish its
     * allocations first to avoid concurrent PMM access. */
    INFO("Initializing VFS...");
    vfs_init();

    /* Initialize Block Device Subsystem and LittleFS */
    INFO("Initializing filesystem subsystems...");
    blkdev_init();
    littlefs_init();

    {
        struct lfs_mount *lfs_mnt = NULL;
        bool seed_defaults = true;
        bool persistent_files_mounted = false;
        struct blkdev *files_dev =
            persistent_lfs_store_create("filesstore0", &seed_defaults);

        if (files_dev && blkdev_register(files_dev) == BLKDEV_OK) {
            lfs_mnt = littlefs_mount_at("/mnt/files", files_dev, seed_defaults);
            if (lfs_mnt) {
                persistent_files_mounted = true;
                INFO("  LittleFS mounted at /mnt/files (persistent boot-FAT image)");
            } else {
                WARN("Failed to mount persistent LittleFS store; reformatting backing image");
                if (persistent_lfs_store_reset(files_dev) == BLKDEV_OK) {
                    seed_defaults = true;
                    lfs_mnt = littlefs_mount_at("/mnt/files", files_dev, true);
                }
                if (lfs_mnt) {
                    persistent_files_mounted = true;
                    INFO("  LittleFS mounted at /mnt/files (persistent boot-FAT image, reformatted)");
                } else {
                    WARN("Failed to recover persistent LittleFS store; falling back to RAM disk");
                }
            }
        } else if (files_dev) {
            WARN("Failed to register persistent LittleFS store; falling back to RAM disk");
            persistent_lfs_store_destroy(files_dev);
        }

        if (!lfs_mnt) {
            struct blkdev *ramdisk = ramdisk_create_default("ramdisk0");
            seed_defaults = true;
            if (ramdisk) {
                if (blkdev_register(ramdisk) == BLKDEV_OK) {
                    lfs_mnt = littlefs_mount_at("/mnt/files", ramdisk, true);
                    if (lfs_mnt) {
                        INFO("  LittleFS mounted at /mnt/files (1 MB RAM fallback)");
                    } else {
                        WARN("Failed to mount LittleFS");
                    }
                } else {
                    WARN("Failed to register RAM disk");
                }
            } else {
                WARN("Failed to create RAM disk");
            }
        }

        if (lfs_mnt) {
            /* Standard device-local storage layout for host tooling,
             * runtime policy blobs, and boot-managed copies. */
            (void)littlefs_mkdir(lfs_mnt, "/policies");
            (void)littlefs_mkdir(lfs_mnt, "/models");
            (void)littlefs_mkdir(lfs_mnt, "/autoload");

            if (seed_defaults) {
                int f = littlefs_file_open(lfs_mnt, "/hello.txt",
                                           LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
                if (f >= 0) {
                    const char *msg = "Hello from SLM-OS LittleFS!\n";
                    littlefs_file_write(lfs_mnt, f, msg, 28);
                    littlefs_file_close(lfs_mnt, f);
                }

                f = littlefs_file_open(lfs_mnt, "/readme.txt",
                                       LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC);
                if (f >= 0) {
                    const char *readme =
                        "SLM-OS LittleFS File System\n"
                        "===========================\n"
                        "This filesystem is persisted in 0:/slmstore/files.lfs\n"
                        "on the boot FAT volume when that storage is available.\n";
                    littlefs_file_write(lfs_mnt, f, readme, 158);
                    littlefs_file_close(lfs_mnt, f);
                }
            }

            /* Boot-managed files under /mnt/files/help, demo scripts,
             * and embedded sample assets are refreshed every boot so
             * built-in admin/demo commands continue to work even on an
             * existing persistent filesystem. */
            if (help_init() == 0) {
                INFO("  Help system initialized (/mnt/files/help/)");
            }

            /* Write demo script to filesystem */
            {
                extern int demo_init(void);
                demo_init();
            }

            if (persistent_files_mounted) {
                persistent_lfs_store_suspend_sync(files_dev);
            }
            blob_autoload_init();
            if (persistent_files_mounted) {
                persistent_lfs_store_resume_sync(files_dev);
                (void)files_dev->ops->sync(files_dev);
            }

            /* Phase 6.2c: write the embedded scheduler MLP .hef
             * (if the kernel was built with SCHEDULER_HEF_BLOB=...)
             * so `hailo load /mnt/files/scheduler_mlp.hef sched`
             * can reach it. Stub is a no-op when not embedded. */
            {
                extern int sched_hef_init(void);
                sched_hef_init();
            }

            /* Phase 8: write the embedded user .hef (if built with
             * USER_HEF_BLOB=...) to /mnt/files/user.hef so Lua
             * scripts can reach it without a FAT driver. Stub is
             * a no-op when not embedded. */
            {
                extern int user_hef_init(void);
                user_hef_init();
            }

            /* Write the embedded MNIST test digits (if built with
             * MNIST_DIGITS_DIR=...) to /mnt/files/digits/ so
             * slm.model_infer_file() has a known-good demo set
             * out of the box. Stub is a no-op when not embedded. */
            {
                extern int mnist_digit_init(void);
                mnist_digit_init();
            }
        }
    }

    /* Initialize Rust runtime */
    INFO("Initializing Rust runtime...");

    /* Allocate heap for Rust. RUST_HEAP_MB is per-platform in
     * <config.h> — sized by SLM forward-path demand (KV cache +
     * ForwardScratch); see the comment block there. Both multiplies
     * are size_t-promoted explicitly so a hypothetical >4 GB heap
     * (RUST_HEAP_MB > 4096) cannot wrap unsigned int before the
     * promotion. The compile-time assert in <config.h> caps
     * RUST_HEAP_MB well below that, but the cast is cheap insurance. */
    const size_t rust_heap_pages = (size_t)RUST_HEAP_MB * (size_t)256; /* 256 pages = 1 MB */
    void *rust_heap = pmm_alloc_pages(rust_heap_pages);
    if (!rust_heap) {
        panic("Failed to allocate Rust heap (%u MB requested)", RUST_HEAP_MB);
    }
    rust_heap_init(rust_heap, rust_heap_pages * (size_t)4096);
    INFO("  Rust heap: %p (%u MB)", rust_heap, RUST_HEAP_MB);

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

    /* Initialize message router (topic-based pub/sub for component IPC) */
    {
        extern void msg_router_init(void);
        msg_router_init();
        component_direct_init();
        INFO("  Message router: OK");
    }

    /* Initialize model memory pools (per-platform sizes from config.h) */
    INFO("Initializing model memory...");
    int model_init = rust_model_mem_init(MODEL_MEM_WEIGHT_MB, MODEL_MEM_WORKSPACE_MB);
    if (model_init != 0) {
        WARN("Model memory init failed (code=%d)", model_init);
    } else {
        INFO("  Model memory: OK (%u MB weights, %u MB workspace)",
             (unsigned)MODEL_MEM_WEIGHT_MB, (unsigned)MODEL_MEM_WORKSPACE_MB);
    }

    /* Initialize model loader registry */
    rust_model_loader_init();

    /* Initialize GPU subsystem */
    INFO("Initializing GPU...");
#if defined(PLATFORM_JETSON_ORIN_NANO)
    /* Best-effort BPMP bring-up. #25 Step 2 ported edk2-nvidia's
     * BPMP IPC client to SLM-OS (kernel/drivers/bpmp/{hsp,ivc,mrq,
     * bpmp}.c); the handshake re-synchronises Sync → Ack → Established
     * on a channel Linux may have left mid-transaction. Failure is
     * non-fatal — the slmos-kexec helper still pre-holds GPU + USB
     * clocks via /sys/kernel/debug/bpmp, so the rest of boot works
     * without an SLM-OS-driven BPMP. A working bpmp unblocks PCIe
     * Step 3 (replay MRQ_UPHY/MRQ_CLK/MRQ_RESET for pex2_c8).
     *
     * The diagnostic BOOT_0 read still confirms the kexec helper
     * ungated the GPU clocks. */
    {
        int bpmp_rc = bpmp_init();
        INFO("BPMP init: rc=%d", bpmp_rc);
        if (bpmp_rc == 0) {
            bool responding = bpmp_is_available();
            INFO("BPMP: %s", responding ? "MRQ_PING round-trip OK"
                                         : "no PING response (firmware busy?)");
        }
    }
    {
        uint32_t boot0_raw = *(volatile uint32_t *)(GPU_BASE + 0x0);
        INFO("GPU raw BOOT_0 read: 0x%08lx (expect 0xB7B000A1 for GA10B)",
             (unsigned long)boot0_raw);
    }
    nvidia_gpu_set_mmio_base(GPU_BASE);
    gpu_register_driver(&gpu_nvidia_driver);
#else
    gpu_register_driver(&gpu_stub_driver);
#endif
    int gpu_ret = gpu_init();
    if (gpu_ret != GPU_OK) {
        WARN("GPU init failed (code=%d)", gpu_ret);
    }
#if defined(PLATFORM_JETSON_ORIN_NANO)
    if (gpu_ret == GPU_OK) {
        jetson_gsp_platform_install();
        int gsp_ret = gsp_init();
        if (gsp_ret < 0) {
            WARN("GSP init incomplete (rc=%d) — GPU compute unavailable", gsp_ret);
        }
    }
#endif

    /* Initialize PCI and GPU subsystems (x86-64 only) */
#if defined(PLATFORM_X86_64)
    {
        extern void pci_init(void);
        extern void nvidia_gpu_init(void);
        pci_init();
        nvidia_gpu_init();
        /* Shell commands registered in shell_init() after shell starts */
    }
#endif

    /* Initialize the ARM64 PCIe host-controller subsystem. Platforms
     * without a backend (Jetson today) return PCIE_ERR_UNSUPPORTED
     * and the call is harmless. Deliberately after GPU init so the
     * GPU subsystem can later register itself on a PCIe GPU once we
     * have one. */
#if !defined(PLATFORM_X86_64)
    /* Parse hailo_trace.{phase,mech}=... from /chosen/bootargs and
     * arm the trace masks (if any). Must run BEFORE pcie_init so
     * Phase 1 (link train) is taggable; the parser is silent when
     * the tokens are absent. Phase explicitly bumped to LINKUP so
     * pcie_init's MMIO carries that tag. */
    {
        const dtb_chosen_t *chosen = dtb_get_chosen();
        if (chosen) hailo_trace_cmdline_parse(chosen->bootargs);
        hailo_trace_set_phase(HAILO_TRACE_PHASE_LINKUP);
    }
    {
        extern int pcie_init(void);
        int pcie_rc = pcie_init();
        (void)pcie_rc;  /* logged by pcie_init itself */
    }

    /* Install the Hailo-8 platform shim. On RASPI5 this finds the
     * AI HAT+ via pcie_find_device and maps its BARs; on other
     * ARM64 platforms the stub returns HAILO_ERR_NODEV and we move
     * on. Either way, the `hailo` shell command reports state. */
    {
        extern int hailo_platform_install(void);
        (void)hailo_platform_install();
    }
#endif

    /* Register platform-specific network driver (before net_init) */
#if defined(ENABLE_NETWORKING)
#if defined(PLATFORM_QEMU_VIRT)
    virtio_net_register();
#elif defined(PLATFORM_X86_64)
    {
        extern void virtio_net_pci_register(void);
        virtio_net_pci_register();
    }
#elif defined(PLATFORM_RASPI5)
    {
        extern void macb_register(void);
        macb_register();
    }
#elif defined(PLATFORM_JETSON_ORIN_NANO)
    {
        /*
         * Jetson networking rides on USB CDC-ECM, not native PCIe
         * Ethernet. The first probe at boot is expected to no-op on
         * kexec boots (xHCI defers enumeration until the user re-
         * plugs — see #309) and succeed cleanly on direct-boot cases.
         * Either way, `net_poll()` retries `cdc_ecm_probe_and_register`
         * on every tick until the class driver binds.
         */
        extern int cdc_ecm_probe_and_register(void);
        (void)cdc_ecm_probe_and_register();
    }
#endif
#endif

#if defined(PLATFORM_JETSON_ORIN_NANO)
    /*
     * Probe the Tegra XHCI host controller (#266 Phase 3A). No-op if
     * the xusb clocks are gated — the init path bails with a warn
     * rather than writing to a dead aperture. Later Phase-3A steps
     * will register this as a usb_hcd and drive URB transfers; today
     * this is just capability parse + halt/reset so the `xhci` shell
     * diagnostic works.
     */
    {
        extern int xhci_init(void);
        (void)xhci_init();
    }
#endif

    /* Initialize scheduler and IPC AFTER all initial PMM allocations.
     * On Pi 5 without SMPEN, PMM spinlock doesn't provide cross-CPU
     * mutual exclusion. scheduler_init() signals secondary CPUs to proceed
     * with their own allocations (idle task stacks). Doing it here ensures
     * CPU 0's allocations (ramdisk, Rust heap, model memory) are complete. */
    uart_puts("\n");
    scheduler_init();

    /* #414: now that scheduler is up and secondary CPUs have
     * cleared their `scheduler_is_initialized` wait, boot media
     * (Pi 5 SDHCI) is allowed to do its expensive bring-up. Without
     * this gate, calling sdhci_create_bcm2712 from VFS init would
     * busy-wait long enough that secondaries time out. */
    boot_media_allow_creates();

#ifdef CONFIG_AI_SCHEDULER
    /* Register the CPU-MLP inference-device backend BEFORE the AI
     * scheduling policies, so sched_ai.c can route MLP inference
     * through the device abstraction (Phase 2 of the AI HAT+ plan).
     * Ordering matters: the MLP policy's self-test in its init()
     * runs via the direct ai_schedule_mlp path and doesn't depend
     * on the registry, but any live assign_cpu() call afterwards
     * will look for the "cpu-mlp" device.
     *
     * The Hailo-8 backend registers alongside the CPU-MLP one on
     * every non-x86 target (Phase 6.2). `load_model` returns
     * INF_ERR_NODEV until hailo_boot succeeds, so registering
     * pre-boot is safe — the device is just inert until the
     * firmware is running. */
    {
        extern int inference_cpu_register(void);
        extern void sched_ai_init(void);
        inference_cpu_register();
#ifndef PLATFORM_X86_64
        extern int inference_device_hailo_register(void);
        inference_device_hailo_register();
#endif
        sched_ai_init();
    }
#endif

    ipc_init();

    /* Create main task (runs tests) */
    struct task *main_task = task_create("main", main_task_func, NULL);
    if (!main_task) {
        panic("Failed to create main task");
    }
#ifdef ENABLE_BOOT_TESTS
    /* Pin the test harness to CPU 0 — several SMP tests assert
     * cpu_id() == 0 and would fail if work-stealing migrated the
     * test task to an idle AP. Non-test builds leave affinity ANY so
     * the scheduler can balance freely. */
    main_task->cpu_affinity = 0;
#endif
    scheduler_add_task(main_task);

#if !defined(ENABLE_BOOT_TESTS)
    /* Spawn the CPU-resurrection supervisor (#216 Tier 2) on platforms
     * that need it. No-op stub on QEMU / Jetson / x86-64. The
     * supervisor watches sched_diag_idle_loops once per second and
     * issues psci_cpu_on for any CPU whose counter has been frozen
     * for SUPERVISOR_FROZEN_THRESHOLD samples. Independent of the
     * test harness's per-test dormancy detector (PR #290) — both
     * can fire on the same condition; the test harness reports first
     * and the supervisor recovers.
     *
     * Gated on !ENABLE_BOOT_TESTS for the same reason as net_pump:
     * scheduler tests check exact CPU 0 ready-count and task lists,
     * and a low-priority background task on CPU 0 perturbs those
     * counts. Tests rely on the manual `cpu resurrect <N>` shell
     * command instead — driven from integration tests when explicit
     * recovery is needed. */
    cpu_supervisor_start();
#endif

#if defined(ENABLE_NETWORKING) && !defined(ENABLE_BOOT_TESTS)
    /* Background network-pump task — drives net_poll() at ~100 Hz so
     * RX + lwIP timers run even when the shell is idle. Without this,
     * SLM-OS only processes inbound traffic while a foreground command
     * explicitly polls (ping's wait loop, net_init's DHCP wait), so
     * the system wouldn't respond to an inbound ping sitting at a
     * prompt. Low priority so shell, tests, and workloads preempt
     * trivially. Safe to spawn before net_init — net_poll returns
     * early until net_initialized is true.
     *
     * Pinned to CPU 0 because:
     *  1. Peripheral IRQs (virtio-mmio SPI, MACB SPI) are affined to
     *     CPU 0, so RX completion lands there anyway.
     *  2. Prevents work-stealing from migrating the pump off CPU 0
     *     during scheduler-migration tests, which rely on task
     *     placement being stable.
     *
     * Priority IDLE so it doesn't compete with any test fixture or
     * workload. net_poll is latency-insensitive at ~10 ms cadence —
     * running only when nothing else is runnable is fine.
     *
     * Guarded on !ENABLE_BOOT_TESTS: the test kernel's scheduler tests
     * check exact assigned_cpu values and task counts; an extra
     * background task pinned to CPU 0 shifts those counts and makes
     * policy tests fail. Live network tests in test_net.c drive
     * net_poll() inline within their wait loops, so they don't need
     * the pump to be running. */
    {
        struct task *net_pump =
            task_create_with_priority("net_pump", net_pump_task_entry, NULL,
                                      TASK_PRIORITY_IDLE);
        if (!net_pump) {
            panic("Failed to create net_pump task");
        }
        net_pump->cpu_affinity = 0;
        scheduler_add_task(net_pump);
    }
#endif

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
    scheduler_start(0);

    /* Should never reach here */
    panic("scheduler_start returned!");
}
