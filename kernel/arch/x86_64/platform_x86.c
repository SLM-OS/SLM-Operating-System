/*
 * platform_x86.c - x86-64 platform initialization and stubs
 *
 * Provides boot glue, SMP/VMM/DTB/Rust stubs, and the entry point
 * wrapper that bridges the Multiboot2 entry to the main kernel.
 */

#include "platform.h"

#if defined(PLATFORM_X86_64)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "smp.h"
#include "dtb.h"
#include "uart.h"
#include "spinlock.h"

/* ---- Boot entry bridge ---- */

/* Saved Multiboot2 info address (set by entry64.S → kernel_main_x86) */
uint32_t multiboot_info_addr;

/* Main kernel entry (from kernel/src/main.c) */
extern void kernel_main(void *dtb);

/*
 * x86-64 boot entry point — called from entry64.S.
 * Saves the multiboot info pointer, then calls the main kernel.
 */
void kernel_main_x86(uint32_t mb_addr)
{
    multiboot_info_addr = mb_addr;
    kernel_main(NULL);  /* No DTB on x86-64 */
}

/* ---- SMP stubs (single core) ---- */

struct per_cpu cpu_data[MAX_CPUS];
uint64_t cpu_logical_map[MAX_CPUS];
uint32_t cpu_count = 1;
volatile uint32_t cpus_online = 1;

void smp_init(void)
{
    cpu_data[0].cpu_id = 0;
    cpu_data[0].mpidr = 0;
    cpu_data[0].online = true;
    cpu_data[0].stack_top = NULL;
    cpu_logical_map[0] = 0;
    cpu_count = 1;
    cpus_online = 1;

    uart_printf("[SMP] Single core (x86-64, no APIC SMP yet)\n");
}

/* cpu_logical_id is used by ARM64 cpu_id() — provide stub */
int cpu_logical_id(uint64_t mpidr)
{
    (void)mpidr;
    return 0;
}

void secondary_init(uint32_t cpu)
{
    (void)cpu;
}

int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_point, uintptr_t context_id)
{
    (void)target_mpidr;
    (void)entry_point;
    (void)context_id;
    return -1;  /* Not supported */
}

void psci_cpu_off(void)
{
    __asm__ volatile("cli; hlt");
    while (1);
}

void psci_system_reset(void)
{
    /* Attempt keyboard controller reset */
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0xFE), "Nd"((uint16_t)0x64));
    /* If that didn't work, triple fault */
    __asm__ volatile("lidt %0" : : "m"(*(uint64_t *)0));
    __asm__ volatile("int3");
    while (1);
}

void psci_system_off(void)
{
    uart_puts("System halted.\n");
    __asm__ volatile("cli");
    while (1)
        __asm__ volatile("hlt");
}

/* ---- VMM stub ---- */

/* spinlock_hw_enabled is referenced by spinlock.h */
volatile int spinlock_hw_enabled = 1;  /* x86-64 coherency always works */

void vmm_init(void)
{
    /* Identity mapping is already set up by trampoline32.S (1GB, 2MB pages).
     * No additional page table setup needed for Phase 1. */
    uart_printf("[VMM] Using boot identity mapping (1 GB)\n");
}

/* ---- DTB stubs ---- */

static fdt_info_t dummy_fdt_info;

int dtb_parse(const void *dtb, fdt_info_t *info)
{
    (void)dtb;
    if (info) {
        /* Zero out — main.c checks fields */
        for (unsigned i = 0; i < sizeof(*info); i++)
            ((uint8_t *)info)[0] = 0;
    }
    return FDT_ERR_BADPTR;  /* No DTB on x86-64 */
}

void dtb_print_info(const fdt_info_t *info)
{
    (void)info;
}

const fdt_info_t *dtb_get_info(void)
{
    return &dummy_fdt_info;
}

/* ---- Rust FFI stubs ---- */

void rust_heap_init(void *heap_start, size_t heap_size)
{
    (void)heap_start;
    (void)heap_size;
}

int rust_init(void)
{
    return 42;  /* Magic value expected by main.c */
}

void rust_hello(void)
{
    /* No Rust runtime on x86-64 yet */
}

int rust_model_mem_init(void)
{
    return 0;
}

int rust_run_tests(void)
{
    return 0;
}

/* Component system init (normally in Rust runtime) */
int component_system_init(void)
{
    return 0;
}

/* ---- GPU cache stubs (gpu.h declares non-static versions) ---- */

void cache_clean_range(void *addr, size_t size)
{
    (void)addr; (void)size;
}

void cache_invalidate_range(void *addr, size_t size)
{
    (void)addr; (void)size;
}

void cache_flush_range(void *addr, size_t size)
{
    (void)addr; (void)size;
}

/* ---- Lua shell stub ---- */

void lua_shell_init(void)
{
    /* No Lua runtime on x86-64 yet */
}

/* ---- VMM stats stub ---- */

struct vmm_stats {
    uint64_t total_mappings;
    uint64_t device_mappings;
    uint64_t table_pages;
};

void vmm_get_stats(struct vmm_stats *stats)
{
    if (stats) {
        stats->total_mappings = 512;  /* 1GB identity map */
        stats->device_mappings = 0;
        stats->table_pages = 3;       /* PML4 + PDPT + PD */
    }
}

/* ---- Rust model memory stubs ---- */

struct pool_stats {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint32_t num_allocs;
};

void rust_weight_pool_stats(struct pool_stats *stats)
{
    if (stats) {
        stats->total_bytes = 0;
        stats->used_bytes = 0;
        stats->free_bytes = 0;
        stats->num_allocs = 0;
    }
}

void rust_workspace_pool_stats(struct pool_stats *stats)
{
    if (stats) {
        stats->total_bytes = 0;
        stats->used_bytes = 0;
        stats->free_bytes = 0;
        stats->num_allocs = 0;
    }
}

/* ---- Component system stubs ---- */

uint32_t component_count(void) { return 0; }

struct component_info {
    uint32_t id;
    char name[32];
    uint32_t state;
    uint32_t type;
};

int component_get_info(uint32_t index, struct component_info *info)
{
    (void)index; (void)info;
    return -1;
}

const char *component_state_name(uint32_t state)
{
    (void)state;
    return "unknown";
}

const char *component_type_name(uint32_t type)
{
    (void)type;
    return "unknown";
}

int component_register(const char *name, uint32_t type, void *config)
{
    (void)name; (void)type; (void)config;
    return -1;
}

int component_unregister(uint32_t id)
{
    (void)id;
    return -1;
}

int component_find(const char *name)
{
    (void)name;
    return -1;
}

/* ---- VMM mapping stubs ---- */

void *vmm_map_region(uint64_t phys, size_t size, uint64_t flags)
{
    (void)flags;
    (void)size;
    /* Identity mapping — virtual == physical */
    return (void *)phys;
}

void vmm_unmap_block(void *virt)
{
    (void)virt;
}

#endif /* PLATFORM_X86_64 */
